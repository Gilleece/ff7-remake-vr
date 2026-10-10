// D3D11 object census ([log] d3d11_census; docs/benchmarking.md, "Mid-session stall").
//
// Counts the objects created on the game's D3D11 device (buffers, textures, views, queries,
// state objects) by who created them: this mod's DLL, the game's executable, or any other
// module (NVIDIA's DLSS library, overlays). The device's vtable slots are replaced once the
// device is known; every call through them is counted and passed on unchanged. A creation
// that comes back every frame shows as a steady rate in the 10-second timing block
// (`timing:   d3d11 objects created ...`); the running totals are in `census status`.
//
// The counts are of creations, not of live objects: a per-frame view that is released again
// shows the same rate as one that leaks. The process's private bytes and the device's
// memory figures in the same block tell the two apart.
#pragma once

#include <string>

struct ID3D11Device;

namespace ff7vr {
class Config;
}

namespace ff7vr::render::census {

// [log] d3d11_census (0 = off, the default; 1 = on). Registers the `census` dev command.
void Configure(const Config& cfg);

// Any thread: the game's device, seen at a Present.
void NoteDevice(ID3D11Device* device);

// XR thread, once per timing window: the creations since the previous call ("" when off).
// The counting hooks are installed at the second window in stereo: inline hooks that other
// parts of the mod place on device functions when stereo starts (DLSS's CreateBuffer hook
// reads the function from the vtable) must see d3d11's own function, not the census's
// detour, or the game's calls would be counted as the mod's.
std::string TakeWindowLine(bool stereo);

std::string Command(const std::string& args);

}  // namespace ff7vr::render::census
