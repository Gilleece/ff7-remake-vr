#pragma once
// Desktop mirror: with stereo on, the engine renders the scene into its separate eye
// target and the window would only show Slate's UI. The stereo device's
// RenderTexture_RenderThread queues an RHI command (rhi_command.h) that blits part of the
// eye target into the window's back buffer, before Slate draws on top of it.
//
// Queued on the render thread, executed on the RHI thread at a point where the D3D11
// immediate context has executed the frame's scene rendering. Every piece of pipeline
// state it changes is saved and restored, so the engine's D3D11 state cache stays valid.

#include "ff7vr/engine/stereo_host.h"

#include <string>

struct ID3D11Texture2D;

namespace ff7vr::engine::mirror {

enum class Mode : int {
    Off = 0,
    Left,   // left eye, whole, letterboxed
    Right,  // right eye, whole, letterboxed
    Both,   // both eyes side by side, letterboxed
    Crop,   // left eye, centre crop with the window's aspect ratio (default)
};
const char* to_string(Mode m);
bool parse_mode(const std::string& s, Mode& out);

// Render thread. Queues the blit into the engine's command list; false if it could not be
// queued (layout check failed, list not recording) or mode is Off.
bool enqueue(void* cmd_list, ID3D11Texture2D* eye_texture, ID3D11Texture2D* back_buffer, const EyeRect& left,
             const EyeRect& right, Mode mode);

// Typed format for reading a typeless engine render target; UNKNOWN if not supported.
DXGI_FORMAT typed_view_format(DXGI_FORMAT f);

}  // namespace ff7vr::engine::mirror
