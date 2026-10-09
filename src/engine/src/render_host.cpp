// StereoHost backed by the render module (src/render): eye size and views come from the
// XR session it runs, the eye texture goes to its Present hook (render.h, "STEREO
// INTERFACE"). Compiled only when the render module is part of the build.

#include "render_host.h"

#if FF7VR_ENGINE_WITH_DLSS
#include "dlss.h"
#endif

#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <string>

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

// The image each eye's projection layer gets: the region of the texture handed over, which the
// XR module copies 1:1 into the eye's swapchain image when it is not larger (so it is the
// layer's sub-image size), and where it comes from (DLSS's own output texture or the engine's
// eye target). Logged whenever either changes, with how long the previous state lasted.
struct Handover {
    bool valid = false;
    bool from_dlss = false;
    std::uint32_t w[2]{}, h[2]{};
    std::uint64_t frames = 0;
    std::chrono::steady_clock::time_point since{};
    std::uint64_t changes = 0;
};
Handover g_handover;  // the presenting thread only

std::string handover_text(const std::uint32_t w[2], const std::uint32_t h[2], bool from_dlss) {
    std::string share;
    render::EyeSetup es;
    if (render::GetEyeSetup(&es) && es.eyeWidth && es.eyeHeight) {
        auto pct = [&](std::uint32_t v, std::uint32_t full) { return 100.0 * std::min(v, full) / full; };
        share = std::format(" ({:.0f} % x {:.0f} % and {:.0f} % x {:.0f} % of the runtime's eye {}x{})", pct(w[0], es.eyeWidth), pct(h[0], es.eyeHeight),
                            pct(w[1], es.eyeWidth), pct(h[1], es.eyeHeight), es.eyeWidth, es.eyeHeight);
    }
    return std::format("eye images {}x{} and {}x{}{} from {}", w[0], h[0], w[1], h[1], share, from_dlss ? "DLSS's texture" : "the engine's eye target");
}

void note_handover(const EyeRect rects[2], bool from_dlss) {
    Handover& H = g_handover;
    const std::uint32_t w[2] = {rects[0].width, rects[1].width}, h[2] = {rects[0].height, rects[1].height};
    if (H.valid && H.from_dlss == from_dlss && H.w[0] == w[0] && H.w[1] == w[1] && H.h[0] == h[0] && H.h[1] == h[1]) {
        ++H.frames;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    ++H.changes;
    if (H.changes <= 50 || (H.changes & (H.changes - 1)) == 0) {
        try {
            const std::string before =
                H.valid ? std::format("; before: {} for {} frames ({:.1f} s)", handover_text(H.w, H.h, H.from_dlss), H.frames,
                                      std::chrono::duration<double>(now - H.since).count())
                        : std::string("; the first stereo image of this session");
            log::info("projection layer: {}{} (change {}{})", handover_text(w, h, from_dlss), before, H.changes,
                      H.changes == 50 ? "; from now on only every power of two of these changes is logged" : "");
        } catch (...) {
        }
    }
    H.valid = true;
    H.from_dlss = from_dlss;
    for (int e = 0; e < 2; ++e) {
        H.w[e] = w[e];
        H.h[e] = h[e];
    }
    H.frames = 1;
    H.since = now;
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
        EyeRect rects[2] = {eyes.eyes[0], eyes.eyes[1]};
#if FF7VR_ENGINE_WITH_DLSS
        dlss::output_rects(rects);  // DLSS upscaling writes the whole half of each eye
        note_handover(rects, dlss::is_output_texture(eyes.texture));
#else
        note_handover(rects, false);
#endif
        for (int e = 0; e < 2; ++e) {
            s.eyeRects[e].x = rects[e].x;
            s.eyeRects[e].y = rects[e].y;
            s.eyeRects[e].width = rects[e].width;
            s.eyeRects[e].height = rects[e].height;
        }
        if (eyes.depth) {
            s.depthTexture = eyes.depth;
            s.depthSrvFormat = eyes.depth_srv_format;
            for (int e = 0; e < 2; ++e)
                s.depthRects[e] = xr::Rect{eyes.depth_rects[e].x, eyes.depth_rects[e].y, eyes.depth_rects[e].width, eyes.depth_rects[e].height};
            // Unreal's reversed infinite depth: 0 at infinity (nearZ, the distance at depth 0),
            // 1 at the near plane (farZ, the distance at depth 1).
            s.depthNearZ = std::numeric_limits<float>::infinity();
            s.depthFarZ = eyes.depth_near_m;
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

    bool depth_wanted() override { return render::DepthLayerWanted(); }

    bool relocate_views(std::uint64_t frame_id, HostView out[2]) override {
        xr::View v[2];
        if (!frame_id || !render::RelocateViews(frame_id, v)) return false;
        out[0] = to_host(v[0]);
        out[1] = to_host(v[1]);
        return true;
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
