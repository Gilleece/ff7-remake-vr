// StereoHost backed by the render module (src/render): eye size and views come from the
// XR session it runs, the eye texture goes to its Present hook (render.h, "STEREO
// INTERFACE"). Compiled only when the render module is part of the build.

#include "render_host.h"

#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <atomic>
#include <cstring>

namespace ff7vr::engine {
namespace {

static_assert(sizeof(HostPose) == sizeof(xr::Pose) && sizeof(HostFov) == sizeof(xr::Fov) && sizeof(HostView) == sizeof(xr::View),
              "host and XR layer pose types must match");

HostView to_host(const xr::View& v) {
    HostView h;
    std::memcpy(&h, &v, sizeof(h));
    return h;
}
HostPose to_host(const xr::Pose& p) {
    HostPose h;
    std::memcpy(&h, &p, sizeof(h));
    return h;
}

class RenderStereoHost final : public StereoHost {
public:
    bool eye_render_size(std::uint32_t& width, std::uint32_t& height) override {
        render::EyeSetup s;
        if (!render::GetEyeSetup(&s) || !s.eyeWidth || !s.eyeHeight) return false;
        width = s.eyeWidth;
        height = s.eyeHeight;
        return true;
    }

    void begin_game_frame(bool stereo_wanted, GameFrame& out) override {
        if (stereo_wanted != mode_stereo_) {
            render::SetMode(stereo_wanted ? render::Mode::Stereo : render::Mode::Screen);
            mode_stereo_ = stereo_wanted;
        }
        const render::StereoFrame f = render::BeginGameFrame();
        out.stereo = f.stereo;
        out.frame_id = f.stereo ? f.frameId : 0;
        out.views_valid = f.stereo;
        if (f.stereo) {
            out.views[0] = to_host(f.views[0]);
            out.views[1] = to_host(f.views[1]);
            out.head = to_host(f.head);
        }
    }

    // Called on the presenting thread right before this frame's Present (see stereo_host.h).
    //
    // The render module ends its waited XR frames in order, one per Present. The engine
    // keeps up to two frames in flight between the game thread (where frames are waited)
    // and the RHI thread (where they are presented), so the frame ended at a given Present
    // is not always the one this image was rendered for: right after stereo starts, the
    // Presents of the last mono frames already end the first stereo frames, and the
    // offset can change after a hitch. Because this call happens immediately before the
    // Present that will show the image, the image is offered for each frame id that
    // Present may end (the waited frames that are still open), always with the views it
    // was actually rendered with, so the runtime re-projects it correctly whichever frame
    // carries it. Entries for ids that are ended later are replaced by the next frame's.
    void eye_texture_ready(const EyeTexture& eyes) override {
        if (!eyes.texture || !eyes.latest_frame_id) return;
        render::StereoSubmit s;
        s.texture = eyes.texture;
        s.viewFormat = eyes.view_format;
        s.encoding = eyes.srgb_encoded ? xr::ColorEncoding::Srgb : xr::ColorEncoding::Linear;
        for (int e = 0; e < 2; ++e) {
            s.eyeRects[e].x = eyes.eyes[e].x;
            s.eyeRects[e].y = eyes.eyes[e].y;
            s.eyeRects[e].width = eyes.eyes[e].width;
            s.eyeRects[e].height = eyes.eyes[e].height;
        }
        if (eyes.views_valid) {
            s.haveRenderedViews = true;
            std::memcpy(&s.renderedViews[0], &eyes.views[0], sizeof(xr::View));
            std::memcpy(&s.renderedViews[1], &eyes.views[1], sizeof(xr::View));
        }
        // Open frames at this point: from one before the image's own frame (its
        // predecessor may still be open after a frame without a Present) up to the newest
        // waited frame, at most four (the module keeps four images).
        const std::uint64_t hi = eyes.latest_frame_id;
        std::uint64_t lo = eyes.frame_id ? eyes.frame_id : hi;
        if (lo > 1) --lo;
        if (hi >= 3 && lo + 3 < hi) lo = hi - 3;
        for (std::uint64_t id = lo; id <= hi; ++id) {
            s.frameId = id;
            render::SubmitStereoFrame(s);
        }
    }

    bool gpu_frame_time(float& gpu_ms, std::uint64_t& samples, float& refresh_hz) override {
        render::EyeSetup s;
        if (!render::GetEyeSetup(&s) || !(s.refreshHz > 0)) return false;
        refresh_hz = s.refreshHz;
        return render::GetGpuFrameTime(&gpu_ms, &samples);
    }

private:
    bool mode_stereo_ = false;  // game thread only
};

}  // namespace

StereoHost* render_stereo_host() {
    static RenderStereoHost* host = new RenderStereoHost();  // never freed
    return host;
}

}  // namespace ff7vr::engine
