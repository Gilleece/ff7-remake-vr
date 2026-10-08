#pragma once
// Square Enix's distortion (heat haze and refraction: fire, hot air, glass) in stereo: the
// left view's composite covered both eyes and the right view's composite drew nothing.
//
// The distortion pipeline (docs/re/engine.md, section 10, "Distortion") runs per view: a
// full-size accumulate ("Distortion"), half-size passes ("DistortDirection",
// "DistortHistory", two "DistortBlurred"), then a composite into the scene colour. The
// composite's draw (UE's DrawRectangle) is given the view's size as its target size while
// its viewport is the whole double-wide scene colour target: the left view's rectangle is
// stretched over both eyes, and the right view's rectangle lands at clip x 1 to 3, outside
// the viewport, so the right view's composite produces no pixels. Whenever distortion is on
// screen, the right eye therefore shows the left view's composite stretched over it.
//
// The fix (RHI thread, on the composite's draw only): the viewport (and scissor) are set to
// the view's rectangle, and for a view that does not start at the origin the vertex shader's
// DrawRectangle constants are replaced by a copy whose position bias is 0, so the view's
// rectangle maps onto its own viewport. The engine's own buffers are not changed.
//
// Threads: a hook on the composite function (render thread) appends an RHI command with the
// view's rectangle; on the RHI thread it arms the next DrawIndexed, which is checked against
// the expected shape before anything is changed.

#include <d3d11.h>

#include <cstdint>
#include <string>

#include "gpu_trace.h"

namespace ff7vr::engine::distortion_fix {

// composite: the distortion composite function (signature "Distortion composite").
bool init(std::uintptr_t composite, bool enabled);
void set_enabled(bool on);
bool enabled();
bool wants_hooks();
// RHI thread, once per stereo frame.
void frame();
// RHI thread, from the shared DrawIndexed override: true if it issued the draw itself.
bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original);
std::string status();
// Test: the composite's draws write without blending, so its output becomes visible even with
// nothing to distort (a forced pipeline). Development only.
void set_opaque(bool on);

}  // namespace ff7vr::engine::distortion_fix
