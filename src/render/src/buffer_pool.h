// Pool for the game's per-draw dynamic vertex buffers ([stereo] light_volume_pool;
// docs/benchmarking.md, "Mid-session stall").
//
// With the skin lighting fix ([stereo] light_fix, the light sort-key patch) Square Enix's
// RenderLights draws a group of lights one by one as light volumes, and for every light and
// eye it creates a new 128-byte dynamic vertex buffer (RHICreateVertexBuffer with
// BUF_Volatile: D3D11_USAGE_DYNAMIC, CPU write, no initial data), fills it once with
// Map(WRITE_DISCARD) and draws it: about 20 creations and releases a frame in the first room.
// The pool answers those CreateBuffer calls from the game's executable with buffers it keeps
// (a ring, each handed out again only after all the others), so the driver renames memory
// on each Map instead of allocating and freeing system memory 1,800 times a second.
//
// Every buffer is used once right after it is created and filled; the ring is long enough
// (256 buffers, about 12 frames) that a buffer is never handed out again while the engine
// still holds the one before.
#pragma once

#include <string>

struct ID3D11Device;

namespace ff7vr {
class Config;
}

namespace ff7vr::render::buffer_pool {

// [stereo] light_volume_pool (0 = off, the default; 1 = on). Registers the `bufpool` dev command.
void Configure(const Config& cfg);

// Any thread: the game's device, seen at a Present.
void NoteDevice(ID3D11Device* device);

// XR thread, once per timing window: installs the hook at the second window in stereo (after
// the inline hooks other parts of the mod place on device functions when stereo starts).
// Returns one line for the timing block ("" when off or idle).
std::string TakeWindowLine(bool stereo);

std::string Command(const std::string& args);

}  // namespace ff7vr::render::buffer_pool
