#pragma once
// In-game controls for the player: recenter, stereo on/off, UI panel nearer/farther, and the
// gamepad side of the first-person toggle.
//
// Keyboard: one virtual-key code per action ([controls] in ff7vr.ini), read with
// GetAsyncKeyState once per engine frame while the game window has the focus.
// Gamepad: combinations with View/Back held, through the loader's XInput filter, and on a
// PlayStation pad the game reads through libScePad (sce_pad.h) with the touch pad click as
// View/Back. The
// buttons of a combination are removed from the state the game receives; with
// `pad_hold_view` View itself is held back while it is down and handed to the game as a
// short press when it is released without a combination.
// Gamepad chord (`fp_toggle_chord`, default both stick clicks): the buttons pressed together
// within `fp_toggle_chord_ms` switch first/third person once and are hidden from the game
// until all of them are released. The first button of a possible chord is held back for at
// most that window: if the chord does not form, the game gets the press (late by the
// window at most, or replayed as a short press when it was released within it).
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
    // [controls] fp_toggle_chord: XInput buttons pressed together that switch first/third
    // person (default L3+R3, 0 = off); fp_toggle_chord_ms: how close together they must go down.
    std::atomic<unsigned short> fp_chord{0x0040 | 0x0080};
    std::atomic<int> fp_chord_ms{150};
    // [controls] pad_source: which pads the combinations and the chord read. auto = both,
    // xinput = XInput pads only, playstation = PlayStation pads read by the game itself only.
    std::atomic<int> pad_source{0};
};
constexpr int kSourceAuto = 0, kSourceXInput = 1, kSourcePlayStation = 2;
Settings& settings();
void read_config(const Config& cfg);

// Game thread, once per engine frame: the keyboard keys.
void tick();

// Any thread (the game's XInput poll): View/Back combinations, removed from `buttons`.
void filter_pad(unsigned long user, unsigned short* buttons);
// Any thread (the game's libScePad poll, sce_pad.h), PlayStation pad 0..3: the same
// combinations and chord on a ScePadData button word (mapped to the XInput buttons: touch
// pad click = View/Back, Options = Start, cross = A, ...); withheld buttons are cleared.
void filter_sce(unsigned long index, std::uint32_t* buttons);
// ScePadData button word -> the XInput buttons the filter sees.
unsigned short sce_to_xinput(std::uint32_t sce);
std::uint64_t pad_polls();

// `controls status | pad <hex buttons> | padlog 0|1 | chord <buttons|off> [ms] | recenter |
// stereo | nearer | farther`
std::string command(const std::string& args);

}  // namespace ff7vr::engine::controls
