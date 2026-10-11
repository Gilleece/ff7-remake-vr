// GPU time of each stereo frame, whatever else is switched on (docs/render.md, "Timing").
//
// The engine module marks, in the frame's command order on the presenting thread, where the
// 3D scene of a stereo view family begins and where it ends (before the UI pass and the
// post-processing; render.h, FoveationSceneBegin / FoveationSceneEnd). This module puts a
// D3D11 timestamp at the scene's start, one at its end and one at the Present that ends the
// frame, all three under one disjoint query per frame, and reads them back a few frames later
// without stalling. The results are the timing block's `gpu scene` and `gpu after the scene
// until Present` series, the frame budget's GPU figures and the engine's dynamic resolution
// input (GetGpuFrameTime). Foveated rendering uses the same scene window for its shading-rate
// mask but no longer owns these timers, so the figures exist with foveation off.
#pragma once

#include "d3d11_hooks.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ff7vr::render::scene_timing {

// Any thread: the timers run (on by default; dev command `gputime on|off`).
bool Enabled();

// Presenting thread, first thing in the Present hook (before any of the mod's own GPU work):
// closes the frame's measurement and reads back finished ones.
void OnPresent(const PresentInfo& p);

// Presenting thread, in the frame's command order.
void SceneBegin();
void SceneEnd();

// Medians (and sample counts) of the current timing period, without resetting it.
void PeekMedians(double* sceneMs, size_t* sceneSamples, double* afterMs, size_t* afterSamples);
// The newest frame's scene and after-the-scene times; false while nothing was measured.
bool LastTimes(double* sceneMs, double* afterMs);
// The period's summaries for the timing block (resets the period).
std::vector<std::string> TakeTimingLines();

// Dev command `gputime status | on | off`.
std::string Command(const std::string& args);

}  // namespace ff7vr::render::scene_timing
