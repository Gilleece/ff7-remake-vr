// The frame budget's cause for a window over budget (docs/render.md, "Frame budget").
// Pure: no state, no logging; the unit tests call it directly (src/render/tests).
#pragma once

#include "frame_budget.h"

#include <cstdint>
#include <string>

namespace ff7vr::render::frame_budget {

struct ClassifyInput {
    Window w;                    // the window's figures (its medians)
    double intervalMs = 0;       // median frame interval
    double budgetMs = 0;         // the headset's refresh period
    bool haveGpu = false;        // the GPU scene and after-the-scene times were measured
    double dlssTwoEyesMs = 0;    // DLSS's GPU time for both eyes per frame (0: none)
    std::string dlssPreset;      // "" or "default", "J" .. "M"
    uint32_t dlssOutWidth = 0, dlssOutHeight = 0;
    bool haveRenderScale = false;
    float renderScale = 1.0f;
    // GPU time (scene + after) of the newest window within budget at the same settings, with
    // no load, mode change or settings change since; 0 when there is none.
    double baselineGpuMs = 0;
};

struct Verdict {
    std::string kind;    // "transfer-bound", "GPU-bound", "runtime-bound", "game-thread-bound", "Cause unclear"
    std::string detail;  // the parts with their times
    std::string advice;  // one suggestion
    std::string brief;   // for the summary line
    bool stall = false;  // the signature of the mid-session stall (GPU time risen without a cause, or slow uploads)
};

// Shares of the frame interval / refresh period the tests use.
constexpr double kGpuBound = 0.85;        // GPU total at least this share of the interval ...
constexpr double kGpuFillsBudget = 0.90;  // ... or of the refresh period (the runtime then waits for a slow GPU)
constexpr double kSlowUploadGBps = 0.5;   // transfer-bound: the upload check below this
constexpr double kLowUploadGBps = 3.0;    // below this (normally 9-14 GB/s) the stall is suspected
constexpr double kStallRise = 2.0;        // GPU time this many times the baseline ...
constexpr double kStallRiseMs = 4.0;      // ... and at least this much more
constexpr double kGpuLow = 0.70;          // game-thread-bound: GPU total below this share of the interval ...
constexpr double kWaitLow = 0.30;         // ... and the frame wait below this share

Verdict Classify(const ClassifyInput& in);

}  // namespace ff7vr::render::frame_budget
