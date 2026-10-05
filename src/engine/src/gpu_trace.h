#pragma once
// One-frame trace of the engine's GPU work (development tool, dev pipe `gpu ...`).
//
// While a trace runs, the D3D11 immediate context's draw, dispatch, clear and copy calls are
// intercepted for exactly one engine frame (from one stereo frame end to the next, on the
// RHI thread, the only user of the immediate context). For every call the log records the
// bound render targets, depth target, viewport, shaders, shader resources, unordered access
// views and constant buffer sizes, plus the GPU time to the previous call (timestamp
// queries). Optionally the first render target (or first UAV of a dispatch) is read back
// after each call in a range and written to a file, and the first constant buffer's
// contents are logged for those calls.
//
// Textures are labelled with the names the engine gives its pooled render targets
// (FRenderTargetPool::FindFreeElement is hooked while names are enabled), so the passes of
// the frame can be told apart.
//
// Output, for `gpu trace <prefix> ...`:
//   <prefix>.txt          one line per call
//   <prefix>_<seq>.rgba   read-backs: 16-byte header {'GTD1', width, height, dxgi format}
//                         then RGBA8 rows (float formats tonemapped x/(1+x), then gamma 2.2)
//   tools/re/gpu_trace_view.py turns read-backs into PNGs and contact sheets.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace ff7vr::engine::gpu_trace {

// Dev pipe command `gpu ...`: `gpu names on`, `gpu trace <prefix> [dump <from> <to>] [scale <n>]`,
// `gpu status`.
std::string command(const std::string& args);

// RHI thread, once per stereo frame (from the frame-end command): starts or ends a trace.
// texture: any texture of the engine's device (the eye texture).
void frame_boundary(ID3D11Texture2D* texture);

// The hooks on the immediate context are shared with fixes that have to act on one
// particular draw: they are installed once (the first trace or the first call here, on the
// RHI thread) and stay. An override is called for every DrawIndexed on the immediate
// context (RHI thread); it returns true if it issued the draw itself through `original`.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedOverride = bool (*)(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, DrawIndexedFn original);
bool install_context_hooks(ID3D11Texture2D* texture);
void set_draw_indexed_override(DrawIndexedOverride fn);
// The same for CopyResource: returns true if it did the copy (or its replacement) itself.
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopyResourceOverride = bool (*)(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, CopyResourceFn original);
void set_copy_resource_override(CopyResourceOverride fn);

}  // namespace ff7vr::engine::gpu_trace
