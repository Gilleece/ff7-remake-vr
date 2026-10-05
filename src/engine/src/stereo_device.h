#pragma once
// Our IStereoRendering device and IStereoRenderTargetManager, as plain function tables
// with static storage (see stereo_abi.h and docs/re/stereo-hook-plan.md).
//
// Threads: the engine calls the device from the game thread (camera, view rects,
// reallocation decisions) and from the render thread (render target size, the eye texture).
// State that both threads read is kept in atomics; state that only the game thread uses
// is latched once per frame at the start of UGameEngine::Tick.

#include "fixed_host.h"
#include "mirror.h"

#include "ff7vr/engine/stereo_abi.h"
#include "ff7vr/engine/stereo_host.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace ff7vr::engine::device {

struct Settings {
    std::atomic<float> world_scale{1.0f};      // multiplies the world's WorldToMeters (head motion, IPD)
    std::atomic<bool> decouple_pitch{true};    // drop the game camera's pitch and roll
    std::atomic<bool> positional{true};        // apply head position (false: rotation and IPD only)
    std::atomic<int> mirror{static_cast<int>(mirror::Mode::Crop)};
    std::atomic<int> log_frames{0};            // log the eye cameras of the next N stereo frames
    std::atomic<bool> swap_rects{false};       // test: right eye in the left half of the target, left eye in the right half
    std::atomic<int> frame_window_ms{10000};   // length of a frame time measurement window
    std::atomic<bool> frame_window_reset{false};  // start a new window at the next frame (discards the current one)
};
Settings& settings();

void init(float* near_plane, bool start_active, const FixedStereoHost::Options& fixed);

// The objects to store in UEngine::StereoRenderingDevice (object, reference controller).
ue::SharedPtrRaw shared_ptr();
const void* vtable();

// Host providing eye size and views (nullptr = built-in fixed values).
void set_host(StereoHost* host);
StereoHost* host();
FixedStereoHost* fixed_host();  // the built-in host (for dev commands)

// Stereo on/off, applied at the start of the next engine frame. Any thread.
void request_active(bool on);
bool wanted();  // switched on
bool active();  // the current frame renders in stereo (wanted and the host can show it)

// Game thread, from the UGameEngine::Tick hook.
void tick_begin();
void tick_end();

std::string status();
std::string last_views();

}  // namespace ff7vr::engine::device
