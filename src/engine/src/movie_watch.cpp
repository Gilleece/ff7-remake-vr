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
bool g_suppressing = false;  // we switched stereo off for a movie
void* g_player_class = nullptr;
void* g_is_playing = nullptr;
int g_scan_pos = 0;
int g_class_scan_passes = 0;
std::uint64_t g_ticks = 0;
std::uint64_t g_calls = 0, g_call_failures = 0;
std::string g_current;  // path of the playing movie
std::mutex g_status_mutex;

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
    if (any && g_playing.load()) ++g_movie_frames;
    if (any != g_playing.load()) {
        g_playing = any;
        if (any) {
            g_started_ms = GetTickCount64();
            g_movie_frames = 0;
            log::info("movie: playing {}", current);
        } else {
            // The game's frame rate during the movie: far below the movie's own 30 fps means the
            // uploads of its frames were slow (docs/engine-module.md, "Movies").
            const double s = (GetTickCount64() - g_started_ms) / 1000.0;
            log::info("movie: stopped {} (after {:.1f} s, {} engine frames, {:.1f} fps)", g_current, s, g_movie_frames,
                      s > 0 ? g_movie_frames / s : 0.0);
        }
        if (any && device::wanted()) {
            g_suppressing = true;
            device::request_active(false);
        } else if (!any && g_suppressing) {
            g_suppressing = false;
            device::request_active(true);
        }
    }
    std::lock_guard lock(g_status_mutex);
    g_current = any ? current : std::string();
}

void set_simulate(bool on) { g_simulate = on; }

void tick() {
    if (g_simulate.load()) {
        update(true, "(simulated: movie simulate on)");
        return;
    }
    if (!g_enabled.load()) {
        if (g_playing.load() || g_suppressing) update(false, {});
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
