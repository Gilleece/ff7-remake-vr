#include "controls.h"

#include "player.h"
#include "stereo_device.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace ff7vr::engine::controls {
namespace {

Settings g_settings;

enum class Action { Recenter, Stereo, UiNearer, UiFarther, FirstPerson };

const char* action_name(Action a) {
    switch (a) {
        case Action::Recenter: return "recenter";
        case Action::Stereo: return "stereo on/off";
        case Action::UiNearer: return "UI nearer";
        case Action::UiFarther: return "UI farther";
        case Action::FirstPerson: return "first/third person";
    }
    return "?";
}

std::atomic<std::uint64_t> g_triggers{0}, g_pad_triggers{0}, g_pad_polls{0};

// Runs a registered dev command (another module's) off the calling thread and logs the reply.
void run_command(std::string line, std::string what) {
    std::thread([line = std::move(line), what = std::move(what)] {
        std::string reply;
        if (!dev_commands::dispatch(line, reply)) reply = "err command not registered (render module not running)";
        log::info("controls: {}: {}", what, reply);
    }).detach();
}

// "ok ui layer on (active), 2.00 m high at 3.00 m, ..." -> 3.00
bool ui_distance_from_status(const std::string& s, float& out) {
    const std::size_t at = s.find(" m high at ");
    if (at == std::string::npos) return false;
    out = std::strtof(s.c_str() + at + 11, nullptr);
    return out > 0.0f;
}

void change_ui_distance(bool nearer, std::string what) {
    std::thread([nearer, what = std::move(what)] {
        static std::mutex s_serial;  // presses in quick succession read and change the distance one after the other
        std::lock_guard serial(s_serial);
        std::string reply;
        float d = 0.0f;
        if (!dev_commands::dispatch("ui status", reply) || !ui_distance_from_status(reply, d)) {
            log::info("controls: {}: UI layer not available ({})", what, reply);
            return;
        }
        const Settings& s = g_settings;
        const float step = std::max(0.05f, s.ui_step.load());
        const float next = std::clamp(d + (nearer ? -step : step), s.ui_min.load(), s.ui_max.load());
        dev_commands::dispatch(std::format("ui distance {:.2f}", next), reply);
        log::info("controls: {}: UI panel {:.2f} m -> {:.2f} m ({})", what, d, next, reply);
    }).detach();
}

void trigger(Action a, const char* source) {
    ++g_triggers;
    const std::string what = std::format("{} ({})", action_name(a), source);
    switch (a) {
        case Action::Recenter:
            run_command("recenter", what);
            break;
        case Action::Stereo: {
            // The game window stays as it is (a normal window while VR runs), so the virtual
            // screen shows the game at once and switching back needs no mode change.
            const bool on = !device::wanted();
            device::request_active(on);
            log::info("controls: {}: stereo {}", what, on ? "on" : "off (the virtual screen shows the game)");
            break;
        }
        case Action::UiNearer:
        case Action::UiFarther:
            change_ui_distance(a == Action::UiNearer, what);
            break;
        case Action::FirstPerson:
            player::request_pad_toggle();
            log::info("controls: {}: toggle requested", what);
            break;
    }
}

// Keyboard: edge per key, only while the game window has the focus.
struct Key {
    std::atomic<int>* vk;
    Action action;
    bool down = false;
};
Key g_keys[] = {
    {&g_settings.recenter_key, Action::Recenter},
    {&g_settings.stereo_key, Action::Stereo},
    {&g_settings.ui_nearer_key, Action::UiNearer},
    {&g_settings.ui_farther_key, Action::UiFarther},
};

// Gamepad state (XInput poll thread).
constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kStart = 0x0010, kView = 0x0020, kLeftThumb = 0x0040,
                         kRightThumb = 0x0080;
constexpr unsigned kViewReplayMs = 120;
std::mutex g_pad_mutex;
unsigned short g_prev = 0;
bool g_latched = false;     // a combination was used since View went down: View and the combination buttons are hidden
bool g_view_held = false;   // View is down and held back from the game
std::uint64_t g_replay_until = 0;

unsigned short combo_buttons() {
    unsigned short m = 0;
    if (g_settings.pad.load()) m |= kUp | kDown | kStart | kLeftThumb;
    const player::Settings& p = player::settings();
    if (p.pad_toggle.load() && p.fp_available.load()) m |= kRightThumb;
    return m;
}

}  // namespace

Settings& settings() { return g_settings; }

void read_config(const Config& cfg) {
    Settings& s = g_settings;
    s.recenter_key = static_cast<int>(cfg.get_int("controls", "recenter_key", s.recenter_key.load()));
    s.stereo_key = static_cast<int>(cfg.get_int("controls", "stereo_key", s.stereo_key.load()));
    s.ui_nearer_key = static_cast<int>(cfg.get_int("controls", "ui_nearer_key", s.ui_nearer_key.load()));
    s.ui_farther_key = static_cast<int>(cfg.get_int("controls", "ui_farther_key", s.ui_farther_key.load()));
    s.pad = cfg.get_bool("controls", "pad", s.pad.load());
    s.pad_hold_view = cfg.get_bool("controls", "pad_hold_view", s.pad_hold_view.load());
    s.ui_step = static_cast<float>(std::clamp(cfg.get_float("controls", "ui_step", s.ui_step.load()), 0.05, 2.0));
    s.ui_min = static_cast<float>(std::clamp(cfg.get_float("controls", "ui_min", s.ui_min.load()), 0.3, 50.0));
    s.ui_max = static_cast<float>(std::clamp(cfg.get_float("controls", "ui_max", s.ui_max.load()), static_cast<double>(s.ui_min.load()), 50.0));
}

void tick() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focus = pid == GetCurrentProcessId();
    for (Key& k : g_keys) {
        const int vk = k.vk->load();
        const bool down = focus && vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !k.down) trigger(k.action, "keyboard");
        k.down = down;
    }
}

void filter_pad(unsigned long user, unsigned short* buttons) {
    if (user != 0 || !buttons) return;
    const unsigned short combos = combo_buttons();
    if (combos == 0) return;
    ++g_pad_polls;
    std::vector<Action> fired;
    unsigned short b = *buttons;
    {
        std::lock_guard lock(g_pad_mutex);
        const unsigned short pressed = static_cast<unsigned short>(b & ~g_prev);
        g_prev = b;
        if (b & kView) {
            const unsigned short hits = pressed & combos;
            if (hits & kRightThumb) fired.push_back(Action::FirstPerson);
            if (hits & kLeftThumb) fired.push_back(Action::Recenter);
            if (hits & kStart) fired.push_back(Action::Stereo);
            if (hits & kUp) fired.push_back(Action::UiFarther);
            if (hits & kDown) fired.push_back(Action::UiNearer);
            if (hits) {
                g_latched = true;
                g_view_held = false;
                g_replay_until = 0;
            }
        }
        if (g_latched) {
            if ((b & (kView | combos)) == 0) g_latched = false;  // everything released
            b = static_cast<unsigned short>(b & ~(kView | combos));
        } else if (g_settings.pad_hold_view.load()) {
            // View alone: held back until released, then handed to the game as a short press.
            if (b & kView) {
                b = static_cast<unsigned short>(b & ~kView);
                g_view_held = true;
            } else if (g_view_held) {
                g_view_held = false;
                g_replay_until = GetTickCount64() + kViewReplayMs;
            }
            if (g_replay_until && !(b & kView)) {
                if (GetTickCount64() < g_replay_until) b |= kView;
                else g_replay_until = 0;
            }
        }
    }
    *buttons = b;
    for (Action a : fired) {
        ++g_pad_triggers;
        trigger(a, "gamepad");
    }
}

std::uint64_t pad_polls() { return g_pad_polls.load(); }

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    const Settings& s = g_settings;
    if (a.empty() || a[0] == "status") {
        return std::format("ok keys recenter {} stereo {} ui nearer {} farther {}; pad {} hold view {}; ui step {:.2f} m ({:.2f}..{:.2f}); "
                           "triggers {} (gamepad {}), pad polls {}; stereo {}",
                           s.recenter_key.load(), s.stereo_key.load(), s.ui_nearer_key.load(), s.ui_farther_key.load(),
                           s.pad.load() ? 1 : 0, s.pad_hold_view.load() ? 1 : 0, s.ui_step.load(), s.ui_min.load(), s.ui_max.load(),
                           g_triggers.load(), g_pad_triggers.load(), g_pad_polls.load(), device::wanted() ? "on" : "off");
    }
    if (a[0] == "pad" && a.size() == 2) {
        // Test without a pad: one XInput button state through the filter.
        unsigned short b = static_cast<unsigned short>(std::strtoul(a[1].c_str(), nullptr, 16));
        filter_pad(0, &b);
        return std::format("ok buttons after filter {:#06x}", b);
    }
    const struct {
        const char* word;
        Action action;
    } kWords[] = {{"recenter", Action::Recenter}, {"stereo", Action::Stereo}, {"nearer", Action::UiNearer}, {"farther", Action::UiFarther}};
    for (const auto& w : kWords) {
        if (a[0] == w.word) {
            trigger(w.action, "dev");
            return std::string("ok ") + action_name(w.action);
        }
    }
    return "err usage: controls status | pad <hex buttons> | recenter | stereo | nearer | farther";
}

}  // namespace ff7vr::engine::controls
