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
// Number of XInputGetState calls seen (diagnostics: proves the game polls us).
std::uint64_t get_state_calls();

}  // namespace ff7vr::loader::xinput
