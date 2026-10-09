#include "ff7vr/engine/engine.h"

#include "addresses.h"
#include "engine_internal.h"
#include "fixes.h"
#include "gpu_trace.h"
#include "audio_listener.h"
#include "graphics.h"
#include "rhi_command.h"
#include "bloom_fix.h"
#include "distortion_fix.h"
#include "lens_vignette.h"
#include "controls.h"
#include "snap_turn.h"
#include "movie_watch.h"
#include "player.h"
#include "stereo_device.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "render_host.h"
#endif
#if FF7VR_ENGINE_WITH_DLSS
#include "dlss.h"
#endif

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/engine/cvars.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
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
    bool light_fix = true;
    bool movie_screen = true;
    bool bloom_fix = true;
    bool ao_fix = true;
    bool distortion_fix = true;
    float game_vignette = 0.0f;  // share of the game's lens vignette kept in stereo
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
    // Applied while stereo renders (stereo_device.cpp, transitions).
    fixes::set_light_fix(g_opt.light_fix);
    log::info("light sort-key fix: {}", g_opt.light_fix ? "on while stereo renders" : "off ([stereo] light_fix = 0)");
    for (const auto& [name, value] : g_opt.cvars) cvar::set(log::widen(name), log::widen(value));
    return true;
}

void __fastcall tick_detour(void* engine, float delta_seconds, bool idle) {
    g_game_thread = GetCurrentThreadId();
    cvar::apply_pending();
    const bool installed = g_installed.load();
    if (installed) {
        movie::tick();
        device::tick_begin();
        player::tick(device::active(), delta_seconds);
        controls::tick();
        snap_turn::tick();
        if (!g_view_states_logged) log_view_states(engine);
    }
    g_tick_hook->original<TickFn>()(engine, delta_seconds, idle);
    if (installed) device::tick_end();
}

// Camera effects meant for a flat screen, switched off while the engine renders in stereo
// and put back afterwards ([stereo] comfort_cvars = 0 keeps them; a [stereo_cvars] entry of
// the same name wins). See docs/engine-module.md, "Flat-screen camera effects".
const std::pair<const wchar_t*, const wchar_t*> kComfortCvars[] = {
    {L"r.MotionBlurQuality", L"0"},       // camera motion blur smears the image while the view turns
    {L"r.SceneColorFringeQuality", L"0"}, // chromatic aberration: the headset's lenses have their own
};

// Default battle signal for the automatic third person in combat (docs/engine-module.md,
// "Combat"): the ID of the current battle scene (an FName; its index is 0, None, outside a
// battle). "" = none.
constexpr const char* kBattleSignal = "EndBattleAPI.GetBattleSceneID result=0:4";

void read_camera_settings(const Config& cfg) {
    player::Settings& p = player::settings();
    const std::string boom = cfg.get_string("camera", "boom", "level");
    p.level_boom = boom != "game";
    p.pivot_height = static_cast<float>(cfg.get_float("camera", "pivot_height", p.pivot_height.load()));
    p.follow_distance = static_cast<float>(cfg.get_float("camera", "follow_distance", p.follow_distance.load()));
    p.aim_tolerance = static_cast<float>(cfg.get_float("camera", "aim_tolerance", p.aim_tolerance.load()));
    p.camera_blend_seconds = std::max(0.0f, static_cast<float>(cfg.get_float("camera", "blend_seconds", p.camera_blend_seconds.load())));
    p.combat_level = cfg.get_string("camera", "combat", "level") != "game";
    p.miss_seconds = std::clamp(static_cast<float>(cfg.get_float("camera", "miss_seconds", p.miss_seconds.load())), 0.0f, 30.0f);
    p.collision = cfg.get_bool("camera", "collision", p.collision.load());
    p.collision_margin = std::max(0.0f, static_cast<float>(cfg.get_float("camera", "collision_margin", p.collision_margin.load())));
    p.fp_available = cfg.get_bool("first_person", "enabled", p.fp_available.load());
    p.fp_default = cfg.get_bool("first_person", "default", p.fp_default.load());
    p.auto_combat = cfg.get_bool("first_person", "auto_combat", p.auto_combat.load());
    p.blend_seconds = static_cast<float>(cfg.get_float("first_person", "blend_seconds", p.blend_seconds.load()));
    p.toggle_key = static_cast<int>(cfg.get_int("first_person", "toggle_key", p.toggle_key.load()));
    const std::string hide = cfg.get_string("first_person", "hide", "pass");
    p.hide = hide == "none" ? 0 : hide == "head" ? 2 : hide == "bones" ? 3 : hide == "pass" ? 4 : 1;
    p.head_bob = cfg.get_bool("first_person", "head_bob", p.head_bob.load());
    p.steady_seconds = std::clamp(static_cast<float>(cfg.get_float("first_person", "steady_seconds", p.steady_seconds.load())), 0.01f, 5.0f);
    p.pad_toggle = cfg.get_bool("first_person", "pad_toggle", p.pad_toggle.load());
    p.eye_head = cfg.get_string("first_person", "eye", "head") != "offset";
    player::set_battle_signal(cfg.get_string("first_person", "battle_signal", kBattleSignal));
    const std::string hoff = cfg.get_string("first_person", "head_offset", "");
    float hf = 0, hr = 0, hu = 0;
    if (!hoff.empty() && sscanf_s(hoff.c_str(), "%f %f %f", &hf, &hr, &hu) == 3) {
        p.head_forward = hf;
        p.head_right = hr;
        p.head_up = hu;
    }
    const std::string off = cfg.get_string("first_person", "eye_offset", "");
    float f = 0, r = 0, u = 0;
    if (!off.empty() && sscanf_s(off.c_str(), "%f %f %f", &f, &r, &u) == 3) {
        p.eye_forward = f;
        p.eye_right = r;
        p.eye_up = u;
    }
}

// Full resolution and the Tick hook, off the loader thread.
void resolve_and_prepare() {
    g_addr = resolve_addresses(g_opt.allow_unknown_build, reinterpret_cast<const void*>(&initialize_hmd_device_detour));
    rhi::verify_layout(g_addr.base);
    if (g_addr.stereo_ok) {
        device::init(g_addr.GNearClippingPlane, g_opt.start_in_stereo, fixed_options());
        movie::init({g_addr.GUObjectArray, g_addr.FNamePool}, g_opt.movie_screen);
        player::init(g_addr.GUObjectArray, g_addr.FNamePool, g_addr.GEngine);
        bloom_fix::init(g_addr.BloomReduceProcess, g_opt.bloom_fix, g_opt.ao_fix);
        distortion_fix::init(g_addr.DistortionComposite, g_opt.distortion_fix);
        lens_vignette::init(g_addr.PostProcessSettingsCtor, g_opt.game_vignette);
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
        g_opt.enabled = cfg.get_bool("stereo", "enabled", true);
        g_opt.start_in_stereo = cfg.get_bool("stereo", "start_in_stereo", true);
        g_opt.allow_unknown_build = cfg.get_bool("stereo", "allow_unknown_build", false);
        g_opt.light_fix = cfg.get_bool("stereo", "light_fix", true);
        g_opt.movie_screen = cfg.get_bool("stereo", "movie_screen", true);
        movie::set_cutscene(cfg.get_bool("stereo", "cutscene_screen", false));
        movie::set_cutscene_times(static_cast<int>(cfg.get_int("stereo", "cutscene_screen_delay_ms", 500)),
                                  static_cast<int>(cfg.get_int("stereo", "cutscene_screen_hold_ms", 300)));
        audio_listener::set_enabled(cfg.get_bool("first_person", "audio_listener", true));
        g_opt.bloom_fix = cfg.get_bool("stereo", "bloom_fix", true);
        g_opt.ao_fix = cfg.get_bool("stereo", "ao_fix", true);
        g_opt.distortion_fix = cfg.get_bool("stereo", "distortion_fix", true);
        g_opt.game_vignette = static_cast<float>(cfg.get_float("stereo", "game_vignette", 0.0));
        fixes::set_vr_window_size(cfg.get_string("stereo", "vr_window", "1280x720"));
        device::Settings& s = device::settings();
        s.world_scale = static_cast<float>(cfg.get_float("stereo", "world_scale", 1.0));
        s.decouple_pitch = cfg.get_bool("stereo", "decoupled_pitch", true);
        s.positional = cfg.get_bool("stereo", "positional", true);
        mirror::Mode mode{};
        if (mirror::parse_mode(cfg.get_string("stereo", "mirror", "crop"), mode)) s.mirror = static_cast<int>(mode);
        s.log_frames = static_cast<int>(cfg.get_int("stereo", "log_frames", 0));
        fixed_options() = FixedStereoHost::from_config(cfg);
        read_camera_settings(cfg);
        controls::read_config(cfg);
        snap_turn::read_config(cfg);
        device::configure_render_scale(cfg);
        std::vector<std::pair<std::wstring, std::wstring>> stereo_cvars;
        if (cfg.get_bool("stereo", "comfort_cvars", true))
            for (const auto& [name, value] : kComfortCvars) stereo_cvars.emplace_back(name, value);
        for (const std::string& line : cfg.dump()) {
            // "cvars.<name> = <value>" (set once when the device is installed) and
            // "stereo_cvars.<name> = <value>" (held only while stereo renders)
            constexpr std::string_view prefix = "cvars.";
            constexpr std::string_view stereo_prefix = "stereo_cvars.";
            const std::size_t eq = line.find(" = ");
            if (eq == std::string::npos) continue;
            if (line.rfind(prefix, 0) == 0)
                g_opt.cvars.emplace_back(line.substr(prefix.size(), eq - prefix.size()), line.substr(eq + 3));
            else if (line.rfind(stereo_prefix, 0) == 0) {
                std::wstring name = log::widen(line.substr(stereo_prefix.size(), eq - stereo_prefix.size()));
                std::erase_if(stereo_cvars, [&](const auto& kv) { return _wcsicmp(kv.first.c_str(), name.c_str()) == 0; });
                stereo_cvars.emplace_back(std::move(name), log::widen(line.substr(eq + 3)));
            }
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
        dev_commands::add("fp", "fp status|toggle|first|third|combat <0|1|auto>|offset|eye|hide|boom|pivot|bones|funcs|call (fp help): camera modes",
                          [](std::string_view args) {
                              std::string reply = player::command(std::string(args));
                              if (args.empty() || args.starts_with("status")) reply += " | " + audio_listener::status();
                              if (args.starts_with("audio")) reply = audio_listener::command(std::string(args.substr(5)));
                              return reply;
                          });
        dev_commands::add("controls", "controls status | pad <hex buttons> | recenter | stereo | nearer | farther: the player's keys and gamepad combinations",
                          [](std::string_view args) { return controls::command(std::string(args)); });
        dev_commands::add("snapturn", "snapturn status | snap <deg>|off | deadzone | repeat | log | stick <lx> <ly> <rx> <ry>: snap turn ([comfort] snap_turn)",
                          [](std::string_view args) { return snap_turn::command(std::string(args)); });
        dev_commands::add("gpu", "gpu status | gpu names on | gpu trace <prefix> [dump <from> <to>] [scale <n>]: one-frame GPU trace",
                          [](std::string_view args) { return gpu_trace::command(std::string(args)); });
        graphics::register_command();
        dev_commands::add("re", "re peek <rva> <n> | re poke <rva> <hex bytes>: read or patch the game image",
                          [](std::string_view args) {
                              std::string reply;
                              handle_command("re " + std::string(args), reply);
                              return reply;
                          });
#if FF7VR_ENGINE_WITH_DLSS
        dlss::start(cfg, ctx.dll_dir);
        dev_commands::add("dlss", "dlss status|on|off|init|preset <default|j|k|l|m>|reset|recreate|dump|timing ...: DLSS in place of the anti-aliasing",
                          [](std::string_view args) { return dlss::command(std::string(args)); });
#endif

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
void filter_pad(unsigned long user, unsigned short* buttons) { controls::filter_pad(user, buttons); }
void filter_sticks(unsigned long user, short* lx, short* ly, short* rx, short* ry) { snap_turn::filter_sticks(user, lx, ly, rx, ry); }

}  // namespace ff7vr::engine
