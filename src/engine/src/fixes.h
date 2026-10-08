#pragma once
// Game-specific patches that matter for stereo.

#include <d3d11.h>

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

// The patch as a stereo fix ([stereo] light_fix, `stereo lightfix`): applied while the
// engine renders in stereo and the fix is wanted, removed otherwise, so the flat game keeps
// the game's code. Without it the tiled lighting pass that renders the lights with bit
// 0x40 leaves white, tile-shaped blocks on skin in the eye views (docs/engine-module.md,
// "Skin lighting fix"). set_light_fix: any thread; light_fix_stereo: called at every
// stereo/mono transition (game thread). False if the patch site is not available.
bool set_light_fix(bool wanted);
bool light_fix_wanted();
void light_fix_stereo(bool stereo_active);

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

// Screen-space reflections per eye ([stereo] ssr_per_eye, `stereo ssrfix`). Square Enix's
// reflection pass runs once per view, each time as one full-screen triangle over the whole
// side-by-side target, and the next pass of that view reads only its own half; the other
// half is overwritten by the next view's run before anything reads it. With the switch on,
// each run is limited by a scissor rectangle to its view's half (the first run of a frame
// to the left half, the second to the right half), which halves the pass's cost and leaves
// every pixel that is read afterwards as it was. Recognised on the RHI thread by its shape:
// a full-screen triangle over a whole R16G16B16A16 target at least 1.5 times as wide as
// high, without depth, reading a hierarchical depth texture (R16_FLOAT with mips).
// docs/engine-module.md, "Reflections per eye".
void set_ssr_per_eye(bool on);
bool ssr_per_eye();
void set_ssr_poison(int mode);  // test: 1 fills the half a run skipped with a loud colour, 2 its own half (control), 0 off

// Right-eye reflections ([stereo] ssr_fix, `ssr fix 0|1`). The run of the view in the right
// half of the target computes that view's reflections at the origin of the target and leaves
// its own half, the one its composite reads, at zero: without the fix the right eye has no
// screen-space reflections. With it, that draw renders into a scratch target half as wide
// (the half at the origin) and the result is copied into the right half. Works with and
// without ssr_per_eye. docs/engine-module.md, "Reflections per eye".
// Mode 2 (`ssr fix 2`) instead clears both views' reflection runs: no screen-space reflections
// in either eye, so the eyes match (the fixed right view's reflections are not the right view's
// own; docs/engine-module.md, "The right eye's reflections are not its own").
void set_ssr_fix(int mode);  // 0 off, 1 on, 2 both eyes without
bool ssr_fix();
bool ssr_wants_hooks();  // either switch on: the context hooks are needed
// RHI thread, before every DrawIndexed: places a fixed right-eye result once the draw that
// reads it (the right view's composite) shows where the right view's rectangle starts.
void ssr_before_draw(ID3D11DeviceContext* ctx);

// RHI thread: true if the draw was recognised and run (limited to its half).
bool ssr_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
              void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT));
// The hierarchical depth chain nothing reads ([stereo] hzb_skip, `hzb 0-4`). Per view the engine
// builds two chains from the view's depth with one two-target draw for mip 0 and one draw per
// further mip and chain; only the second chain is read (ambient occlusion, reflections, ray traced
// shadows). Mode 1 leaves out the further mips of the first chain; 2 and 3 fill that chain with
// near / far depth instead (tests: nothing may change), 4 fills the further mips of the read chain
// (control: the picture must change). docs/engine-module.md, "Hierarchical depth".
void set_hzb_skip(int mode);
int hzb_skip();
void hzb_tick();  // game thread, every frame: follows r.HZBOcclusion (only 0 lets the chain be left out)
// RHI thread: true if the draw was handled (left out or replaced).
bool hzb_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
              void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT));
std::string hzb_status();

// RHI thread, once per frame (frame end).
void ssr_frame();
std::string ssr_status();

}  // namespace ff7vr::engine::fixes
