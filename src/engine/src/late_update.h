#pragma once
// Late update of the head pose ([stereo] late_update): the eye views of a stereo frame are
// located again on the render thread, right before the frame's scene renders, and the two
// views' matrices are rebuilt from the newer pose, the way Unreal's own head-mounted
// display path does it (FDefaultXRCamera::PreRenderView_RenderThread ->
// FSceneView::UpdateViewMatrix), which this game does not contain.
//
// At the start of FDeferredShadingSceneRenderer::Render (render thread; ui_layer.cpp's
// hook) for a stereo view family, with both FViewInfo:
//   1. the frame is identified by its eye cameras: the queued frame (stereo_device) whose
//      left and right eye positions equal the views' ViewMatrices.ViewOrigin exactly;
//   2. the matrices the views hold are checked against the same matrices rebuilt from the
//      frame's own eye cameras (layout and math proven every frame; a mismatch skips it);
//   3. the host locates the views again for the frame's predicted display time;
//   4. each eye camera is composed again from the game camera of that frame and the new
//      eye pose (stereo_device's math: world scale, decoupled pitch, positional), and every
//      view-dependent matrix of FViewMatrices (both copies: ViewMatrices and
//      ShadowViewMatrices), PreViewTranslation, ViewOrigin, the view frustum's planes and
//      the near clipping plane are written; the projection is left alone, so the temporal
//      anti-aliasing jitter and the previous frame's matrices (taken in InitViews, later in
//      Render) stay consistent with what is rendered;
//   5. the frame's hand-over views (StereoSubmit::renderedViews) become the new views, so
//      the runtime re-projects from the pose the image was rendered with.
// Where FViewInfo keeps these is found at the first stereo frame (docs/re/engine.md,
// "FViewMatrices in FViewInfo") and checked as above; nothing is written until it is.

#include "ff7vr/core/config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ff7vr::engine::late_update {

void configure(const Config& cfg);
bool enabled();

// Render thread, at the start of FDeferredShadingSceneRenderer::Render for a stereo view
// family (left view, right view: FViewInfo, `stride` bytes each).
void before_scene(std::uint8_t* left, std::uint8_t* right, std::size_t stride);

// Thread that hands the frame over (RHI thread), per stereo frame: how old the views were
// at that moment, in ms (game thread location; late location, < 0 when not relocated).
void note_handover(double game_age_ms, double late_age_ms);

// `stereo lateupdate status | on | off | dump`.
std::string command(const std::vector<std::string>& a);

}  // namespace ff7vr::engine::late_update
