#include "uobj.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ff7vr::engine::uobj {
namespace {

// Layouts (docs/re/engine.md section 2 and section 11).
constexpr std::size_t kObjects = 0x10;        // FUObjectArray: FUObjectItem* Objects
constexpr std::size_t kNumElements = 0x1c;    // int32
constexpr std::size_t kItemSize = 0x18;       // FUObjectItem {UObject* Object; ...}
constexpr std::size_t kClass = 0x10;          // UObject::ClassPrivate
constexpr std::size_t kName = 0x18;           // UObject::NamePrivate (FName: ComparisonIndex, Number)
constexpr std::size_t kOuter = 0x20;          // UObject::OuterPrivate
constexpr std::size_t kInternalIndex = 0x0c;  // UObject::InternalIndex
constexpr std::size_t kSuperStruct = 0x30;    // UStruct::SuperStruct, right after UField::Next (no FStructBaseChain in 4.18)
constexpr std::size_t kProcessEventSlot = 64;

std::uint8_t* g_objects = nullptr;
std::uint8_t* g_names = nullptr;

bool process_event(void* obj, void* fn, void* params) {
    __try {
        auto pe = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>((*static_cast<void***>(obj))[kProcessEventSlot]);
        pe(obj, fn, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool copy_guarded(void* dst, const void* src, std::size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void init(std::uint8_t* object_array, std::uint8_t* name_pool) {
    g_objects = object_array;
    g_names = name_pool;
}

bool available() { return g_objects && g_names; }

int num_objects() {
    std::int32_t n = 0;
    if (!g_objects) return 0;
    read(g_objects + kNumElements, n);
    return n;
}

void* object_at(int i) {
    std::uint8_t* items = nullptr;
    if (!g_objects || i < 0 || !read(g_objects + kObjects, items) || !items) return nullptr;
    void* o = nullptr;
    read(items + static_cast<std::size_t>(i) * kItemSize, o);
    return o;
}

std::string name_text(std::uint32_t id) {
    std::uint8_t* block = nullptr;
    if (!g_names || !read(g_names + 0x10 + static_cast<std::size_t>(id >> 16) * 8, block) || !block) return {};
    const std::uint8_t* e = block + 2 * static_cast<std::size_t>(id & 0xffff);
    std::uint16_t hdr = 0;
    if (!read(e, hdr)) return {};
    const unsigned len = hdr >> 6;
    if (hdr & 1) return "?";
    if (len == 0 || len > 1024) return {};
    char buf[1024];
    if (!copy_guarded(buf, e + 2, len)) return {};
    return std::string(buf, len);
}

std::string object_name(void* o) {
    std::uint32_t id = 0;
    if (!o || !read(static_cast<std::uint8_t*>(o) + kName, id)) return {};
    return name_text(id);
}

void* class_of(void* o) {
    void* c = nullptr;
    if (o) read(static_cast<std::uint8_t*>(o) + kClass, c);
    return c;
}

void* outer_of(void* o) {
    void* c = nullptr;
    if (o) read(static_cast<std::uint8_t*>(o) + kOuter, c);
    return c;
}

bool readable(const void* p, std::size_t n) {
    // VirtualQuery instead of a guarded read: a fault is caught, but the crash handler
    // reports first-chance access violations, so pointers that may be stale or wrong are
    // checked first.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const std::uint8_t*>(p) + n <= end;
}

void* super_of(void* s) {
    void* c = nullptr;
    if (s && readable(static_cast<std::uint8_t*>(s) + kSuperStruct, sizeof(void*))) read(static_cast<std::uint8_t*>(s) + kSuperStruct, c);
    return c && readable(c, 0x28) && alive(c) ? c : nullptr;
}

std::string path_of(void* o) {
    std::vector<std::string> parts;
    for (int depth = 0; o && depth < 16; ++depth, o = outer_of(o)) parts.push_back(object_name(o));
    std::string s;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) s += (s.empty() ? "" : ".") + *it;
    return s;
}

std::int32_t index_of(void* o) {
    std::int32_t idx = -1;
    if (o) read(static_cast<std::uint8_t*>(o) + kInternalIndex, idx);
    return idx;
}

bool alive(void* o) {
    if (!o) return false;
    const std::int32_t idx = index_of(o);
    return idx >= 0 && idx < num_objects() && object_at(idx) == o;
}

bool is_a(void* o, void* cls) {
    if (!o || !cls) return false;
    void* c = class_of(o);
    for (int depth = 0; c && depth < 32; ++depth, c = super_of(c))
        if (c == cls) return true;
    return false;
}

bool class_named(void* o, std::string_view name) {
    return object_name(class_of(o)) == name;
}

bool class_or_super_named(void* o, std::string_view name) {
    void* c = class_of(o);
    for (int depth = 0; c && depth < 32; ++depth, c = super_of(c))
        if (object_name(c) == name) return true;
    return false;
}

bool call(void* obj, void* function, void* params) {
    if (!obj || !function) return false;
    return process_event(obj, function, params);
}

std::vector<Found> find_all(std::string_view name, std::string_view outer_name, std::string_view class_name, std::size_t max) {
    std::vector<Found> out;
    const int n = num_objects();
    for (int i = 0; i < n && out.size() < max; ++i) {
        void* o = object_at(i);
        if (!o || object_name(o) != name) continue;
        if (!outer_name.empty() && object_name(outer_of(o)) != outer_name) continue;
        const std::string cls = object_name(class_of(o));
        if (!class_name.empty() && cls != class_name) continue;
        out.push_back(Found{o, i, cls, path_of(o)});
    }
    return out;
}

Lookup::Lookup(std::vector<Entry> entries) : entries_(std::move(entries)), missing_(entries_.size()) {}

void Lookup::step(int slots) {
    if (ready()) return;
    const int n = num_objects();
    if (n <= 0) return;
    const int end = std::min(n, pos_ + slots);
    for (int i = pos_; i < end; ++i) {
        void* o = object_at(i);
        if (!o) continue;
        std::string name;
        for (Entry& e : entries_) {
            if (e.obj) continue;
            if (name.empty()) name = object_name(o);
            if (name != e.name) continue;
            if (!e.outer.empty() && object_name(outer_of(o)) != e.outer) continue;
            if (!e.cls.empty() && object_name(class_of(o)) != e.cls) continue;
            e.obj = o;
            --missing_;
        }
    }
    pos_ = end >= n ? 0 : end;
    if (pos_ == 0) ++passes_;
}

}  // namespace ff7vr::engine::uobj
