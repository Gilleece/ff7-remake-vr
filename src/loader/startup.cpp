// ============================================================================
//  PLUG-IN POINT for engine hooks, XR initialisation and dev tooling.
// ============================================================================
//
// start_modules() runs once, on the loader's bootstrap thread, after:
//   - ff7vr.log is open, ff7vr.ini is loaded, the crash handler is installed,
//   - MinHook is initialised (ff7vr::hook::init()),
//   - the real xinput1_3.dll is resolved.
//
// It runs very early: the game's static imports are loaded but WinMain may not
// have run yet, so GEngine, the RHI and the D3D11 device usually do not exist.
// Modules should install hooks here (signature scan + hook) and do their real
// initialisation from inside those hooks once the engine reaches the right
// state. Never block this thread for long on the game; spawn your own thread
// if you must wait.
//
// How to add a module (e.g. src/engine, target ff7vr_engine):
//   1. Expose one function, e.g. `bool ff7vr::engine::start(const ff7vr::StartupContext&)`
//      (ff7vr/core/startup_context.h) in a header under your module's include dir.
//   2. The loader's CMakeLists.txt links ff7vr_engine automatically if the target
//      exists and defines FF7VR_HAVE_ENGINE=1 (same for XR, RENDER, DEV).
//   3. Call it below inside the matching #if block. Keep the order:
//      engine -> render -> xr -> dev.
//   4. Gate optional features with an ini switch, e.g. [engine] enabled=1.

#include "startup.h"

#include "dev_input.h"

#include "ff7vr/core/log.h"

#if FF7VR_HAVE_DEV
#include "ff7vr/dev/dev.h"
#endif
#if FF7VR_HAVE_ENGINE
#include "ff7vr/engine/engine.h"
#include "ff7vr/engine/ui_layer.h"
#endif
#if FF7VR_HAVE_RENDER
#include "ff7vr/render/render.h"
#endif

namespace ff7vr::loader {

void start_modules(const StartupContext& ctx) {
    const Config& cfg = *ctx.config;

    // Dev command pipe and virtual pad (send-input.ps1). Off unless the ini enables it.
    if (cfg.get_bool("dev", "pipe", false)) dev_input::start();

#if FF7VR_HAVE_ENGINE
    // Stereo device in the engine ([stereo] keys, gated by [stereo] enabled inside the module).
    ff7vr::engine::start(ctx);
    ff7vr::engine::start_ui_layer(ctx);  // in-game UI on its own layer in stereo ([ui] keys)
#endif
#if FF7VR_HAVE_RENDER
    // D3D11 hooks and the XR session (src/xr is driven by the render module; [xr] keys configure it).
    if (cfg.get_bool("render", "enabled", true)) ff7vr::render::start(ctx);
#endif
#if FF7VR_HAVE_DEV
    // Measurement tools (frame timer for tools/bench); each is off unless the ini enables it.
    ff7vr::dev::start(ctx);
#endif

    log::info("startup: modules started{}", ctx.is_game ? "" : " (host is not ff7remake_.exe)");
}

void stop_modules() {
    // Nothing yet. Do not log or free here beyond trivial work (loader lock).
}

}  // namespace ff7vr::loader
