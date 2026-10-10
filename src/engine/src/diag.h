#pragma once
// Diagnostic logs of game state for the camera modes (off by default; read-only, no
// behaviour change). Polled every 100 ms from the end of player::tick (game thread).
//
// [first_person] battle_log = 1 (`fp battlelog 0|1`): several candidate battle signals
// side by side, one line `battle diag: ...` whenever one of them changes, plus a heartbeat
// every 10 s: the battle signal's raw value and the mod's verdict, widgets whose class or
// name suggests the battle HUD (Battle, Command, ATB, Gauge) and whether they are visible,
// target and lock-on properties of the player controller and the pawn, a few properties of
// objects whose class name contains Battle, and the camera manager's state (its class,
// CameraStyle, the view target). The widget blueprint classes present are listed as they
// come and go.
//
// [first_person] prompt_log = 1 (`fp promptlog 0|1`, `fp prompt status`): widgets whose
// class or name suggests the interaction prompt (Interact, Prompt, Talk, Action, Pick),
// one line `prompt diag: ...` whenever one of them changes visibility or moves on screen,
// with the game camera, the head, the camera mode, the [ui] align state and the distances
// to the pawn.

#include <string>

namespace ff7vr::engine::diag {

struct Frame {
    void* pc;            // local player controller (nullptr: none)
    void* pawn;          // its pawn
    void* view_target;   // the camera manager's view target
    int battle_raw;      // the battle signal's last raw value (-1 none)
    bool in_battle;      // the mod's battle verdict
    bool first_person;   // mode outside the automatic switch
    bool fp_applies;     // first person applies this frame (follow camera, blend past half way)
    bool follow_camera;  // the game uses its follow camera
    bool stereo;
};

// Game thread, end of player::tick.
void player_frame(const Frame& f);

void set_battle_log(bool on);
void set_prompt_log(bool on);
// battlelog 0|1, promptlog 0|1, prompt status, diag status
std::string command(const std::string& args);

}  // namespace ff7vr::engine::diag
