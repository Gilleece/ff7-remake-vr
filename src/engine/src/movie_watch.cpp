#include "movie_watch.h"

#include "stereo_device.h"

#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace ff7vr::engine::movie {
namespace {

// Layouts (docs/re/engine.md section 2).
constexpr std::size_t kObjects = 0x10;        // FUObjectArray: FUObjectItem* Objects
constexpr std::size_t kNumElements = 0x1c;    // int32
constexpr std::size_t kItemSize = 0x18;       // FUObjectItem {UObject* Object; ...}
constexpr std::size_t kClass = 0x10;          // UObject::ClassPrivate
constexpr std::size_t kName = 0x18;           // UObject::NamePrivate (FName: ComparisonIndex, Number)
constexpr std::size_t kOuter = 0x20;          // UObject::OuterPrivate
constexpr std::size_t kInternalIndex = 0x0c;  // UObject::InternalIndex
constexpr std::size_t kProcessEventSlot = 64;
constexpr int kScanPerTick = 16384;           // object array slots inspected per frame

Addresses g_a;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_include_menu{false};  // test: menu players count too
std::atomic<bool> g_playing{false};
std::atomic<bool> g_simulate{false};  // test: behave as if a movie played (the switch to the screen and back)
ULONGLONG g_started_ms = 0;           // when the current movie started
std::uint64_t g_movie_frames = 0;     // engine frames while it played
bool g_suppressing = false;  // we switched stereo off (for a movie or a cutscene)
bool g_want_off = false;     // last frame's request: a movie plays or a cutscene is on the screen
std::string g_off_reason;    // what switched stereo off
void* g_player_class = nullptr;
void* g_is_playing = nullptr;
int g_scan_pos = 0;
int g_class_scan_passes = 0;
std::uint64_t g_ticks = 0;
std::uint64_t g_calls = 0, g_call_failures = 0;
std::string g_current;  // path of the playing movie
std::mutex g_status_mutex;

// Cutscenes on the virtual screen ([stereo] cutscene_screen): an authored camera outside a
// battle for longer than the delay holds stereo off like a movie; the follow camera back
// for the hold time ends it. The camera state comes from the player module once per frame.
std::atomic<bool> g_cut_enabled{false};
std::atomic<int> g_cut_delay_ms{500};
std::atomic<int> g_cut_hold_ms{300};
std::atomic<bool> g_cam_authored{false};
std::atomic<bool> g_cam_combat{false};
std::atomic<void*> g_cam_target{nullptr};
std::atomic<bool> g_cut_simulate{false};  // test: behave as if the view target were an authored camera
std::atomic<int> g_cut_simulate_ms{0};     // test: for this long from the next frame (0 = until switched off)
ULONGLONG g_cut_simulate_until = 0;
bool g_cut_active = false;
ULONGLONG g_authored_since = 0;  // first frame of the current authored-camera run (0 = none)
ULONGLONG g_follow_since = 0;    // first frame of the follow camera while a cutscene is on the screen
ULONGLONG g_cut_started = 0;
std::uint64_t g_cut_count = 0, g_cut_frames = 0, g_authored_runs = 0, g_short_runs = 0;
std::string g_cut_target;  // name and class of the authored view target that started it

struct Player {
    void* obj;
    int index;
    bool menu;
    std::string path;
};
std::vector<Player> g_players;  // game thread
std::atomic<std::size_t> g_player_count{0};

template <class T>
bool rd(const void* p, T& out) {
    __try {
        std::memcpy(&out, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int num_objects() {
    std::int32_t n = 0;
    rd(g_a.GUObjectArray + kNumElements, n);
    return n;
}

void* object_at(int i) {
    std::uint8_t* items = nullptr;
    if (!rd(g_a.GUObjectArray + kObjects, items) || !items) return nullptr;
    void* o = nullptr;
    rd(items + static_cast<std::size_t>(i) * kItemSize, o);
    return o;
}

bool copy_guarded(void* dst, const void* src, std::size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FName entry text (ASCII names only; wide names are returned as "?").
std::string name_text(std::uint32_t id) {
    std::uint8_t* block = nullptr;
    if (!rd(g_a.FNamePool + 0x10 + static_cast<std::size_t>(id >> 16) * 8, block) || !block) return {};
    const std::uint8_t* e = block + 2 * static_cast<std::size_t>(id & 0xffff);
    std::uint16_t hdr = 0;
    if (!rd(e, hdr)) return {};
    const unsigned len = hdr >> 6;
    if (hdr & 1) return "?";
    if (len == 0 || len > 1024) return {};
    char buf[1024];
    if (!copy_guarded(buf, e + 2, len)) return {};
    return std::string(buf, len);
}

std::string object_name(void* o) {
    std::uint32_t id = 0;
    if (!o || !rd(static_cast<std::uint8_t*>(o) + kName, id)) return {};
    return name_text(id);
}

void* class_of(void* o) {
    void* c = nullptr;
    if (o) rd(static_cast<std::uint8_t*>(o) + kClass, c);
    return c;
}

void* outer_of(void* o) {
    void* c = nullptr;
    if (o) rd(static_cast<std::uint8_t*>(o) + kOuter, c);
    return c;
}

std::string path_of(void* o) {
    std::vector<std::string> parts;
    for (int depth = 0; o && depth < 16; ++depth, o = outer_of(o)) parts.push_back(object_name(o));
    std::string s;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) s += (s.empty() ? "" : ".") + *it;
    return s;
}

bool process_event(void* obj, void* fn, void* params) {
    __try {
        auto pe = reinterpret_cast<void(__fastcall*)(void*, void*, void*)>((*static_cast<void***>(obj))[kProcessEventSlot]);
        pe(obj, fn, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Finds UClass "MediaPlayer" and its UFunction "IsPlaying" (a slice of the array per call).
void find_class_slice(int n) {
    const int end = std::min(n, g_scan_pos + 4 * kScanPerTick);
    for (int i = g_scan_pos; i < end; ++i) {
        void* o = object_at(i);
        if (!o) continue;
        const std::string name = object_name(o);
        if (name == "MediaPlayer" && object_name(class_of(o)) == "Class") {
            g_player_class = o;
        } else if (name == "IsPlaying" && object_name(class_of(o)) == "Function" && object_name(outer_of(o)) == "MediaPlayer") {
            g_is_playing = o;
        }
    }
    g_scan_pos = end >= n ? 0 : end;
    if (g_scan_pos == 0) ++g_class_scan_passes;
    if (g_player_class && g_is_playing && outer_of(g_is_playing) != g_player_class) g_is_playing = nullptr;  // another class's IsPlaying
    if (g_player_class && g_is_playing)
        log::info("movie: MediaPlayer class {} and IsPlaying {} found ({} objects)", g_player_class, g_is_playing, n);
}

void scan_players_slice(int n) {
    const int end = std::min(n, g_scan_pos + kScanPerTick);
    for (int i = g_scan_pos; i < end; ++i) {
        void* o = object_at(i);
        if (!o || class_of(o) != g_player_class) continue;
        if (std::any_of(g_players.begin(), g_players.end(), [&](const Player& p) { return p.obj == o; })) continue;
        Player p{o, i, false, path_of(o)};
        if (p.path.find("Default__") != std::string::npos) continue;  // class default object
        p.menu = p.path.find("/Menu/") != std::string::npos;
        log::info("movie: media player {} ({})", p.path, p.menu ? "menu, ignored" : "movie");
        g_players.push_back(std::move(p));
    }
    g_scan_pos = end >= n ? 0 : end;
}

bool still_valid(const Player& p) {
    std::int32_t idx = -1;
    return object_at(p.index) == p.obj && rd(static_cast<std::uint8_t*>(p.obj) + kInternalIndex, idx) && idx == p.index &&
           class_of(p.obj) == g_player_class;
}

}  // namespace

void init(const Addresses& a, bool enabled) {
    g_a = a;
    g_enabled = enabled && a.GUObjectArray && a.FNamePool;
    if (enabled && !g_enabled) log::warn("movie: object array or name pool not found; movies are not detected");
}

void set_include_menu(bool on) { g_include_menu = on; }

void set_enabled(bool on) {
    g_enabled = on && g_a.GUObjectArray && g_a.FNamePool;
}

bool playing() { return g_playing.load(); }

// Game thread: the playing state changed or not; switches stereo off for a movie and back.
static void update(bool any, const std::string& current) {
    if (any && g_playing.load()) {
        ++g_movie_frames;
        // Every 10 s while a movie plays: its frame rate so far, so a slow movie shows in the
        // log while it runs (the timing block's copy engine line tells whether uploads are slow).
        static ULONGLONG window_ms = 0;
        static std::uint64_t window_frames = 0;
        const ULONGLONG now = GetTickCount64();
        if (g_movie_frames == 1) window_ms = now, window_frames = 0;
        ++window_frames;
        if (now - window_ms >= 10000) {
            const double fps = window_frames * 1000.0 / static_cast<double>(now - window_ms);
            log::info("movie: {:.1f} fps over the last {:.1f} s{}", fps, (now - window_ms) / 1000.0,
                      fps < 30.0 ? " (slow: the movies run at 60; see the copy engine in the timing block)" : "");
            window_ms = now;
            window_frames = 0;
        }
    }
    if (any != g_playing.load()) {
        g_playing = any;
        if (any) {
            g_started_ms = GetTickCount64();
            g_movie_frames = 0;
            log::info("movie: playing {}", current);
        } else {
            // The game's frame rate during the movie: far below the movie's own 60 fps means the
            // uploads of its frames were slow (docs/engine-module.md, "Movies").
            const double s = (GetTickCount64() - g_started_ms) / 1000.0;
            log::info("movie: stopped {} (after {:.1f} s, {} engine frames, {:.1f} fps)", g_current, s, g_movie_frames,
                      s > 0 ? g_movie_frames / s : 0.0);
        }
    }
    std::lock_guard lock(g_status_mutex);
    g_current = any ? current : std::string();
}

void set_simulate(bool on) { g_simulate = on; }

namespace {

std::string target_text(void* o) {
    if (g_cut_simulate.load()) return "(simulated: stereo cutscene simulate on)";
    if (!o) return "none";
    return object_name(o) + " (" + object_name(class_of(o)) + ")";
}

// Game thread, once per frame after the movie state: the cutscene screen's own state.
void cutscene_tick(ULONGLONG now) {
    if (g_playing.load()) return;  // the movie owns the switch; the cutscene state waits
    const bool authored = g_cam_authored.load();
    if (!g_cut_enabled.load() || g_cam_combat.load()) {
        if (g_cut_active) {
            g_cut_active = false;
            log::info("cutscene: ended after {:.1f} s ({})", (now - g_cut_started) / 1000.0,
                      g_cut_enabled.load() ? "battle" : "cutscene_screen off");
        }
        g_authored_since = 0;
        g_follow_since = 0;
        return;
    }
    if (authored) {
        g_follow_since = 0;
        if (!g_authored_since) {
            g_authored_since = now;
            ++g_authored_runs;
        }
        if (g_cut_active) {
            ++g_cut_frames;
        } else if (now - g_authored_since >= static_cast<ULONGLONG>(std::max(0, g_cut_delay_ms.load()))) {
            g_cut_active = true;
            g_cut_started = now;
            g_cut_frames = 0;
            ++g_cut_count;
            {
                std::lock_guard lock(g_status_mutex);
                g_cut_target = target_text(g_cam_target.load());
            }
            log::info("cutscene: authored camera {} for {} ms (delay {} ms): stereo held off, the scene plays on the screen",
                      g_cut_target, now - g_authored_since, g_cut_delay_ms.load());
        }
        return;
    }
    if (g_authored_since && !g_cut_active) {
        ++g_short_runs;
        log::info("cutscene: authored camera for {} ms only (delay {} ms): stays in 3D", now - g_authored_since,
                  g_cut_delay_ms.load());
    }
    g_authored_since = 0;
    if (!g_cut_active) return;
    if (!g_follow_since) g_follow_since = now;
    if (now - g_follow_since >= static_cast<ULONGLONG>(std::max(0, g_cut_hold_ms.load()))) {
        g_cut_active = false;
        log::info("cutscene: follow camera back for {} ms (hold {} ms) after {:.1f} s, {} frames: ended", now - g_follow_since,
                  g_cut_hold_ms.load(), (now - g_cut_started) / 1000.0, g_cut_frames);
        g_follow_since = 0;
    }
}

// The one owner of the stereo switch for movies and cutscenes: switches stereo off when
// either starts (only when it was on) and back on when neither holds it any more.
void arbitrate() {
    const bool want_off = g_playing.load() || g_cut_active;
    if (want_off && !g_want_off && device::wanted()) {
        g_suppressing = true;
        {
            std::lock_guard lock(g_status_mutex);
            g_off_reason = g_playing.load() ? "movie" : "cutscene";
        }
        log::info("movie/cutscene: stereo held off for a {}", g_off_reason);
        device::request_active(false);
    } else if (!want_off && g_suppressing) {
        g_suppressing = false;
        log::info("movie/cutscene: stereo released (was held off for a {})", g_off_reason);
        {
            std::lock_guard lock(g_status_mutex);
            g_off_reason.clear();
        }
        device::request_active(true);
    } else if (want_off && g_suppressing && g_playing.load() && g_off_reason != "movie") {
        log::info("movie/cutscene: a movie started during a cutscene; the movie holds stereo off now");
        std::lock_guard lock(g_status_mutex);
        g_off_reason = "movie";
    }
    g_want_off = want_off;
}

}  // namespace

void note_camera(void* view_target, bool authored, bool combat) {
    g_cam_target = view_target;
    if (const int ms = g_cut_simulate_ms.exchange(0); ms > 0) g_cut_simulate_until = GetTickCount64() + static_cast<ULONGLONG>(ms);
    if (g_cut_simulate_until && GetTickCount64() >= g_cut_simulate_until) {
        g_cut_simulate_until = 0;
        g_cut_simulate = false;
        log::info("cutscene: simulated authored camera off (timed)");
    }
    g_cam_authored = authored || g_cut_simulate.load();
    g_cam_combat = combat;
}

void set_cutscene(bool on) {
    if (g_cut_enabled.exchange(on) != on) log::info("cutscene: screen {}", on ? "on" : "off");
}

void set_cutscene_times(int delay_ms, int hold_ms) {
    if (delay_ms >= 0) g_cut_delay_ms = std::min(delay_ms, 60000);
    if (hold_ms >= 0) g_cut_hold_ms = std::min(hold_ms, 60000);
}

bool cutscene_active() { return g_cut_active; }

void set_cutscene_simulate(bool on, int ms) {
    if (on && ms > 0) g_cut_simulate_ms = ms;
    if (g_cut_simulate.exchange(on) != on)
        log::info("cutscene: simulated authored camera {}{}", on ? "on" : "off", on && ms > 0 ? std::format(" for {} ms", ms) : std::string());
}

// Game thread or the pipe thread: approximate values are fine for a status line.
std::string cutscene_status() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG since = g_authored_since;
    std::lock_guard lock(g_status_mutex);
    return std::format(
        "cutscene screen {} delay {} ms hold {} ms: active {} ({}{}) authored now {} ({} ms) combat {} target {} | cutscenes {} "
        "authored runs {} short runs {} | stereo held off {} ({})",
        g_cut_enabled.load() ? "on" : "off", g_cut_delay_ms.load(), g_cut_hold_ms.load(), g_cut_active ? 1 : 0,
        g_cut_active ? g_cut_target : std::string("-"),
        g_cut_active ? std::format(", {:.1f} s", (now - g_cut_started) / 1000.0) : std::string(), g_cam_authored.load() ? 1 : 0,
        since ? now - since : 0, g_cam_combat.load() ? 1 : 0, g_cam_authored.load() ? target_text(g_cam_target.load()) : "-",
        g_cut_count, g_authored_runs, g_short_runs, g_suppressing ? 1 : 0, g_off_reason.empty() ? std::string("-") : g_off_reason);
}

void tick() {
    // The cutscene state and the switch run every frame, after the movie state.
    struct After {
        ~After() {
            try {
                cutscene_tick(GetTickCount64());
                arbitrate();
            } catch (...) {
            }
        }
    } after;
    if (g_simulate.load()) {
        update(true, "(simulated: movie simulate on)");
        return;
    }
    if (!g_enabled.load()) {
        if (g_playing.load()) update(false, {});
        return;
    }
    ++g_ticks;
    try {
        const int n = num_objects();
        if (n <= 0) return;
        if (!g_player_class || !g_is_playing) {
            find_class_slice(n);
            return;
        }
        scan_players_slice(n);
        bool any = false;
        std::string current;
        for (auto it = g_players.begin(); it != g_players.end();) {
            if (!still_valid(*it)) {
                it = g_players.erase(it);
                continue;
            }
            if (!it->menu || g_include_menu.load()) {
                alignas(16) std::uint8_t params[16]{};
                ++g_calls;
                if (process_event(it->obj, g_is_playing, params)) {
                    if (params[0]) {
                        any = true;
                        current = it->path;
                    }
                } else {
                    ++g_call_failures;
                }
            }
            ++it;
        }
        g_player_count = g_players.size();
        update(any, current);
    } catch (...) {
    }
}

std::string status() {
    std::lock_guard lock(g_status_mutex);
    return std::format("movie watch {}{}: class {} IsPlaying {} class scans {} players {} playing {} ({}) calls {} failed {} stereo held off {}",
                       g_enabled.load() ? "on" : "off", g_include_menu.load() ? " (menu players count)" : "", g_player_class, g_is_playing, g_class_scan_passes, g_player_count.load(),
                       g_playing.load() ? 1 : 0, g_current, g_calls, g_call_failures, g_suppressing ? 1 : 0);
}

}  // namespace ff7vr::engine::movie
