#pragma once
// Console variables of the game (r.HZBOcclusion, r.InGameUI.FixedWidth, ...).
//
// Lookup goes through the engine's IConsoleManager (FindConsoleVariable, vtable slot 18);
// values are read and written through IConsoleVariable (GetInt 13, GetFloat 14, Set 12).
// See docs/re/engine.md section 7.
//
// Reads work from any thread. Writes must happen on the game thread (the engine forwards
// render-thread copies of a variable through a render command); set() from another thread
// queues the write and it is applied at the start of the next engine frame.
//
// Writes use the console's priority (ECVF_SetByConsole), so they win over the game's own
// settings until changed again.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ff7vr::engine::cvar {

// The console manager exists and its vtable is the expected one.
bool available();

struct Value {
    int i = 0;
    float f = 0.0f;
    unsigned flags = 0;  // EConsoleVariableFlags (set-by priority in the top byte)
};
std::optional<Value> get(std::wstring_view name);

// Game thread: applies now. Other threads: queued for the next frame; returns true when
// queued. Returns false if the variable does not exist.
bool set(std::wstring_view name, std::wstring_view value);

// Game thread, called once per frame by the engine module: applies queued writes.
void apply_pending();

// Values that hold only while the engine renders in stereo ([stereo_cvars] in ff7vr.ini).
// set_stereo_overrides() before stereo starts; stereo_overrides(true) on the game thread
// when stereo rendering starts saves each variable's current value and sets the override,
// stereo_overrides(false) when it stops puts the saved values back.
void set_stereo_overrides(std::vector<std::pair<std::wstring, std::wstring>> overrides);
void stereo_overrides(bool on);

// Any thread: adds or replaces one stereo override at run time (the live `graphics profile`
// command). Applied on the game thread with the next frame; while stereo renders the
// variable's current value is saved first (if it is not already) and put back when stereo
// stops, like every [stereo_cvars] entry.
void hold_in_stereo(std::wstring_view name, std::wstring_view value);
// Any thread: removes a stereo override at run time; while stereo renders the saved value is
// put back at once.
void release_in_stereo(std::wstring_view name);

}  // namespace ff7vr::engine::cvar
