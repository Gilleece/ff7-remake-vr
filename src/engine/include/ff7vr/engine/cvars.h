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

}  // namespace ff7vr::engine::cvar
