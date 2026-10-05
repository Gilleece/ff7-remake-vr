# Stereo device implementation plan

How to make the engine render both eyes itself by installing our own
`IStereoRendering` device and `IStereoRenderTargetManager` into the game's UE 4.18 fork.
Every engine fact used here is documented, with its status, in `docs/re/engine.md`, and
every address has a signature in `tools/re/signatures.json` (names in `code font` below
are entry names there).

Confidence per step: **high** = verified on this exe (live or by reading the calling
code), **medium** = consistent with the disassembly but not exercised yet, **low** = an
educated guess that the first test must confirm.

## Overview

```
process start ── proxy DLL loads (before engine init)
   │  resolve signatures, check the build, hook UEngine::InitializeHMDDevice
UGameEngine::Init (game thread)
   └─ UEngine::Init ─ InitializeHMDDevice ── our hook: original, then install our device into GEngine+0xD50/+0xD58
   └─ create viewport client, FSceneViewport, ULocalPlayer
         └─ ULocalPlayer::PostInitProperties sees the device → allocates StereoViewState/MonoViewState
every frame, game thread
   FSceneViewport::EnqueueBeginRenderFrame → RTM: ShouldUseSeparateRenderTarget / NeedReAllocateViewportRenderTarget / UpdateViewport
   UGameViewportClient::Draw → per eye: GetProjectionData → AdjustViewRect, CalculateStereoViewOffset, GetStereoProjectionMatrix
render thread
   FSceneViewport::InitDynamicRHI (on reallocation) → RTM: CalculateRenderTargetSize, GetNumberOfBufferedFrames, AllocateRenderTargetTexture
   scene renders both eyes into the double-wide separate target
   Slate DrawWindow_RenderThread → device: RenderTexture_RenderThread(BackBuffer, SrcTexture = eye target)
   IDXGISwapChain::Present (our hook) → copy the two eye rects to the XR swapchain / PNG
```

## Step 1. Locate everything at startup

Confidence: high.

The proxy DLL is loaded by the exe's import of `XINPUT1_3.dll`, i.e. before
`UGameEngine::Init` runs, so everything below can be resolved and hooked before the engine
creates its device, viewport and local player.

1. Check the build: `SizeOfImage == 0x5efc000` and PE timestamp `0x698ba49c` (or hash the
   file). On a mismatch, still scan, but refuse to enable stereo when any required
   signature fails.
2. Resolve with the pattern scanner (rule arguments in `signatures.json` map 1:1 to
   `ff7vr::pattern::rip(insn, disp, len)`):

   | Needed for | Entries |
   |---|---|
   | install | `GEngine`, `UEngine::InitializeHMDDevice`, `UGameEngine::vftable`, `UEngine slot InitializeHMDDevice`, `UEngine::StereoRenderingDevice` |
   | layout checks | `IStereoRendering slot *`, `IStereoRenderTargetManager slot *` (each must equal the slot the device implements; refuse to install otherwise) |
   | projection | `GNearClippingPlane` |
   | render target | `UEngine::GameViewport`, `UGameViewportClient::Viewport`, `FSceneViewport::bForceSeparateRenderTarget` (for logging/diagnostics) |
   | late-install fallback | `ULocalPlayer::StereoViewState`, `FSceneViewStateReference::Allocate` |
   | fixes (later) | `RenderLights light sort-key patch site`, `GSystemResolution`, `IConsoleManager::Singleton` |

## Step 2. Install the device

Confidence: high for the mechanism (the engine does exactly this for `-emulatestereo`,
verified live), medium for our hook point.

Hook `UEngine::InitializeHMDDevice` by replacing slot 111 (+0x378) of
`UGameEngine::vftable` (`.rdata`, needs `VirtualProtect`). `UEngine::Init` calls it through
that slot. A vtable slot swap avoids patching code; an inline hook at the function start
works as well.

```cpp
bool __fastcall InitializeHMDDevice_Hook(UEngine* engine) {
    bool had = original(engine);          // keeps -emulatestereo and any HMD plugin behaviour
    if (!vr_enabled()) return had;
    auto& sp = *reinterpret_cast<SharedPtrRaw*>(reinterpret_cast<uint8_t*>(engine) + kStereoRenderingDevice); // 0xD50
    if (sp.object && sp.object != &g_device) {
        log("replacing existing stereo device (vtable RVA {:#x})", vtable_rva(sp.object));
        // Do not release the engine's object: leaking one small object is safer than
        // running its destructor while other code may hold a copy.
    }
    sp.object = &g_device;                // static storage in our DLL
    sp.controller = &g_device_controller; // static storage, refcounts 1/1
    return true;
}
```

The engine's `-emulatestereo` path proves the write is enough: with the device present
before the local player is created, `ULocalPlayer::PostInitProperties` allocates
`StereoViewState` and `MonoViewState` itself (live: three distinct non-null view states).
UEVR has to re-run `PostInitProperties` because it installs its device late; we do not.

Late-install fallback (only if the DLL ever loads after `UEngine::Init`, e.g. injected by
hand): write the two pointers on the game thread, then for each local player call
`FSceneViewStateReference::Allocate` on `LocalPlayer+0xB8` and `+0xE0` when their
`Reference` (+8) is null, and make `NeedReAllocateViewportRenderTarget` return true once.
Confidence: medium.

Reference controller (layout verified, see engine.md §3):

```cpp
struct RefControllerVtbl {
    void (*DestroyObject)(RefController* self);              // engine calls when SharedReferenceCount hits 0
    void* (*ScalarDeletingDtor)(RefController* self, uint32_t flags); // when WeakReferenceCount hits 0
};
struct RefController {
    const RefControllerVtbl* vtbl;
    volatile int32_t SharedReferenceCount;   // start at 1
    volatile int32_t WeakReferenceCount;     // start at 1
    void* Object;                            // &g_device
};
// Both functions are no-ops: the device and controller live for the whole process.
```

## Step 3. Declare the layouts

Confidence: high (every slot matched to an engine call site or to the engine's own fake
device).

Use plain function-pointer tables with an explicit `self` and explicit hidden return
pointers instead of C++ virtual classes. That pins the ABI regardless of how our compiler
treats member functions returning `FVector2D`/`FMatrix` (MSVC returns class types from
member functions through a hidden pointer in RDX; the engine relies on that, see slot 4
and 6).

```cpp
// UE 4.18 basic types as used by this build.
struct FVector   { float X, Y, Z; };
struct FVector2D { float X, Y; };
struct FRotator  { float Pitch, Yaw, Roll; };          // degrees
struct alignas(16) FMatrix { float M[4][4]; };          // row-vector convention
struct FIntPoint { int32_t X, Y; };
struct FIntRect  { FIntPoint Min, Max; };
enum EStereoscopicPass : int32_t { eSSP_FULL = 0, eSSP_LEFT_EYE = 1, eSSP_RIGHT_EYE = 2, eSSP_MONOSCOPIC_EYE = 3 };
enum EPixelFormat : uint8_t { PF_Unknown = 0, PF_B8G8R8A8 = 2 };

struct FRHITexture2D;          // D3D11: vtable[7] GetNativeResource() -> ID3D11Resource* (field +0xA0); SizeX +0x60, SizeY +0x64
struct FTexture2DRHIRef { FRHITexture2D* Reference; };  // intrusive ref (AddRef = vtable/refcount of FRHIResource)
struct FViewport;              // FSceneViewport+8, see engine.md §3
struct FSceneView;
struct UCanvas;
struct FRHICommandListImmediate;
struct IStereoRenderTargetManager;

// IStereoRendering, 14 slots + virtual destructor (slot 14). Thread in brackets.
struct StereoDeviceVtbl {
    bool  (*IsStereoEnabled)(const void* self);                                              // 0  [game, render]
    bool  (*IsStereoEnabledOnNextFrame)(const void* self);                                   // 1  [render, game]
    bool  (*EnableStereo)(void* self, bool stereo);                                          // 2  [game]
    void  (*AdjustViewRect)(const void* self, EStereoscopicPass pass,
                            int32_t* x, int32_t* y, uint32_t* sizeX, uint32_t* sizeY);       // 3  [game]
    FVector2D* (*GetTextSafeRegionBounds)(const void* self, FVector2D* out);                 // 4  [game]
    void  (*CalculateStereoViewOffset)(void* self, EStereoscopicPass pass, FRotator* viewRotation,
                                       float worldToMeters, FVector* viewLocation);          // 5  [game]
    FMatrix* (*GetStereoProjectionMatrix)(const void* self, FMatrix* out, EStereoscopicPass pass); // 6 [game]
    void  (*InitCanvasFromView)(void* self, FSceneView* view, UCanvas* canvas);              // 7  [game]
    bool  (*Unknown8)(void* self);                                                           // 8  never called via GEngine; return false
    void  (*RenderTexture_RenderThread)(const void* self, FRHICommandListImmediate* cmdList,
                                        FRHITexture2D* backBuffer, FRHITexture2D* srcTexture,
                                        FVector2D windowSize);                               // 9  [render]
    void  (*GetOrthoProjection)(const void* self, int32_t rtWidth, int32_t rtHeight,
                                float orthoDistance, FMatrix orthoProjection[2]);            // 10 [game]
    void* (*Unknown11)(void* self);                                                          // 11 never called via GEngine; return nullptr
    IStereoRenderTargetManager* (*GetRenderTargetManager)(void* self);                       // 12 [game, render]
    void* (*GetStereoLayers)(void* self);                                                    // 13 [game]; return nullptr
    void* (*ScalarDeletingDtor)(void* self, uint32_t flags);                                 // 14 no-op
};

// IStereoRenderTargetManager, 8 slots, no destructor.
struct RenderTargetManagerVtbl {
    bool (*ShouldUseSeparateRenderTarget)(const void* self);                                 // 0  [game]
    void (*UpdateViewport)(void* self, bool useSeparateRT, const FViewport* vp, void* sviewport); // 1 [game]
    void (*CalculateRenderTargetSize)(void* self, const FViewport* vp, uint32_t* sizeX, uint32_t* sizeY); // 2 [render]
    bool (*NeedReAllocateViewportRenderTarget)(void* self, const FViewport* vp);             // 3  [game]
    bool (*NeedReAllocateDepthTexture)(void* self, const void* depthTargetRef);              // 4  [render]
    uint32_t (*GetNumberOfBufferedFrames)(const void* self);                                 // 5  [render]
    bool (*AllocateRenderTargetTexture)(void* self, uint32_t index, uint32_t sizeX, uint32_t sizeY,
                                        uint8_t format, uint32_t numMips, uint32_t flags,
                                        uint32_t targetableFlags, FTexture2DRHIRef* outTargetable,
                                        FTexture2DRHIRef* outShaderResource, uint32_t numSamples); // 6 [render]
    bool (*AllocateDepthTexture)(void* self, uint32_t index, uint32_t sizeX, uint32_t sizeY,
                                 uint8_t format, uint32_t numMips, uint32_t flags,
                                 uint32_t targetableFlags, FTexture2DRHIRef* outTargetable,
                                 FTexture2DRHIRef* outShaderResource, uint32_t numSamples);   // 7  [render]
};

struct StereoDevice        { const StereoDeviceVtbl* vtbl;        /* our state follows */ };
struct RenderTargetManager { const RenderTargetManagerVtbl* vtbl; /* our state follows */ };
```

Notes:

- Pointer-sized/`bool`/`float` arguments map to RCX, RDX, R8, R9 / XMM2-3 exactly as the
  engine passes them (checked at each call site, e.g. `WorldToMeters` arrives in XMM3,
  `GetStereoProjectionMatrix` gets the out pointer in RDX and the pass in R8D,
  `RenderTexture_RenderThread` gets `WindowSize` as an 8-byte value in the 5th stack slot).
- Every function runs inside engine threads: no exceptions may escape, keep them short,
  never block on the XR runtime from the game thread inside these calls except where
  stated.

## Step 4. What each function returns

Confidence: high for the engine's expectations, medium for the exact values (first test
confirms).

Shared state written by the XR layer, read atomically (game and render threads both read
it): `eyeWidth`, `eyeHeight` (per-eye render size, e.g. 2500x2600), per-eye FOV tangents,
per-eye pose relative to the head, head pose for the frame being simulated, `active` flag.

| Function | Return / effect |
|---|---|
| IsStereoEnabled, IsStereoEnabledOnNextFrame | `active` (true while VR is on). Must be true before the viewport's first `InitDynamicRHI` with a separate target, because InitDynamicRHI only asks the RTM when slot 1 is true. |
| EnableStereo(b) | store and return the new state (console `stereo on/off`). |
| AdjustViewRect(pass, X, Y, SizeX, SizeY) | The incoming rect is the window rect (GetProjectionData builds it from `Viewport->GetSizeXY()`). Overwrite: `X = (pass == eSSP_RIGHT_EYE) ? eyeWidth : 0; Y = 0; SizeX = eyeWidth; SizeY = eyeHeight`. |
| GetTextSafeRegionBounds | `*out = {0.75f, 0.75f}; return out;` (same as the engine). |
| CalculateStereoViewOffset | Step 6. Do nothing for pass 0 and 3. |
| GetStereoProjectionMatrix | Step 6. |
| InitCanvasFromView(view, canvas) | Minimum: nothing. Better: do what the engine does for mono, copy the view's view-projection matrix into the canvas (engine copies 64 bytes from `view+0x220` to `canvas+0x270`), so HUD markers projected through UCanvas use the eye's matrices. Confidence: low for the offsets' meaning, verify with a HUD marker. |
| Unknown8 / Unknown11 | `false` / `nullptr` (what the engine's fake device returns). |
| RenderTexture_RenderThread | Step 7. |
| GetOrthoProjection | Same as the engine: `out[0] = identity; out[1] = translation(rtWidth * 0.5f, 0, 0)` so stereo canvas items land in the right half. Rarely used. |
| GetRenderTargetManager | `active ? &g_rtm : nullptr`. |
| GetStereoLayers | `nullptr` (no IStereoLayers implementation). |
| ShouldUseSeparateRenderTarget | `active`. |
| UpdateViewport | Remember the `FViewport*` (for diagnostics); nothing else. |
| CalculateRenderTargetSize | `*sizeX = 2 * eyeWidth; *sizeY = eyeHeight;` (render thread). |
| NeedReAllocateViewportRenderTarget(vp) | `true` when `2*eyeWidth x eyeHeight` differs from the size last returned by CalculateRenderTargetSize, or when a reallocation was requested; the engine then sets `bForceSeparateRenderTarget` and calls `UpdateViewportRHI`. |
| NeedReAllocateDepthTexture | `false`. |
| GetNumberOfBufferedFrames | `1`. |
| AllocateRenderTargetTexture | `false`: the engine allocates the target itself with `RHICreateTargetableShaderResource2D` at the size from CalculateRenderTargetSize (UEVR does the same on this exe and got a 7036x2996 BGRA8 target). |
| AllocateDepthTexture | `false`. |
| ScalarDeletingDtor, controller functions | no-ops. |

## Step 5. Size the render target independently of the window

Confidence: high (the mechanism is the engine's; UEVR got 7036x2996 for a 1920x1080
window on this exe).

The separate target's size is whatever `CalculateRenderTargetSize` returns, so
`2 x 2500 x 2600` works with a 1280x720 desktop window. The window size only affects the
back buffer. When the XR runtime changes the recommended size, flip the flag that makes
`NeedReAllocateViewportRenderTarget` return true; the engine reallocates on the next frame.

The scene renderer sizes its own buffers from the view rects, so nothing else needs to
change. `r.ScreenPercentage` still scales the internal resolution inside each eye rect.

Verify with `tools/re/live_check.py`: `bForce = 1`, `RTTSize` and the texture size equal
the requested size, the native resource is a D3D11 texture.

## Step 6. Camera and projection

Confidence: medium (formulas are standard UE/OpenXR; the call sites and argument order
are verified).

`CalculateStereoViewOffset(pass, Rotation, WorldToMeters, Location)` runs on the game
thread once per eye per frame, from `ULocalPlayer::GetProjectionData`, after the game
camera has been resolved (`GetViewPoint`). `Rotation`/`Location` arrive as the game
camera; whatever we write becomes the eye's view.

```
eye        = (pass == eSSP_RIGHT_EYE) ? 1 : 0           // ignore pass 0 and 3
Qcam       = quaternion(Rotation)                       // UE FRotator -> FQuat
Qhead      = toUE(headPose.orientation)                 // tracking space, recentered
Phead      = toUE(headPose.position - recenterOrigin)   // metres
Peye, Qeye = eye pose relative to the head (from xrLocateViews: view pose * inverse(head pose))
Rotation   = rotator(Qcam * Qhead * Qeye)               // Qcam applied last
Location   = Location + Qcam.rotate((Phead + Qhead.rotate(Peye)) * WorldToMeters * worldScale)

toUE: OpenXR (x right, y up, -z forward) -> UE (x forward, y right, z up), metres:
   UE.X = -XR.z,  UE.Y = XR.x,  UE.Z = XR.y
   quaternion: (x, y, z, w)_UE = (-q.z, q.x, q.y, -q.w)  then normalise (or convert via axes)
```

Decoupled pitch (UEVR's default for this game) = remove the camera's pitch and roll from
`Qcam` before composing, so only the camera yaw moves the player's head; recommended for
comfort in third person.

`GetStereoProjectionMatrix(out, pass)` with OpenXR FOV angles of the eye
(`l = tan(angleLeft)` < 0, `r = tan(angleRight)`, `u = tan(angleUp)`, `d = tan(angleDown)` < 0)
and `n = *GNearClippingPlane` (cm; image default 10.0):

```
out->M = {
  { 2/(r-l),          0,                0, 0 },
  { 0,                2/(u-d),          0, 0 },
  { -(r+l)/(r-l),     -(u+d)/(u-d),     0, 1 },
  { 0,                0,                n, 0 } }      // reversed-Z, infinite far plane
return out;
```

This is the same form as the engine's own fake device (row 2 column 3 = 1, row 3
column 2 = near). Square Enix added a 64-byte matrix at `FSceneViewProjectionData+0x90`
that the stereo branch never fills (see engine.md §5); watch for anything aspect-dependent
looking wrong (bloom/lens effects, UI composite).

Pose timing: views for frame N are built on the game thread, rendered on the render thread
one frame later. Read the head pose predicted for frame N's display time once per frame
(first `CalculateStereoViewOffset` call with pass 1, or a `UGameEngine::Tick` hook) and use
the same pose for both eyes and for the composition layer submitted with that frame.
M1 uses the Null backend's fixed/scripted poses, so this matters from M2.

## Step 7. Getting the eye images out

Confidence: high for where the image is, medium for the copy point.

The double-wide target is `FViewport::RenderTargetTextureRHI` (`GEngine->GameViewport->
Viewport + 0x08`) and is also passed as `srcTexture` to `RenderTexture_RenderThread`.
Native texture: `*(ID3D11Texture2D**)((uint8_t*)srcTexture + 0xA0)` (or call vtable slot 7),
DXGI format `B8G8R8A8_TYPELESS` (UEVR log: format 90). Left eye is
`[0, eyeWidth) x [0, eyeHeight)`, right eye `[eyeWidth, 2*eyeWidth)`.

Recommended copy point: the render thread, inside our `IDXGISwapChain::Present` hook
(Present is called on the render thread at the end of `RHIEndDrawingViewport`).

1. In `RenderTexture_RenderThread` store `srcTexture` (and, for the desktop mirror, the
   back buffer) in a render-thread variable. Do not touch the D3D11 context here: this is a
   shipping build, so the immediate RHI command list is not in bypass mode and the scene's
   commands may still be queued when Slate calls us (INFERRED from UE 4.18; can be checked
   by copying here and comparing with the Present copy).
2. In the Present hook, everything queued for the frame has been executed on the immediate
   context. `CopySubresourceRegion` each eye's box into its XR swapchain image (or into a
   staging texture for the Null backend's PNG dump), then submit, then call the real
   Present. Same-family formats (`B8G8R8A8_TYPELESS` → `B8G8R8A8_UNORM[_SRGB]`) copy
   directly; anything else needs a shader blit.
3. Desktop mirror (optional): blit the left eye into the back buffer before Present.
   Because the scene no longer goes to the back buffer, the window shows only Slate UI
   (nearly nothing in this game) unless we do this.

Note for coexistence: ReShade/Luma (`dxgi.dll` proxy) also hooks Present; it is renamed
away during development runs.

## Step 8. Game-specific fixes to add after first light

| Fix | When | Confidence |
|---|---|---|
| In-game UI: set `r.InGameUI.FixedWidth/FixedHeight` (cvar API, engine.md §7) to a fixed size and handle the UI composite (it is drawn into each eye at zero parallax, cropped 1:1 from the full-size UI). M3 work: redirect `InGameUIRenderTarget` to a quad layer like the community plugin. | M3 | medium |
| `GSystemResolution`: the UI canvas preset switches at `ResX > 1920`; with the fixed-size cvars set this no longer matters. | M3 | high (code read) |
| Light patch: write `0x60` to the byte at RVA `0x22351b1` (`RenderLights light sort-key patch site` + 1). Test scenes with and without; it is a widescreen fix too. | M1 test, M3 | medium |
| HZB occlusion: keep `r.HZBOcclusion` at the game's value. Each eye has its own view state when the device is installed at startup (verified), which is what HZB needs. Only disable if the right eye shows popping. | M1 test | medium |
| Instance culling: nothing to do (the UEVR toggle targets a UE5 cvar that does not exist here). | - | high |
| Movies: detect `MediaPlayer::IsPlaying` (reflection) and present them as a flat quad. Implemented as `[stereo] movie_screen` (stereo held off while a movie plays, the virtual screen shows it); class and function found live, a playing movie not seen yet. | M3 | medium |
| Vignette: zero `VignetteIntensity` at the end of `FPostProcessSettings`' constructor or lower `r.Tonemapper.Quality`. | M3 | low |

## Pitfalls specific to this game

1. **Install before the local player exists.** Otherwise the right eye shares the left
   eye's view state (TAA history, eye adaptation, occlusion), unless the fallback in step 2
   allocates them.
2. **AdjustViewRect gets the window rect**, not the render target rect. Always overwrite
   all four values.
3. **IsStereoEnabledOnNextFrame gates the render target manager** in `InitDynamicRHI`.
   If it is false at that moment, the target is allocated at window size; flip a
   reallocation afterwards.
4. **UEVR renders this game twice per frame** ("native stereo fix": one view per family,
   second family into a separate target). We start with true two-view rendering, which the
   engine's own `-emulatestereo` path showed working in menus and gameplay. If artefacts
   appear only in the right eye (TAA smearing, motion blur, SSR, decals, lights), compare
   with `-emulatestereo` first, then consider UEVR's approach as a fallback.
5. **The light sort-key bug** appears at non-16:9 aspect ratios; eye views are about 0.96
   aspect at 2500x2600.
6. **No `GetDesiredNumberOfViews` in this build**; the monoscopic far field (third view)
   is off, keep it off.
7. **The fake device vtable is not used for inline checks** in this build, so no vtable
   check patching is needed (UEVR's `patch_vtable_checks` is unnecessary here).
8. **Thread safety:** slots 0, 1 and 12 and the RTM are called from both the game and the
   render thread. Keep the shared state in atomics or a seqlock.
9. **Never free the device or controller**; the engine keeps copies of the shared pointer.
10. **Shipping RHI command list:** do raw D3D11 work in Present, not in engine callbacks on
    the render thread (step 7).

## Build and test order

1. **Device skeleton, Null backend, fixed 2 x 1280x1440 eyes, 90 deg symmetric FOV, zero
   IPD.** Expect: log line from the hook; `live_check.py` shows our vtable at
   `GEngine+0xD50`, three view states, `bForce = 1`, `RTTSize = 2560x1440` with a
   1280x720 window. Dump both eye rects to PNG at Present. (Confidence high.)
2. **IPD and per-eye offset** (fixed 64 mm): PNGs show horizontal parallax, near objects
   shift more. Compare against an `-emulatestereo` capture of the same save. (high)
3. **Asymmetric FOV from real headset numbers** (e.g. the Quest 3 tangents in the UEVR
   log). Check the horizon lines up across eyes. (medium)
4. **Scripted head poses** from the Null backend (yaw sweep, then position). Check the
   world stays fixed while the head moves. (medium)
5. **Full resolution** (2 x 2500x2600) with a small window; measure frame time. (high)
6. **Fix toggles**: light patch on/off, `r.HZBOcclusion` on/off, compare right eye
   artefacts. (medium)
7. Hand over to M2: OpenXR backend, pose prediction and frame pacing.
