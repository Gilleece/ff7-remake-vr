#pragma once
// xinput1_3.dll proxy: every export forwards to the real system DLL.
//
// The real DLL is loaded by absolute path from the system directory on first
// use (never from DllMain). If it is missing or lacks an export, the proxy
// behaves like "no controller connected" instead of crashing.
//
// It also hosts the dev virtual gamepad: when enabled, an injected button /
// stick state is merged into controller 0 (see dev_input.h).

#include <windows.h>

#include <cstdint>
#include <string>

namespace ff7vr::loader::xinput {

// Resolve the real DLL now (call from the bootstrap thread). Idempotent.
bool load_real();
// Path that was loaded, or empty.
std::wstring real_path();

// Virtual gamepad state, merged into user index 0 when enabled.
struct VirtualPad {
    WORD buttons = 0;     // XINPUT_GAMEPAD_* bits
    BYTE lt = 0, rt = 0;
    SHORT lx = 0, ly = 0, rx = 0, ry = 0;
};
void set_virtual_pad_enabled(bool enabled);
bool virtual_pad_enabled();
void set_virtual_pad(const VirtualPad& state);
VirtualPad virtual_pad();
// Filter applied to every successful XInputGetState / XInputGetStateEx result (after the
// virtual pad is merged): it may change the buttons. Set once at start-up; nullptr = none.
using PadFilter = void (*)(unsigned long user, unsigned short* buttons);
void set_pad_filter(PadFilter filter);
// Stick filter, applied after the pad filter: it may change the thumbstick axes (snap turn).
using StickFilter = void (*)(unsigned long user, short* lx, short* ly, short* rx, short* ry);
void set_stick_filter(StickFilter filter);
// Deflection of the left and right sticks (0..1) in the last successful poll of user 0,
// virtual pad included.
void stick_magnitudes(float* left, float* right);
// Number of XInputGetState calls seen (diagnostics: proves the game polls us).
std::uint64_t get_state_calls();
// Call once after load_real. With `wrap_import`, points the game's import of XInputGetState
// at a wrapper that calls our export through its current entry and applies the pad filter to
// the result, so a hook on the export (the Steam overlay places one) cannot bypass the filter.
// Also the diagnostics for the player's log: the dev command `xinput status | timing | probe`
// and, from the game's first XInputGetState call on, one-time lines about who polls (thread,
// calling module, rate) and a line whenever a user index gains or loses a pad. Counters only
// on the call path.
void register_diagnostics(bool wrap_import);

}  // namespace ff7vr::loader::xinput
