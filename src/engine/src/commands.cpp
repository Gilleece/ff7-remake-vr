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
//   stereo scale <f>              world scale
//   stereo pitch <0|1>            decoupled pitch
//   stereo positional <0|1>
//   stereo lightfix <0|1>         light sort-key patch
//   stereo log <n>                log the eye cameras of the next n stereo frames
//   stereo host <render|fixed>    switch where eye size and views come from
//   cvar get <name>
//   cvar set <name> <value>

#include "ff7vr/engine/cvars.h"
#include "ff7vr/engine/engine.h"

#include "fixes.h"
#include "stereo_device.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "render_host.h"
#define FF7VR_HOST_RENDER_HELP "|render"
#else
#define FF7VR_HOST_RENDER_HELP ""
#endif

#include "ff7vr/core/log.h"

#include <format>
#include <sstream>
#include <vector>

namespace ff7vr::engine {
namespace {

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
        if (!fixes::set_light_patch(a[2] == "1")) return "err light patch not available";
        return std::format("ok lightfix {}", fixes::light_patch().value_or(false) ? 1 : 0);
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
        return false;
    } catch (const std::exception& e) {
        reply = std::string("err ") + e.what();
        return true;
    }
}

}  // namespace ff7vr::engine
