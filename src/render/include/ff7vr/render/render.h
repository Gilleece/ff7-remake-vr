#pragma once
// ff7vr render module: owns D3D11 inside the game process and drives the XR
// session (src/xr). See docs/render.md for modes, ini keys and dev commands.
//
// Screen mode (default, and the fallback whenever stereo is not active): every
// frame the game's back buffer is shown in the headset on a flat virtual
// screen (a world-locked quad layer).
//
// Stereo mode: the engine renders both eyes side by side and hands the image
// over with SubmitStereoFrame. The interface for that is below.
//
// ============================ STEREO INTERFACE =============================
// Threads: GT = the engine's game thread, RT = the thread that calls
// IDXGISwapChain::Present (the render thread in this build; logged as
// "Present on <tid>" at start-up). All D3D11 work of this module happens on
// the RT inside its Present hook, where the frame's commands have been
// executed on the immediate context.
//
//   SetMode(Mode::Stereo)                 any thread, when the engine wants stereo
//   GT, frame start:
//     StereoFrame f = BeginGameFrame();   blocks for XR frame pacing (xrWaitFrame)
//     if (!f.stereo) render mono;         the screen layer shows the back buffer
//     else render 2 x EyeSetup::eyeWidth by eyeHeight side by side, eye e
//          with f.views[e] (tracking space; xr_math.h converts to Unreal),
//          and carry f.frameId along with the frame to the RT
//   RT, after the frame's scene was submitted to the RHI and before Present
//   (e.g. in IStereoRendering::RenderTexture_RenderThread):
//     SubmitStereoFrame({f.frameId, sideBySideTexture, ...});   only records it
//   RT, Present (this module): copies the eye rects into the XR swapchains and
//     ends the XR frame.
//
// Rules:
//   * Every frame BeginGameFrame returned with stereo == true is ended by one
//     of the following Presents, in order: with the stereo image if
//     SubmitStereoFrame named that frame, otherwise with the screen layer
//     showing the back buffer. A frame never blocks the runtime for long.
//   * BeginGameFrame returns stereo == false (without blocking) when the mode
//     is Screen, no XR session is running, or earlier frames have not been
//     presented for a while (game paused, minimised, loading without Present).
//   * The texture given to SubmitStereoFrame must stay alive until the next
//     Present on the RT returns (the module keeps a reference until then).
//   * Images are kept per frame id (the four most recent ids). A second
//     SubmitStereoFrame for the same id replaces the first, so one image may
//     be offered for every frame the coming Present may end.
//   * A frame without an image re-shows the previous stereo image if it is
//     younger than 300 ms, otherwise shows the screen layer; when the game
//     thread stops calling BeginGameFrame for 100 ms the module starts frames
//     itself (docs/render.md, "Switching between stereo and the screen").
//   * Mode changes take effect at frame boundaries; both directions are safe
//     at any time (menus, movies and cutscenes switch back to Screen).
// ===========================================================================

#include "ff7vr/core/startup_context.h"
#include "ff7vr/xr/xr.h"

#include <cstdint>

struct ID3D11Texture2D;

namespace ff7vr::render {

// Loader entry points (src/loader/startup.cpp). start() installs the D3D11
// hooks and starts the XR thread; it returns quickly and never blocks on the game.
bool start(const StartupContext& ctx);
void stop();

enum class Mode { Screen, Stereo };

// Any thread.
void SetMode(Mode mode);
Mode GetMode();

// What the engine needs to size its render target and build projections.
struct EyeSetup {
    uint32_t eyeWidth = 0, eyeHeight = 0;  // per-eye render size (runtime recommendation x [xr] resolution_scale)
    xr::Fov fov[2]{};                      // from the most recent frame (or the runtime's first frame)
    float refreshHz = 0;
};
// Any thread. False while no XR session is initialised.
bool GetEyeSetup(EyeSetup* out);

struct StereoFrame {
    bool stereo = false;      // false: render mono this frame
    uint64_t frameId = 0;     // pass to SubmitStereoFrame
    bool shouldRender = true; // false: the runtime will not show this frame (render cheaply or skip the scene)
    xr::View views[2]{};      // eye poses and FOVs, tracking space after recenter
    xr::Pose head{};
    int64_t predictedDisplayTime = 0;    // runtime clock, ns
    int64_t predictedDisplayPeriod = 0;  // ns
};
// GT, once per frame at frame start.
StereoFrame BeginGameFrame();

struct StereoSubmit {
    uint64_t frameId = 0;                          // from BeginGameFrame
    ID3D11Texture2D* texture = nullptr;            // side-by-side image (e.g. B8G8R8A8_TYPELESS)
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // required for typeless textures (B8G8R8A8_UNORM for the engine's target)
    xr::ColorEncoding encoding = xr::ColorEncoding::Srgb;
    xr::Rect eyeRects[2]{};                        // width/height 0 = left / right half
    bool haveRenderedViews = false;                // true: renderedViews are what the image was rendered with
    xr::View renderedViews[2]{};                   // (default: the views BeginGameFrame returned)
};
// RT, before the frame's Present. Records the image; the Present hook submits it.
void SubmitStereoFrame(const StereoSubmit& submit);

// Any thread. GPU time of the most recently measured stereo frame (from the start of
// its scene to its Present, timestamp queries; measured by the foveation module, so
// only while foveated rendering is initialised) and the number of frames measured
// so far. False while nothing has been measured.
bool GetGpuFrameTime(float* ms, uint64_t* samples);

// ============================ UI LAYER =====================================
// In stereo the game's in-game UI (HUD, command menu, menus, dialogue) is drawn
// once into its own texture and shown on a quad layer floating in front of the
// user, instead of being composited into each eye at zero parallax. The engine
// module redirects it (src/engine/src/ui_layer.cpp) and reports each frame's UI
// texture from the presenting thread, before that frame's Present:
//
//   if (UiLayerWanted()) { ...keep the UI out of the eye images... }
//   presenting thread:  SubmitUiLayer({texture, ..., redirected = true});
//   Present (this module): the frame's stereo image goes out as the projection
//                          layer and the UI texture is copied into the UI quad.
//
// Frames ended with a stereo image and a redirected UI show the quad; frames
// that re-show the last stereo image keep the quad's last image; frames shown on
// the virtual screen never show it (their UI is in the game's own image).
// ===========================================================================
struct UiLayerSource {
    ID3D11Texture2D* texture = nullptr;            // holds this frame's UI in (0,0,width,height)
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // typed format to read it with (required for typeless textures)
    xr::ColorEncoding encoding = xr::ColorEncoding::Linear;
    uint32_t width = 0, height = 0;
    xr::SourceAlpha alpha = xr::SourceAlpha::PremultipliedInverted;
    bool redirected = true;  // false: the UI was composited into the frame as usual (reported only for diagnostics)
};
// Any thread. True while the UI should go to its own layer: stereo mode, an XR
// session runs and the layer is switched on ([ui] layer, `ui on|off`).
bool UiLayerWanted();
// Any thread. True while a `ui dump` waits for the next UI texture.
bool UiDumpRequested();
// Presenting thread, before the frame's Present (after the frame's UI was drawn).
void SubmitUiLayer(const UiLayerSource& source);

// ======================== FIXED FOVEATED RENDERING =========================
// In stereo the periphery of each eye is shaded at a lower rate than its centre
// (NVIDIA variable rate shading through NVAPI; docs/render.md, "Foveated
// rendering"). The engine module marks, in the order of the frame's GPU work
// (RHI commands, executed on the presenting thread), where the scene of a stereo
// view family is rendered; only render targets bound in between get the mask:
//
//   presenting thread, before the scene's first draw:   FoveationSceneBegin(eyes)
//   presenting thread, before the UI pass / post-processing: FoveationSceneEnd()
//
// Nothing else is affected: mono frames, screen mode, the UI pass, the
// post-processing chain, the desktop mirror and this module's own work.
// ===========================================================================
struct FoveationEye {
    xr::Rect rect{};               // the eye's view rect in the scene render targets, pixels
    float projScaleX = 0, projScaleY = 0;    // projection matrix [0][0], [1][1]
    float projOffsetX = 0, projOffsetY = 0;  // [2][0], [2][1]: where the view axis is, in NDC
};
// Any thread. True while foveated rendering is switched on and not known to be unsupported.
bool FoveationWanted();
// Presenting thread (inside the frame's command stream).
void FoveationSceneBegin(const FoveationEye eyes[2]);
void FoveationSceneEnd();
// Presenting thread, at the end of each stereo frame, from an upscaler (DLSS): whether it
// upscaled this frame and the input's share of the output width. While it does, foveation
// shades one step finer than its preset ([foveation] dlss_finer).
void FoveationSetUpscaling(bool upscaled, float inputShare);

}  // namespace ff7vr::render
