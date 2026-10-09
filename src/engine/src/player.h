#pragma once
// The player's character and the camera modes built on it.
//
// Each frame (game thread) the module finds the local player controller, the pawn it
// controls and the camera manager's view target through reflected functions
// (`Controller.K2_GetPawn`, `Actor.K2_GetActorLocation`, `Controller.GetViewTarget`;
// docs/re/engine.md section 11). From that it decides
// where the eyes go:
//
// - third person, `[camera] boom = level` (default): the game's follow camera moves along
//   its boom when it pitches; the eyes are put where the camera would be at zero pitch
//   around a pivot above the character, so pitch input only changes where the game camera
//   points (dropped with decoupled pitch) and not the player's height.
// - third person, `boom = game`: the game camera's position as it is.
// - first person: the eyes between the character's eye bones (or at its head bone, or at a
//   fixed offset from the pawn), turned by the game camera's yaw; the character's skeletal
//   meshes are hidden while it applies, and it gives way to third person in a battle.
//
// Both camera modes apply only while the game uses its normal follow camera: the view
// target is the pawn or the game's camera actor (EndCameraActor), and the camera looks at
// the pivot above the pawn from no farther than follow_distance. Otherwise
// (cutscenes, conversations with authored cameras, scripted camera moves) the game's camera
// is used as it is.

#include "ff7vr/engine/stereo_abi.h"

#include <atomic>
#include <string>

namespace ff7vr::engine::player {

struct Settings {
    std::atomic<bool> level_boom{true};      // [camera] boom = level | game
    std::atomic<float> pivot_height{55.0f};  // [camera] pivot_height (cm above the pawn's location)
    std::atomic<float> aim_tolerance{75.0f}; // [camera] aim_tolerance: the pivot's largest distance from the camera's line of sight (cm)
    std::atomic<float> follow_distance{1500.0f};  // camera farther than this from the pawn: not the follow camera (cm)
    std::atomic<float> camera_blend_seconds{0.35f};  // [camera] blend_seconds: moves between level boom, game boom and game camera (0 = cut)
    std::atomic<bool> combat_level{true};    // [camera] combat = level: in a battle the level boom holds while the camera frames
                                             // enemies (view target still the game's camera, pivot within follow_distance);
                                             // game = the same test as outside battles
    std::atomic<float> miss_seconds{1.5f};   // [camera] miss_seconds: how long the camera must look away before the game camera takes over
    std::atomic<bool> collision{true};       // [camera] collision: the level boom is shortened in front of walls and people
    std::atomic<float> collision_margin{20.0f};  // [camera] collision_margin (cm kept between the eyes and what the trace hit)
    std::atomic<bool> fp_available{true};    // [first_person] enabled
    std::atomic<bool> fp_default{true};      // [first_person] default: first person outside battles
    std::atomic<bool> auto_combat{true};     // [first_person] auto_combat
    std::atomic<float> eye_forward{10.0f}, eye_right{0.0f}, eye_up{75.0f};  // [first_person] eye_offset (cm, from the pawn's location; fallback)
    std::atomic<bool> eye_head{true};        // [first_person] eye = head (the head bone) | offset
    std::atomic<float> head_forward{2.0f}, head_right{0.0f}, head_up{0.0f};  // [first_person] head_offset (cm, from the eye bones or the head bone)
    std::atomic<int> hide{1};                // [first_person] hide: 0 none, 1 the character's skeletal meshes,
                                             // 2 the head (its bones), 3 the whole body through its bones,
                                             // 4 out of the main pass (shadow kept); 2 to 4 experimental
    std::atomic<bool> head_bob{false};       // [first_person] head_bob: 1 = the view follows every step of the head
    std::atomic<float> steady_seconds{0.3f}; // [first_person] steady_seconds: time constant of each of the two filter
                                             // stages that take the step motion out of the head's offset (head_bob = 0)
    std::atomic<int> toggle_key{0x24};       // [first_person] toggle_key (virtual key, 0 = none); default Home
    std::atomic<float> blend_seconds{0.35f};
    std::atomic<bool> pad_toggle{true};      // [first_person] pad_toggle: View/Back + right stick click
};
Settings& settings();
// [first_person] battle_signal: "Class.Function", a reflected function without parameters
// returning bool, called each frame on a live instance of Class. Before init().
void set_battle_signal(const std::string& spec);

void init(std::uint8_t* object_array, std::uint8_t* name_pool, void** gengine);

// Game thread, start of every engine frame (before the engine's Tick). `stereo` = this
// frame renders in stereo.
void tick(bool stereo, float delta_seconds);

// Game thread, from CalculateStereoViewOffset: replaces the game camera's location (and
// yaw-only rotation for first person) by the mode's eye base. Returns true when it changed
// something; `force_decouple` is set when the mode needs a level view. The level boom
// applies only with `decoupled` (decoupled pitch); first person always levels the view.
bool adjust_camera(ue::FRotator& rotation, ue::FVector& location, bool decoupled, bool& force_decouple);

// Manual toggle (any thread; applied at the next frame).
void request_toggle();
void request_mode(bool first_person);
// Any thread: the mode outside the automatic switch, as of the last game frame (true = first person).
bool first_person_selected();
// The gamepad combination for the toggle was pressed (any thread; controls.cpp).
void request_pad_toggle();
// Test: pretend a battle is in progress (-1 = use the real signal).
void set_combat_override(int v);

std::string status();
std::string command(const std::string& args);

}  // namespace ff7vr::engine::player
