#pragma once
// Pause when the headset comes off ([xr] pause_on_remove, pause_key).
//
// When the XR session leaves FOCUSED (to VISIBLE or SYNCHRONIZED: the headset was taken
// off, or the runtime's own menu opened) while 3D runs, the game's pause key is sent once
// with SendInput, only if a window of the game has the focus, at most once per 5 s.
// Nothing is sent when the focus returns. When the runtime reports XR_EXT_user_presence
// events, the user-absent event is used instead of the focus loss.
// See docs/render.md, "Session life cycle".

#include "ff7vr/core/config.h"
#include "ff7vr/xr/xr.h"

#include <string>

namespace ff7vr::render::pause_on_remove {

void read_config(const Config& cfg);

// The frame loop, every XR frame: the session state and the user presence
// (IXrBackend::UserPresence: -1 not reported, 0 absent, 1 present). `stereo`: 3D runs.
void note(xr::SessionState state, int presence, bool stereo);

// `xr-pause status | on | off | key <vk> | send`
std::string command(const std::string& args);

}  // namespace ff7vr::render::pause_on_remove
