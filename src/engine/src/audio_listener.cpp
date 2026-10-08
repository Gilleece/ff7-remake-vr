#include "audio_listener.h"

#include "movie_watch.h"
#include "stereo_device.h"
#include "uobj.h"

#include "ff7vr/core/log.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::engine::audio_listener {
namespace {

enum Fn : std::size_t { kSet, kClear };

std::atomic<bool> g_enabled{true};
uobj::Lookup* g_lookup = nullptr;
bool g_lookup_logged = false;
std::atomic<void*> g_override_pc{nullptr};  // controller whose listener we override (nullptr: none)
void* g_override_pawn = nullptr;
std::uint64_t g_last_pose_frame = ~0ull;
std::atomic<std::uint64_t> g_sets{0}, g_set_failures{0}, g_clears{0}, g_clear_failures{0}, g_stale{0};
std::mutex g_mutex;
ue::FVector g_last_loc{};
ue::FRotator g_last_rot{};

// The game's own camera actor in normal play (player.cpp: is_game_camera_actor).
bool is_game_camera_actor(void* o) {
    return uobj::class_named(o, "CameraActor") && uobj::object_name(o).rfind("EndCameraActor", 0) == 0;
}

bool ready() {
    if (!g_lookup) {
        if (!uobj::available()) return false;
        g_lookup = new uobj::Lookup({
            {"SetAudioListenerOverride", "PlayerController", "Function"},
            {"ClearAudioListenerOverride", "PlayerController", "Function"},
        });
    }
    if (g_lookup->ready()) return true;
    if (g_lookup->passes() >= 3) {
        if (!g_lookup_logged) {
            g_lookup_logged = true;
            log::warn("audio: PlayerController.SetAudioListenerOverride or ClearAudioListenerOverride not found; the listener stays at the game camera");
        }
        return false;
    }
    g_lookup->step();
    if (!g_lookup->ready()) return false;
    log::info("audio: PlayerController.SetAudioListenerOverride {} and ClearAudioListenerOverride {} found", g_lookup->get(kSet),
              g_lookup->get(kClear));
    return true;
}

void clear(const char* why) {
    if (!g_override_pc) return;
    void* pc = g_override_pc;
    g_override_pc = nullptr;
    g_override_pawn = nullptr;
    if (!uobj::alive(pc)) {
        log::info("audio: listener override dropped ({}; the controller is gone)", why);
        return;
    }
    alignas(16) std::uint8_t params[16]{};
    if (uobj::call(pc, g_lookup->get(kClear), params)) {
        ++g_clears;
        log::info("audio: listener back at the game camera ({})", why);
    } else {
        ++g_clear_failures;
        log::warn("audio: ClearAudioListenerOverride failed ({})", why);
    }
}

}  // namespace

void player_frame(const Frame& f) {
    // The cutscene screen: an authored camera is a view target that is neither the pawn nor
    // the game's EndCameraActor (no pawn needed: a cutscene may leave the controller without one).
    const bool authored = f.pc && f.view_target && f.view_target != f.pawn && !is_game_camera_actor(f.view_target);
    movie::note_camera(f.view_target, authored, f.combat);

    if (!ready()) return;
    const bool want = g_enabled.load() && f.stereo && f.first_person && f.pc;
    if (g_override_pc && (!want || f.pc != g_override_pc || f.pawn != g_override_pawn))
        clear(!g_enabled.load() ? "audio_listener off" : !f.stereo ? "stereo off" : !f.first_person ? "first person stopped" : "controller or pawn changed");
    if (!want) return;
    ue::FVector loc{};
    ue::FRotator rot{};
    std::uint64_t frame = 0;
    if (!device::last_listener_pose(loc, rot, frame)) return;
    if (frame == g_last_pose_frame) ++g_stale;  // no new stereo frame since the last call: the same pose again
    g_last_pose_frame = frame;
    // SetAudioListenerOverride(USceneComponent* AttachToComponent, FVector Location, FRotator Rotation):
    // pointer at 0, location at 8, rotation at 20 (UE4.18 float vectors).
    alignas(16) std::uint8_t params[48]{};
    std::memcpy(params + 8, &loc, sizeof(loc));
    std::memcpy(params + 20, &rot, sizeof(rot));
    if (uobj::call(f.pc, g_lookup->get(kSet), params)) {
        if (!g_override_pc) log::info("audio: listener at the head (first person)");
        g_override_pc = f.pc;
        g_override_pawn = f.pawn;
        ++g_sets;
        std::lock_guard lock(g_mutex);
        g_last_loc = loc;
        g_last_rot = rot;
    } else if (g_set_failures.fetch_add(1) < 5) {
        log::warn("audio: SetAudioListenerOverride failed");
    }
}

void set_enabled(bool on) {
    if (g_enabled.exchange(on) != on) log::info("audio: listener at the head {}", on ? "on" : "off");
}

std::string status() {
    std::lock_guard lock(g_mutex);
    return std::format("audio listener {}: functions {} override {} sets {} failed {} clears {} failed {} repeated poses {} last ({:.1f} {:.1f} {:.1f}) yaw {:.1f} pitch {:.1f}",
                       g_enabled.load() ? "on" : "off", g_lookup && g_lookup->ready() ? "found" : "not found", g_override_pc ? 1 : 0,
                       g_sets.load(), g_set_failures.load(), g_clears.load(), g_clear_failures.load(), g_stale.load(), g_last_loc.X,
                       g_last_loc.Y, g_last_loc.Z, g_last_rot.Yaw, g_last_rot.Pitch);
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::string w;
    in >> w;
    if (w == "on" || w == "1") set_enabled(true);
    else if (w == "off" || w == "0") set_enabled(false);
    return "ok " + status();
}

}  // namespace ff7vr::engine::audio_listener
