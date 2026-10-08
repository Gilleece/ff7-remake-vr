#pragma once
// Snap turn ([comfort] snap_turn): the right stick's X turns the player's view in fixed
// steps instead of turning the game camera smoothly.
//
// While on and 3D renders, the right stick's X is reported to the game as 0. A push beyond
// `snap_turn_deadzone` turns the view once by `snap_turn` degrees; the next step needs the
// stick back under half the deadzone (or, with `snap_turn_repeat_ms`, the stick held that
// long). The turn is the render module's snap yaw (the `snap` command): the views turn on
// top of the recenter, the game camera and the quad layers (HUD panel, virtual screen) do
// not. The game moves the character relative to its own camera, so the left stick is
// rotated by the turn in effect: forward on the stick moves the character where the player
// looks. Keyboard movement (WASD) is not rotated. A recenter clears the turn.
// See docs/engine-module.md, "Snap turn".

#include "ff7vr/core/config.h"

#include <string>

namespace ff7vr::engine::snap_turn {

void read_config(const Config& cfg);

// Game thread, once per engine frame: the optional snap keys.
void tick();

// Any thread (the game's XInput poll), user index 0..3: hides the right stick's X while
// snap turn is on, fires the steps, rotates the left stick by the turn in effect.
void filter_sticks(unsigned long user, short* lx, short* ly, short* rx, short* ry);

// `snapturn status | snap <deg>|off | deadzone <0..1> | repeat <ms> | log <n>|on |
// stick <lx> <ly> <rx> <ry> [user]`
std::string command(const std::string& args);

}  // namespace ff7vr::engine::snap_turn
