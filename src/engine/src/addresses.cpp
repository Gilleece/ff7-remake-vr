#include "addresses.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/core/pattern.h"
#include "ff7vr/engine/stereo_abi.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <optional>

namespace ff7vr::engine {
namespace {

enum class Rule { Rip, Match, I32, U8 };

struct Sig {
    const char* name;      // name in tools/re/signatures.json
    const char* pattern;
    Rule rule;
    int a, b, c;           // Rip: insn, disp, len; Match/I32/U8: offset in a
    std::int64_t expect;   // value for file version 1.0.0.7 (RVA or offset)
};

// Copied from tools/re/signatures.json. Keep the two in sync.
constexpr Sig kGEngine{"GEngine",
                       "48 83 EC 28 48 8B 42 20 33 C9 48 85 C0 0F 95 C1 48 03 C8 48 89 4A 20 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??",
                       Rule::Rip, 28, 3, 7, 0x5831188};
constexpr Sig kInitializeHMDDevice{
    "UEngine::InitializeHMDDevice",
    "48 8B C4 48 89 48 08 55 41 56 41 57 48 8D 68 88 48 81 EC 60 01 00 00 80 3D ?? ?? ?? ?? 00 4C 8B F1 0F 85 ?? ?? ?? ??",
    Rule::Match, 0, 0, 0, 0x3317ca0};
constexpr Sig kGameEngineVtable{"UGameEngine::vftable",
                                "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 28 33 C0 48 89 83 78 10 00 00",
                                Rule::Rip, 0, 3, 7, 0x4e56fa8};
constexpr Sig kGameEngineTick{"UGameEngine::Tick", "48 8B C4 44 88 40 18 48 89 48 08 55 41 55", Rule::Match, 0, 0, 0,
                              0x2f32420};
constexpr Sig kSlotInitializeHMDDevice{"UEngine slot InitializeHMDDevice", "FF 90 ?? ?? ?? ?? 44 38 25 ?? ?? ?? ?? 74 08",
                                       Rule::I32, 2, 0, 0, 0x378};
constexpr Sig kStereoRenderingDevice{"UEngine::StereoRenderingDevice", "49 83 BE ?? ?? ?? ?? 00 0F 95 C0 48 81 C4 60 01 00 00",
                                     Rule::I32, 3, 0, 0, 0xd50};
constexpr Sig kGNearClippingPlane{"GNearClippingPlane", "F3 0F 10 05 ?? ?? ?? ?? EB 05 F3 0F 10 46 0C", Rule::Rip, 0, 4, 8,
                                  0x53a49e4};

constexpr Sig kGameViewport{"UEngine::GameViewport", "48 8B 8F 70 10 00 00 48 89 B7 ?? ?? ?? ?? 48 8B 51 30 48 89 B2 38 02 00 00",
                            Rule::I32, 10, 0, 0, 0x980};
constexpr Sig kViewportClientViewport{"UGameViewportClient::Viewport",
                                      "4D 8B 80 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 78", Rule::I32, 3, 0,
                                      0, 0xa0};
constexpr Sig kForceSeparate{"FSceneViewport::bForceSeparateRenderTarget", "3A 9F ?? ?? ?? ?? 75 15", Rule::I32, 2, 0, 0, 0x27c};
constexpr Sig kStereoViewState{"ULocalPlayer::StereoViewState",
                               "48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 8B E0 00 00 00 48 83 C4 30", Rule::I32, 3, 0, 0, 0xb8};

constexpr Sig kConsoleManager{
    "IConsoleManager::Singleton",
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 4C 8D 0D ?? ?? ?? ?? 0F 57 D2 89 7C 24 20",
    Rule::Rip, 0, 3, 7, 0x57f9ca0};
constexpr Sig kConsoleManagerVtable{"FConsoleManager::vftable", "48 8D 05 ?? ?? ?? ?? 48 89 5C 24 40 48 89 07", Rule::Rip, 0, 3, 7,
                                    0x4d1de20};
constexpr Sig kLightPatch{"RenderLights light sort-key patch site", "BE 40 00 00 00 F3 0F 10 81 C0 00 00 00", Rule::Match, 0, 0,
                          0, 0x22351b0};
constexpr Sig kGSystemResolution{"GSystemResolution", "81 3D ?? ?? ?? ?? 80 07 00 00", Rule::Rip, 0, 2, 10, 0x53e3ae0};
// The movzx after the AllocateRenderTargetTexture call in FSceneViewport::InitDynamicRHI
// loads the pixel format the engine falls back to for the separate render target.
constexpr Sig kSceneTargetFormat{"FSceneViewport separate target format",
                                 "FF 50 ?? 84 C0 75 47 44 0F B6 05 ?? ?? ?? ??", Rule::Rip, 7, 4, 8, 0x53e1c78};

// Call sites whose vtable displacement must equal the slot our device implements.
struct SlotCheck {
    Sig sig;
    std::int64_t ours;  // byte offset in our function table
};
constexpr SlotCheck kSlotChecks[] = {
    {{"IStereoRendering slot IsStereoEnabledOnNextFrame", "FF 50 ?? 84 C0 74 49 48 8B 05 ?? ?? ?? ??", Rule::U8, 2, 0, 0, 0x8},
     offsetof(ue::StereoDeviceVtbl, IsStereoEnabledOnNextFrame)},
    {{"IStereoRendering slot AdjustViewRect", "FF 50 ?? 0F B6 4C 24 30 48 8B 56 30", Rule::U8, 2, 0, 0, 0x18},
     offsetof(ue::StereoDeviceVtbl, AdjustViewRect)},
    {{"IStereoRendering slot CalculateStereoViewOffset", "FF 50 ?? 4D 85 F6 74 2E", Rule::U8, 2, 0, 0, 0x28},
     offsetof(ue::StereoDeviceVtbl, CalculateStereoViewOffset)},
    {{"IStereoRendering slot GetStereoProjectionMatrix", "FF 50 ?? 8B 4C 24 40 44 8B 44 24 38", Rule::U8, 2, 0, 0, 0x30},
     offsetof(ue::StereoDeviceVtbl, GetStereoProjectionMatrix)},
    {{"IStereoRendering slot InitCanvasFromView", "FF 50 ?? EB 3C 0F 28 87 20 02 00 00", Rule::U8, 2, 0, 0, 0x38},
     offsetof(ue::StereoDeviceVtbl, InitCanvasFromView)},
    {{"IStereoRendering slot RenderTexture_RenderThread", "FF 50 ?? 48 8B 4C 24 60 48 85 C9 74 34", Rule::U8, 2, 0, 0, 0x48},
     offsetof(ue::StereoDeviceVtbl, RenderTexture_RenderThread)},
    {{"IStereoRendering slot GetRenderTargetManager", "FF 50 ?? 48 89 45 90 4C 8B F0", Rule::U8, 2, 0, 0, 0x60},
     offsetof(ue::StereoDeviceVtbl, GetRenderTargetManager)},
    {{"IStereoRenderTargetManager slot UpdateViewport",
      "FF 50 ?? 48 85 DB 74 22 83 6B 08 01 75 1C 48 8B 03 48 8B CB FF 10 83 6B 0C 01 75 0E 48 8B 03 BA 01 00 00 00 48 8B CB FF 50 08 "
      "48 8B 8D A8 04 00 00",
      Rule::U8, 2, 0, 0, 0x8},
     offsetof(ue::RenderTargetManagerVtbl, UpdateViewport)},
    {{"IStereoRenderTargetManager slot CalculateRenderTargetSize", "41 FF 52 ?? 49 8B 16", Rule::U8, 3, 0, 0, 0x10},
     offsetof(ue::RenderTargetManagerVtbl, CalculateRenderTargetSize)},
    {{"IStereoRenderTargetManager slot NeedReAllocateViewportRenderTarget", "FF 50 ?? 84 C0 74 31 8B 87 C4 00 00 00", Rule::U8, 2, 0,
      0, 0x18},
     offsetof(ue::RenderTargetManagerVtbl, NeedReAllocateViewportRenderTarget)},
    {{"IStereoRenderTargetManager slot NeedReAllocateDepthTexture", "FF 50 ?? 84 C0 74 13 48 8B 4F 68", Rule::U8, 2, 0, 0, 0x20},
     offsetof(ue::RenderTargetManagerVtbl, NeedReAllocateDepthTexture)},
    {{"IStereoRenderTargetManager slot GetNumberOfBufferedFrames", "FF 52 ?? 89 87 10 03 00 00", Rule::U8, 2, 0, 0, 0x28},
     offsetof(ue::RenderTargetManagerVtbl, GetNumberOfBufferedFrames)},
    {{"IStereoRenderTargetManager slot AllocateRenderTargetTexture", "FF 50 ?? 84 C0 75 47 44 0F B6 05 ?? ?? ?? ??", Rule::U8, 2, 0,
      0, 0x30},
     offsetof(ue::RenderTargetManagerVtbl, AllocateRenderTargetTexture)},
};

constexpr std::size_t kTickSlotIndex = 78;  // UGameEngine vtable slot of Tick (engine.md, UEVR)

class Resolver {
public:
    Resolver(std::uintptr_t base, std::size_t size, bool known) : base_(base), size_(size), known_(known) {}

    // Returns the resolved value: an absolute address for Rip/Match, a number for I32/U8.
    std::optional<std::int64_t> resolve(const Sig& s) {
        auto r = pattern::scan_module(base_, s.pattern, pattern::Sections::Executable, 2);
        if (!r.found()) {
            log::warn("engine: signature '{}' not found", s.name);
            return std::nullopt;
        }
        if (!r.unique()) {
            log::warn("engine: signature '{}' is not unique", s.name);
            return std::nullopt;
        }
        const std::uintptr_t m = r.first();
        std::int64_t value = 0;
        std::int64_t as_rva = 0;
        bool is_address = false;
        switch (s.rule) {
            case Rule::Rip:
                value = static_cast<std::int64_t>(pattern::rip(m + s.a, s.b, s.c));
                is_address = true;
                break;
            case Rule::Match:
                value = static_cast<std::int64_t>(m + s.a);
                is_address = true;
                break;
            case Rule::I32: {
                std::int32_t v;
                std::memcpy(&v, reinterpret_cast<const void*>(m + s.a), 4);
                value = v;
                break;
            }
            case Rule::U8:
                value = *reinterpret_cast<const std::uint8_t*>(m + s.a);
                break;
        }
        if (is_address) {
            as_rva = value - static_cast<std::int64_t>(base_);
            if (as_rva < 0 || as_rva >= static_cast<std::int64_t>(size_)) {
                log::warn("engine: signature '{}' resolves outside the module ({:#x})", s.name, value);
                return std::nullopt;
            }
        } else {
            as_rva = value;
        }
        if (known_ && as_rva != s.expect) {
            log::warn("engine: signature '{}' resolved to {:#x}, expected {:#x} for this build", s.name, as_rva, s.expect);
            return std::nullopt;
        }
        log::debug("engine: {:<60} {:#x}", s.name, as_rva);
        return value;
    }

private:
    std::uintptr_t base_;
    std::size_t size_;
    bool known_;
};

bool readable_pointer_slot(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

}  // namespace

bool find_hmd_hook_point(bool allow_unknown_build, HookPoint& out) {
    const module::Info exe = module::main_module();
    const bool known = exe.size == ue::rva_1_0_0_7::SizeOfImage &&
                       module::timestamp(exe.base) == ue::rva_1_0_0_7::TimeDateStamp;
    if (!known && !allow_unknown_build) return false;
    Resolver r(exe.base, exe.size, known);
    auto vt = r.resolve(kGameEngineVtable);
    auto fn = r.resolve(kInitializeHMDDevice);
    auto slot = r.resolve(kSlotInitializeHMDDevice);
    if (!vt || !fn || !slot || *slot <= 0 || (*slot % 8) != 0 || *slot > 0x1000) return false;
    void** table = reinterpret_cast<void**>(*vt);
    void** p = table + *slot / 8;
    if (!readable_pointer_slot(p) || reinterpret_cast<std::uintptr_t>(*p) != static_cast<std::uintptr_t>(*fn)) {
        log::warn("engine: UGameEngine vtable slot {:#x} does not hold UEngine::InitializeHMDDevice", *slot);
        return false;
    }
    out.vtable = table;
    out.slot_index = static_cast<std::size_t>(*slot / 8);
    out.function = static_cast<std::uintptr_t>(*fn);
    return true;
}

Addresses resolve_addresses(bool allow_unknown_build, const void* hmd_detour) {
    const auto t0 = std::chrono::steady_clock::now();
    Addresses a;
    const module::Info exe = module::main_module();
    a.base = exe.base;
    a.size = exe.size;
    const std::uint32_t stamp = module::timestamp(exe.base);
    a.known_build = exe.size == ue::rva_1_0_0_7::SizeOfImage && stamp == ue::rva_1_0_0_7::TimeDateStamp;

    auto fail = [&](std::string why) {
        if (a.failure.empty()) a.failure = std::move(why);
    };

    if (!a.known_build) {
        log::warn("engine: unknown game build (SizeOfImage {:#x}, PE timestamp {:#x}; expected {:#x} / {:#x} for 1.0.0.7)",
                  exe.size, stamp, ue::rva_1_0_0_7::SizeOfImage, ue::rva_1_0_0_7::TimeDateStamp);
        if (!allow_unknown_build) fail("unknown game build (set [stereo] allow_unknown_build = 1 to try anyway)");
    }

    Resolver r(exe.base, exe.size, a.known_build);
    auto addr = [&](const Sig& s, bool required) -> std::uintptr_t {
        auto v = r.resolve(s);
        if (!v) {
            if (required) fail(std::string("signature '") + s.name + "' failed");
            return 0;
        }
        return static_cast<std::uintptr_t>(*v);
    };
    auto num = [&](const Sig& s, bool required) -> std::int64_t {
        auto v = r.resolve(s);
        if (!v) {
            if (required) fail(std::string("signature '") + s.name + "' failed");
            return -1;
        }
        return *v;
    };

    // Required.
    a.GEngine = reinterpret_cast<void**>(addr(kGEngine, true));
    a.InitializeHMDDevice = addr(kInitializeHMDDevice, true);
    a.GameEngineTick = addr(kGameEngineTick, true);
    a.GameEngineVtable = reinterpret_cast<void**>(addr(kGameEngineVtable, true));
    const std::int64_t slot_hmd = num(kSlotInitializeHMDDevice, true);
    const std::int64_t off_dev = num(kStereoRenderingDevice, true);
    a.GNearClippingPlane = reinterpret_cast<float*>(addr(kGNearClippingPlane, true));
    if (slot_hmd > 0) a.slot_InitializeHMDDevice = static_cast<std::size_t>(slot_hmd);
    if (off_dev > 0) a.off_StereoRenderingDevice = static_cast<std::size_t>(off_dev);
    a.slot_Tick = kTickSlotIndex * sizeof(void*);

    // The vtable slots we replace must hold the functions found by signature.
    if (a.GameEngineVtable && a.InitializeHMDDevice && a.slot_InitializeHMDDevice) {
        void** slot = a.GameEngineVtable + a.slot_InitializeHMDDevice / sizeof(void*);
        const bool ok = readable_pointer_slot(slot) &&
                        (reinterpret_cast<std::uintptr_t>(*slot) == a.InitializeHMDDevice || (hmd_detour && *slot == hmd_detour));
        if (!ok)
            fail("UGameEngine vtable slot InitializeHMDDevice does not hold UEngine::InitializeHMDDevice");
    }
    if (a.GameEngineVtable && a.GameEngineTick) {
        void** slot = a.GameEngineVtable + kTickSlotIndex;
        if (!readable_pointer_slot(slot) || reinterpret_cast<std::uintptr_t>(*slot) != a.GameEngineTick)
            fail("UGameEngine vtable slot 78 does not hold UGameEngine::Tick");
    }

    // Our device's function table layout must match the slots the engine calls.
    int slot_failures = 0;
    for (const SlotCheck& c : kSlotChecks) {
        const std::int64_t v = num(c.sig, true);
        if (v >= 0 && v != c.ours) {
            log::warn("engine: {}: engine calls offset {:#x}, our table has it at {:#x}", c.sig.name, v, c.ours);
            fail(std::string("layout check '") + c.sig.name + "' failed");
            ++slot_failures;
        }
    }

    // Diagnostics.
    std::int64_t v = num(kGameViewport, false);
    if (v > 0) a.off_GameViewport = static_cast<std::size_t>(v);
    v = num(kViewportClientViewport, false);
    if (v > 0) a.off_ViewportClientViewport = static_cast<std::size_t>(v);
    v = num(kForceSeparate, false);
    if (v > 0) a.off_bForceSeparateRenderTarget = static_cast<std::size_t>(v);
    v = num(kStereoViewState, false);
    if (v > 0) a.off_StereoViewState = static_cast<std::size_t>(v);

    // Optional features.
    a.ConsoleManager = reinterpret_cast<void**>(addr(kConsoleManager, false));
    a.ConsoleManagerVtable = addr(kConsoleManagerVtable, false);
    if (std::uintptr_t p = addr(kLightPatch, false)) a.LightSortKeyImm = reinterpret_cast<std::uint8_t*>(p + 1);
    a.GSystemResolution = reinterpret_cast<std::int32_t*>(addr(kGSystemResolution, false));
    a.SceneTargetFormat = reinterpret_cast<std::uint8_t*>(addr(kSceneTargetFormat, false));

    a.stereo_ok = a.failure.empty();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log::info("engine: build {} ({}), signatures resolved in {} ms, layout checks {}", a.known_build ? "1.0.0.7" : "unknown",
              a.known_build ? "known" : "not known", ms, slot_failures == 0 ? "passed" : "FAILED");
    return a;
}

}  // namespace ff7vr::engine
