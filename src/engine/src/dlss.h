#pragma once
// NVIDIA DLSS in place of the game's temporal anti-aliasing, per eye (optional; built only
// with -DFF7VR_DLSS=ON and the NVIDIA DLSS SDK, see docs/dlss.md).
//
// In stereo the engine renders both eyes into double-wide targets and runs its temporal
// anti-aliasing pass once per view, at the view's rectangle. That pass is recognised on the
// RHI thread (a full-screen draw whose inputs are the scene depth, the jittered scene colour,
// the history and the velocity buffer). When DLSS is on, the draw is replaced: the velocity
// buffer and the camera motion are turned into per-pixel motion vectors for the eye's
// rectangle, DLSS (one feature instance per eye, so each eye keeps its own history) reads
// the eye's rectangle of colour, depth and motion vectors and writes the eye's rectangle of
// an output texture, which is copied into the pass's render target. Everything after the
// pass (bloom, tonemapper, the eye texture) is unchanged.
//
// The jitter of the frame comes from the view's uniform buffer, captured when the engine
// writes it (a hook on the immediate context's Map/Unmap).
//
// Threads: NGX and every draw-time decision run on the RHI thread (the only user of the
// immediate context); settings are changed from the dev pipe through atomics.

#include "gpu_trace.h"

#include <d3d11.h>

#include <filesystem>
#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::engine::dlss {

// Reads [dlss] from ff7vr.ini. dll_dir: folder of the mod (also searched for nvngx_dlss.dll).
void start(const Config& cfg, const std::filesystem::path& dll_dir);

// True while DLSS wants the immediate context hooks (enabled or initialising).
bool wants_hooks();

// RHI thread, once per stereo frame (from the frame-end command).
void frame(ID3D11Texture2D* any_texture);

// RHI thread, for every DrawIndexed on the immediate context: true if it replaced the draw.
bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original);

// Dev pipe: "dlss ..." (see docs/dlss.md).
std::string command(const std::string& args);

}  // namespace ff7vr::engine::dlss
