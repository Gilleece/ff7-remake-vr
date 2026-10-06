#include "controls.h"

#include "player.h"
#include "stereo_device.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"
#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
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
#if FF7VR_ENGINE_WITH_RENDER
            // No XR session (the runtime asked the game to let go of the headset, or it was
            // never reached): reconnect now and keep stereo on, instead of switching it off
            // where nothing can be seen.
            if (!render::GetEyeSetup(nullptr)) {
                device::request_active(true);
                run_command("xr-restart", what + ": no XR session, reconnecting (stereo on)");
                break;
            }
#endif
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

// XInput button names for `fp_toggle_chord` (several spellings each).
const struct {
    const char* name;
    unsigned short bit;
} kButtonNames[] = {
    {"UP", 0x0001},    {"DPADUP", 0x0001},   {"DOWN", 0x0002}, {"DPADDOWN", 0x0002}, {"LEFT", 0x0004},
    {"DPADLEFT", 0x0004}, {"RIGHT", 0x0008}, {"DPADRIGHT", 0x0008}, {"START", 0x0010}, {"MENU", 0x0010},
    {"BACK", 0x0020},  {"VIEW", 0x0020},     {"L3", 0x0040},   {"LS", 0x0040},       {"R3", 0x0080},
    {"RS", 0x0080},    {"LB", 0x0100},       {"L1", 0x0100},   {"RB", 0x0200},       {"R1", 0x0200},
    {"A", 0x1000},     {"B", 0x2000},        {"X", 0x4000},    {"Y", 0x8000},
};

// "L3+R3" -> 0x00c0. Empty, "0", "off" or "none" -> 0 (off). False on an unknown name.
bool parse_chord(const std::string& text, unsigned short& out) {
    out = 0;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return true;
        std::string up;
        for (char c : word) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        word.clear();
        if (up == "0" || up == "OFF" || up == "NONE") return true;
        for (const auto& n : kButtonNames) {
            if (up == n.name) {
                out |= n.bit;
                return true;
            }
        }
        return false;
    };
    for (char c : text) {
        if (c == '+' || c == ' ' || c == '\t' || c == ',') {
            if (!flush()) return false;
        } else {
            word += c;
        }
    }
    return flush();
}

std::string chord_name(unsigned short m) {
    if (m == 0) return "off";
    std::string s;
    for (unsigned short bit = 1; bit != 0; bit = static_cast<unsigned short>(bit << 1)) {
        if (!(m & bit)) continue;
        const char* name = "?";
        for (const auto& n : kButtonNames) {
            if (n.bit == bit) {
                name = n.name;  // the first spelling in the table is the display name...
                break;
            }
        }
        if (bit == 0x0040) name = "L3";  // ...except for these, which read better in their short form
        if (bit == 0x0080) name = "R3";
        if (bit == 0x0100) name = "LB";
        if (bit == 0x0200) name = "RB";
        if (!s.empty()) s += '+';
        s += name;
    }
    return s;
}

// The chord (guarded by g_pad_mutex). Idle -> Pending (some of its buttons down, held back
// from the game) -> Fired (all down within the window: toggled once, hidden until all are
// released) or Passed (window over: the game gets the buttons as they are) or back to Idle
// with the held-back press replayed (released within the window).
enum class Chord { Idle, Pending, Fired, Passed };
Chord g_chord = Chord::Idle;
std::uint64_t g_chord_t0 = 0;
unsigned short g_chord_seen = 0;    // chord buttons seen down while pending
unsigned short g_chord_replay = 0;  // buttons replayed to the game after a short press
std::uint64_t g_chord_replay_until = 0;
std::atomic<std::uint64_t> g_chord_fired{0}, g_chord_passed{0}, g_chord_replayed{0};
std::atomic<unsigned short> g_last_in{0}, g_last_out{0};
std::atomic<bool> g_pad_log{false};

// Applies the chord to `b` (the buttons the game will get). Returns true when it fired.
bool chord_filter(unsigned short& b, unsigned short chord, std::uint64_t now) {
    const unsigned short d = b & chord;
    const unsigned window = static_cast<unsigned>(std::clamp(g_settings.fp_chord_ms.load(), 0, 2000));
    bool fired = false;
    switch (g_chord) {
        case Chord::Idle:
            if (d == 0) break;
            g_chord_replay_until = 0;  // a new press ends a replay
            if (d == chord) {
                g_chord = Chord::Fired;
                fired = true;
            } else {
                g_chord = Chord::Pending;
                g_chord_t0 = now;
                g_chord_seen = d;
            }
            break;
        case Chord::Pending:
            if (d == chord) {
                g_chord = Chord::Fired;
                fired = true;
            } else if (d == 0) {
                // Released before the chord formed: the game gets the press now, short.
                g_chord = Chord::Idle;
                g_chord_replay = g_chord_seen;
                g_chord_replay_until = now + kViewReplayMs;
                ++g_chord_replayed;
            } else if (now - g_chord_t0 >= window) {
                g_chord = Chord::Passed;
                ++g_chord_passed;
            } else {
                g_chord_seen |= d;
            }
            break;
        case Chord::Fired:
        case Chord::Passed:
            if (d == 0) g_chord = Chord::Idle;
            break;
    }
    if (g_chord == Chord::Pending || g_chord == Chord::Fired) b = static_cast<unsigned short>(b & ~chord);
    if (g_chord_replay_until) {
        if (now < g_chord_replay_until) b |= g_chord_replay;
        else g_chord_replay_until = 0;
    }
    return fired;
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
    const std::string chord_text = cfg.get_string("controls", "fp_toggle_chord", chord_name(s.fp_chord.load()));
    unsigned short chord = 0;
    if (!parse_chord(chord_text, chord)) {
        log::warn("controls: fp_toggle_chord '{}' has an unknown button name; the chord is off", chord_text);
        chord = 0;
    } else if (chord != 0 && (chord & (chord - 1)) == 0) {
        log::warn("controls: fp_toggle_chord '{}' is a single button; it needs two or more; the chord is off", chord_text);
        chord = 0;
    }
    s.fp_chord = chord;
    s.fp_chord_ms = static_cast<int>(std::clamp<long long>(cfg.get_int("controls", "fp_toggle_chord_ms", s.fp_chord_ms.load()), 0, 2000));
    log::info("controls: first/third person chord {} (within {} ms)", chord_name(s.fp_chord.load()), s.fp_chord_ms.load());
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
    const unsigned short chord = player::settings().fp_available.load() ? g_settings.fp_chord.load() : 0;
    if (combos == 0 && chord == 0) return;
    ++g_pad_polls;
    std::vector<Action> fired;
    bool chord_fired = false;
    const unsigned short in = *buttons;
    unsigned short b = in;
    {
        std::lock_guard lock(g_pad_mutex);
        // The chord first, unless a View/Back combination is in progress (View down or still
        // latched: those use the stick clicks themselves). A chord already under way finishes.
        if (chord && (g_chord != Chord::Idle || (!(b & kView) && !g_latched))) {
            if (chord_filter(b, chord, GetTickCount64())) {
                ++g_chord_fired;
                chord_fired = true;
            }
        }
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
    const unsigned short last_in = g_last_in.exchange(in), last_out = g_last_out.exchange(b);
    if (g_pad_log.load() && (last_in != in || last_out != b)) log::info("controls: pad in {:#06x} -> game {:#06x}", in, b);
    if (chord_fired) {
        ++g_pad_triggers;
        trigger(Action::FirstPerson, "gamepad chord");
    }
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
                           g_triggers.load(), g_pad_triggers.load(), g_pad_polls.load(), device::wanted() ? "on" : "off") +
               std::format("; fp chord {} within {} ms: fired {}, passed late {}, replayed short {}; last pad in {:#06x} -> game {:#06x}; pad log {}",
                           chord_name(s.fp_chord.load()), s.fp_chord_ms.load(), g_chord_fired.load(), g_chord_passed.load(),
                           g_chord_replayed.load(), g_last_in.load(), g_last_out.load(), g_pad_log.load() ? 1 : 0);
    }
    if (a[0] == "padlog" && a.size() == 2) {
        // Logs every change of the pad state the game asks for and of what it receives.
        g_pad_log = a[1] == "1";
        return std::format("ok pad log {}", g_pad_log.load() ? 1 : 0);
    }
    if (a[0] == "chord" && a.size() >= 2) {
        unsigned short chord = 0;
        if (!parse_chord(a[1], chord)) return "err unknown button name in " + a[1];
        g_settings.fp_chord = chord;
        if (a.size() >= 3) g_settings.fp_chord_ms = std::clamp(std::atoi(a[2].c_str()), 0, 2000);
        return std::format("ok fp chord {} within {} ms", chord_name(chord), g_settings.fp_chord_ms.load());
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
    return "err usage: controls status | pad <hex buttons> | padlog 0|1 | chord <L3+R3|off> [ms] | recenter | stereo | nearer | farther";
}

}  // namespace ff7vr::engine::controls
