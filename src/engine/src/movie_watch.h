#pragma once
// Pre-rendered movies: detection, so the headset can show them on the virtual screen.
//
// The game plays its movies (`.emov` files under Content/GameContents/Movie) through
// Unreal's media framework: each movie has a UMediaPlayer asset (`<name>_MediaPlayer`), and
// menu backgrounds use the same mechanism from packages under `/Menu/`. A movie is a flat
// image; rendered into both eyes of a stereo view it would sit at the depth of whatever
// surface shows it, cropped by the eye views.
//
// The watcher finds the MediaPlayer class and its IsPlaying function once (by name, through
// the object array and the name pool), keeps a list of MediaPlayer objects (the object
// array is scanned a slice per frame), and asks each one IsPlaying through ProcessEvent on
// the game thread. While a non-menu player plays, stereo is switched off, so the engine
// renders the normal window and the render module shows it on the virtual screen; stereo
// comes back when no movie plays any more.

#include <cstdint>
#include <string>

namespace ff7vr::engine::movie {

struct Addresses {
    std::uint8_t* GUObjectArray = nullptr;  // FUObjectArray
    std::uint8_t* FNamePool = nullptr;
};

// [stereo] movie_screen (default 1: detection verified on a movie in a headset session).
void init(const Addresses& a, bool enabled);
// Game thread, once per engine frame, before the frame's stereo decision.
void tick();
bool playing();
std::string status();
void set_enabled(bool on);
// Test: count the menu background players as movies too.
void set_include_menu(bool on);
// Test: behave as if a movie played (stereo off, the virtual screen) until switched off again.
void set_simulate(bool on);

}  // namespace ff7vr::engine::movie
