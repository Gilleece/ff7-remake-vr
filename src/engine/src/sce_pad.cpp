#include "sce_pad.h"

#include "controls.h"
#include "snap_turn.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::engine::sce_pad {
namespace {

// ScePadData (libScePad), 0x78 bytes; the fields used here. The game's poll reads the same
// offsets (buttons at +0, sticks at +4..+7, connected at +0x4C; checked by the signature).
constexpr std::size_t kDataSize = 0x78, kButtons = 0x00, kSticks = 0x04, kConnected = 0x4c, kConnectedCount = 0x64;
constexpr std::uint32_t kIntercepted = 0x80000000u;
constexpr int kHandles = 4;

using ReadStateFn = int (*)(int handle, void* data);
hook::InlineHook g_hook;
std::atomic<bool> g_installed{false};

// Handles in the order the game first polls them (its users 0..3): pad index 0..3.
std::mutex g_mutex;
int g_handles[kHandles] = {0, 0, 0, 0};
int g_handle_count = 0;
int g_connected[kHandles] = {-1, -1, -1, -1};  // -1 unknown, 0 no pad, 1 pad

// The virtual pad (dev): button word presented on pad index 0.
std::atomic<bool> g_virtual{false};
std::atomic<std::uint32_t> g_virtual_buttons{0};
std::atomic<std::uint8_t> g_virtual_lx{0x80}, g_virtual_ly{0x80};

std::atomic<std::uint64_t> g_calls{0}, g_with_pad{0}, g_changed{0};
std::atomic<std::uint32_t> g_last_in{0}, g_last_out{0};
std::atomic<int> g_last_rc{0};
std::atomic<std::uint64_t> g_sticks_changed{0};
std::atomic<int> g_last_stick[4] = {0x80, 0x80, 0x80, 0x80};  // left x, y in; left x, y to the game

// Stick bytes (0..255, 0x80 centre, Y down) <-> XInput axes (-32768..32767, Y up).
short to_axis(std::uint8_t v, bool flip) {
    const double a = (static_cast<double>(v) - 128.0) * (32767.0 / 127.0);
    return static_cast<short>(std::clamp(std::lround(flip ? -a : a), -32768L, 32767L));
}
std::uint8_t to_byte(short v, bool flip) {
    const double a = static_cast<double>(flip ? -v : v) * (127.0 / 32767.0) + 128.0;
    return static_cast<std::uint8_t>(std::clamp(std::lround(a), 0L, 255L));
}

int index_of(int handle) {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < g_handle_count; ++i)
        if (g_handles[i] == handle) return i;
    if (g_handle_count < kHandles) {
        g_handles[g_handle_count] = handle;
        return g_handle_count++;
    }
    return -1;
}

void note_connected(int index, bool on, int handle) {
    int was;
    {
        std::lock_guard lock(g_mutex);
        was = g_connected[index];
        g_connected[index] = on ? 1 : 0;
    }
    if (was != (on ? 1 : 0) && (on || was == 1))
        log::info("controls: PlayStation pad {} {} (libScePad handle {:#x}){}", index, on ? "connected" : "disconnected",
                  static_cast<unsigned>(handle), g_virtual.load() && index == 0 ? " (virtual pad)" : "");
}

int read_state_detour(int handle, void* data) {
    int rc = g_hook.original<ReadStateFn>()(handle, data);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!data) return rc;
    auto* d = static_cast<std::uint8_t*>(data);
    const int index = index_of(handle);
    if (index == 0 && g_virtual.load(std::memory_order_relaxed)) {
        if (rc != 0 || !d[kConnected]) {
            // No pad behind this handle: present a connected, neutral one.
            std::memset(d, 0, kDataSize);
            std::memset(d + kSticks, 0x80, 4);
            d[kConnected] = 1;
            d[kConnectedCount] = 1;
            rc = 0;
        }
        std::uint32_t b;
        std::memcpy(&b, d + kButtons, 4);
        b |= g_virtual_buttons.load(std::memory_order_relaxed);
        std::memcpy(d + kButtons, &b, 4);
        if (g_virtual_lx.load() != 0x80 || g_virtual_ly.load() != 0x80) {
            d[kSticks] = g_virtual_lx.load();
            d[kSticks + 1] = g_virtual_ly.load();
        }
    }
    g_last_rc.store(rc, std::memory_order_relaxed);
    const bool connected = rc == 0 && d[kConnected] != 0;
    if (index >= 0) note_connected(index, connected, handle);
    if (!connected || index < 0) return rc;
    g_with_pad.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t b;
    std::memcpy(&b, d + kButtons, 4);
    if (b & kIntercepted) return rc;  // the game ignores this state anyway
    const std::uint32_t in = b;
    controls::filter_sce(static_cast<unsigned long>(index), &b);
    if (b != in) {
        std::memcpy(d + kButtons, &b, 4);
        g_changed.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_in.store(in, std::memory_order_relaxed);
    g_last_out.store(b, std::memory_order_relaxed);
    // The sticks: snap turn and head-directed movement, as for an XInput pad (pad slots 4..7).
    // Written back only when the filter changed an axis, so an untouched stick keeps its bytes.
    short ax[4] = {to_axis(d[kSticks], false), to_axis(d[kSticks + 1], true), to_axis(d[kSticks + 2], false), to_axis(d[kSticks + 3], true)};
    const short before[4] = {ax[0], ax[1], ax[2], ax[3]};
    snap_turn::filter_sticks(4 + static_cast<unsigned long>(index), &ax[0], &ax[1], &ax[2], &ax[3]);
    g_last_stick[0].store(d[kSticks], std::memory_order_relaxed);
    g_last_stick[1].store(d[kSticks + 1], std::memory_order_relaxed);
    bool moved = false;
    for (int i = 0; i < 4; ++i) {
        if (ax[i] == before[i]) continue;
        d[kSticks + i] = to_byte(ax[i], i % 2 == 1);
        moved = true;
    }
    if (moved) g_sticks_changed.fetch_add(1, std::memory_order_relaxed);
    g_last_stick[2].store(d[kSticks], std::memory_order_relaxed);
    g_last_stick[3].store(d[kSticks + 1], std::memory_order_relaxed);
    return rc;
}

}  // namespace

bool init(std::uintptr_t read_state) {
    if (!read_state) {
        log::info("controls: PlayStation pads: scePadReadState not found; the combinations work with XInput pads only");
        return false;
    }
    if (controls::settings().pad_source.load() == controls::kSourceXInput) {
        log::info("controls: PlayStation pads: not filtered ([controls] pad_source = xinput)");
        return false;
    }
    if (!g_hook.create(reinterpret_cast<void*>(read_state), reinterpret_cast<void*>(&read_state_detour))) {
        log::warn("controls: PlayStation pads: could not hook scePadReadState");
        return false;
    }
    g_installed = true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    log::info("controls: PlayStation pads: scePadReadState (ff7remake_.exe+{:#x}) hooked; the combinations and the chord also apply to a "
              "DualShock 4 / DualSense the game reads itself (touch pad click = View/Back)",
              read_state - base);
    return true;
}

std::string status() {
    std::string pads;
    {
        std::lock_guard lock(g_mutex);
        for (int i = 0; i < g_handle_count; ++i)
            pads += std::format("{}{} {}", pads.empty() ? "" : ", ", i, g_connected[i] == 1 ? "pad" : g_connected[i] == 0 ? "none" : "?");
    }
    return std::format("PlayStation pads: hook {}, scePadReadState calls {} ({} with a pad, {} changed), handles [{}], last rc {:#x}, last "
                       "buttons {:#010x} -> game {:#010x}, left stick ({}, {}) -> game ({}, {}) (sticks changed in {} polls), virtual pad {}",
                       g_installed.load() ? "on" : "off", g_calls.load(), g_with_pad.load(), g_changed.load(), pads,
                       static_cast<unsigned>(g_last_rc.load()), g_last_in.load(), g_last_out.load(), g_last_stick[0].load(),
                       g_last_stick[1].load(), g_last_stick[2].load(), g_last_stick[3].load(), g_sticks_changed.load(),
                       g_virtual.load() ? std::format("{:#010x}", g_virtual_buttons.load()) : std::string("off"));
}

std::string timing() {
    static std::mutex m;
    static std::uint64_t last_calls = 0, last_pad = 0, last_changed = 0;
    std::lock_guard lk(m);
    const std::uint64_t c = g_calls.load(), p = g_with_pad.load(), ch = g_changed.load();
    const std::string line = std::format("PlayStation pads: {} polls, {} with a pad, {} filtered{}", c - last_calls, p - last_pad,
                                         ch - last_changed, g_installed.load() ? "" : " (not hooked)");
    last_calls = c, last_pad = p, last_changed = ch;
    return line;
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") return "ok " + status();
    if (a[0] == "off" && a.size() == 1) {
        g_virtual = false;
        g_virtual_buttons = 0;
        g_virtual_lx = 0x80;
        g_virtual_ly = 0x80;
        return "ok virtual PlayStation pad off";
    }
    if (a.size() == 1 || a.size() == 3) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(a[0].c_str(), &end, 16);
        if (!end || *end) return "err usage: controls ps <hex buttons> [<left x> <left y>] | off | status";
        if (!g_installed.load()) return "err scePadReadState is not hooked";
        // Optional left stick, -1..1 each, +y = forward (as `stick L`).
        const double x = a.size() == 3 ? std::clamp(std::strtod(a[1].c_str(), nullptr), -1.0, 1.0) : 0.0;
        const double y = a.size() == 3 ? std::clamp(std::strtod(a[2].c_str(), nullptr), -1.0, 1.0) : 0.0;
        g_virtual_lx = to_byte(static_cast<short>(std::lround(x * 32767.0)), false);
        g_virtual_ly = to_byte(static_cast<short>(std::lround(y * 32767.0)), true);
        g_virtual_buttons = static_cast<std::uint32_t>(v);
        g_virtual = true;
        return std::format("ok virtual PlayStation pad buttons {:#010x}, left stick bytes ({}, {})", v, g_virtual_lx.load(), g_virtual_ly.load());
    }
    return "err usage: controls ps <hex buttons> [<left x> <left y>] | off | status";
}

}  // namespace ff7vr::engine::sce_pad
