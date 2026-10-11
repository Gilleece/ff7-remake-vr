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
#include <chrono>
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
    // The game's CreateBuffer calls: by call site (return address in the executable) and by
    // description (size, usage, bind flags, CPU access, misc flags, initial data or not).
    std::unordered_map<uintptr_t, uint64_t> gameBufferSites, gameBufferSitesTaken;
    std::unordered_map<std::string, uint64_t> gameBufferKinds, gameBufferKindsTaken;
    // Call stacks (offsets into the executable, nearest first) of the game's dynamic vertex buffers.
    std::unordered_map<std::string, uint64_t> gameDynamicStacks, gameDynamicStacksTaken;
    // Large textures and buffers (kLargeBytes or more) by caller and description: count and bytes.
    struct Large {
        uint64_t count = 0, bytes = 0;
    };
    std::unordered_map<std::string, Large> large, largeTaken, largeMarked;  // under siteMutex
    std::array<std::array<uint64_t, kCallers>, kMaxSlot> markTaken{};       // under siteMutex
    std::chrono::steady_clock::time_point markTime = std::chrono::steady_clock::now();
};
State* g = new State;  // never freed: the hooks stay valid until the process ends

// A creation this large counts as a large one (render targets, staging copies, big buffers).
constexpr uint64_t kLargeBytes = 4ull << 20;

// Bytes per texel (block formats: per 4x4 block divided by 16), for the size estimate.
double BytesPerTexel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_A8_UNORM: return 1;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM: return 2;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM: return 0.5;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return 1;
        default: return 4;
    }
}

Caller CallerOf(void* ret) {
    const auto a = reinterpret_cast<uintptr_t>(ret);
    if (a >= g->modBase && a < g->modEnd) return kMod;
    if (a >= g->exeBase && a < g->exeEnd) return kGame;
    return kOther;
}

void NoteLarge(Caller c, std::string what, uint64_t bytes) {
    if (bytes < kLargeBytes) return;
    std::lock_guard lk(g->siteMutex);
    auto key = std::format("{} {}", kCallerName[c], what);
    auto it = g->large.find(key);
    if (it == g->large.end()) {
        if (g->large.size() >= 1024) return;
        it = g->large.emplace(std::move(key), State::Large{}).first;
    }
    ++it->second.count;
    it->second.bytes += bytes;
}

void CountLarge(int slot, void* ret, const void* desc) {
    if (!desc) return;
    if (slot == 5) {
        const auto* d = static_cast<const D3D11_TEXTURE2D_DESC*>(desc);
        double bytes = double(d->Width) * double(d->Height) * BytesPerTexel(d->Format) * std::max(1u, d->ArraySize) * std::max(1u, d->SampleDesc.Count);
        if (d->MipLevels != 1) bytes *= 4.0 / 3.0;
        const char* usage = d->Usage == D3D11_USAGE_STAGING ? " staging" : d->Usage == D3D11_USAGE_DYNAMIC ? " dynamic" : "";
        NoteLarge(CallerOf(ret),
                  std::format("tex {}x{}{} f{} b{:x}{}", d->Width, d->Height, d->ArraySize > 1 ? std::format("x{}", d->ArraySize) : std::string(),
                              static_cast<int>(d->Format), d->BindFlags, usage),
                  static_cast<uint64_t>(bytes));
    } else if (slot == 3) {
        const auto* d = static_cast<const D3D11_BUFFER_DESC*>(desc);
        NoteLarge(CallerOf(ret), std::format("buffer {} b{:x}", d->ByteWidth, d->BindFlags), d->ByteWidth);
    }
}

// Large creations since `taken` (and `taken` moved up to now), largest first.
std::string LargeText(std::unordered_map<std::string, State::Large>& taken) {
    std::vector<std::pair<std::string, State::Large>> v;
    {
        std::lock_guard lk(g->siteMutex);
        for (const auto& [key, now] : g->large) {
            State::Large& t = taken[key];
            if (now.count > t.count) v.emplace_back(key, State::Large{now.count - t.count, now.bytes - t.bytes});
            t = now;
        }
    }
    if (v.empty()) return " none";
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.bytes > y.second.bytes; });
    uint64_t n = 0, bytes = 0;
    for (const auto& e : v) {
        n += e.second.count;
        bytes += e.second.bytes;
    }
    std::string s = std::format(" {} ({:.0f} MB):", n, double(bytes) / (1024.0 * 1024.0));
    for (size_t i = 0; i < v.size() && i < 10; ++i)
        s += std::format(" [{} x{} {:.0f} MB]", v[i].first, v[i].second.count, double(v[i].second.bytes) / (1024.0 * 1024.0));
    if (v.size() > 10) s += std::format(" and {} more kinds", v.size() - 10);
    return s;
}

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

void CountGameBuffer(uintptr_t ret, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init) {
    if (ret < g->exeBase || ret >= g->exeEnd || !desc) return;
    const std::string kind = std::format("{}/u{}/b{:x}/c{:x}/m{:x}/{}", desc->ByteWidth, static_cast<int>(desc->Usage), desc->BindFlags,
                                         desc->CPUAccessFlags, desc->MiscFlags, init ? "init" : "empty");
    std::lock_guard lk(g->siteMutex);
    if (auto it = g->gameBufferSites.find(ret); it != g->gameBufferSites.end()) ++it->second;
    else if (g->gameBufferSites.size() < 512) g->gameBufferSites.emplace(ret, 1);
    if (auto it = g->gameBufferKinds.find(kind); it != g->gameBufferKinds.end()) ++it->second;
    else if (g->gameBufferKinds.size() < 512) g->gameBufferKinds.emplace(kind, 1);
    if (desc->Usage == D3D11_USAGE_DYNAMIC && (desc->BindFlags & D3D11_BIND_VERTEX_BUFFER)) {
        void* frames[12]{};
        const USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
        std::string stack;
        int kept = 0;
        for (USHORT i = 0; i < n && kept < 7; ++i) {
            const auto a = reinterpret_cast<uintptr_t>(frames[i]);
            if (a < g->exeBase || a >= g->exeEnd) continue;
            stack += std::format("{}{:x}", kept ? "<" : "", a - g->exeBase);
            ++kept;
        }
        if (auto it = g->gameDynamicStacks.find(stack); it != g->gameDynamicStacks.end()) ++it->second;
        else if (g->gameDynamicStacks.size() < 256) g->gameDynamicStacks.emplace(stack, 1);
    }
}

template <int Slot>
HRESULT STDMETHODCALLTYPE Detour(ID3D11Device* d, void* a, void* b, void* c, void* e, void* f, void* h) {
    Count(Slot, _ReturnAddress());
    if constexpr (Slot == 3 || Slot == 5) CountLarge(Slot, _ReturnAddress(), a);
    if constexpr (Slot == 3)
        CountGameBuffer(reinterpret_cast<uintptr_t>(_ReturnAddress()), static_cast<const D3D11_BUFFER_DESC*>(a),
                        static_cast<const D3D11_SUBRESOURCE_DATA*>(b));
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

// The busiest entries of a counting map since the previous window (or in total).
template <class K, class F>
std::string Busiest(std::unordered_map<K, uint64_t>& now, std::unordered_map<K, uint64_t>& taken, bool sinceLast, size_t n, F text) {
    std::vector<std::pair<uint64_t, K>> v;
    {
        std::lock_guard lk(g->siteMutex);
        for (const auto& [key, count] : now) {
            uint64_t& last = taken[key];
            const uint64_t d = sinceLast ? count - last : count;
            if (sinceLast) last = count;
            if (d) v.emplace_back(d, key);
        }
    }
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::string s;
    for (size_t i = 0; i < v.size() && i < n; ++i) s += std::format(" {} {}", text(v[i].second), v[i].first);
    return s.empty() ? std::string(" none") : s;
}

std::string GameBuffers(bool sinceLast) {
    return std::format("; game buffer call sites{}; game buffer kinds (bytes/usage/bind/cpu/misc/data){}; game dynamic vertex buffer stacks{}",
                       Busiest(g->gameBufferSites, g->gameBufferSitesTaken, sinceLast, 4,
                               [](uintptr_t a) { return std::format("exe+0x{:x}", a - g->exeBase); }),
                       Busiest(g->gameBufferKinds, g->gameBufferKindsTaken, sinceLast, 6, [](const std::string& k) { return k; }),
                       Busiest(g->gameDynamicStacks, g->gameDynamicStacksTaken, sinceLast, 4, [](const std::string& k) { return "[" + k + "]"; }));
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
    return std::format("d3d11 objects created in the window: {}; large (4 MB or more){}; mod call sites{}{}{}", Totals(true), LargeText(g->largeTaken),
                       ModSites(true), GameBuffers(true), MemoryText());
}

std::string Command(const std::string& args) {
    if (args == "mark" || args.starts_with("mark ")) {
        // The creations since the previous mark, for one event (dlss off, stereo off, xr-restart).
        if (!g->enabled) return "err off ([log] d3d11_census = 0)";
        if (!g->installed) return "err not installed yet (two timing windows in stereo first)";
        const std::string label = args.size() > 5 ? args.substr(5) : std::string("mark");
        std::string totals;
        {
            std::lock_guard lk(g->siteMutex);
            for (int c = 0; c < kCallers; ++c) {
                std::string part;
                for (const SlotInfo& s : kSlots) {
                    const uint64_t now = g->total[s.slot][c].load(std::memory_order_relaxed);
                    const uint64_t v = now - g->markTaken[s.slot][c];
                    g->markTaken[s.slot][c] = now;
                    if (v) part += std::format(" {} {}", s.name, v);
                }
                totals += std::format("{}{}:{}", totals.empty() ? "" : "; ", kCallerName[c], part.empty() ? " none" : part);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - g->markTime).count();
        g->markTime = now;
        const std::string line = std::format("d3d11 census mark '{}': {:.1f} s since the previous mark: {}; large (4 MB or more){}{}", label, secs, totals,
                                             LargeText(g->largeMarked), MemoryText());
        log::info("{}", line);
        return "ok " + line;
    }
    if (args == "status" || args.empty()) {
        if (!g->enabled) return "ok off ([log] d3d11_census = 0)";
        if (!g->installed) return "ok on, no device yet";
        std::string others;
        {
            std::lock_guard lk(g->siteMutex);
            for (const auto& [name, n] : g->otherModules) others += std::format(" {} {}", log::narrow(name), n);
        }
        return std::format("ok totals since the start: {}; mod call sites{}; other modules{}{}{}", Totals(false), ModSites(false),
                           others.empty() ? std::string(" none") : others, GameBuffers(false), MemoryText());
    }
    return "err census status | mark [label]";
}

}  // namespace ff7vr::render::census
