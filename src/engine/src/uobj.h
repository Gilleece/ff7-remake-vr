#pragma once
// Reading the game's objects through the engine's object array and name pool, and calling
// reflected functions through ProcessEvent (docs/re/engine.md section 2 and section 11).
//
// Every read is guarded: an object can be destroyed between two frames, so a stale pointer
// gives a failed read instead of a crash. ProcessEvent must only be called on the game
// thread.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::engine::uobj {

void init(std::uint8_t* object_array, std::uint8_t* name_pool);
bool available();

int num_objects();
void* object_at(int index);
std::string name_text(std::uint32_t id);
std::string object_name(void* obj);
void* class_of(void* obj);
void* outer_of(void* obj);
void* super_of(void* ustruct);  // UStruct::SuperStruct (+0x30; the pointer is checked with VirtualQuery)
// The memory range is committed and readable (VirtualQuery; no fault).
bool readable(const void* p, std::size_t n);
std::string path_of(void* obj);
std::int32_t index_of(void* obj);
// The object is still the live object at its index of the object array.
bool alive(void* obj);
// The object's class is `cls` or derives from it.
bool is_a(void* obj, void* cls);
// The object's class is named `name`.
bool class_named(void* obj, std::string_view name);
// The class or one of its super classes is named `name` (walks UStruct::SuperStruct).
bool class_or_super_named(void* obj, std::string_view name);

template <class T>
bool read(const void* p, T& out);
bool copy_guarded(void* dst, const void* src, std::size_t n);

// ProcessEvent (UObject vtable slot 64). Game thread only.
bool call(void* obj, void* function, void* params);

// Finds objects by name, `outer_name` (empty = any) and the name of their class (empty =
// any), scanning the whole object array. Slow (tens of milliseconds): call once, or from
// the dev pipe.
struct Found {
    void* obj;
    int index;
    std::string cls;
    std::string path;
};
std::vector<Found> find_all(std::string_view name, std::string_view outer_name = {}, std::string_view class_name = {},
                            std::size_t max = 64);

// A set of objects wanted by name, found a slice of the object array per call (so a game
// frame is never held up). `ready()` once every entry is found.
class Lookup {
public:
    struct Entry {
        std::string name;
        std::string outer;  // outer object's name ("" = any)
        std::string cls;    // class name ("" = any)
        void* obj = nullptr;
    };
    explicit Lookup(std::vector<Entry> entries);
    void step(int slots = 32768);
    bool ready() const { return missing_ == 0; }
    void* get(std::size_t i) const { return entries_[i].obj; }
    int passes() const { return passes_; }
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
    std::size_t missing_;
    int pos_ = 0;
    int passes_ = 0;
};

template <class T>
bool read(const void* p, T& out) {
    return copy_guarded(&out, p, sizeof(T));
}

}  // namespace ff7vr::engine::uobj
