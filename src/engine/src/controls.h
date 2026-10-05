#pragma once
// In-game controls for the player: recenter, stereo on/off, UI panel nearer/farther, and the
// gamepad side of the first-person toggle.
//
// Keyboard: one virtual-key code per action ([controls] in ff7vr.ini), read with
// GetAsyncKeyState once per engine frame while the game window has the focus.
// Gamepad: combinations with View/Back held, through the loader's XInput filter. The
// buttons of a combination are removed from the state the game receives; with
// `pad_hold_view` View itself is held back while it is down and handed to the game as a
// short press when it is released without a combination.
//
// Recenter and the UI distance go through the render module's registered commands
// (`recenter`, `ui distance`) on a worker thread, so the game thread never waits for the
// render module's command lock. See docs/engine-module.md, "Player controls".

#include "ff7vr/core/config.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace ff7vr::engine::controls {

struct Settings {
    std::atomic<int> recenter_key{0x23};    // [controls] recenter_key: End
    std::atomic<int> stereo_key{0x2D};      // [controls] stereo_key: Insert
    std::atomic<int> ui_nearer_key{0x22};   // [controls] ui_nearer_key: Page Down
    std::atomic<int> ui_farther_key{0x21};  // [controls] ui_farther_key: Page Up
    std::atomic<bool> pad{true};            // [controls] pad: View/Back combinations
    std::atomic<bool> pad_hold_view{true};  // [controls] pad_hold_view
    std::atomic<float> ui_step{0.25f};      // [controls] ui_step (m)
    std::atomic<float> ui_min{0.75f};       // [controls] ui_min (m)
    std::atomic<float> ui_max{8.0f};        // [controls] ui_max (m)
};
Settings& settings();
void read_config(const Config& cfg);

// Game thread, once per engine frame: the keyboard keys.
void tick();

// Any thread (the game's XInput poll): View/Back combinations, removed from `buttons`.
void filter_pad(unsigned long user, unsigned short* buttons);
std::uint64_t pad_polls();

// `controls status | pad <hex buttons> | recenter | stereo | nearer | farther`
std::string command(const std::string& args);

}  // namespace ff7vr::engine::controls
