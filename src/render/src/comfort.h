// Comfort vignette: while the player moves or turns with the sticks, the periphery of
// both eye images darkens (docs/render.md, "Comfort vignette"). The stick input comes
// from the XInput proxy (SetStickSource in render.h); the darkening itself is done by
// the eye blit (xr::Vignette).
#pragma once

#include "ff7vr/xr/xr.h"

#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::render::comfort {

// [comfort] vignette, vignette_radius, vignette_softness; registers the `comfort` command.
void Start(const Config& cfg);

// Presenting thread, once per submitted stereo frame: the vignette for this frame (strength
// 0 when off or at rest), with each eye's centre where its view axis meets the image.
xr::Vignette Update(const xr::Fov fov[2]);

std::string Command(const std::string& args);

// [picture] sharpen (0..1, 0 = off), the eye images' unsharp mask; live: `sharpen <v>`.
float Sharpen();
// [comfort] vignette as set now (0..1).
float VignetteStrength();

}  // namespace ff7vr::render::comfort
