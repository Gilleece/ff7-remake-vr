#include "controls.h"

#include "head_move.h"
#include "player.h"
#include "sce_pad.h"
#include "stereo_device.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"
#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <windows.h>

#include <algorithm>
#include <chrono>
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

std::atomic<std::uint64_t> g_triggers{0}, g_pad_triggers{0};

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

// Gamepad state, one per XInput user index (the game's poll thread; guarded by g_pad_mutex).
// The filter acts on every user index: a pad does not have to sit at index 0.
constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kStart = 0x0010, kView = 0x0020, kLeftThumb = 0x0040,
                         kRightThumb = 0x0080;
constexpr std::int64_t kViewReplayUs = 120000;
// Pad slots: XInput user indexes 0..3, then PlayStation pads (read by the game through
// libScePad, sce_pad.h) 0..3 as slots 4..7.
constexpr unsigned long kXInputPads = 4, kPads = 8;
std::mutex g_pad_mutex;

enum class Chord { Idle, Pending, Fired, Passed };

struct Pad {
    unsigned short prev = 0;
    bool latched = false;    // a combination was used since View went down: View and the combination buttons are hidden
    bool view_held = false;  // View is down and held back from the game
    std::int64_t replay_until = 0;
    // The chord: see chord_filter.
    Chord chord = Chord::Idle;
    std::int64_t chord_t0 = 0;
    unsigned short chord_seen = 0;    // chord buttons seen down while pending
    unsigned short chord_replay = 0;  // buttons replayed to the game after a short press
    std::int64_t chord_replay_until = 0;
    // Diagnostics.
    std::int64_t last_poll = 0, last_change = 0;
    unsigned short last_in = 0, last_out = 0;
};
Pad g_pads[kPads];

// Microseconds on a steady clock (the chord window is 150 ms; GetTickCount64 steps in 15.6 ms).
std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

std::string pad_name(unsigned long user) {
    return user < kXInputPads ? std::format("pad {}", user) : std::format("PlayStation pad {}", user - kXInputPads);
}

// Counters for `controls status` and the timing block (totals; the timing line shows the
// change since the previous line).
struct Counters {
    std::atomic<std::uint64_t> polls{0}, changes{0}, chord_pending{0}, chord_fired{0}, chord_passed{0}, chord_replayed{0},
        chord_skipped_view{0}, view_combos{0};
};
Counters g_count;
std::atomic<unsigned short> g_last_in{0}, g_last_out{0};
std::atomic<unsigned long> g_last_user{0};
std::atomic<int> g_pad_log{0};  // state changes still to be logged (-1 = all)

const char* chord_state_name(Chord c) {
    switch (c) {
        case Chord::Idle: return "idle";
        case Chord::Pending: return "pending";
        case Chord::Fired: return "fired";
        case Chord::Passed: return "passed";
    }
    return "?";
}

// The chord. Idle -> Pending (some of its buttons down, held back from the game) -> Fired
// (all down within the window of the first: toggled once, hidden until all are released) or
// Passed (window over: the game gets the buttons as they are until all are released) or back
// to Idle with the held-back press replayed short (released within the window).
// Applies the chord to `b` (the buttons the game will get). Returns true when it fired.
bool chord_filter(Pad& p, unsigned short& b, unsigned short chord, std::int64_t now) {
    const unsigned short d = b & chord;
    const std::int64_t window = static_cast<std::int64_t>(std::clamp(g_settings.fp_chord_ms.load(), 0, 2000)) * 1000;
    bool fired = false;
    switch (p.chord) {
        case Chord::Idle:
            if (d == 0) break;
            p.chord_replay_until = 0;  // a new press ends a replay
            if (d == chord) {
                p.chord = Chord::Fired;  // all in the same poll
                fired = true;
            } else {
                p.chord = Chord::Pending;
                p.chord_t0 = now;
                p.chord_seen = d;
                ++g_count.chord_pending;
            }
            break;
        case Chord::Pending:
            if (d == chord) {
                // Complete. Also when this poll is past the window: every earlier poll was
                // inside it (the first one past it ends Pending), so the game's poll gap made
                // the second press late, not the player.
                p.chord = Chord::Fired;
                fired = true;
            } else if (d == 0) {
                // Released before the chord formed: the game gets the press now, short.
                p.chord = Chord::Idle;
                p.chord_replay = p.chord_seen;
                p.chord_replay_until = now + kViewReplayUs;
                ++g_count.chord_replayed;
            } else if (now - p.chord_t0 > window) {
                // The window is over without the chord: the game gets the buttons as they are.
                p.chord = Chord::Passed;
                ++g_count.chord_passed;
            } else {
                p.chord_seen |= d;
            }
            break;
        case Chord::Fired:
        case Chord::Passed:
            if (d == 0) p.chord = Chord::Idle;
            break;
    }
    if (p.chord == Chord::Pending || p.chord == Chord::Fired) b = static_cast<unsigned short>(b & ~chord);
    if (p.chord_replay_until) {
        if (now < p.chord_replay_until) b |= p.chord_replay;
        else p.chord_replay_until = 0;
    }
    return fired;
}

bool parse_source(const std::string& text, int& out) {
    std::string t;
    for (char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "auto" || t == "both") out = kSourceAuto;
    else if (t == "xinput") out = kSourceXInput;
    else if (t == "playstation" || t == "ps" || t == "dinput" || t == "sony") out = kSourcePlayStation;
    else return false;
    return true;
}

const char* source_name(int v) {
    return v == kSourceXInput ? "xinput" : v == kSourcePlayStation ? "playstation" : "auto (xinput and playstation)";
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
    g_pad_log = static_cast<int>(std::clamp<long long>(cfg.get_int("controls", "pad_log", 0), -1, 100000));
    const std::string source = cfg.get_string("controls", "pad_source", "auto");
    int src = kSourceAuto;
    if (!parse_source(source, src)) log::warn("controls: pad_source '{}' is not auto, xinput or playstation; using auto", source);
    s.pad_source = src;
    head_move::read_config(cfg);
    log::info("controls: first/third person chord {} (within {} ms){}", chord_name(s.fp_chord.load()), s.fp_chord_ms.load(),
              g_pad_log.load() ? std::format("; logging the first {} pad state changes", g_pad_log.load()) : std::string());
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

namespace {

// PlayStation buttons (ScePadData::buttons, libScePad's SCE_PAD_BUTTON_* bits) and the
// XInput button the filter treats them as. The game maps them the same way (WinDualShock:
// Options = SpecialRight like Menu/Start, the touch pad click = SpecialLeft like View/Back;
// Share/Create is not reported). L2/R2 (0x100/0x200) are not mapped and pass unchanged.
constexpr struct {
    std::uint32_t sce;
    unsigned short xi;
} kSceMap[] = {
    {0x00000002, 0x0040},  // L3
    {0x00000004, 0x0080},  // R3
    {0x00000008, 0x0010},  // Options -> Start/Menu
    {0x00000010, 0x0001},  // D-pad up
    {0x00000020, 0x0008},  // D-pad right
    {0x00000040, 0x0002},  // D-pad down
    {0x00000080, 0x0004},  // D-pad left
    {0x00000400, 0x0100},  // L1 -> LB
    {0x00000800, 0x0200},  // R1 -> RB
    {0x00001000, 0x8000},  // triangle -> Y
    {0x00002000, 0x2000},  // circle -> B
    {0x00004000, 0x1000},  // cross -> A
    {0x00008000, 0x4000},  // square -> X
    {0x00100000, 0x0020},  // touch pad click -> View/Back
};

void filter_buttons(unsigned long user, unsigned short* buttons);

}  // namespace

void filter_pad(unsigned long user, unsigned short* buttons) {
    if (user >= kXInputPads || g_settings.pad_source.load(std::memory_order_relaxed) == kSourcePlayStation) return;
    filter_buttons(user, buttons);
}

unsigned short sce_to_xinput(std::uint32_t sce) {
    unsigned short xi = 0;
    for (const auto& m : kSceMap)
        if (sce & m.sce) xi = static_cast<unsigned short>(xi | m.xi);
    return xi;
}

void filter_sce(unsigned long index, std::uint32_t* buttons) {
    if (index >= kPads - kXInputPads || !buttons || g_settings.pad_source.load(std::memory_order_relaxed) == kSourceXInput) return;
    const unsigned short in = sce_to_xinput(*buttons);
    unsigned short out = in;
    filter_buttons(kXInputPads + index, &out);
    if (out == in) return;
    std::uint32_t b = *buttons;
    for (const auto& m : kSceMap) {
        if ((in & m.xi) && !(out & m.xi)) b &= ~m.sce;  // withheld from the game
        if (!(in & m.xi) && (out & m.xi)) b |= m.sce;   // a held-back press handed over
    }
    *buttons = b;
}

namespace {

void filter_buttons(unsigned long user, unsigned short* buttons) {
    if (user >= kPads || !buttons) return;
    const unsigned short combos = combo_buttons();
    const unsigned short chord = player::settings().fp_available.load() ? g_settings.fp_chord.load() : 0;
    if (combos == 0 && chord == 0) return;
    ++g_count.polls;
    std::vector<Action> fired;
    bool chord_fired = false;
    const unsigned short in = *buttons;
    unsigned short b = in;
    const std::int64_t now = now_us();
    std::string log_line;
    {
        std::lock_guard lock(g_pad_mutex);
        Pad& p = g_pads[user];
        const Chord chord_before = p.chord;
        // The chord first, unless a View/Back combination is in progress (View down or still
        // latched: those use the stick clicks themselves). A chord under way finishes.
        if (chord && (p.chord != Chord::Idle || (!(b & kView) && !p.latched))) {
            if (chord_filter(p, b, chord, now)) {
                ++g_count.chord_fired;
                chord_fired = true;
            }
        } else if (chord && (b & chord)) {
            ++g_count.chord_skipped_view;
        }
        const unsigned short pressed = static_cast<unsigned short>(b & ~p.prev);
        p.prev = b;
        if (b & kView) {
            const unsigned short hits = pressed & combos;
            if (hits & kRightThumb) fired.push_back(Action::FirstPerson);
            if (hits & kLeftThumb) fired.push_back(Action::Recenter);
            if (hits & kStart) fired.push_back(Action::Stereo);
            if (hits & kUp) fired.push_back(Action::UiFarther);
            if (hits & kDown) fired.push_back(Action::UiNearer);
            if (hits) {
                p.latched = true;
                p.view_held = false;
                p.replay_until = 0;
            }
        }
        if (p.latched) {
            if ((b & (kView | combos)) == 0) p.latched = false;  // everything released
            b = static_cast<unsigned short>(b & ~(kView | combos));
        } else if (g_settings.pad_hold_view.load()) {
            // View alone: held back until released, then handed to the game as a short press.
            if (b & kView) {
                b = static_cast<unsigned short>(b & ~kView);
                p.view_held = true;
            } else if (p.view_held) {
                p.view_held = false;
                p.replay_until = now + kViewReplayUs;
            }
            if (p.replay_until && !(b & kView)) {
                if (now < p.replay_until) b |= kView;
                else p.replay_until = 0;
            }
        }
        g_count.view_combos += fired.size();
        // Diagnostics: a state change in or out ([controls] pad_log, `controls padlog`).
        const bool changed = p.last_in != in || p.last_out != b, chord_moved = p.chord != chord_before;
        if (changed) ++g_count.changes;
        if (changed || chord_moved) {
            int left = g_pad_log.load();
            while (left != 0 && !g_pad_log.compare_exchange_weak(left, left > 0 ? left - 1 : left)) {
            }
            if (left != 0)
                log_line = std::format("controls: {} in {:#06x} -> game {:#06x} (poll +{:.1f} ms, {}{}{})", pad_name(user), in, b,
                                       p.last_poll ? double(now - p.last_poll) / 1000.0 : 0.0,
                                       p.last_change ? std::format("previous change {:.1f} ms before", double(now - p.last_change) / 1000.0)
                                                     : std::string("first change"),
                                       chord_moved ? std::format("; chord {} -> {}", chord_state_name(chord_before), chord_state_name(p.chord)) : "",
                                       chord_fired ? ", toggles" : "");
        }
        if (changed) p.last_change = now;
        p.last_in = in;
        p.last_out = b;
        p.last_poll = now;
    }
    *buttons = b;
    g_last_in = in;
    g_last_out = b;
    g_last_user = user;
    if (!log_line.empty()) log::info("{}", log_line);
    if (chord_fired) {
        ++g_pad_triggers;
        trigger(Action::FirstPerson, "gamepad chord");
    }
    for (Action a : fired) {
        ++g_pad_triggers;
        trigger(a, "gamepad");
    }
}

}  // namespace

std::uint64_t pad_polls() { return g_count.polls.load(); }

namespace {

// One line of the render module's timing block: the counters since the previous line.
std::string timing_line() {
    static std::mutex m;
    static std::uint64_t last[8] = {};
    std::lock_guard lk(m);
    const std::uint64_t now[8] = {g_count.polls.load(),        g_count.changes.load(),      g_count.chord_pending.load(),
                                  g_count.chord_fired.load(),  g_count.chord_passed.load(), g_count.chord_replayed.load(),
                                  g_count.chord_skipped_view.load(), g_count.view_combos.load()};
    std::uint64_t d[8];
    for (int i = 0; i < 8; ++i) d[i] = now[i] - last[i], last[i] = now[i];
    std::string states;
    {
        std::lock_guard lock(g_pad_mutex);
        for (unsigned long u = 0; u < kPads; ++u)
            if (g_pads[u].last_poll) states += std::format("{}{} {}", states.empty() ? "" : ", ", pad_name(u), chord_state_name(g_pads[u].chord));
    }
    const Settings& s = g_settings;
    return std::format("ok pad filter: {} polls, {} state changes; chord {} ({} ms){}: started {}, fired {}, passed late {}, replayed short {}, "
                       "ignored while View held {}; View combinations {}; now {}; last {} in {:#06x} -> game {:#06x}",
                       d[0], d[1], chord_name(s.fp_chord.load()), s.fp_chord_ms.load(), player::settings().fp_available.load() ? "" : " (first person off)",
                       d[2], d[3], d[4], d[5], d[6], d[7], states.empty() ? "no pad polled" : states, pad_name(g_last_user.load()), g_last_in.load(),
                       g_last_out.load()) + "; " + sce_pad::timing();
}

}  // namespace

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    const Settings& s = g_settings;
    if (a.empty() || a[0] == "status") {
        return std::format("ok keys recenter {} stereo {} ui nearer {} farther {}; pad {} hold view {}; ui step {:.2f} m ({:.2f}..{:.2f}); "
                           "triggers {} (gamepad {}), pad polls {}; pad source {}; stereo {}",
                           s.recenter_key.load(), s.stereo_key.load(), s.ui_nearer_key.load(), s.ui_farther_key.load(),
                           s.pad.load() ? 1 : 0, s.pad_hold_view.load() ? 1 : 0, s.ui_step.load(), s.ui_min.load(), s.ui_max.load(),
                           g_triggers.load(), g_pad_triggers.load(), g_count.polls.load(), source_name(s.pad_source.load()), device::wanted() ? "on" : "off") +
               std::format("; fp chord {} within {} ms: started {}, fired {}, passed late {}, replayed short {}; last {} in {:#06x} -> game {:#06x}; "
                           "pad log {}; {}; {}",
                           chord_name(s.fp_chord.load()), s.fp_chord_ms.load(), g_count.chord_pending.load(), g_count.chord_fired.load(),
                           g_count.chord_passed.load(), g_count.chord_replayed.load(), pad_name(g_last_user.load()), g_last_in.load(), g_last_out.load(),
                           g_pad_log.load(), sce_pad::status(), head_move::status());
    }
    if (a[0] == "timing") return timing_line();
    if (a[0] == "padlog" && a.size() == 2) {
        // Logs the next N changes of the pad state the game asks for and of what it receives
        // ("on" = all, 0 = off).
        g_pad_log = a[1] == "on" ? -1 : std::max(0, std::atoi(a[1].c_str()));
        return std::format("ok pad log {}", g_pad_log.load());
    }
    if (a[0] == "chord" && a.size() >= 2) {
        unsigned short chord = 0;
        if (!parse_chord(a[1], chord)) return "err unknown button name in " + a[1];
        g_settings.fp_chord = chord;
        if (a.size() >= 3) g_settings.fp_chord_ms = std::clamp(std::atoi(a[2].c_str()), 0, 2000);
        return std::format("ok fp chord {} within {} ms", chord_name(chord), g_settings.fp_chord_ms.load());
    }
    if (a[0] == "pad" && (a.size() == 2 || a.size() == 3)) {
        // Test without a pad: one XInput button state through the filter (user index 0, or the
        // one given second).
        unsigned short b = static_cast<unsigned short>(std::strtoul(a[1].c_str(), nullptr, 16));
        const unsigned long user = a.size() == 3 ? std::strtoul(a[2].c_str(), nullptr, 10) : 0;
        if (user >= kXInputPads) return "err user index 0..3";
        filter_buttons(user, &b);
        return std::format("ok buttons after filter {:#06x}", b);
    }
    if (a[0] == "psfilter" && (a.size() == 2 || a.size() == 3)) {
        // Test without a pad: one PlayStation button word (ScePadData::buttons) through the
        // filter (pad 0, or the one given second).
        std::uint32_t b = static_cast<std::uint32_t>(std::strtoul(a[1].c_str(), nullptr, 16));
        const unsigned long index = a.size() == 3 ? std::strtoul(a[2].c_str(), nullptr, 10) : 0;
        if (index >= kPads - kXInputPads) return "err pad index 0..3";
        const std::uint32_t before = b;
        filter_sce(index, &b);
        return std::format("ok PlayStation buttons {:#010x} -> game {:#010x}", before, b);
    }
    if (a[0] == "ps") return sce_pad::command(args.substr(args.find("ps") + 2));
    if (a[0] == "move") return head_move::command(args.substr(args.find("move") + 4));
    if (a[0] == "source" && a.size() == 2) {
        int v = 0;
        if (!parse_source(a[1], v)) return "err usage: controls source auto|xinput|playstation";
        g_settings.pad_source = v;
        return std::string("ok pad source ") + source_name(v);
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
    return "err usage: controls status | timing | pad <hex buttons> [user] | psfilter <hex> [pad] | ps <hex>|off|status | source auto|xinput|playstation | "
           "move status|camera|head [first|third] | padlog <n>|on | chord <L3+R3|off> [ms] | recenter | stereo | nearer | farther";
}

}  // namespace ff7vr::engine::controls
