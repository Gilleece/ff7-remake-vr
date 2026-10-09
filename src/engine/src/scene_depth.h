#pragma once
// The engine's scene depth for the headset's depth layer (XR_KHR_composition_layer_depth).
//
// The scene renderer keeps its targets in the static FSceneRenderTargets
// (GSceneRenderTargets, docs/re/engine.md): pooled render targets
// (TRefCountPtr<IPooledRenderTarget>), each {vtable, TargetableTexture +0x08,
// ShaderResourceTexture +0x10}. The scene depth (pool name SceneDepthZ) is the first of
// them, in member order, that is a single-sampled depth-stencil texture of the scene's
// size (both eyes side by side), read without calling into the engine: its RHI texture must
// have the eye texture's vtable, the native texture is at FD3D11Texture2D +0xA0 and must
// answer QueryInterface for ID3D11Texture2D. Its member offset is found on the render thread at the
// first request and kept; the texture is read again every frame (it changes when the
// scene buffers are reallocated).

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace ff7vr::engine::scene_depth {

// Resolves GSceneRenderTargets by signature (any thread, once at start-up).
void init(std::uintptr_t game_base, bool known_build);

// Render thread, after the frame's scene was recorded: the scene depth texture of a
// width x height scene, referenced (the caller releases it), and the format to read its
// depth plane with. Only pooled targets whose RHI texture has the vtable `rhi_vtable` (the
// eye texture's: a 2D texture of the D3D11 RHI) are looked at. Null when it is not found
// (logged at failures 1, 2, 4, 8, ...).
ID3D11Texture2D* acquire(std::uint32_t width, std::uint32_t height, const void* rhi_vtable, DXGI_FORMAT* srv_format);

std::string status();

}  // namespace ff7vr::engine::scene_depth
