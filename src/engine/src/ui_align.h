#pragma once
// World-anchored UI elements (target markers, enemy names, prompts) lined up with the world
// on the UI layer's panel in stereo ([ui] align, docs/engine-module.md "UI marker alignment").
//
// The game places those elements on the game thread with ULocalPlayer::GetProjectionData
// for the full (non-stereo) pass: the game camera's location, rotation and field of view on
// the window's rectangle. The eyes do not see the world from there: they drop the camera's
// pitch, sit at the eye base of the camera mode (the head in first person) and look through
// a panel of their own size. With [ui] align = 1 a hook on GetProjectionData replaces the
// full pass's view with the panel's: its origin is the point the eye base maps the
// headset's origin to (the head when the panel follows the head), its axes are the panel's
// axes in the world and its projection spans exactly the panel's tangents as seen from that
// origin. Every element the game projects then lands on the panel where the line from the
// head to its object crosses the panel, at any panel size and distance.
//
// The panel's pose relative to the game camera is taken from the last stereo frame (the eye
// base is built later in the frame than the UI is laid out) and applied to the game camera
// of the current call, so camera motion is not a frame late.

#include "ff7vr/engine/stereo_abi.h"
#include "ff7vr/engine/stereo_host.h"

#include <cstdint>
#include <string>

namespace ff7vr::engine::ui_align {

// Called once at start-up: resolves GetProjectionData and installs the hook. Never throws.
void start(std::uintptr_t base, bool known);

// Game thread, from CalculateStereoViewOffset for the first eye of each stereo frame: the
// game camera (and its field of view, degrees) and the eye base the eyes were built from.
// `base_level` is the eye base rotation as the eyes use it (pitch and roll dropped when
// the pitch is decoupled).
void publish(const ue::FRotator& game_rot, const ue::FVector& game_loc, float game_fov_deg, const ue::FRotator& base_level,
             const ue::FVector& base_loc, const HostPose& head, double units_per_metre, bool positional);

// `uihook align`: state, counters and the last placement.
std::string status();

// `uihook callers on|off|reset|skip <offsets>|list`: the call chains of the full-pass
// projections (return addresses as offsets in the game's exe, innermost first), and the
// chains to leave out of the alignment ([ui] align_skip: hexadecimal exe offsets; a call
// whose chain contains one keeps the game camera's view).
std::string callers_command(const std::string& args);
void set_skip(const std::string& list);
// [ui] align_target_test: 1 = the game's field-target selection test sees the panel's view
// too; 0 (default) = it keeps the game camera's view (the prompts' placement is aligned
// either way).
void set_target_test_panel(bool panel);
// [ui] align_clamp: 1 (default) = a marker whose object lies outside the panel's view (but
// in front of the head) is placed on the panel's edge in the object's direction instead of
// off the panel; 0 = it is placed where the projection puts it (off the panel, not seen).
void set_clamp(bool on);
// `uihook clamp [0|1]`: the setting live, with counters and the last moved point.
std::string clamp_command(const std::string& args);

// The last published camera, eye base and head pose, with the alignment counters (for
// diagnostics; any thread).
struct Snapshot {
    bool valid = false;
    ue::FRotator game_rot{};
    ue::FVector game_loc{};
    ue::FRotator base_level{};
    ue::FVector base_loc{};
    HostPose head{};
    double upm = 100.0;
    bool positional = true;
    int align_mode = -1;  // [ui] align of the last aligned projection (-1: none yet)
    std::uint64_t full = 0, aligned = 0, skipped = 0;
};
Snapshot snapshot();

}  // namespace ff7vr::engine::ui_align
