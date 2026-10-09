#pragma once
// PlayStation pads read by the game itself.
//
// The game reads DualShock 4 and DualSense pads without XInput: Unreal's WinDualShock plugin
// polls Sony's libScePad (statically linked; it talks to the pad over HID) once per engine
// frame for each of four users through scePadReadState(handle, ScePadData*), see
// docs/re/engine.md, "Gamepad input paths". Such a pad never reaches the XInput proxy, so the
// mod hooks scePadReadState and runs the same combinations and first/third person chord on
// its button word (controls::filter_sce) before the game reads it: buttons used by a
// combination or the chord are withheld from the game exactly as on the XInput path. The
// touch pad click stands in for View/Back (the game maps it to the same action); Share/Create
// is not reported by the library. The sticks go through the same stick filter as an XInput
// pad's (snap turn, head-directed movement; snap_turn.h).
//
// [controls] pad_source = xinput leaves the hook out. Dev pipe: `controls ps <hex> [<lx> <ly>]`
// presents a virtual PlayStation pad (pad 0) with that button word (and left stick, -1..1)
// through the game's own poll,
// `controls ps off` removes it, `controls ps status`.

#include <cstdint>
#include <string>

namespace ff7vr::engine::sce_pad {

// Hooks scePadReadState (address from the signature scan; 0 = not found). Any thread.
bool init(std::uintptr_t read_state);

std::string status();
std::string timing();  // one part of the controls timing line
std::string command(const std::string& args);

}  // namespace ff7vr::engine::sce_pad
