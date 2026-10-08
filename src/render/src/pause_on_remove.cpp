#include "pause_on_remove.h"

#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::render::pause_on_remove {
namespace {

std::atomic<bool> g_enabled{true};  // [xr] pause_on_remove
std::atomic<int> g_key{0x4D};       // [xr] pause_key: M (the game's menu, which pauses)
constexpr ULONGLONG kMinGapMs = 5000;

std::mutex g_mutex;
xr::SessionState g_last_state = xr::SessionState::Uninitialized;
int g_last_presence = -1;
bool g_presence_mode = false;  // a user presence event was seen: it replaces the focus loss
bool g_was_stereo = false;
ULONGLONG g_last_sent = 0;
std::atomic<std::uint64_t> g_sent{0}, g_skipped_focus{0}, g_skipped_rate{0}, g_triggers{0};

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// One press and release of the key, as the dev harness sends it.
bool send_key(int vk) {
    INPUT in[2] = {};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = static_cast<WORD>(vk);
        in[i].ki.wScan = static_cast<WORD>(scan);
        in[i].ki.dwFlags = i == 1 ? KEYEVENTF_KEYUP : 0;
    }
    return SendInput(2, in, sizeof(INPUT)) == 2;
}

// Caller holds g_mutex.
std::string pause_now(const char* why) {
    ++g_triggers;
    const ULONGLONG now = GetTickCount64();
    if (g_last_sent && now - g_last_sent < kMinGapMs) {
        ++g_skipped_rate;
        log::info("xr: {}: pause key not sent (one was sent {} ms ago)", why, now - g_last_sent);
        return "skipped (rate)";
    }
    if (!game_has_focus()) {
        ++g_skipped_focus;
        log::info("xr: {}: pause key not sent (the game window does not have the focus)", why);
        return "skipped (no focus)";
    }
    const int vk = g_key.load();
    if (!send_key(vk)) {
        log::warn("xr: {}: SendInput failed ({})", why, GetLastError());
        return "SendInput failed";
    }
    g_last_sent = now;
    ++g_sent;
    log::info("xr: {}: pause key {} sent", why, vk);
    return std::format("pause key {} sent", vk);
}

const char* presence_text(int p) { return p < 0 ? "not reported" : p ? "present" : "absent"; }

}  // namespace

void read_config(const Config& cfg) {
    g_enabled = cfg.get_bool("xr", "pause_on_remove", true);
    g_key = static_cast<int>(std::clamp<long long>(cfg.get_int("xr", "pause_key", 0x4D), 0, 255));
}

void note(xr::SessionState state, int presence, bool stereo) {
    std::lock_guard lock(g_mutex);
    const xr::SessionState before = g_last_state;
    const int presence_before = g_last_presence;
    const bool stereo_before = g_was_stereo;
    g_last_state = state;
    g_last_presence = presence;
    g_was_stereo = stereo;
    if (presence != presence_before && presence >= 0) {
        if (!g_presence_mode) log::info("xr: the runtime reports the user's presence (XR_EXT_user_presence): pausing on 'user absent' instead of focus loss");
        g_presence_mode = true;
        log::info("xr: user {}", presence_text(presence));
        if (presence == 0 && stereo_before && g_enabled.load() && g_key.load() > 0) pause_now("user absent (headset off)");
        return;
    }
    if (g_presence_mode || state == before) return;
    const bool lost = before == xr::SessionState::Focused && (state == xr::SessionState::Visible || state == xr::SessionState::Running);
    if (!lost) return;
    log::info("xr: session {} -> {} (headset off or the runtime's menu){}", xr::ToString(before), xr::ToString(state),
              !g_enabled.load() ? ": pause_on_remove off" : !stereo_before ? ": 3D not running, nothing sent" : "");
    if (g_enabled.load() && stereo_before && g_key.load() > 0) pause_now("focus lost");
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") {
        std::lock_guard lock(g_mutex);
        return std::format("ok pause_on_remove {} key {}; mode {}; session {}, user {}; triggers {}, sent {}, skipped (no focus) {}, skipped (rate) {}",
                           g_enabled.load() ? 1 : 0, g_key.load(), g_presence_mode ? "user presence" : "focus loss", xr::ToString(g_last_state),
                           presence_text(g_last_presence), g_triggers.load(), g_sent.load(), g_skipped_focus.load(), g_skipped_rate.load());
    }
    if (a[0] == "on" || a[0] == "off") {
        g_enabled = a[0] == "on";
        return std::string("ok pause_on_remove ") + (g_enabled.load() ? "1" : "0");
    }
    if (a[0] == "key" && a.size() == 2) {
        g_key = std::clamp(std::atoi(a[1].c_str()), 0, 255);
        return std::format("ok pause key {}", g_key.load());
    }
    if (a[0] == "send") {
        std::lock_guard lock(g_mutex);
        return "ok " + pause_now("dev");
    }
    return "err usage: xr-pause status | on | off | key <vk> | send";
}

}  // namespace ff7vr::render::pause_on_remove
