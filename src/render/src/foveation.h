// Fixed foveated rendering in stereo: NVIDIA variable rate shading on D3D11
// through NVAPI. See docs/render.md, "Foveated rendering".
//
// How it works:
//   * A shading-rate surface (one R8_UINT texel per 16x16 pixel tile) covers the
//     engine's side-by-side scene targets. Each tile holds a ring index from its
//     distance to its eye's optical centre (where the eye's view axis meets the
//     image, off-centre because headset FOVs are asymmetric), plus an index for
//     tiles the headset cannot show at all (the runtime's hidden area mesh).
//     A per-viewport table maps the indices to shading rates.
//   * The engine module reports, inside the frame's command stream, where the
//     scene of a stereo view family begins (with both eyes' view rects and
//     projections) and where it ends (before the UI pass and post-processing).
//   * Hooks on the immediate context's OMSetRenderTargets switch variable rate
//     shading on while a render target with the scene targets' size is bound
//     inside that window, and off for everything else.
// Everything runs on the thread that owns the immediate context (the RHI
// thread: the engine's commands, the context hooks and the Present hook).
#pragma once

#include "d3d11_hooks.h"

#include "ff7vr/core/config.h"
#include "ff7vr/render/render.h"

#include <string>
#include <vector>

namespace ff7vr::render::foveation {

// start(): reads [foveation] from the ini.
void Configure(const Config& config);

// Presenting thread, at the start of the Present hook (before this module's own
// D3D11 work): notes the game's device (NVAPI and the context hooks are set up
// at the first stereo scene), switches variable rate shading off, reads back GPU
// timings.
void OnPresent(const PresentInfo& p);

// Any thread.
bool Wanted();

// Presenting thread, in the frame's command order (see render.h).
void SceneBegin(const FoveationEye eyes[2]);
void SceneEnd();

// Dev command `fov ...`.
std::string Command(const std::string& args);

// One line for the status command.
std::string Status();

// Summaries of the foveation timing series for the periodic timing block (resets them).
std::vector<std::string> TakeTimingLines();

}  // namespace ff7vr::render::foveation
