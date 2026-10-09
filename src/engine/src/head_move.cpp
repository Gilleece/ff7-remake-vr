#include "head_move.h"

#include "ue_math.h"

#include "ff7vr/core/log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <sstream>
#include <vector>

namespace ff7vr::engine::head_move {
namespace {

std::atomic<bool> g_fp_head{false};     // [first_person] move = head
std::atomic<bool> g_third_head{false};  // [camera] move = head

// Published by the stereo device once per stereo frame.
std::atomic<float> g_offset{0.0f};         // head heading minus game camera yaw (degrees, -180..180)
std::atomic<float> g_head_heading{0.0f};   // head heading in the world (diagnostics)
std::atomic<float> g_camera_yaw{0.0f};     // game camera yaw (diagnostics)
std::atomic<bool> g_first_person{false};
std::atomic<std::int64_t> g_published_us{0};
std::atomic<std::uint64_t> g_frames{0}, g_steep{0};

// A heading older than this is not used (loading screens, stereo off: no stereo frames).
constexpr std::int64_t kFreshUs = 250000;

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float wrap180(double d) {
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<float>(d - 180.0);
}

const char* mode_name(bool head) { return head ? "head" : "camera"; }

}  // namespace

void read_config(const Config& cfg) {
    g_fp_head = cfg.get_string("first_person", "move", "camera") == "head";
    g_third_head = cfg.get_string("camera", "move", "camera") == "head";
    if (g_fp_head.load() || g_third_head.load())
        log::info("controls: the left stick moves where the head points in {} (move = head); keyboard movement is not rotated",
                  g_fp_head.load() && g_third_head.load() ? "first and third person" : g_fp_head.load() ? "first person" : "third person");
}

void publish(const ue::FRotator& game_camera, const ue::FRotator& head_world, bool first_person) {
    // The head's heading: its forward vector projected on the floor. Looking steeply up or
    // down that projection gets short and unstable, so the head's up vector (which points
    // forward when the head is bent down, backward when it is tilted up) is added with the
    // matching sign: the sum stays horizontal-forward at any pitch.
    const math::Quat q = math::quat_from_rotator(head_world);
    const math::Vec f = math::rotate(q, math::Vec{1, 0, 0});
    const math::Vec u = math::rotate(q, math::Vec{0, 0, 1});
    const double sgn = f.z < 0.0 ? 1.0 : -1.0;
    const double hx = f.x + sgn * u.x, hy = f.y + sgn * u.y;
    if (hx * hx + hy * hy < 1e-6) {
        ++g_steep;
        return;  // keep the previous heading
    }
    const double heading = std::atan2(hy, hx) * 180.0 / 3.14159265358979323846;
    g_head_heading.store(wrap180(heading), std::memory_order_relaxed);
    g_camera_yaw.store(wrap180(game_camera.Yaw), std::memory_order_relaxed);
    g_offset.store(wrap180(heading - game_camera.Yaw), std::memory_order_relaxed);
    g_first_person.store(first_person, std::memory_order_relaxed);
    g_published_us.store(now_us(), std::memory_order_release);
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

bool stick_rotation(float& degrees) {
    const bool fp = g_first_person.load(std::memory_order_relaxed);
    if (!(fp ? g_fp_head : g_third_head).load(std::memory_order_relaxed)) return false;
    const std::int64_t t = g_published_us.load(std::memory_order_acquire);
    if (t == 0 || now_us() - t > kFreshUs) return false;
    degrees = g_offset.load(std::memory_order_relaxed);
    return true;
}

std::string status() {
    const std::int64_t t = g_published_us.load();
    return std::format("move first person {}, third person {}; head heading {:.1f} deg, game camera yaw {:.1f} deg, head relative to the camera "
                       "{:+.1f} deg ({}, {}); frames {}, steep {}",
                       mode_name(g_fp_head.load()), mode_name(g_third_head.load()), g_head_heading.load(), g_camera_yaw.load(),
                       g_offset.load(), g_first_person.load() ? "first person" : "third person",
                       t ? std::format("{:.0f} ms ago", double(now_us() - t) / 1000.0) : std::string("no stereo frame yet"), g_frames.load(),
                       g_steep.load());
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") return "ok " + status();
    if ((a[0] == "camera" || a[0] == "head") && a.size() <= 2) {
        const bool head = a[0] == "head";
        const std::string which = a.size() == 2 ? a[1] : "both";
        if (which != "first" && which != "third" && which != "both") return "err usage: controls move camera|head [first|third|both]";
        if (which != "third") g_fp_head = head;
        if (which != "first") g_third_head = head;
        log::info("controls: move {} for {}", mode_name(head), which == "both" ? "first and third person" : which + " person");
        return "ok " + status();
    }
    return "err usage: controls move status | camera|head [first|third|both]";
}

}  // namespace ff7vr::engine::head_move
