#pragma once
// Game-specific patches that matter for stereo.

#include <cstdint>
#include <optional>

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

}  // namespace ff7vr::engine::fixes
