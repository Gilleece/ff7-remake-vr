// Frame budget warning ([log] frame_budget; docs/render.md, "Frame budget").
//
// Once per timing window (the render module's 10-second timing block) the XR thread hands
// over the window's figures. In stereo gameplay (stereo mode, the 3D scene rendered in the
// window's frames, no capture in the window, at least 30 s after a load, a stall, a mode
// change or the session's start) the median frame interval is compared with the headset's
// refresh period. When it exceeds the period by more than 5 % in two windows in a row, one
// warning names the likely cause (GPU-bound and its largest part, the game's own frame, or
// the runtime's pacing) with one suggestion; one line says when frames are back within the
// budget. At most one warning a minute; nothing else is logged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::render::frame_budget {

struct Window {
    double seconds = 0;
    size_t frames = 0;           // Presents of the main swap chain in the window
    double intervalMs = 0;       // median game frame interval
    double intervalMaxMs = 0;
    double waitMs = 0;           // median xr frame wait
    size_t waitSamples = 0;
    double sceneMs = 0, afterMs = 0;  // medians of the GPU scene and after-the-scene times (foveation's timers)
    size_t sceneSamples = 0, afterSamples = 0;
    bool stereoMode = false;     // mode stereo when the window ended
    uint64_t stereoSubmitted = 0;  // frames ended with a stereo image in the window
    float refreshHz = 0;
    std::string runtime;
    uint32_t eyeWidth = 0, eyeHeight = 0;
    std::string foveationPreset;
    double copyEnginePercent = -1;  // the game's copy engines (video memory report), -1 unknown
};

// [log] frame_budget (1 = on, the default; 0 = off). Registers the `budget` dev command.
void Configure(const Config& cfg);

// Any thread: the presentation mode changed (stereo started or ended) / a capture was taken.
void NoteModeChange();
void NoteCapture();

// XR thread, once per timing window.
void Evaluate(const Window& w);

// One short line for the settings panel and `budget status`.
std::string Summary();

std::string Command(const std::string& args);

}  // namespace ff7vr::render::frame_budget
