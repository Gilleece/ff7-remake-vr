#pragma once
// ff7vr_engine: the game's Unreal Engine 4.18 fork, seen from the mod.
//
// start() runs on the loader's bootstrap thread at process start, long before the engine
// initialises. It checks the game build, resolves every engine address by signature and
// hooks two UGameEngine vtable slots:
//   InitializeHMDDevice (slot 111): after the engine's own code runs, our stereo device is
//       stored in GEngine->StereoRenderingDevice, before the viewport and the local player
//       exist (so the engine allocates per-eye view states itself);
//   Tick (slot 78): the per-frame game-thread point where the host's views are fetched.
// If anything does not check out, stereo stays off with a log line saying why and the
// game runs unmodified.
//
// ini keys: see docs/engine-module.md ([stereo] section of ff7vr.ini).

#include "ff7vr/core/startup_context.h"
#include "ff7vr/engine/stereo_host.h"

#include <string>

namespace ff7vr::engine {

// Called once by the loader. Never throws. Returns false when stereo will not be enabled.
bool start(const StartupContext& ctx);

// Dev pipe commands whose first word is "stereo" or "cvar" (see docs/engine-module.md).
// Returns true and sets `reply` when the command belongs to this module.
bool handle_command(const std::string& line, std::string& reply);

// Attach the provider of eye size and views (nullptr: built-in fixed values). Any thread;
// attach before the engine initialises or at least before switching stereo on.
void set_stereo_host(StereoHost* host);

// Our device is installed in the engine.
bool stereo_installed();
// The engine renders in stereo (this frame).
bool stereo_active();
// Switch stereo rendering on or off from the next engine frame. Any thread. Off renders the
// game normally into the window (the device stays installed).
void request_stereo(bool on);

// Gamepad filter, called by the XInput proxy with every successful XInputGetState result of
// any thread. Combinations with View/Back held (first person, recenter, stereo on/off, UI
// panel distance; [controls] and [first_person] pad_toggle) trigger their action and are
// removed from the state until released, so the game never acts on them; View alone is
// held back while down and handed to the game as a short press on release
// ([controls] pad_hold_view). See docs/engine-module.md, "Player controls".
void filter_pad(unsigned long user, unsigned short* buttons);

}  // namespace ff7vr::engine
