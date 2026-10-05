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

    void eye_texture_ready(const EyeTexture& eyes) override {
        if (!eyes.frame_id || !eyes.texture) return;
        render::StereoSubmit s;
        s.frameId = eyes.frame_id;
        s.texture = eyes.texture;
        s.viewFormat = eyes.view_format;
        s.encoding = eyes.srgb_encoded ? xr::ColorEncoding::Srgb : xr::ColorEncoding::Linear;
        for (int e = 0; e < 2; ++e) {
            s.eyeRects[e].x = eyes.eyes[e].x;
            s.eyeRects[e].y = eyes.eyes[e].y;
            s.eyeRects[e].width = eyes.eyes[e].width;
            s.eyeRects[e].height = eyes.eyes[e].height;
        }
        render::SubmitStereoFrame(s);
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
