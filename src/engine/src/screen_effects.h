#pragma once
// The game's screen-wide blur and colour fringe in stereo.
//
// Square Enix added post-process settings of their own to FPostProcessSettings. Two of them
// put an effect over the whole picture: ChromaticAberrationIntensity (their own colour
// separation pass, PostProcessChromaticAberration, before the bloom: a blur outwards from the
// image centre with red and blue fringes) and DirectionalBlurDistortionIntensity (a full-screen
// directional blur drawn by their distortion pipeline). Both default to 0 and are blended in by
// cameras, volumes or particle effects. Tactical mode (the battle command menu) shows the
// particle effect FX_R_WaitFilter_01, whose post-process module sets ChromaticAberrationIntensity
// (a curve up to 2.0) and ChromaticSaturation (0.3, a greying done in the same pass, kept). In a
// headset the blur smears everything around a point that is not where the eye looks, and the
// fringes double the lenses' own.
//
// The mechanism: a hook on FSceneView::EndFinalPostprocessSettings, which every view runs once
// per frame after all blends (cameras, camera modifiers, volumes) and after the engine's own
// quality clamps (for example r.SceneColorFringeQuality 0 zeroing SceneFringeIntensity). While
// the engine renders in stereo, the detour zeroes the two fields in the view's final settings
// (and the stock SceneFringeIntensity), so the blur and the fringes are gone whichever source
// blended them in (the directional blur's passes are not added at all). Outside stereo (the flat game, the virtual-screen phase of cutscenes and
// movies) nothing is changed.
//
// docs/engine-module.md, "Screen blur and colour fringe"; docs/re/engine.md, "Square Enix's
// post-process settings".

#include <cstdint>
#include <string>

namespace ff7vr::engine::screen_effects {

// end_final: FSceneView::EndFinalPostprocessSettings (signature of the same name; its stores
// of the final SceneFringeIntensity and BloomIntensity are checked by the caller). remove:
// true = the effects are removed in stereo ([comfort] screen_blur = 0).
bool init(std::uintptr_t end_final, bool known_build, bool remove);
void set_remove(bool remove);
bool remove();
std::string status();
// Reads the field offsets from the engine's reflection data (ScriptStruct PostProcessSettings)
// and compares them with the ones the hook uses.
std::string layout();
// Test: writes a value into every view's final settings before the removal runs, as if the
// game had blended it in (offset into FPostProcessSettings, hex). Up to four; "clear" ends it.
std::string force(const std::string& offset, const std::string& value);
// Reverse engineering: the post-process objects (particle modules, components) whose path
// contains `name` (for example the tactical-mode effect FX_R_WaitFilter_01), with their
// float and bool values and those of the distributions they point to.
std::string vfx(const std::string& name);

}  // namespace ff7vr::engine::screen_effects
