#pragma once
// Live battle objects for the fight detection ([first_person] battle_objects,
// battle_state.h): the live instances of a few classes, found a slice of the object array
// per poll, and whether any of them is shown. A widget counts when `Widget.IsVisible()` is
// true; an object of any other class counts while it is alive. Objects destroyed and
// waiting for the garbage collector (which runs about once a minute) are left out.
//
// Default class: BattleTarget_New_C, the battle HUD's target marker. In the recorded
// session (docs/re/engine.md, "Fights in a recorded session") it was shown for the whole of
// each fight and hidden between fights; the other battle classes stay in the object array
// long after a fight (until the next garbage collection) or appear late in it.

#include <string>

namespace ff7vr::engine::battle {

struct ObjectsState {
    int shown = -1;     // -1 not known yet (first pass of the object array not done), 0 none shown, 1 shown
    int instances = 0;  // live instances of the classes
    int visible = 0;    // of which shown
};

// Comma or space separated class names. Any thread.
void set_classes(const std::string& list);
std::string classes();

// Game thread (it calls reflected functions). Scans a slice of the object array and checks
// the instances at most every `poll_seconds`; returns the latest state and whether this
// call polled.
ObjectsState poll(double now_seconds, bool& polled);

// The state of the last poll (game thread).
ObjectsState last();

// One line for `fp combat status`.
std::string status();

}  // namespace ff7vr::engine::battle
