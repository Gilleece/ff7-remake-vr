#include "ff7vr/engine/engine.h"

#include "addresses.h"
#include "engine_internal.h"
#include "fixes.h"
#include "rhi_command.h"
#include "stereo_device.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "render_host.h"
#endif

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/engine/cvars.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ff7vr::engine {
namespace {

struct Options {
    bool enabled = true;
    bool start_in_stereo = true;
    bool allow_unknown_build = false;
    bool light_fix = false;
    std::string host;  // render | fixed
    // [cvars] section: console variables set when the device is installed (game thread,
    // inside UEngine::Init, before the game creates its viewport and UI).
    std::vector<std::pair<std::string, std::string>> cvars;
};

Options g_opt;
Addresses g_addr;
HANDLE g_ready = nullptr;  // set when the full address resolution has finished
std::atomic<bool> g_installed{false};
std::atomic<bool> g_hmd_hook_called{false};
std::atomic<DWORD> g_game_thread{0};
// Heap allocated and never destroyed: the slots must not be restored from a static
// destructor while the process exits.
hook::VTableHook* g_hmd_hook = new hook::VTableHook();
hook::VTableHook* g_tick_hook = new hook::VTableHook();
bool g_view_states_logged = false;

FixedStereoHost::Options& fixed_options() {
    static FixedStereoHost::Options o;
    return o;
}

using InitializeHMDDeviceFn = bool(__fastcall*)(void* engine);
using TickFn = void(__fastcall*)(void* engine, float delta_seconds, bool idle);

// Reads the first local player's three view states (diagnostics only).
bool read_view_states(void* engine, std::uintptr_t out[3]) {
    __try {
        auto* gi = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(engine) + ue::offsets::UGameEngine_GameInstance);
        if (!gi) return false;
        auto* players = *reinterpret_cast<std::uint8_t***>(gi + ue::offsets::UGameInstance_LocalPlayers);
        const int num = *reinterpret_cast<int*>(gi + ue::offsets::UGameInstance_LocalPlayers + 8);
        if (!players || num < 1) return false;
        std::uint8_t* lp = players[0];
        if (!lp) return false;
        const std::size_t stereo = g_addr.off_StereoViewState ? g_addr.off_StereoViewState : ue::offsets::ULocalPlayer_StereoViewState;
        for (int i = 0; i < 3; ++i)
            out[i] = *reinterpret_cast<std::uintptr_t*>(lp + stereo - 0x28 + i * 0x28 + ue::offsets::FSceneViewStateReference_Reference);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void log_view_states(void* engine) {
    std::uintptr_t vs[3]{};
    if (!read_view_states(engine, vs)) return;
    g_view_states_logged = true;
    const bool distinct = vs[0] && vs[1] && vs[2] && vs[0] != vs[1] && vs[1] != vs[2] && vs[0] != vs[2];
    log::info("engine: local player view states ViewState={:#x} StereoViewState={:#x} MonoViewState={:#x} ({})", vs[0], vs[1],
              vs[2], distinct ? "three distinct, per-eye history available" : "NOT three distinct");
}

void install_device(void* engine) {
    auto* sp = reinterpret_cast<ue::SharedPtrRaw*>(static_cast<std::uint8_t*>(engine) + g_addr.off_StereoRenderingDevice);
    if (sp->Object) {
        const auto vt = *static_cast<std::uintptr_t*>(sp->Object);
        log::warn("engine: replacing the engine's own stereo device (vtable {}); it is left allocated",
                  module::describe(vt));
    }
    const ue::SharedPtrRaw ours = device::shared_ptr();
    sp->Controller = ours.Controller;
    sp->Object = ours.Object;
    g_installed = true;
    log::info("engine: stereo device installed at GEngine+{:#x} (stereo {} at start)", g_addr.off_StereoRenderingDevice,
              g_opt.start_in_stereo ? "on" : "off");
}

bool __fastcall initialize_hmd_device_detour(void* engine) {
    g_game_thread = GetCurrentThreadId();
    g_hmd_hook_called = true;
    const bool had = g_hmd_hook->original<InitializeHMDDeviceFn>()(engine);
    if (WaitForSingleObject(g_ready, 15000) != WAIT_OBJECT_0) {
        log::error("engine: address resolution did not finish in time; stereo disabled");
        return had;
    }
    if (!g_addr.stereo_ok) {
        log::warn("engine: stereo disabled: {}", g_addr.failure);
        return had;
    }
    if (g_addr.GEngine && *g_addr.GEngine != engine)
        log::warn("engine: InitializeHMDDevice called on {} but GEngine is {}", engine, *g_addr.GEngine);
    install_device(engine);
    if (g_opt.light_fix) fixes::set_light_patch(true);
    for (const auto& [name, value] : g_opt.cvars) cvar::set(log::widen(name), log::widen(value));
    return true;
}

void __fastcall tick_detour(void* engine, float delta_seconds, bool idle) {
    g_game_thread = GetCurrentThreadId();
    cvar::apply_pending();
    const bool installed = g_installed.load();
    if (installed) {
        device::tick_begin();
        if (!g_view_states_logged) log_view_states(engine);
    }
    g_tick_hook->original<TickFn>()(engine, delta_seconds, idle);
    if (installed) device::tick_end();
}

// Full resolution and the Tick hook, off the loader thread.
void resolve_and_prepare() {
    g_addr = resolve_addresses(g_opt.allow_unknown_build, reinterpret_cast<const void*>(&initialize_hmd_device_detour));
    rhi::verify_layout(g_addr.base);
    if (g_addr.stereo_ok) {
        device::init(g_addr.GNearClippingPlane, g_opt.start_in_stereo, fixed_options());
        if (!g_tick_hook->create(g_addr.GameEngineVtable, g_addr.slot_Tick / sizeof(void*), &tick_detour)) {
            g_addr.stereo_ok = false;
            g_addr.failure = "could not hook UGameEngine::Tick";
        }
    }
    if (g_addr.GEngine && *g_addr.GEngine && !g_hmd_hook_called) {
        g_addr.stereo_ok = false;
        g_addr.failure = "the engine was initialised before the mod could hook it";
        log::error("engine: GEngine already exists and InitializeHMDDevice has not been seen; stereo disabled");
    }
    if (g_addr.stereo_ok) log::info("engine: ready, waiting for the engine to initialise the stereo device");
    else log::warn("engine: stereo will NOT be enabled: {}", g_addr.failure);
    SetEvent(g_ready);
}

}  // namespace

const Addresses& addresses() { return g_addr; }
bool on_game_thread() { return GetCurrentThreadId() == g_game_thread.load(); }

bool start(const StartupContext& ctx) {
    try {
        const Config& cfg = *ctx.config;
        g_opt.enabled = cfg.get_bool("stereo", "enabled", false);
        g_opt.start_in_stereo = cfg.get_bool("stereo", "start_in_stereo", true);
        g_opt.allow_unknown_build = cfg.get_bool("stereo", "allow_unknown_build", false);
        g_opt.light_fix = cfg.get_bool("stereo", "light_fix", false);
        device::Settings& s = device::settings();
        s.world_scale = static_cast<float>(cfg.get_float("stereo", "world_scale", 1.0));
        s.decouple_pitch = cfg.get_bool("stereo", "decoupled_pitch", true);
        s.positional = cfg.get_bool("stereo", "positional", true);
        mirror::Mode mode{};
        if (mirror::parse_mode(cfg.get_string("stereo", "mirror", "crop"), mode)) s.mirror = static_cast<int>(mode);
        s.log_frames = static_cast<int>(cfg.get_int("stereo", "log_frames", 0));
        fixed_options() = FixedStereoHost::from_config(cfg);
        std::vector<std::pair<std::wstring, std::wstring>> stereo_cvars;
        for (const std::string& line : cfg.dump()) {
            // "cvars.<name> = <value>" (set once when the device is installed) and
            // "stereo_cvars.<name> = <value>" (held only while stereo renders)
            constexpr std::string_view prefix = "cvars.";
            constexpr std::string_view stereo_prefix = "stereo_cvars.";
            const std::size_t eq = line.find(" = ");
            if (eq == std::string::npos) continue;
            if (line.rfind(prefix, 0) == 0)
                g_opt.cvars.emplace_back(line.substr(prefix.size(), eq - prefix.size()), line.substr(eq + 3));
            else if (line.rfind(stereo_prefix, 0) == 0)
                stereo_cvars.emplace_back(log::widen(line.substr(stereo_prefix.size(), eq - stereo_prefix.size())),
                                          log::widen(line.substr(eq + 3)));
        }
        cvar::set_stereo_overrides(std::move(stereo_cvars));
#if FF7VR_ENGINE_WITH_RENDER
        g_opt.host = cfg.get_string("stereo", "host", "render");
#else
        g_opt.host = "fixed";
#endif

        dev_commands::add("stereo", "stereo status|views|on|off|mirror|eye|fov|ipd|motion|scale|pitch|positional|lightfix|log ...",
                          [](std::string_view args) {
                              std::string reply;
                              handle_command("stereo " + std::string(args), reply);
                              return reply;
                          });
        dev_commands::add("cvar", "cvar get <name> | cvar set <name> <value>: console variables", [](std::string_view args) {
            std::string reply;
            handle_command("cvar " + std::string(args), reply);
            return reply;
        });

        if (!ctx.is_game) {
            log::info("engine: host is not the game, nothing to do");
            return false;
        }
        if (!g_opt.enabled) {
            log::info("engine: stereo disabled in ff7vr.ini ([stereo] enabled = 0); the game runs unmodified");
            return false;
        }
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HookPoint hp;
        if (!find_hmd_hook_point(g_opt.allow_unknown_build, hp)) {
            log::warn("engine: stereo disabled: unknown game build or InitializeHMDDevice not found; the game runs unmodified");
            return false;
        }
        if (!g_hmd_hook->create(hp.vtable, hp.slot_index, &initialize_hmd_device_detour)) {
            log::warn("engine: stereo disabled: could not hook InitializeHMDDevice");
            return false;
        }
#if FF7VR_ENGINE_WITH_RENDER
        if (g_opt.host == "render") device::set_host(render_stereo_host());
#endif
        log::info("engine: eye size, views and eye texture via {}", g_opt.host == "render" ? "the render module's XR session"
                                                                                            : "built-in fixed values ([stereo] host = fixed)");
        std::thread(resolve_and_prepare).detach();
        return true;
    } catch (const std::exception& e) {
        log::error("engine: start failed: {}", e.what());
        return false;
    }
}

void set_stereo_host(StereoHost* host) { device::set_host(host); }
bool stereo_installed() { return g_installed.load(); }
bool stereo_active() { return g_installed.load() && device::active(); }
void request_stereo(bool on) { device::request_active(on); }

}  // namespace ff7vr::engine
