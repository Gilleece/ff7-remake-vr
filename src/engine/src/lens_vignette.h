#pragma once
// The game's lens vignette in stereo. The engine's tonemapper darkens the image towards its
// edges ("natural vignetting", cos^4 of the angle from the view axis, scaled by the
// post-process setting VignetteIntensity, which this game defaults to 1.0 where stock UE4 uses
// 0.4). In a headset each eye gets its own dark rim: with the vignette removed, the edge of a
// 3072x3264 eye image is about 4.7 times as bright and its corners about 16 times (captures).
//
// The fix scales the default VignetteIntensity while the engine renders in stereo: a hook on
// FPostProcessSettings' default constructor, which builds every view's final post-process
// settings each frame before cameras and volumes blend theirs in. A camera or volume that
// overrides the vignette itself keeps its own value. The constructor's store of the default
// (1.0 at +0x418) is checked at start-up, and a value other than 1.0 found after the
// constructor is left alone (counted as "unexpected").

#include <cstdint>
#include <string>

namespace ff7vr::engine::lens_vignette {

// ctor: FPostProcessSettings::FPostProcessSettings (signature of the same name); scale: the
// share of the game's vignette kept in stereo (0 = none, 1 = as in the flat game).
bool init(std::uintptr_t ctor, float scale);
void set_scale(float scale);
float scale();
std::string status();

}  // namespace ff7vr::engine::lens_vignette
