#pragma once
// Per-frame hook at the end of player::tick (game thread): the audio listener at the head
// in first person, and the camera state the cutscene screen needs (movie_watch.h).
//
// Audio listener ([first_person] audio_listener, default 1): the game's listener stays at
// the game camera (behind the character) and ignores the headset. While first person
// applies, PlayerController.SetAudioListenerOverride(nullptr, location, rotation) puts it at
// the centre between the last stereo frame's two eye cameras, turned like the left eye
// camera (the headset's yaw, pitch and roll included). ClearAudioListenerOverride gives it
// back to the game when first person stops applying, when stereo goes off and when the
// controller or the pawn changes. Both functions are found by name through the object array
// and called through ProcessEvent. Cost: one call per frame.

#include <string>

namespace ff7vr::engine::audio_listener {

struct Frame {
    void* pc;           // local player controller (nullptr: none)
    void* pawn;         // its pawn
    void* view_target;  // the camera manager's view target
    bool combat;        // battle signal (third person in battles)
    bool stereo;        // this frame renders in stereo
    bool first_person;  // first person applies this frame (follow camera, blend past half way)
};

void player_frame(const Frame& f);

void set_enabled(bool on);
std::string status();
std::string command(const std::string& args);

}  // namespace ff7vr::engine::audio_listener
