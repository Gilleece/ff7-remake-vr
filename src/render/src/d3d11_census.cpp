#include "d3d11_census.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ff7vr::render::census {
namespace {

// ID3D11Device vtable slots (d3d11.h order) that create an object. Every one of them returns
// an HRESULT and takes at most six arguments after `this`, none of them floating point, so one
// forwarding detour with six pointer-sized arguments fits all of them.
struct SlotInfo {
    int slot;
    const char* name;
};
constexpr std::array<SlotInfo, 18> kSlots{{{3, "buffer"},
                                           {4, "tex1d"},
                                           {5, "tex2d"},
                                           {6, "tex3d"},
                                           {7, "srv"},
                                           {8, "uav"},
                                           {9, "rtv"},
                                           {10, "dsv"},
                                           {11, "layout"},
                                           {12, "vs"},
                                           {15, "ps"},
                                           {18, "cs"},
                                           {20, "blend"},
                                           {21, "depthstate"},
                                           {22, "raster"},
                                           {23, "sampler"},
                                           {24, "query"},
                                           {25, "predicate"}}};
constexpr int kMaxSlot = 26;
enum Caller { kMod = 0, kGame = 1, kOther = 2, kCallers = 3 };
constexpr const char* kCallerName[kCallers] = {"mod", "game", "other"};

using Fn6 = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, void*, void*, void*, void*, void*, void*);

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<bool> installed{false};
    std::atomic<ID3D11Device*> device{nullptr};  // the game's device (a reference is held once installed)
    int stereoWindows = 0;                       // XR thread only
    std::mutex installMutex;
    std::array<hook::VTableHook, kMaxSlot> hooks;
    std::array<void*, kMaxSlot> original{};
    std::array<std::array<std::atomic<uint64_t>, kCallers>, kMaxSlot> total{};
    std::array<std::array<uint64_t, kCallers>, kMaxSlot> lastTaken{};  // XR thread only
    uintptr_t modBase = 0, modEnd = 0, exeBase = 0, exeEnd = 0;
    std::mutex siteMutex;
    std::unordered_map<uintptr_t, uint64_t> modSites;      // return address in this DLL -> calls (bounded)
    std::unordered_map<uintptr_t, uint64_t> modSitesTaken;  // value at the previous window
    std::unordered_map<std::wstring, uint64_t> otherModules;
};
State* g = new State;  // never freed: the hooks stay valid until the process ends

void ModuleRange(HMODULE m, uintptr_t& base, uintptr_t& end) {
    MODULEINFO mi{};
    if (m && GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) {
        base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        end = base + mi.SizeOfImage;
    }
}

void Count(int slot, void* ret) {
    const auto a = reinterpret_cast<uintptr_t>(ret);
    Caller c = kOther;
    if (a >= g->modBase && a < g->modEnd) c = kMod;
    else if (a >= g->exeBase && a < g->exeEnd) c = kGame;
    g->total[slot][c].fetch_add(1, std::memory_order_relaxed);
    if (c == kMod) {
        std::lock_guard lk(g->siteMutex);
        auto it = g->modSites.find(a);
        if (it != g->modSites.end()) ++it->second;
        else if (g->modSites.size() < 512) g->modSites.emplace(a, 1);
    } else if (c == kOther) {
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(ret), &m) && m)
            GetModuleFileNameW(m, path, MAX_PATH);
        std::wstring name(path);
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        std::lock_guard lk(g->siteMutex);
        if (g->otherModules.size() < 64 || g->otherModules.count(name)) ++g->otherModules[name];
    }
}

template <int Slot>
HRESULT STDMETHODCALLTYPE Detour(ID3D11Device* d, void* a, void* b, void* c, void* e, void* f, void* h) {
    Count(Slot, _ReturnAddress());
    return reinterpret_cast<Fn6>(g->original[Slot])(d, a, b, c, e, f, h);
}

template <int... S>
constexpr std::array<void*, sizeof...(S)> MakeDetours(std::integer_sequence<int, S...>) {
    return {reinterpret_cast<void*>(&Detour<S>)...};
}
const std::array<void*, kMaxSlot> kDetours = MakeDetours(std::make_integer_sequence<int, kMaxSlot>{});

std::string Totals(bool sinceLast) {
    std::string out;
    for (int c = 0; c < kCallers; ++c) {
        std::string part;
        for (const SlotInfo& s : kSlots) {
            const uint64_t now = g->total[s.slot][c].load(std::memory_order_relaxed);
            const uint64_t v = sinceLast ? now - g->lastTaken[s.slot][c] : now;
            if (sinceLast) g->lastTaken[s.slot][c] = now;
            if (v) part += std::format(" {} {}", s.name, v);
        }
        out += std::format("{}{}:{}", out.empty() ? "" : "; ", kCallerName[c], part.empty() ? " none" : part);
    }
    return out;
}

std::string ModSites(bool sinceLast) {
    std::vector<std::pair<uint64_t, uintptr_t>> v;
    {
        std::lock_guard lk(g->siteMutex);
        for (const auto& [addr, n] : g->modSites) {
            uint64_t& last = g->modSitesTaken[addr];
            const uint64_t d = sinceLast ? n - last : n;
            if (sinceLast) last = n;
            if (d) v.emplace_back(d, addr);
        }
    }
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (size_t i = 0; i < v.size() && i < 6; ++i) s += std::format(" +0x{:x} {}", v[i].second - g->modBase, v[i].first);
    return s.empty() ? std::string(" none") : s;
}

std::string MemoryText() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) return {};
    return std::format("; process private {:.0f} MB, handles {}", double(pm.PrivateUsage) / (1024.0 * 1024.0), [] {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return n;
    }());
}

}  // namespace

void Configure(const Config& cfg) {
    g->enabled = cfg.get_bool("log", "d3d11_census", false);
    if (g->enabled) log::info("d3d11 census: on ([log] d3d11_census = 1): object creations on the game's device counted by caller");
}

void NoteDevice(ID3D11Device* device) {
    if (!g->enabled.load(std::memory_order_relaxed) || !device) return;
    g->device.store(device, std::memory_order_release);
}

namespace {
void Install(ID3D11Device* device) {
    std::lock_guard lk(g->installMutex);
    if (g->installed.load()) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&NoteDevice), &self);
    ModuleRange(self, g->modBase, g->modEnd);
    ModuleRange(GetModuleHandleW(nullptr), g->exeBase, g->exeEnd);
    void** vt = *reinterpret_cast<void***>(device);
    int ok = 0;
    for (const SlotInfo& s : kSlots) {
        if (g->hooks[s.slot].create(vt, static_cast<size_t>(s.slot), kDetours[s.slot])) {
            g->original[s.slot] = g->hooks[s.slot].original<void*>();
            ++ok;
        }
    }
    device->AddRef();  // the vtable stays in use for the rest of the process
    g->installed = true;
    log::info("d3d11 census: {} of {} device slots counted (vtable {}), this DLL 0x{:x}-0x{:x}", ok, kSlots.size(), static_cast<void*>(vt),
              g->modBase, g->modEnd);
}
}  // namespace

std::string TakeWindowLine(bool stereo) {
    if (!g->enabled.load(std::memory_order_relaxed)) return {};
    if (!g->installed.load(std::memory_order_acquire)) {
        ID3D11Device* d = g->device.load(std::memory_order_acquire);
        g->stereoWindows = stereo ? g->stereoWindows + 1 : 0;
        if (d && g->stereoWindows >= 2) Install(d);
        return {};
    }
    return std::format("d3d11 objects created in the window: {}; mod call sites{}{}", Totals(true), ModSites(true), MemoryText());
}

std::string Command(const std::string& args) {
    if (args == "status" || args.empty()) {
        if (!g->enabled) return "ok off ([log] d3d11_census = 0)";
        if (!g->installed) return "ok on, no device yet";
        std::string others;
        {
            std::lock_guard lk(g->siteMutex);
            for (const auto& [name, n] : g->otherModules) others += std::format(" {} {}", log::narrow(name), n);
        }
        return std::format("ok totals since the start: {}; mod call sites{}; other modules{}{}", Totals(false), ModSites(false),
                           others.empty() ? std::string(" none") : others, MemoryText());
    }
    return "err census status";
}

}  // namespace ff7vr::render::census
