#pragma once
// The connection between the engine's stereo device and whatever provides the headset:
// per-eye render size and eye views go in, the rendered eye texture comes out.
//
// The engine module drives it from three places inside the game:
//
//   game thread, start of every UGameEngine::Tick:
//       begin_game_frame(stereo_wanted, frame)
//           The host returns the views for the frame the engine is about to simulate
//           and render (this is where an OpenXR host waits for the frame and locates
//           the views). The engine builds both eye cameras from `frame.views` later in
//           the same Tick (UGameViewportClient::Draw). With frame.stereo == false the
//           engine renders this frame mono (no session, headset not ready, ...).
//   any thread:
//       eye_render_size(w, h)
//           Wanted per-eye render size. A change reallocates the engine's eye target on
//           the next frame.
//   render thread, once per rendered stereo frame, after the scene has been rendered:
//       eye_texture_ready(eyes)
//           The engine's side-by-side eye texture and the frame id of the views it was
//           rendered with. The texture is the engine's own render target: read it on the
//           render thread before the next frame starts (for example at Present) and do
//           not keep references beyond that.
//
// Conventions: poses and FOV use OpenXR conventions (right-handed, +X right, +Y up,
// -Z forward, metres; FOV angles in radians, left/down negative). The structures have
// the same layout as ff7vr::xr::Pose and ff7vr::xr::Fov.
//
// Without a host the engine uses built-in fixed values (FixedStereoHost: ini driven eye
// size, FOV and IPD, optional scripted head motion) and nobody consumes the eye texture.

#include <dxgiformat.h>

#include <cstdint>

struct ID3D11Texture2D;

namespace ff7vr::engine {

struct HostQuat {
    float x = 0, y = 0, z = 0, w = 1;
};
struct HostVec3 {
    float x = 0, y = 0, z = 0;
};
struct HostPose {
    HostQuat orientation{};
    HostVec3 position{};
};
struct HostFov {
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0;  // radians
};
struct HostView {
    HostPose pose{};  // eye pose in tracking space (after recenter)
    HostFov fov{};
};

struct GameFrame {
    bool stereo = true;          // false: render this frame mono (the host cannot show stereo now)
    std::uint64_t frame_id = 0;  // host's id for this frame, returned with the eye texture; 0 = none
    bool views_valid = false;    // false: the engine keeps using the last valid views
    HostView views[2]{};         // [0] left eye, [1] right eye
    HostPose head{};             // head pose in tracking space (informational)
};

struct EyeRect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
};

struct EyeTexture {
    ID3D11Texture2D* texture = nullptr;            // side by side: left eye rect, right eye rect
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;  // typed format to read the (typeless) texture with
    bool srgb_encoded = true;                       // values are gamma encoded (what the tonemapper writes)
    EyeRect eyes[2]{};
    std::uint64_t frame_id = 0;                     // GameFrame::frame_id of the views used, 0 = unknown
};

class StereoHost {
public:
    virtual ~StereoHost() = default;

    // Any thread. Per-eye render size wanted now; return false to keep the current one.
    virtual bool eye_render_size(std::uint32_t& width, std::uint32_t& height) = 0;

    // Game thread, once per engine frame, before the views are built. stereo_wanted tells
    // whether stereo is switched on (false: the engine renders mono, views unused); the
    // host answers with out.stereo whether this particular frame can be stereo.
    virtual void begin_game_frame(bool stereo_wanted, GameFrame& out) = 0;

    // Render thread, once per rendered stereo frame (see above). frame_id 0 means the
    // engine could not tell which frame's views the image was rendered with.
    virtual void eye_texture_ready(const EyeTexture& eyes) = 0;
};

}  // namespace ff7vr::engine
