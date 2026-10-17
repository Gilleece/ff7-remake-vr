#pragma once
// The game's screen-wide blur and colour fringe in stereo.
//
// Square Enix added post-process settings of their own to FPostProcessSettings. Two of them
// put an effect over the whole picture: DirectionalBlurDistortionIntensity (a full-screen
// directional blur drawn by their distortion pipeline, around a centre and with a fall-off
// set by the DirectionalBlurDistortion* fields that follow it) and
// ChromaticAberrationIntensity (their own colour-separation pass, PostProcessChromaticAberration,
// which runs before the bloom). Both default to 0 and are blended in by cameras, volumes or
// effects, for example while the battle command menu is open (tactical mode). In a headset
// they are hard on the eyes: the blur smears the whole view around a point of the flat image,
// and each eye's image centre is not where its lens is.
//
// The mechanism: a hook on FSceneView::EndFinalPostprocessSettings, which every view runs once
// per frame after all blends (cameras, camera modifiers, volumes) and after the engine's own
// quality clamps (for example r.SceneColorFringeQuality 0 zeroing SceneFringeIntensity). While
// the engine renders in stereo, the detour zeroes the two fields in the view's final settings
// (and the stock SceneFringeIntensity), so the passes are not added at all, whichever source
// blended them in. Outside stereo (the flat game, the virtual-screen phase of cutscenes and
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

}  // namespace ff7vr::engine::screen_effects
