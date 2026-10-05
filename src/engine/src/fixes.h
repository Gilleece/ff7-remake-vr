#pragma once
// Game-specific patches that matter for stereo.

#include <optional>

namespace ff7vr::engine::fixes {

// Light sort-key patch in FDeferredShadingSceneRenderer::RenderLights: the immediate of
// `mov esi, 0x40` becomes 0x60 so lights that get sort-key bit 0x40 also get 0x20 (the
// same one-byte change as the community "light flag" / widescreen fix). See
// docs/re/engine.md section 6. Any thread; the byte is written atomically.
bool set_light_patch(bool on);
std::optional<bool> light_patch();  // nullopt: site not found or holds an unexpected value

}  // namespace ff7vr::engine::fixes
