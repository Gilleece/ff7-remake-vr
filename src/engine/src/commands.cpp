// Dev pipe commands of the engine module (\\.\pipe\ff7vr-dev, tools/dev/send-input.ps1 -Pipe):
//
//   stereo status                 state, sizes, call counters, frame time of the last 10 s window
//   stereo views                  last eye cameras and projection terms
//   stereo on | off               switch stereo rendering from the next frame
//   stereo mirror <off|left|right|both|crop>
//   stereo eye <w> <h>            per-eye render size (built-in fixed host)
//   stereo fov <l> <r> <u> <d>    left eye FOV in degrees (built-in fixed host; right eye mirrored)
//   stereo ipd <mm>               (built-in fixed host)
//   stereo motion <static|yaw|sway|yawsway>   scripted head motion (built-in fixed host)
//   stereo head <yaw> [pitch]     fixed head rotation in degrees, left / up positive (built-in fixed host)
//   stereo scale <f>              world scale
//   stereo pitch <0|1>            decoupled pitch
//   stereo positional <0|1>
//   stereo lightfix <0|1>         light sort-key fix (patch applied while stereo renders)
//   stereo log <n>                log the eye cameras of the next n stereo frames
//   stereo host <render|fixed>    switch where eye size and views come from
//   stereo bloomfix [0|1]         right-eye bloom fix (bloom_fix.h), with its counters
//   stereo aofix [0|1]            right-eye ambient occlusion fix (bloom_fix.h), with its counters
//   stereo distortfix [0|1]       right-eye distortion (heat haze) fix (distortion_fix.h), with its counters
//   stereo movie [on|off]         movie detection (movie_watch.h), with its state
//   stereo cutscene [on|off|status|delay <ms>|hold <ms>|simulate on|off|<ms>]   cutscenes on the virtual screen (movie_watch.h)
//   stereo window [<w>x<h>|0]     game window size while VR renders in a fullscreen mode (fixes.h)
//   stereo frametime <s>          frame time window length in seconds; restarts the window
//   stereo swap <0|1>             test: right eye rendered into the left half and vice versa, to tell
//                                 bugs that follow the view's position from bugs that follow its index
//   re peek <rva> <n>, re poke <rva> <hex bytes>
//   cvar get <name>
//   cvar set <name> <value>

#include "ff7vr/engine/cvars.h"
#include "ff7vr/engine/engine.h"

#include "bloom_fix.h"
#include "distortion_fix.h"
#include "engine_internal.h"
#include "fixes.h"
#include "movie_watch.h"
#include "stereo_device.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "render_host.h"
#define FF7VR_HOST_RENDER_HELP "|render"
#else
#define FF7VR_HOST_RENDER_HELP ""
#endif

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <format>
#include <sstream>
#include <vector>

namespace ff7vr::engine {
namespace {

// ------------------------------------------------------------------ reverse-engineering probes
// `re peek <rva> <n>`, `re poke <rva> <hex bytes>`: read or patch the game image (RVAs
// relative to the exe's base), for trying a patch in a running game before writing it.
namespace probe {

std::uintptr_t base() {
    static const std::uintptr_t b = module::main_module().base;
    return b;
}

bool read_guarded(const void* p, void* out, std::size_t n) {
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += std::format("{}{:02x}", i ? " " : "", p[i]);
    return s;
}

std::string command(const std::vector<std::string>& a) {
    try {
        if (a.size() == 4 && a[1] == "peek") {
            const std::uintptr_t rva = std::stoull(a[2], nullptr, 16);
            const std::size_t n = std::min<std::size_t>(std::stoul(a[3]), 256);
            std::vector<std::uint8_t> buf(n);
            if (!read_guarded(reinterpret_cast<const void*>(base() + rva), buf.data(), n)) return "err not readable";
            return std::format("ok {:#x}: {}", rva, hex_bytes(buf.data(), n));
        }
        if (a.size() >= 4 && a[1] == "poke") {
            const std::uintptr_t rva = std::stoull(a[2], nullptr, 16);
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 3; i < a.size(); ++i) bytes.push_back(static_cast<std::uint8_t>(std::stoul(a[i], nullptr, 16)));
            std::vector<std::uint8_t> old(bytes.size());
            void* p = reinterpret_cast<void*>(base() + rva);
            if (!read_guarded(p, old.data(), old.size())) return "err not readable";
            if (!hook::write_memory(p, bytes.data(), bytes.size())) return "err write failed";
            log::info("probe: poke {:#x}: {} -> {}", rva, hex_bytes(old.data(), old.size()), hex_bytes(bytes.data(), bytes.size()));
            return std::format("ok {:#x}: was {}", rva, hex_bytes(old.data(), old.size()));
        }
    } catch (const std::exception& e) {
        return std::string("err ") + e.what();
    }
    return "err usage: re peek <rva> <n> | re poke <rva> <hex bytes>";
}

}  // namespace probe

std::vector<std::string> split(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

bool to_float(const std::string& s, double& out) {
    try {
        std::size_t n = 0;
        out = std::stod(s, &n);
        return n == s.size();
    } catch (...) {
        return false;
    }
}

std::string stereo_command(const std::vector<std::string>& a) {
    if (a.size() < 2 || a[1] == "status") {
        return std::format("ok installed={} {}", stereo_installed() ? 1 : 0, device::status());
    }
    const std::string& c = a[1];
    device::Settings& s = device::settings();
    FixedStereoHost* fixed = device::fixed_host();
    double v[4]{};
    if (c == "views") return "ok " + device::last_views();
    if (c == "on" || c == "off") {
        if (!stereo_installed()) return "err stereo device not installed";
        request_stereo(c == "on");
        if (c == "off") fixes::vr_window_leave();
        return "ok stereo " + c + " requested";
    }
    if (c == "mirror" && a.size() == 3) {
        mirror::Mode m{};
        if (!mirror::parse_mode(a[2], m)) return "err mirror mode: off|left|right|both|crop";
        s.mirror = static_cast<int>(m);
        return std::string("ok mirror ") + mirror::to_string(m);
    }
    if (c == "eye" && a.size() == 4 && fixed && to_float(a[2], v[0]) && to_float(a[3], v[1])) {
        auto o = fixed->options();
        o.eye_width = static_cast<std::uint32_t>(v[0]);
        o.eye_height = static_cast<std::uint32_t>(v[1]);
        fixed->set_options(o);
        return "ok " + fixed->describe();
    }
    if (c == "fov" && a.size() == 6 && fixed && to_float(a[2], v[0]) && to_float(a[3], v[1]) && to_float(a[4], v[2]) &&
        to_float(a[5], v[3])) {
        constexpr double k = 3.14159265358979323846 / 180.0;
        auto o = fixed->options();
        o.fov_left = HostFov{static_cast<float>(v[0] * k), static_cast<float>(v[1] * k), static_cast<float>(v[2] * k),
                             static_cast<float>(v[3] * k)};
        fixed->set_options(o);
        return "ok " + fixed->describe();
    }
    if (c == "ipd" && a.size() == 3 && fixed && to_float(a[2], v[0])) {
        auto o = fixed->options();
        o.ipd_metres = static_cast<float>(v[0] / 1000.0);
        fixed->set_options(o);
        return "ok " + fixed->describe();
    }
    if (c == "head" && (a.size() == 3 || a.size() == 4) && fixed && to_float(a[2], v[0]) && (a.size() == 3 || to_float(a[3], v[1]))) {
        auto o = fixed->options();
        o.head_yaw_deg = static_cast<float>(v[0]);
        o.head_pitch_deg = static_cast<float>(a.size() == 4 ? v[1] : 0.0);
        fixed->set_options(o);
        return "ok " + fixed->describe();
    }
    if (c == "motion" && a.size() == 3 && fixed) {
        auto o = fixed->options();
        if (!parse_head_motion(a[2], o.motion)) return "err motion: static|yaw|sway|yawsway";
        fixed->set_options(o);
        return "ok " + fixed->describe();
    }
    if (c == "scale" && a.size() == 3 && to_float(a[2], v[0]) && v[0] > 0) {
        s.world_scale = static_cast<float>(v[0]);
        return std::format("ok world_scale {}", s.world_scale.load());
    }
    if (c == "pitch" && a.size() == 3) {
        s.decouple_pitch = a[2] == "1";
        return std::format("ok decoupled_pitch {}", s.decouple_pitch.load() ? 1 : 0);
    }
    if (c == "positional" && a.size() == 3) {
        s.positional = a[2] == "1";
        return std::format("ok positional {}", s.positional.load() ? 1 : 0);
    }
    if (c == "lightfix" && a.size() == 3) {
        if (!fixes::set_light_fix(a[2] == "1")) return "err light patch not available";
        return std::format("ok lightfix {} (patch {} now; applied while stereo renders)", fixes::light_fix_wanted() ? 1 : 0,
                           fixes::light_patch().value_or(false) ? "in place" : "not in place");
    }
    if (c == "host" && a.size() == 3) {
        if (a[2] == "fixed") {
            device::set_host(nullptr);
            return "ok host fixed";
        }
#if FF7VR_ENGINE_WITH_RENDER
        if (a[2] == "render") {
            device::set_host(render_stereo_host());
            return "ok host render";
        }
#endif
        return "err host: fixed" FF7VR_HOST_RENDER_HELP;
    }
    if (c == "log" && a.size() == 3 && to_float(a[2], v[0])) {
        s.log_frames = static_cast<int>(v[0]);
        return "ok";
    }
    if (c == "bloomfix") {
        if (a.size() == 3) bloom_fix::set_enabled(a[2] == "1");
        return "ok " + bloom_fix::status();
    }
    if (c == "aofix") {
        if (a.size() == 3) bloom_fix::set_ao_enabled(a[2] == "1");
        return "ok " + bloom_fix::ao_status();
    }
    if (c == "distortfix") {
        if (a.size() == 3) distortion_fix::set_enabled(a[2] == "1");
        if (a.size() == 4 && a[2] == "opaque") distortion_fix::set_opaque(a[3] == "1");
        return "ok " + distortion_fix::status();
    }
    if (c == "movie") {
        if (a.size() == 3 && (a[2] == "on" || a[2] == "off")) movie::set_enabled(a[2] == "on");
        if (a.size() == 4 && a[2] == "menu") movie::set_include_menu(a[3] == "1");
        if (a.size() == 4 && a[2] == "simulate") movie::set_simulate(a[3] == "on" || a[3] == "1");
        return "ok " + movie::status();
    }
    if (c == "cutscene") {
        if (a.size() == 3 && (a[2] == "on" || a[2] == "off")) movie::set_cutscene(a[2] == "on");
        if (a.size() == 4 && a[2] == "simulate")
            movie::set_cutscene_simulate(a[3] != "off" && a[3] != "0", to_float(a[3], v[0]) && v[0] > 1 ? static_cast<int>(v[0]) : 0);
        if (a.size() == 4 && (a[2] == "delay" || a[2] == "hold") && to_float(a[3], v[0]) && v[0] >= 0)
            movie::set_cutscene_times(a[2] == "delay" ? static_cast<int>(v[0]) : -1, a[2] == "hold" ? static_cast<int>(v[0]) : -1);
        return "ok " + movie::cutscene_status();
    }
    if (c == "window") {
        if (a.size() == 3) fixes::set_vr_window_size(a[2]);
        return "ok " + fixes::vr_window_status();
    }
    if (c == "frametime" && a.size() == 3 && to_float(a[2], v[0]) && v[0] >= 1 && v[0] <= 600) {
        s.frame_window_ms = static_cast<int>(v[0] * 1000);
        s.frame_window_reset = true;
        return std::format("ok frame time window {} s, restarted", v[0]);
    }
    if (c == "framelog") return device::framelog_command(a);
    if (c == "swap" && a.size() == 3) {
        s.swap_rects = a[2] == "1";
        return std::format("ok swap_rects {}", s.swap_rects.load() ? 1 : 0);
    }
    return "err unknown stereo command (status|views|on|off|mirror|eye|fov|ipd|motion|scale|pitch|positional|lightfix|log)";
}

std::string cvar_command(const std::vector<std::string>& a) {
    if (a.size() == 3 && a[1] == "get") {
        auto v = cvar::get(log::widen(a[2]));
        if (!v) return cvar::available() ? "err no such variable" : "err console manager not available";
        return std::format("ok {} int={} float={} setby={:#x}", a[2], v->i, v->f, v->flags & 0xFF000000u);
    }
    if (a.size() >= 4 && a[1] == "set") {
        std::string value = a[3];
        for (std::size_t i = 4; i < a.size(); ++i) value += " " + a[i];
        if (!cvar::set(log::widen(a[2]), log::widen(value)))
            return cvar::available() ? "err no such variable" : "err console manager not available";
        return "ok " + a[2] + " = " + value + " (applied on the game thread)";
    }
    return "err usage: cvar get <name> | cvar set <name> <value>";
}

}  // namespace

bool handle_command(const std::string& line, std::string& reply) {
    try {
        const auto a = split(line);
        if (a.empty()) return false;
        if (a[0] == "stereo") {
            reply = stereo_command(a);
            return true;
        }
        if (a[0] == "cvar") {
            reply = cvar_command(a);
            return true;
        }
        if (a[0] == "re") {
            reply = probe::command(a);
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        reply = std::string("err ") + e.what();
        return true;
    }
}

}  // namespace ff7vr::engine
