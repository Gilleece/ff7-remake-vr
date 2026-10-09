#include "snap_turn.h"

#include "head_move.h"
#include "stereo_device.h"

#include "ff7vr/core/log.h"
#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::engine::snap_turn {
namespace {

std::atomic<float> g_degrees{0.0f};              // [comfort] snap_turn (0 = off)
std::atomic<float> g_deadzone{0.6f};             // [comfort] snap_turn_deadzone (fraction of full deflection)
std::atomic<int> g_repeat_ms{0};                 // [comfort] snap_turn_repeat_ms (0 = one step per push)
std::atomic<int> g_left_key{0}, g_right_key{0};  // [comfort] snap_left_key / snap_right_key
std::atomic<int> g_log{0};                       // left stick lines still to be logged (-1 = all)

constexpr unsigned long kPads = 8;  // XInput users 0..3, PlayStation pads 0..3 as 4..7 (sce_pad.h)
struct PadState {
    bool armed = true;
    std::int64_t last_step_us = 0;
};
std::mutex g_mutex;
PadState g_pads[kPads];

std::atomic<std::uint64_t> g_steps{0}, g_stick_steps{0}, g_key_steps{0}, g_rx_hidden{0}, g_rotated{0}, g_rotated_head{0};
std::atomic<int> g_last_in_lx{0}, g_last_in_ly{0}, g_last_out_lx{0}, g_last_out_ly{0}, g_last_rx{0};
bool g_key_down[2] = {};

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float turn_now() {
#if FF7VR_ENGINE_WITH_RENDER
    return render::GetSnapYawDeg();
#else
    return 0.0f;
#endif
}

// One step, `sign` +1 = right, -1 = left. Lock-free: the render module applies it with its
// next frame (and logs it there), so the poll thread never waits.
void step(int sign, const char* source) {
    const float deg = g_degrees.load();
    if (deg == 0.0f) return;
    ++g_steps;
    const float d = static_cast<float>(sign) * deg;
#if FF7VR_ENGINE_WITH_RENDER
    render::RequestSnapTurn(d);
#endif
    log::info("comfort: snap turn {:+.0f} deg ({})", d, source);
}

short clamp_axis(double v) { return static_cast<short>(std::clamp<long>(std::lround(v), -32768L, 32767L)); }

std::string degrees_text() {
    const float d = g_degrees.load();
    return d > 0.0f ? std::format("{:.0f} deg", d) : std::string("off");
}

}  // namespace

void read_config(const Config& cfg) {
    g_degrees = static_cast<float>(std::clamp(cfg.get_float("comfort", "snap_turn", 0.0), 0.0, 180.0));
    g_deadzone = static_cast<float>(std::clamp(cfg.get_float("comfort", "snap_turn_deadzone", 0.6), 0.1, 0.95));
    g_repeat_ms = static_cast<int>(std::clamp<long long>(cfg.get_int("comfort", "snap_turn_repeat_ms", 0), 0, 5000));
    g_left_key = static_cast<int>(cfg.get_int("comfort", "snap_left_key", 0));
    g_right_key = static_cast<int>(cfg.get_int("comfort", "snap_right_key", 0));
    g_log = static_cast<int>(std::clamp<long long>(cfg.get_int("comfort", "snap_log", 0), -1, 100000));
    if (g_degrees.load() > 0.0f)
        log::info("comfort: snap turn {} (right stick X beyond {:.2f}, repeat {} ms, keys {} / {}); the right stick X is hidden from the game "
                  "and the left stick follows the view",
                  degrees_text(), g_deadzone.load(), g_repeat_ms.load(), g_left_key.load(), g_right_key.load());
}

void tick() {
    const int keys[2] = {g_left_key.load(), g_right_key.load()};
    if (g_degrees.load() == 0.0f || (keys[0] == 0 && keys[1] == 0) || !device::active()) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focus = pid == GetCurrentProcessId();
    for (int i = 0; i < 2; ++i) {
        const int vk = keys[i];
        const bool down = focus && vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_key_down[i]) {
            ++g_key_steps;
            step(i == 0 ? -1 : 1, "keyboard");
        }
        g_key_down[i] = down;
    }
}

void filter_sticks(unsigned long user, short* lx, short* ly, short* rx, short* ry) {
    (void)ry;
    // Only while 3D renders: menus, movies and the virtual screen get the stick as it is.
    if (user >= kPads || !lx || !ly || !rx || !device::active()) return;
    const float deg = g_degrees.load(std::memory_order_relaxed);
    const std::int64_t now = now_us();
    if (deg != 0.0f) {
        // Right stick X: steps, hidden from the game.
        const float x = static_cast<float>(*rx) / 32767.0f;
        const float dz = g_deadzone.load(std::memory_order_relaxed);
        int fire = 0;
        {
            std::lock_guard lock(g_mutex);
            PadState& p = g_pads[user];
            if (std::fabs(x) >= dz) {
                const int repeat = g_repeat_ms.load(std::memory_order_relaxed);
                if (p.armed || (repeat > 0 && now - p.last_step_us >= std::int64_t(repeat) * 1000)) {
                    fire = x > 0 ? 1 : -1;
                    p.armed = false;
                    p.last_step_us = now;
                }
            } else if (std::fabs(x) < dz * 0.5f) {
                p.armed = true;  // released: the next push steps again
            }
        }
        g_last_rx = *rx;
        if (*rx != 0) ++g_rx_hidden;
        *rx = 0;
        if (fire) {
            ++g_stick_steps;
            step(fire, "right stick");
        }
    }
    // Left stick: rotated so that forward moves the character where the player looks. With
    // [first_person] / [camera] move = head: by the head's heading relative to the game
    // camera (snap turn included, head_move.h); otherwise by snap turn's turn in effect
    // (positive = view turned right).
    float yaw = 0.0f;
    const bool head = head_move::stick_rotation(yaw);
    if (!head) yaw = deg != 0.0f ? turn_now() : 0.0f;
    if (yaw == 0.0f || (*lx == 0 && *ly == 0)) return;
    const double t = yaw * 3.14159265358979 / 180.0, c = std::cos(t), s = std::sin(t);
    const double ix = *lx, iy = *ly;
    const short ox = clamp_axis(ix * c + iy * s), oy = clamp_axis(-ix * s + iy * c);
    ++g_rotated;
    if (head) ++g_rotated_head;
    g_last_in_lx = *lx;
    g_last_in_ly = *ly;
    g_last_out_lx = ox;
    g_last_out_ly = oy;
    *lx = ox;
    *ly = oy;
    // Diagnostics: [comfort] snap_log / `snapturn log <n>`, at most one line per 500 ms.
    static std::atomic<std::int64_t> s_last_log{0};
    int left = g_log.load(std::memory_order_relaxed);
    if (left != 0 && now - s_last_log.load(std::memory_order_relaxed) >= 500000) {
        s_last_log = now;
        if (left > 0) g_log.compare_exchange_strong(left, left - 1);
        log::info("comfort: left stick ({}, {}) -> game ({}, {}) for {} {:.1f} deg", static_cast<int>(ix), static_cast<int>(iy), ox, oy,
                  head ? "the head turned from the game camera by" : "a view turned", yaw);
    }
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") {
        return std::format("ok snap turn {} (deadzone {:.2f}, repeat {} ms, keys {} / {}); view turned {:.1f} deg; steps {} (stick {}, keys {}); "
                           "right stick X hidden in {} polls (last {}); left stick rotated in {} polls ({} by the head) (last ({}, {}) -> ({}, {})); log {}",
                           degrees_text(), g_deadzone.load(), g_repeat_ms.load(), g_left_key.load(), g_right_key.load(), turn_now(),
                           g_steps.load(), g_stick_steps.load(), g_key_steps.load(), g_rx_hidden.load(), g_last_rx.load(), g_rotated.load(),
                           g_rotated_head.load(), g_last_in_lx.load(), g_last_in_ly.load(), g_last_out_lx.load(), g_last_out_ly.load(), g_log.load());
    }
    if (a[0] == "snap" && a.size() == 2) {
        g_degrees = a[1] == "off" ? 0.0f : std::clamp(std::strtof(a[1].c_str(), nullptr), 0.0f, 180.0f);
        return "ok snap turn " + degrees_text();
    }
    if (a[0] == "deadzone" && a.size() == 2) {
        g_deadzone = std::clamp(std::strtof(a[1].c_str(), nullptr), 0.1f, 0.95f);
        return std::format("ok snap turn deadzone {:.2f}", g_deadzone.load());
    }
    if (a[0] == "repeat" && a.size() == 2) {
        g_repeat_ms = std::clamp(std::atoi(a[1].c_str()), 0, 5000);
        return std::format("ok snap turn repeat {} ms", g_repeat_ms.load());
    }
    if (a[0] == "log" && a.size() == 2) {
        g_log = a[1] == "on" ? -1 : std::max(0, std::atoi(a[1].c_str()));
        return std::format("ok snap turn log {}", g_log.load());
    }
    if (a[0] == "stick" && (a.size() == 5 || a.size() == 6)) {
        // Test without a pad: one stick state (lx ly rx ry, -1..1) through the filter.
        short v[4];
        for (int i = 0; i < 4; ++i) v[i] = clamp_axis(std::strtod(a[1 + i].c_str(), nullptr) * 32767.0);
        const unsigned long user = a.size() == 6 ? std::strtoul(a[5].c_str(), nullptr, 10) : 0;
        if (user >= kPads) return "err user index 0..3";
        const short in_lx = v[0], in_ly = v[1], in_rx = v[2];
        filter_sticks(user, &v[0], &v[1], &v[2], &v[3]);
        return std::format("ok sticks after filter: left ({}, {}) -> ({}, {}), right X {} -> {}", in_lx, in_ly, v[0], v[1], in_rx, v[2]);
    }
    return "err usage: snapturn status | snap <deg>|off | deadzone <0.1..0.95> | repeat <ms> | log <n>|on | stick <lx> <ly> <rx> <ry> [user]";
}

}  // namespace ff7vr::engine::snap_turn
