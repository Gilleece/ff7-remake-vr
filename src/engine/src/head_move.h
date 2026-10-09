#pragma once
// Head-directed movement ([first_person] move, [camera] move = camera | head).
//
// The game moves the character relative to its own camera's yaw. With `head`, the left
// stick is rotated by the heading of the player's head relative to that yaw before the game
// reads it, so forward on the stick walks where the head points; with `camera` (default) it
// is not (apart from snap turn's own rotation, see snap_turn.h).
//
// The stereo device publishes, once per stereo frame (first eye), the heading of the head in
// the world (the eye base of the camera mode composed with the tracked head, recenter and
// snap turn included, pitch and roll of the head taken out) minus the game camera's yaw, and
// whether the frame was first person. The stick filter (the game's XInput poll) reads it.
// UE's yaw: degrees about +Z, positive = turning right seen from above (X forward, Y right).
// Keyboard movement (W/A/S/D) does not go through the stick filter and is not rotated.

#include "ff7vr/core/config.h"
#include "ff7vr/engine/stereo_abi.h"

#include <string>

namespace ff7vr::engine::head_move {

void read_config(const Config& cfg);

// Game thread, from CalculateStereoViewOffset (first eye): the game camera's rotation as the
// engine passed it, the rotation of the head in the world (from the same composition as the
// eye cameras) and whether the camera mode is first person (or blending into it).
void publish(const ue::FRotator& game_camera, const ue::FRotator& head_world, bool first_person);

// Any thread (the stick filter): the angle (degrees, positive = right) to rotate the left
// stick by for head-directed movement. False when the mode in effect is `camera`, or no
// fresh stereo frame published a heading (then the caller applies snap turn's own rotation).
bool stick_rotation(float& degrees);

// `controls move status | camera | head [first|third|both]`
std::string command(const std::string& args);
std::string status();

}  // namespace ff7vr::engine::head_move
