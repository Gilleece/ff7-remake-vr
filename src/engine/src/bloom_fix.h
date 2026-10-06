#pragma once
// Square Enix's bloom in stereo: the right eye showed a ghost of the left eye's image.
//
// The game's bloom (render targets "BloomReduce" / "BloomBlur", PostProcessHierarchical)
// works per view in a mip chain whose levels all sit at the origin of their targets, and
// the tonemapper reads the result at the origin as well. Its first pass reduces the view's
// full-resolution input (the anti-aliased scene colour, a target that holds both eyes side
// by side) into level 0. The C++ side passes that pass the view's source rectangle (the
// vertex constants carry the right eye's offset), but the pixel shader samples the input
// relative to the origin, so for the right eye level 0 is built from the left half of the
// input: the left eye's image. Everything after that is consistent, so the right eye got the
// left eye's bloom: a soft copy of lamps, windows and lit surfaces at the left eye's image
// positions (docs/re/engine.md, "The right-eye ghost").
//
// The fix: for the first-level pass of a view whose rectangle does not start at the origin,
// the view's rectangle of the input is copied to the origin of a scratch texture of the same
// size and format, which is bound in place of the input for exactly that draw. The pass then
// reads the right eye; nothing of the engine's own textures is changed. Cost: one copy of the
// eye's rectangle per frame (2064x2208 RGBA16F, about 36 MB of copy traffic).
//
// Threads: the pass hook runs on the render thread and appends an RHI command; the command
// arms the swap on the RHI thread, where the next DrawIndexed is the pass's draw.

#include <cstdint>
#include <string>

struct ID3D11Texture2D;

namespace ff7vr::engine::bloom_fix {

// reduce_process: the first-level reduce pass's Process function (signature
// "Bloom reduce pass Process"). Installs the render-thread hook. ao_enabled: the ambient
// occlusion fix below, which needs no address.
bool init(std::uintptr_t reduce_process, bool enabled, bool ao_enabled);
void set_enabled(bool on);
bool enabled();
// RHI thread, once per stereo frame: makes sure the context hooks are in place.
void frame(ID3D11Texture2D* any_texture);
std::string status();

// Square Enix's ambient occlusion has the same fault (its half-size pass at the origin read
// the right view's full-size setup texture relative to the origin, so the right eye got the
// left eye's occlusion: a dark copy of nearby objects at the left eye's image positions).
// Fixed with the same scratch copy, recognised on the RHI thread from the draw sequence
// (bloom_fix.cpp, ao_fix). [stereo] ao_fix, dev command "stereo aofix".
void set_ao_enabled(bool on);
bool ao_enabled();
std::string ao_status();

// With Luma (a ReShade add-on) loaded, the right view's tonemapping draw is run with its input
// 0 shifted to the origin, because Luma's replacement shader reads it relative to the origin
// (bloom_fix.cpp, tonemap_shift). Mode 0 off, 1 on, 2 auto (default: while Luma's add-on is
// loaded). [stereo] tonemap_shift, dev command "tonemapshift [0|1|2]".
void set_tonemap_shift(int mode);
std::string tonemap_shift_status();

}  // namespace ff7vr::engine::bloom_fix
