#pragma once
// Game-specific patches that matter for stereo.

#include <cstdint>
#include <optional>
#include <string>

namespace ff7vr::engine::fixes {

// Light sort-key patch in FDeferredShadingSceneRenderer::RenderLights: the immediate of
// `mov esi, 0x40` becomes 0x60 so lights that get sort-key bit 0x40 also get 0x20 (the
// same one-byte change as the community "light flag" / widescreen fix). See
// docs/re/engine.md section 6. Any thread; the byte is written atomically.
bool set_light_patch(bool on);
std::optional<bool> light_patch();  // nullopt: site not found or holds an unexpected value

// Square Enix's ULocalPlayer::CalcSceneView replaces the view rect with
// (0, 0, GSystemResolution) when the viewport's window mode is windowed fullscreen, after
// the stereo device has set the eye rects. While stereo renders, the `jne` that skips
// that replacement is made unconditional. Game thread (the only caller of CalcSceneView).
bool set_view_rect_patch(bool on);
std::optional<bool> view_rect_patch();

// Square Enix's FSceneRenderTargets::Allocate sizes the scene buffers from
// GSystemResolution x r.ScreenPercentage, not from the views, so while stereo renders
// GSystemResolution has to cover the eye render target. apply() remembers the game's
// value the first time and writes {width, height}; restore() puts the game's value back
// if nothing else changed it since. Game thread.
void apply_system_resolution(std::int32_t width, std::int32_t height);
void restore_system_resolution();
bool system_resolution_overridden();

// The game window while VR renders. In windowed fullscreen (GSystemResolution.WindowMode
// 1) Square Enix's renderer replaces view and pass rectangles with the full screen in many
// places, which breaks both eyes; in exclusive fullscreen (0) losing focus minimises the
// window and the game stops presenting, and on reactivation the engine re-requests
// GSystemResolution as a display mode. So the first time stereo becomes active while the
// game is in either fullscreen mode, the window is switched to a normal window of the
// configured size (r.SetRes "<w>x<h>w"); vr_window_leave() puts the game's mode back
// (when stereo is switched off by the user). Game thread for enter; leave queues the
// change for the game thread. size "0" (or empty) disables the switch.
void set_vr_window_size(const std::string& size);
void vr_window_enter();
void vr_window_leave();
std::string vr_window_status();

}  // namespace ff7vr::engine::fixes
