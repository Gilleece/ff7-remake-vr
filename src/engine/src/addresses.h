#pragma once
// Everything the engine module needs from ff7remake_.exe, found by signature at start-up.
//
// The signatures are a subset of tools/re/signatures.json (same names, same patterns and
// resolve rules); docs/re/engine.md explains each one. For the known build (file version
// 1.0.0.7) every resolved value is also compared with the RVA recorded for that build, so
// a signature that silently matched something else is caught.

#include <cstddef>
#include <cstdint>
#include <string>

namespace ff7vr::engine {

struct Addresses {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    bool known_build = false;  // SizeOfImage and PE timestamp of file version 1.0.0.7

    // ---- required for the stereo device
    void** GEngine = nullptr;                      // UEngine** (address of the global)
    std::uintptr_t InitializeHMDDevice = 0;        // bool UEngine::InitializeHMDDevice(UEngine*)
    std::uintptr_t GameEngineTick = 0;             // void UGameEngine::Tick(UGameEngine*, float, bool)
    void** GameEngineVtable = nullptr;             // UGameEngine::vftable
    std::size_t slot_InitializeHMDDevice = 0;      // byte offset in the vtable (0x378)
    std::size_t slot_Tick = 0;                     // byte offset in the vtable (78 * 8)
    std::size_t off_StereoRenderingDevice = 0;     // UEngine member (0xD50)
    float* GNearClippingPlane = nullptr;
    std::int32_t* GSystemResolution = nullptr;     // {ResX, ResY, ...}; sizes the scene buffers in this build

    // ---- diagnostics (not required)
    std::size_t off_GameViewport = 0;              // UEngine member (0x980)
    std::size_t off_ViewportClientViewport = 0;    // UGameViewportClient member (0xA0)
    std::size_t off_bForceSeparateRenderTarget = 0;  // relative to FViewport (0x27C)
    std::size_t off_StereoViewState = 0;           // ULocalPlayer member (0xB8)

    // ---- optional features
    void** ConsoleManager = nullptr;               // IConsoleManager** (null until set up)
    std::uintptr_t ConsoleManagerVtable = 0;       // FConsoleManager::vftable, checked before use
    std::uint8_t* LightSortKeyImm = nullptr;       // immediate byte of `mov esi, 0x40` in RenderLights
    std::uint8_t* ViewRectOverrideJump = nullptr;  // `jne` opcode in CalcSceneView that skips the windowed-fullscreen rect
    std::uint8_t* SceneTargetFormat = nullptr;     // EPixelFormat the engine uses for the separate target

    bool stereo_ok = false;      // every required entry resolved and every layout check passed
    std::string failure;         // first reason stereo_ok is false
};

// Resolves everything; logs one line per entry at debug level and a summary at info.
// allow_unknown_build: accept a build other than 1.0.0.7 when every signature and layout
// check still passes. Takes about a second (each signature is checked for uniqueness
// over the whole code section). hmd_detour: what the InitializeHMDDevice slot may hold
// instead of the engine's function once the start-up hook is in place.
Addresses resolve_addresses(bool allow_unknown_build, const void* hmd_detour);

// The vtable slot through which UEngine::Init calls InitializeHMDDevice, resolved with
// the three signatures it needs only (fast), so the hook can be in place long before the
// engine initialises. Returns false (and logs why) if anything does not check out.
struct HookPoint {
    void** vtable = nullptr;      // UGameEngine::vftable
    std::size_t slot_index = 0;   // InitializeHMDDevice
    std::uintptr_t function = 0;  // UEngine::InitializeHMDDevice
};
bool find_hmd_hook_point(bool allow_unknown_build, HookPoint& out);

}  // namespace ff7vr::engine
