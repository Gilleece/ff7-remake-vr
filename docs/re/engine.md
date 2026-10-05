# Engine facts for ff7remake_.exe (stereo rendering)

Reverse-engineering notes for the game executable `End\Binaries\Win64\ff7remake_.exe`
(Unreal Engine 4.18, modified by Square Enix). Everything here is what the stereo
rendering device needs: where the engine keeps the device, how it calls it, how the
viewport render target is sized and allocated, how the camera is built, and what is known
to break in stereo.

All addresses are RVAs (offset from the module base). The module is ASLR-relocated, so
add the runtime base. Every fact has a signature in `tools/re/signatures.json`;
`python tools/re/signatures.py` resolves them all against an exe and reports anything that
moved. The tools are described in [Tools](#tools).

Status labels used below:

- **LIVE**: checked in a running game process by reading its memory (`tools/re/live_check.py`)
  or by its visible behaviour.
- **STATIC**: read from the disassembly of this exe; the calling code was inspected.
- **UEVR**: reported by UEVR on this exe (its log from a run on this build) and consistent
  with the disassembly.
- **INFERRED**: deduced from UE 4.18 source knowledge or from indirect evidence; not
  checked directly.

## 1. Executable identity and obstacles

| Item | Value |
|---|---|
| File version | 1.0.0.7 (ProductVersion 1.0.0.7, InternalName `ff7remake`) |
| Size | 96,927,560 bytes |
| SHA-256 | `25b54c345c94ee9f6c6876b9c7537a69e6b0eef25f322207c32a8ddcee816635` |
| PE timestamp | `0x698ba49c` (2026-02-10 21:35:24 UTC) |
| Image base / SizeOfImage | `0x140000000` / `0x5efc000`, ASLR on, CFG off |
| PDB path in debug directory | `End-Win64-Shipping.pdb` (not shipped) |
| Sections | `.text` 0x1000 (0x409107e), `.rdata` 0x4093000, `.data` 0x5356000, `.pdata` 0x5a1e000, `.rodata`, `_RDATA`, `.rsrc`, `.reloc` |

Obstacles (STATIC unless stated):

- **No packer, no DRM wrapper.** Entry point is in `.text`, section entropies are normal
  (`.text` 6.58), there is no `.bind` section (no SteamStub), no Denuvo/VMProtect/Themida
  markers, no TLS callbacks. `.pdata` covers the code, so function bounds are exact.
- One string match for `arxan` and one `IsDebuggerPresent` import exist; no integrity
  checking was observed. Reading process memory with `PROCESS_VM_READ` works (LIVE), and
  UEVR patches code and vtables in this exe without problems (UEVR log: inline hooks on
  `UGameEngine::Tick`, `FSceneView` constructor, Slate draw; a 1-byte patch in
  `RenderLights`).
- **No RTTI** (`/GR-`): `vtable[-1]` is not a CompleteObjectLocator. Vtables are found
  through the constructor that stores them (`lea reg,[rip+vtable]`), and their end through
  the next slot that is itself referenced by code.
- Steam API is a delay import (`steam_api64.dll`). Starting `ff7remake_.exe` directly works
  when `SteamAppId`/`SteamGameId` are set in the environment and Steam is running
  (LIVE; the dev scripts do this).
- The exe was last modified on 2026-03-05, the UEVR log is from 2026-09-12 and its
  SizeOfImage matches; every UEVR address quoted here was re-found by signature in the
  current file.

## 2. Engine globals

| Name | RVA | How to find it again | Status |
|---|---|---|---|
| `GEngine` (`UEngine*`, the `UGameEngine`) | `0x5831188` | String `L"CALIBRATEMOTION"` has one code reference; the `mov rcx,[rip+x]` just before it loads GEngine. Signature `GEngine`. | LIVE: points to an object whose vtable is `UGameEngine::vftable` |
| `GUObjectArray` (`FUObjectArray`) | `0x53bd470` | `UEngine::Init` reads `GUObjectArray+0x10` (`mov rax,[rip+x]; lea rdx,[rax+rcx*8]` after `lea rcx,[rax+rax*2]`). Signature `GUObjectArray` (rule subtracts 0x10). | STATIC + UEVR (same address, 1,048,576 max objects) |
| FName table (`FNamePool`, static object) | `0x5981300` | The FName entry resolver called from `FName::ToString` (RVA `0x1ce89a0`) does `lea rcx/rdx,[rip+pool]`. Signature `FNamePool`. | STATIC |
| `GWorld` | not located | Not needed for M1: the stereo device receives `WorldToMeters` as an argument. Reach the world through `GEngine->GameViewport` or `GameInstance->WorldContext` if needed. | open |
| `GMalloc` (`FMalloc*`) | `0x5825680` | Any `mov rcx,[GMalloc]; test rcx,rcx; jne; call CreateGMalloc` (e.g. in the fake device destructor). Malloc `+0x10`, Realloc `+0x18`, Free `+0x20`. | STATIC + UEVR |
| `IConsoleManager::Singleton` | `0x57f9ca0` | In every static cvar registration, followed by a call to `IConsoleManager::SetupSingleton` (`0x1c1bda0`) when null. | STATIC + UEVR |
| `GSystemResolution` (`FSystemResolution`) | `0x53e3ae0` | The only `cmp dword ptr [rip+x], 0x780` (1920) in `.text` (in the in-game UI canvas size function `0x1508810`). `{int32 ResX, ResY; ...}`. | STATIC + UEVR plugin log |
| `GNearClippingPlane` (float, cm) | `0x53a49e4` | Read by the engine's own `FFakeStereoRendering::GetStereoProjectionMatrix`. Initial value in the image: 10.0. | STATIC |
| `GDynamicRHI` | `0x582c478` | Used by Slate's `DrawWindow_RenderThread` (`call [vtable+0x2f8]` = `RHIGetViewportBackBuffer`). | STATIC |

FName details (STATIC): this fork uses the newer name pool layout (UE 4.23 style).
`FName` = `{uint32 ComparisonIndex, uint32 Number}`; entry address =
`*(uint8**)(FNamePool + 0x10 + (id >> 16) * 8) + 2 * (id & 0xFFFF)`; entry header is a
`uint16` with `Len = header >> 6`, wide flag in bit 0, ANSI or UTF-16 characters follow.
`GUObjectArray` is not chunked: `FUObjectItem* Objects` at +0x10, item size 0x18,
`MaxElements` +0x18, `NumElements` +0x1C (UEVR). `UObject`: vtable +0, `ObjectFlags` +8,
`InternalIndex` +0xC, `ClassPrivate` +0x10, `NamePrivate` +0x18, `OuterPrivate` +0x20,
size 0x28; `ProcessEvent` is vtable slot 64 (UEVR).

### UEngine / UGameEngine members

| Member | Offset | Source | Status |
|---|---|---|---|
| `TSharedPtr<IStereoRendering, ThreadSafe> StereoRenderingDevice` | `0xD50` (object), `0xD58` (reference controller) | Last statement of `UEngine::InitializeHMDDevice`: `cmp qword [r14+0xd50],0; setne al` | LIVE (fake device found there), STATIC, UEVR |
| `TSharedPtr<IXRTrackingSystem, ThreadSafe> XRSystem` | `0xD60` / `0xD68` | `InitializeHMDDevice`: `lea r15,[r14+0xd60]; cmp [r15],0` before the `L"nohmd"` check; HMD module result is stored there | LIVE (null in an unmodded run), STATIC |
| `TSharedPtr<FSceneViewExtensions> ViewExtensions` | `0xD70` | `ULocalPlayer::GetProjectionData`: `GEngine->ViewExtensions->GatherActiveExtensions()` | LIVE (non-null), STATIC |
| `UGameViewportClient* GameViewport` | `0x980` | `UGameEngine::Init`: `GameViewport = ViewportClient` right after creating it | LIVE |
| `UGameInstance* GameInstance` (UGameEngine) | `0x1070` | `UGameEngine::Init` | LIVE |
| `FExec` subobject of UEngine | `0x28` | UEVR: `UEngine::Exec` is slot 1 of the vtable at UEngine+0x28 | UEVR |

`UGameInstance`: `WorldContext` at +0x30, `TArray<ULocalPlayer*> LocalPlayers` at +0x38 (LIVE).
`FWorldContext::GameViewport` at +0x238 (STATIC).

### Important engine functions

| Function | RVA | Notes | Status |
|---|---|---|---|
| `UEngine::InitializeHMDDevice` | `0x3317ca0` | Virtual, UGameEngine vtable slot 111 (+0x378). See §4. | STATIC + LIVE (behaviour) |
| `UEngine::Init` | `0x3312380` | UEngine vtable slot 74; calls `this->InitializeHMDDevice()` at `0x33125da` | STATIC |
| `UGameEngine::Init` | `0x2f2ff40` | Calls `UEngine::Init` (`0x2f2ff69`), then creates the viewport client, the game viewport and the first local player | STATIC |
| `UGameEngine::Tick` | `0x2f32420` | UGameEngine vtable slot 78; game thread | STATIC + UEVR |
| `UEngine::IsStereoscopic3D(FViewport*)` | `0x33187d0` | `(!Vp || Vp->IsStereoRenderingAllowed()) && Device && Device->IsStereoEnabled()` | STATIC |
| `UGameViewportClient::Draw` | `0x2f65ce0` | Builds the view family; per local player 1 view (mono) or 2 views (3 with the monoscopic far field) | STATIC + UEVR |
| `ULocalPlayer::CalcSceneView` | `0x3018da0` | ULocalPlayer vtable slot 73 (+0x248) | STATIC |
| `ULocalPlayer::GetViewPoint` | `0x30185f0` | slot 72 (+0x240) | STATIC |
| `ULocalPlayer::GetProjectionData` | `0x3019240` | slot 82; consumes AdjustViewRect, CalculateStereoViewOffset, GetStereoProjectionMatrix | STATIC + UEVR (call stack) |
| `ULocalPlayer::PostInitProperties` | `0x3016dd0` | slot 9; allocates the stereo view states only if a stereo device exists | STATIC + LIVE |
| `FSceneViewStateReference::Allocate` | `0x32085c0` | Game thread | STATIC |
| `FSceneViewport::EnqueueBeginRenderFrame` | `0x3260420` | FViewport vtable slot 37; game thread; triggers render target (re)allocation | STATIC + UEVR |
| `FSceneViewport::InitDynamicRHI` | `0x3261520` | Render thread; sizes and allocates the separate render target | STATIC + UEVR |
| `RHICreateTargetableShaderResource2D` | `0x2284ea0` | Fallback allocation when the device does not allocate | STATIC + UEVR |
| `FSlateRHIRenderer::DrawWindow_RenderThread` | `0x287db70` | Calls `RenderTexture_RenderThread` | STATIC + UEVR |
| `FDeferredShadingSceneRenderer::RenderLights` | `0x2234f50` | String `L"ScreenShadowMaskTexture"` | STATIC + community plugin |
| `FSceneView::FSceneView` | `0x3209820` | UEVR hooks it; not needed by our design | UEVR |
| `FRenderTargetPool::FindFreeElement` | `0x253d6b0` | Where named pool targets (`InGameUIRenderTarget`, ...) are created | UEVR |
| `FDeferredShadingSceneRenderer::Render` | `0x21e64a0` | Scene, then the in-game UI pass per view, then post-processing per view (§9) | STATIC + LIVE |
| `FSceneRenderTargets::BeginRenderingInGameUI` / `EndRenderingInGameUI` | `0x2541670` / `0x2541910` | Names ours. Allocate and bind / resolve the UI target (§9) | STATIC + LIVE (hooked) |
| `FName::ToString` | `0x1ce89a0` | | STATIC |

## 3. Stereo interfaces in this build

### IStereoRendering (14 virtuals, no virtual destructor at the start)

The engine's own `FFakeStereoRendering` is compiled in. Its vtable is at RVA `0x4eba6c0`
(constructor `0x3317590` stores it; the constructor is the function that references
`L"r.StereoEmulationHeight"`). It has 15 slots: the 14 `IStereoRendering` virtuals and the
class's virtual destructor at slot 14. The "Engine calls it" column lists call sites found
by `tools/re/stereo_callsites.py` (every load of `GEngine->StereoRenderingDevice`
followed by a virtual call).

| Slot | Offset | Signature (UE 4.18) | Engine calls it from | Thread | FFakeStereoRendering in this build |
|---|---|---|---|---|---|
| 0 | +0x00 | `bool IsStereoEnabled() const` | 15 sites: `IsStereoscopic3D`, `GetProjectionData`, `EnqueueBeginRenderFrame`, renderer depth allocation, Draw, ... | game and render | `return true` (`0x62df40`) |
| 1 | +0x08 | `bool IsStereoEnabledOnNextFrame() const` | `FSceneViewport::InitDynamicRHI`, `0x3212aa8` | render, game | calls slot 0 |
| 2 | +0x10 | `bool EnableStereo(bool stereo)` | `stereo on/off` console handler, `0x2667d77` | game | `return true` |
| 3 | +0x18 | `void AdjustViewRect(EStereoscopicPass, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const` | `GetProjectionData` (`0x30194fc`) | game | `SizeX /= 2; if (pass == 2) X += SizeX` |
| 4 | +0x20 | `FVector2D GetTextSafeRegionBounds() const` | `0x333e55f` (canvas safe area) | game | returns (0.75, 0.75) |
| 5 | +0x28 | `void CalculateStereoViewOffset(EStereoscopicPass, FRotator& ViewRotation, float WorldToMeters, FVector& ViewLocation)` | `GetProjectionData` (`0x30196f9`) | game | skips pass 0 and 3, else ±3.2 cm along the view's Y axis |
| 6 | +0x30 | `FMatrix GetStereoProjectionMatrix(EStereoscopicPass) const` (returned through a hidden pointer in RDX, pass in R8D) | `GetProjectionData` (`0x3019c8f`) | game | symmetric FOV from its FOV/Width/Height fields, near = `GNearClippingPlane` |
| 7 | +0x38 | `void InitCanvasFromView(FSceneView*, UCanvas*)` | canvas update `0x2ee215d`, only when `View->StereoPass != 0` | game | stores a value from the view family into its field +0xC |
| 8 | +0x40 | unknown, returns `bool false` | no call through GEngine found | - | `return false` |
| 9 | +0x48 | `void RenderTexture_RenderThread(FRHICommandListImmediate&, FRHITexture2D* BackBuffer, FRHITexture2D* SrcTexture, FVector2D WindowSize) const` (WindowSize on the stack at [rsp+0x20]) | `FSlateRHIRenderer::DrawWindow_RenderThread` (2 sites) | render | draws SrcTexture into BackBuffer |
| 10 | +0x50 | `void GetOrthoProjection(int32 RTWidth, int32 RTHeight, float OrthoDistance, FMatrix OrthoProjection[2]) const` | `0x333fa3b` | game | identity + translation |
| 11 | +0x58 | unknown, returns `nullptr` | no call through GEngine found | - | `return nullptr` |
| 12 | +0x60 | `IStereoRenderTargetManager* GetRenderTargetManager()` | `EnqueueBeginRenderFrame`, `InitDynamicRHI`, renderer depth allocation | game and render | `return nullptr` |
| 13 | +0x68 | `IStereoLayers* GetStereoLayers()` | 11 sites | game | `return nullptr` |
| 14 | +0x70 | virtual destructor (scalar deleting, `(uint32 flags)`) | the shared pointer's `DestroyObject` | any | frees through GMalloc |

Status: slots 0-7, 9, 10, 12, 13 STATIC (call sites read); 8 and 11 STATIC as "never called
through `GEngine->StereoRenderingDevice`"; the whole table matches the indices UEVR found
on this exe (IsStereoEnabled 0, AdjustViewRect 3, view offset 5, projection 6,
RenderTexture_RenderThread 9, GetRenderTargetManager 12, GetStereoLayers 13). There is no
`GetDesiredNumberOfViews` (UE 4.19+) in this build.

`EStereoscopicPass` (STATIC, from AdjustViewRect/CalculateStereoViewOffset/Draw):
`0 = eSSP_FULL`, `1 = eSSP_LEFT_EYE`, `2 = eSSP_RIGHT_EYE`, `3 = eSSP_MONOSCOPIC_EYE`.

`FFakeStereoRendering` object (size 0x18, LIVE values in brackets): vtable +0, `float FOV`
+8 [100.0], +0xC float written by slot 7 [-50.0], `int32 Width` +0x10 [640],
`int32 Height` +0x14 [480]. The `r.StereoEmulationFOV/Width/Height` cvars override them
when non-zero.

Devirtualisation: the only code references to the fake vtable are its constructor and its
destructor (STATIC), so this build has no "vtable == FFakeStereoRendering" inlined fast
paths. A device with its own vtable is always called through the vtable.

### Shared pointer reference controller

`InitializeHMDDevice` allocates the controller with `new` (GMalloc, 0x18 bytes) and the
vtable at RVA `0x4eb9c98` (STATIC; LIVE: shared 1, weak 1):

```
+0x00 vtable   [0] void DestroyObject()            -> Object->vtable[14](1)
               [1] scalar deleting destructor(uint32 flags)
+0x08 int32 SharedReferenceCount   (lock inc / lock xadd by every TSharedPtr copy)
+0x0C int32 WeakReferenceCount
+0x10 Object*                      (the device)
```

When the shared count drops to 0 the engine calls `DestroyObject`, then when the weak count
drops to 0 it calls the destructor with 1. The engine copies the pointer from several
threads (thread-safe mode, `lock` prefixes, STATIC).

### IStereoRenderTargetManager (8 virtuals, no destructor)

Every slot was matched to an engine call site (STATIC):

| Slot | Offset | Signature | Called from | Thread |
|---|---|---|---|---|
| 0 | +0x00 | `bool ShouldUseSeparateRenderTarget() const` | `EnqueueBeginRenderFrame` (`0x326058f`) | game |
| 1 | +0x08 | `void UpdateViewport(bool bUseSeparateRenderTarget, const FViewport&, SViewport*)` | end of `EnqueueBeginRenderFrame` (`0x3260ba5`), every frame | game |
| 2 | +0x10 | `void CalculateRenderTargetSize(const FViewport&, uint32& SizeX, uint32& SizeY)` | `InitDynamicRHI` (`0x3261777`) | render |
| 3 | +0x18 | `bool NeedReAllocateViewportRenderTarget(const FViewport&)` | `EnqueueBeginRenderFrame` (`0x32605ae`) | game |
| 4 | +0x20 | `bool NeedReAllocateDepthTexture(const TRefCountPtr<IPooledRenderTarget>&)` | renderer scene depth allocation (`0x25469e5`) | render |
| 5 | +0x28 | `uint32 GetNumberOfBufferedFrames() const` | `InitDynamicRHI` (`0x3261781`) | render |
| 6 | +0x30 | `bool AllocateRenderTargetTexture(uint32 Index, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 Flags, uint32 TargetableTextureFlags, FTexture2DRHIRef& OutTargetable, FTexture2DRHIRef& OutShaderResource, uint32 NumSamples)` | `InitDynamicRHI` (`0x3261b54`), called with Format 2 (PF_B8G8R8A8), NumMips 1, Flags 0, TargetableTextureFlags 1, NumSamples 1 | render |
| 7 | +0x38 | `bool AllocateDepthTexture(... same as 6 ...)` | renderer scene depth allocation (`0x2546ba7`) | render |

UEVR uses the same layout on this exe ("old render target manager" path).

### Viewport objects

`UGameViewportClient::Viewport` (+0xA0) points to the `FViewport` subobject of
`FSceneViewport`, which sits at `FSceneViewport+8` (the first base is `FViewportFrame`).
All offsets below are relative to that `FViewport*` (STATIC; LIVE where noted).

| Field | Offset | Notes |
|---|---|---|
| vtable | +0x00 | `FSceneViewport::FViewport::vftable` RVA `0x4eaeff8`, 51 slots (LIVE) |
| `FTexture2DRHIRef RenderTargetTextureRHI` | +0x08 | the separate render target when one is used (LIVE: null without a separate target) |
| `FRenderResource` subobject | +0x10 | `InitDynamicRHI` runs with `this = FViewport+0x10` |
| `uint32 SizeX, SizeY` | +0xB8, +0xBC | window client size (LIVE: 1280x720 with `-ResX=1280 -ResY=720`) |
| `EWindowMode WindowMode` | +0xC4 | LIVE: 2 (windowed) |
| `TWeakPtr<SViewport> ViewportWidget` | +0x258 | `IsStereoRenderingAllowed` pins it |
| `bool bUseSeparateRenderTarget` | +0x27B | |
| `bool bForceSeparateRenderTarget` | +0x27C | set by `EnqueueBeginRenderFrame` from `ShouldUseSeparateRenderTarget()` (UEVR found the same offset) |
| `FIntPoint RTTSize` | +0x2D4 | size of the separate target |
| `TArray BufferedSlateHandles / BufferedRenderTargetsRHI / BufferedShaderResourceTexturesRHI` | +0x2E0 / +0x2F0 / +0x300 | |
| `int32 NumBufferedFrames` | +0x320 | |
| `int32 CurrentBufferedTargetIndex / NextBufferedTargetIndex` | +0x324 / +0x328 | |

FViewport vtable slots (STATIC): [0] destructor, [1] `GetRenderTargetTexture()`
(returns `&RenderTargetTextureRHI`), [3] `GetSizeXY()`, [34] `GetDebugCanvas()`,
[37] `EnqueueBeginRenderFrame(bool)`, [48] `IsStereoRenderingAllowed()` (+0x180),
[49] `GetRenderTargetTextureSizeXY()`, [50] `UpdateViewportRHI(bool bDestroyed, uint32 X,
uint32 Y, EWindowMode, EPixelFormat)` (+0x190).

D3D11 RHI texture (`TD3D11Texture2D`, vtable RVA `0x4d38dc8`): [1] `GetTexture2D()`,
[6] `GetSizeXYZ()` (SizeX +0x60, SizeY +0x64), [7] `GetNativeResource()` returns the
`ID3D11Resource*` at +0xA0, [8] native SRV at +0xA8 (STATIC; UEVR uses index 7).

## 4. How stereo turns on and where the picture goes

### Device creation (STATIC, LIVE)

`UGameEngine::Init` → `UEngine::Init` → `this->InitializeHMDDevice()` (vtable +0x378),
then the viewport client, the `FSceneViewport` and the first `ULocalPlayer` are created.
`InitializeHMDDevice` in this build:

1. returns early when running a commandlet;
2. registers `r.EnableStereoEmulation` (static cvar);
3. if `FParse::Param(cmdline, L"emulatestereo")` or `r.EnableStereoEmulation != 0`:
   allocates `FFakeStereoRendering` and a reference controller and stores them in
   `StereoRenderingDevice`;
4. otherwise, if `XRSystem` is null and `-nohmd` is absent, asks every
   `IHeadMountedDisplayModule` modular feature for a tracking system (none are loaded in
   this game) and takes `XRSystem->GetStereoRenderingDevice()` (XRSystem vtable +0xC0);
5. returns `StereoRenderingDevice.IsValid()`.

**LIVE result:** launching the unmodified game with `-emulatestereo` (windowed 1280x720,
`-d3d11`) installs the engine's FFakeStereoRendering at `GEngine+0xD50` with refcounts 1/1,
and the title screen, menus and gameplay render side by side (two 640x720 views). Because
the device exists before the local player is created, `ULocalPlayer::PostInitProperties`
allocated three distinct view states (`ViewState`, `StereoViewState`, `MonoViewState`, all
non-null and different). Screenshots: `captures/re/emu_title.png`, `emu_menu.png`,
`emu_resume.png`, `emu_game1.png`, `emu_game2.png` (local, not in git).

### Is stereo active this frame

- `UEngine::IsStereoscopic3D(Viewport)` (`0x33187d0`) = viewport allows stereo and the device
  exists and `IsStereoEnabled()`.
- `FSceneViewport::IsStereoRenderingAllowed` (FViewport slot 48) asks the `SViewport`
  widget; it returned true for the game viewport (the stereo views were drawn, LIVE).
- `UGameViewportClient::Draw` evaluates `IsStereoscopic3D(InViewport)` once and then, for
  every local player, calls `CalcSceneView` for passes 1 and 2 (and 3 when the view family
  has the monoscopic far field enabled; it is off).
- `ULocalPlayer::GetProjectionData` uses `bNeedStereo = (pass != 0) && Device &&
  Device->IsStereoEnabled()`.

### Render target sizing and allocation (STATIC, UEVR)

Game thread, every frame, `FSceneViewport::EnqueueBeginRenderFrame`:

```
bAllowed = Device && Viewport->IsStereoRenderingAllowed()
bStereo  = bAllowed && Device->IsStereoEnabled()
RTM      = bAllowed ? Device->GetRenderTargetManager() : null
RenderTargetTextureRHI = BufferedRenderTargetsRHI[CurrentBufferedTargetIndex]
if (bAllowed) {
    want = RTM ? RTM->ShouldUseSeparateRenderTarget() : false
    if (want != bForceSeparateRenderTarget || (want && RTM->NeedReAllocateViewportRenderTarget(*this))) {
        bForceSeparateRenderTarget = want
        UpdateViewportRHI(false, SizeX, SizeY, WindowMode, PF_Unknown)   // re-inits the RHI resources
    }
}
... FViewport::EnqueueBeginRenderFrame ...
if (RTM) RTM->UpdateViewport(bUseSeparateRenderTarget || bForceSeparateRenderTarget, *this, widget)
```

Render thread, `FSceneViewport::InitDynamicRHI` (when bUse or bForce is set):

```
NumBufferedFrames = 1
if (Device && IsStereoRenderingAllowed() && Device->IsStereoEnabledOnNextFrame() && (RTM = Device->GetRenderTargetManager())) {
    RTM->CalculateRenderTargetSize(*this, TexSizeX, TexSizeY)   // starts at the window size
    NumBufferedFrames = RTM->GetNumberOfBufferedFrames()
}
for i in 0..NumBufferedFrames-1:
    if (!RTM || !RTM->AllocateRenderTargetTexture(i, TexSizeX, TexSizeY, PF_B8G8R8A8, 1, 0, TexCreate_RenderTargetable, RT, SRV, 1))
        RHICreateTargetableShaderResource2D(TexSizeX, TexSizeY, <format global 0x53e1c78>, 1, 0, TexCreate_RenderTargetable, false, CreateInfo, RT, SRV, 1)
    BufferedRenderTargetsRHI[i] = RT; BufferedShaderResourceTexturesRHI[i] = SRV
RenderTargetTextureRHI = BufferedShaderResourceTexturesRHI[0]
RTTSize = (TexSizeX, TexSizeY)
```

So the separate target's size is whatever `CalculateRenderTargetSize` returns and does not
depend on the window size. UEVR on this exe returned 7036x2996 for a 1920x1080 window and
the engine allocated it (UEVR log, D3D11 format 90 = `DXGI_FORMAT_B8G8R8A8_TYPELESS`).

Without a render target manager (the plain `-emulatestereo` case) no separate target is
used: the views are rendered straight into the window-sized back buffer, each eye getting
half of it (LIVE: `bUseSeparateRenderTarget/bForce = 0/0`, `RenderTargetTextureRHI` null).

### Where the per-eye images end up

- With a render target manager whose `ShouldUseSeparateRenderTarget()` returns true, both
  eyes are rendered into `FViewport::RenderTargetTextureRHI` (+0x08), a double-wide texture,
  left eye at `x = 0`, right eye at `x = eye width`, with the rects our `AdjustViewRect`
  returns. The scene, post-processing and the game's in-game UI composite (see §6) are in
  this texture.
- Slate then calls `Device->RenderTexture_RenderThread(RHICmdList, BackBuffer,
  SrcTexture = that texture, WindowSize)` on the render thread to put something on the
  desktop window, and draws Slate UI on top.
- The native D3D11 texture is `((FRHITexture2D*)tex)->vtable[7]()`, i.e. the pointer at
  `tex+0xA0`.

Recommendation on obtaining the images and where to copy them: see
`docs/re/stereo-hook-plan.md`, steps 5 and 6.

## 5. Camera

All in `ULocalPlayer::GetProjectionData` (`0x3019240`), game thread, once per view
(STATIC; UEVR's call stack from this exe passes through the same addresses):

1. View rect from `Viewport->GetSizeXY()` (window size, not the separate target size)
   scaled by the local player's `Origin` (+0x60) and `Size` (+0x68).
2. `GetViewPoint(ViewInfo, pass)` (camera manager POV; `FMinimalViewInfo` Location +0,
   Rotation +0xC, FOV +0x18).
3. If stereo: `Device->AdjustViewRect(pass, X, Y, SizeX, SizeY)`. The incoming values are
   the window rect, so the device must write absolute eye rects in render target pixels.
4. `PlayerController->LocalPlayerCachedLODDistanceFactor = FOV / DefaultFOV`.
5. If stereo or `XRSystem->IsHeadTrackingAllowed()`: optional `XRCamera` call (XRSystem is
   null for us, skipped), then
   `Device->CalculateStereoViewOffset(pass, ViewInfo.Rotation, WorldSettings->WorldToMeters,
   StereoViewLocation)`. Rotation is modified in place and is what builds
   `ViewRotationMatrix`; `StereoViewLocation` becomes `ViewOrigin`. `WorldToMeters` is read
   from `GetWorld()->PersistentLevel(+0x30)->WorldSettings(+0x330)->WorldToMeters(+0x488)`.
6. Mono: `FMinimalViewInfo::CalculateProjectionMatrixGivenView` (`0x2dcdb10`) and every
   view extension's `SetupViewProjectionMatrix`. Stereo: `ProjectionMatrix =
   Device->GetStereoProjectionMatrix(pass)` and the view rect is taken from step 3. View
   extensions are not called in the stereo branch.

Units: centimetres; `WorldToMeters` defaults to 100 (INFERRED, value not read live).
`FRotator` = `{float Pitch, Yaw, Roll}` in degrees; `FVector` = 3 floats; `FMatrix` = 4x4
floats, row-vector convention. Projection is reversed-Z with an infinite far plane: the
engine's own stereo matrix is `[xs 0 0 0; 0 ys 0 0; 0 0 0 1; 0 0 near 0]` with
`near = GNearClippingPlane` (STATIC from `FFakeStereoRendering` slot 6; for pass 3 it uses
its field +0xC instead).

Square Enix change found: `FSceneViewProjectionData` has an extra 64-byte matrix at +0x90
(stock 4.18 has the view rect there). Layout here: `ViewOrigin` +0, `ViewRotationMatrix`
+0x10, `ProjectionMatrix` +0x50, unknown matrix +0x90, `ViewRect` +0xD0,
`ConstrainedViewRect` +0xE0. The mono path fills +0x90 with an aspect-dependent scale
matrix; the stereo path never writes it, so stereo views keep whatever the caller
initialised (STATIC; consequence unknown, nothing visibly wrong in the `-emulatestereo`
run).

`FSceneView::StereoPass` is at +0x970 (STATIC). `ULocalPlayer`: `PlayerController` +0x30,
`ViewportClient` +0x58, `Origin` +0x60, `Size` +0x68, `AspectRatioAxisConstraint` +0x7C,
`ViewState` +0x90, `StereoViewState` +0xB8, `MonoViewState` +0xE0 (each a
`FSceneViewStateReference` = {vtable, `FSceneViewStateInterface* Reference` at +8, list
link}, 0x28 bytes; LIVE).

How UEVR applies the HMD on this game (from its source, `calculate_stereo_view_offset`):
it treats the incoming rotation as the game camera, composes `camera * HMD orientation *
per-eye rotation`, writes the result back as Euler angles, and moves the location by
`camera * (HMD position - standing origin) * WorldToMeters * WorldScale` plus the
per-eye offset rotated by the final orientation. "Decoupled pitch" flattens the camera
pitch first. Projection comes from the runtime's per-eye FOV tangents.

## 6. What breaks in stereo here and what the known fixes do

Source for the fixes: the UEVR profile for this game, UEVR's source, the community plugin
`FF7R-UEVR` (source public on GitHub, `src/Plugin.cpp`), the movie fix plugin (binary
only) and the `FF7RemakeFix` mod (source public on GitHub, MIT).

| Issue | What it is | Where | What the fix does | Status |
|---|---|---|---|---|
| Lights at non-16:9 aspect ("light flag") | In `RenderLights`' sorted-light setup SE added sort-key bit 0x40 (`mov esi, 0x40` at RVA `0x22351b0`, then `and ecx,~0x40; or ecx,esi`); some lights also get bit 0x20. The same 1-byte patch is applied by `FF7RemakeFix` as "GreenFix" (from a widescreen script), so the bug is tied to view aspect ratios other than 16:9. Every VR eye view is non-16:9. | patch site `0x22351b0` (`BE 40 00 00 00`) inside `0x2234f50` | Writes 0x60 to the immediate (byte at `0x22351b1`): lights that get 0x40 also get 0x20, which changes which group they are rendered in. | site STATIC; semantics of bits 0x20/0x40 INFERRED. In the `-emulatestereo` run (640x720 eyes) no light loss was visible in the one room tested. |
| GSystemResolution | The game sizes its in-game UI canvas from it: function `0x1508810` returns (0,0,1920,1080), or (0,0,3840,2160) when `GSystemResolution.ResX > 1920`, unless `r.InGameUI.FixedWidth/FixedHeight` are set. | `0x53e3ae0`, `0x1508810` | The plugin writes `ResX = 2 * eye width, ResY = eye height` before every viewport client draw (game thread) and sets `r.InGameUI.FixedWidth/Height` to UEVR's UI size minus one. | STATIC |
| In-game UI in stereo | SE renders UMG HUD and menus into the pooled target `InGameUIRenderTarget` and composites it inside each view's post-processing. With two views each eye shows a 1:1 crop of the full UI (LIVE: the central 640 of 1280 pixels in each eye), at zero parallax. | `FRenderTargetPool::FindFreeElement` `0x253d6b0` (name `InGameUIRenderTarget`); full pipeline in §9 | The plugin replaces the pooled UI texture with UEVR's own UI texture (shown on a quad) and clears the engine's one to (0,0,0,1), which the composite treats as empty. The mod instead draws the UI once and clears the family flag that gates the composite (§9). | LIVE (crop, mod's fix), plugin source |
| Native stereo fix (`VR_NativeStereoFix`, `...SamePass`) | UEVR does **not** render both eyes in one view family on this game: it renders the family once with the left view only, then swaps the right view into slot 0, points the family at a second render target and renders again, decrementing the FScene frame counter (for right-eye motion vectors). SamePass also relabels the right view as pass 1. This is two full scene renders per frame. | UEVR hooks `BeginRenderingViewFamily` (`0x25c9770`) | Avoids whatever breaks when both views are in one family. | UEVR source + log. What it fixes on this game is unknown: true two-view rendering through `-emulatestereo` showed no obvious artefacts in the scenes tested. |
| HZB occlusion (`VR_DisableHZBOcclusion`) | UEVR sets `r.HZBOcclusion 0` (cvar data at RVA `0x5928198`). HZB occlusion uses per-view history from the view state. | cvar | Disables HZB occlusion (falls back to hardware occlusion queries). | UEVR. A real fix looks feasible: with the device installed before the local player exists, each eye has its own view state (LIVE), which is what HZB needs. Test with it on first. |
| Instance culling (`VR_DisableInstanceCulling`) | Sets `r.InstanceCulling.OcclusionCull`, a UE5 cvar. | - | **Nothing on this game**: UEVR's log says the cvar string does not exist in this exe. No performance cost to remove, nothing to fix. | UEVR log |
| HDR compositing (`VR_DisableHDRCompositing`) | Sets `r.HDR.UI.CompositeMode 0` (cvar data at RVA `0x59f7c68`). Only matters when HDR output is on. | cvar | Disables HDR UI compositing. | UEVR log |
| Movies | The movie plugin iterates `/Script/MediaAssets.MediaPlayer` objects, calls `IsPlaying` (ignoring ones whose path contains `/Menu/`) and switches UEVR to its 2D screen mode while a cinematic plays. | UObject reflection | Shows pre-rendered movies on a flat screen instead of in stereo. | The game's movies are `.emov` files (VP9, strings `.emov`, `_MediaPlayer_VP9` at `0x43e8f30`) under `Content/GameContents/Movie/<area>/<name>/`, played through `UMediaPlayer` assets named `<name>_MediaPlayer`; the `MediaPlayer` class and its `IsPlaying` UFunction are found by name in gameplay (LIVE). The mod's detection (`docs/engine-module.md`, "Movies") has not seen a movie play yet. |
| Vignette | `FF7RemakeFix` hooks the end of `FPostProcessSettings::FPostProcessSettings` (`0x32050f0`, hook at the epilogue `0x320590f`) and overrides `VignetteIntensity`. | `0x32050f0` | Sets the default vignette intensity (0 disables it). `r.Tonemapper.Quality` below 2 also drops the vignette in stock 4.18 (INFERRED). | source of the mod |

### Evaluation with the mod's two-view rendering (LIVE, per-eye captures)

Scene: the first room of the latest save (Sector 7 slums, indoors, glossy metal walls, a
lamp-lit counter with a gramophone near the camera), eyes 2064x2208 with the Null
backend's asymmetric Quest 3 class FOV, plus the mod's fixed test host for symmetric FOV.

| Item | Result |
|---|---|
| Light sort-key patch | No visible change in either eye with the patch on or off (pixel differences between the captures are the characters' idle animation and edge aliasing, the same amount as between two captures without any change). Left off by default. |
| HZB occlusion | The game's own value of `r.HZBOcclusion` is **0** (set by its constructor default), so UEVR's "disable HZB" setting changes nothing here. With 1 no popping or missing geometry in either eye in a static view. Each eye has its own view state. |
| TAA, motion blur | TAA is on (`r.PostProcessAAQuality 4`), motion blur quality 4. Neither causes a difference between the eyes; turning them off does not change the problem below. |
| `r.SSR.Quality` | Does not exist in this build (Square Enix's own screen-space reflection setup; `ShowFlag.ScreenSpaceReflections` exists). |
| **Right eye shows a ghost of the left eye's image** | A semi-transparent copy of the left eye's picture appears in the right eye at the left eye's image positions, mostly visible on glossy surfaces (metal walls, the counter) and around bright lamps. Strong with the asymmetric headset FOV (where the two eyes' images are offset by about 360 px), faint with a symmetric FOV (only lamp glows). The left eye is clean. It disappears with `ShowFlag.PostProcessing 0`; it does not go away with TAA off, motion blur off, or `ShowFlag.` `ScreenSpaceReflections`, `ReflectionEnvironment`, `PostProcessMaterial`, `Bloom`, `LensFlares`, `DepthOfField`, `AmbientOcclusion`, `Tonemapper` 0 one at a time. `r.BloomQuality 0` removes it (together with all bloom). **Located and fixed**: it is Square Enix's own bloom, see section 10. |

## 7. Console variables

- Singleton: `IConsoleManager*` at RVA `0x57f9ca0` (null until
  `IConsoleManager::SetupSingleton` `0x1c1bda0` runs; it runs very early). Object is
  `FConsoleManager`, vtable RVA `0x4d1de20`, size 0xB8.
- `IConsoleManager` vtable slot 18 (+0x90) = `IConsoleVariable* FindConsoleVariable(const
  TCHAR* Name) const` (STATIC: its body calls slot 19 `FindConsoleObject`, rejects objects
  with the unregistered flag, then returns `Obj->AsVariable()`; the game's own UI code calls
  it with `L"r.InGameUI.FixedWidth"` at `0x1508900`). Slots 1 and 2 register float and int
  variables, slot 5 is `RegisterConsoleVariableRef(int32&)`.
- `IConsoleObject`/`IConsoleVariable` vtable (UEVR on this exe, consistent with the game's
  call `[vtable+0x68]` = GetInt): [3] `GetFlags`, [5] `AsVariable`, [10]
  `AsConsoleCommand`, [11] `Release`, [12] `Set(const TCHAR* Value, uint32 SetBy)`,
  [13] `GetInt`, [14] `GetFloat`.
- Static cvars (`TAutoConsoleVariable`) keep `IConsoleVariable*` at +8 and the
  `TConsoleVariableData<T>*` at +0x10 of the static object; the data's first value is the
  game-thread copy, the second the render-thread copy. Example: `r.EnableStereoEmulation`
  data pointer at RVA `0x5a06970`.
- `SetBy` priority values for 4.18 (INFERRED): `ECVF_SetByCode = 0x08000000`,
  `ECVF_SetByConsole = 0x09000000`. Check by reading `GetFlags() & 0xFF000000` after a
  set.
- Alternative that needs no vtable indices: `GEngine`'s `FExec` subobject (+0x28) slot 1
  `Exec(UWorld*, const TCHAR* Cmd, FOutputDevice& Ar)` with `L"r.Foo 1"` (UEVR runs its
  cvar presets this way on this exe).

Cvars named in this document: `r.EnableStereoEmulation`, `r.StereoEmulationFOV/Width/Height`,
`r.HZBOcclusion`, `r.HDR.UI.CompositeMode`, `r.InGameUI.FixedWidth/FixedHeight`,
`Slate.DrawToVRRenderTarget` (exists, UEVR found its data at RVA `0x594d908`),
`r.DefaultBackBufferPixelFormat` (does not exist in this build).

## 8. Render thread details used by the stereo device

### RHI command list (STATIC, LIVE)

This shipping build never bypasses the RHI command list: every RHI wrapper records a
command (there is no `Bypass()` branch in the wrappers, e.g. in
`FFakeStereoRendering::RenderTexture_RenderThread`). Square Enix runs D3D11 with an RHI
thread (LIVE: after start-up the swap chain is presented from a thread named `RHIThread`,
render module log), so the recorded commands are dispatched to the RHI thread and executed
there in order, the frame's last ones inside `RHIEndDrawingViewport`, which presents. The
RHI thread is the only user of the D3D11 immediate context during gameplay. Layout (UE
4.18, read from the engine's own append sequence, signature `RHI command list append
sequence`):

```
FRHICommandBase      { FRHICommandBase* Next; void (*ExecuteAndDestruct)(FRHICommandListBase&, FRHICommandBase*); }
FRHICommandListBase  { FRHICommandBase* Root; FRHICommandBase** CommandLink (+0x08); bool bExecuting (+0x10);
                       uint32 NumCommands (+0x14); uint32 UID; IRHICommandContext* Context (+0x20); ...;
                       FMemStackBase MemManager (+0x30: Top, +0x38: End) }
append:  *CommandLink = cmd; CommandLink = &cmd->Next; ++NumCommands
```

The execute function receives the command list in RCX and the command in RDX (checked on
the `SetViewport` command, `0x1f09590`, which calls `Context->RHISetViewport`, context
vtable `+0x168`). A command whose memory belongs to someone else is executed like any
other; the mod appends one from `RenderTexture_RenderThread` (render thread) to draw the
desktop mirror; it runs on the RHI thread at a point where the frame's scene work has been
executed on the D3D11 immediate context and before Slate's UI and Present.

### What the engine draws for the window in stereo (STATIC)

`FFakeStereoRendering::RenderTexture_RenderThread` (`0x3317ab0`) does **not** draw the
source texture: it sets the back buffer as render target (with a clear) and a viewport of
the back buffer's size, nothing else. With a separate render target the window therefore
shows only what the device draws plus Slate. The engine has no spectator screen of its own
in this build.

### Slate clears the back buffer only on request (STATIC, LIVE)

`DrawWindow_RenderThread` calls `RenderTexture_RenderThread` (two call sites, `0x287dd3b`
and `0x287e372`), then binds the back buffer for Slate's own elements through `0x26e2be0`
with a clear flag computed as `Slate.ShowWireFrame != 0 ? 1 : bClear` (the global at RVA
`0x583073c` is the data of `Slate.ShowWireFrame`, registered at `0x51fde0`). With the
defaults nothing is cleared after the device's command, so whatever the device draws into
the back buffer stays under the UI. LIVE: the mod's desktop mirror is visible in window
screenshots in stereo (it had been black only while the blit bound the eye texture as a
shader input while it was still bound as the scene's render target; D3D11 then silently
binds null).

### Scene buffers are sized from GSystemResolution (STATIC, LIVE)

Square Enix's `FSceneRenderTargets::Allocate` (`0x2540c10`, render thread) does not use
the view family size: the scene buffer size is the larger of this frame's and the last
frame's `GSystemResolution.ResX/ResY x r.ScreenPercentage / 100`, rounded up to a multiple
of 4 (signature `FSceneRenderTargets::Allocate size from GSystemResolution`).
`r.SceneRenderTargetResizeMethod` is registered but its data is never read. With a
2560x1440 eye target and a 1280x720 window the GBuffer and the other scene textures stay
1280x720, so the left eye is correct only in its top-left 1280x720 and everything else is
smeared edge pixels (LIVE, window mirror of both eyes in the first stereo run). The
community UEVR plugin for this game writes `GSystemResolution` before every viewport draw
for this reason. The mod sets it to the eye target size (2 x eye width by eye height)
while stereo renders and puts the game's value back afterwards.

Other readers of `GSystemResolution` (STATIC): the UI canvas preset (`0x1508810`,
`0x2541670`: 1920x1080 or 3840x2160 when `ResX > 1920`, unless
`r.InGameUI.FixedWidth/Height` are set), `ULocalPlayer::CalcSceneView` (next section),
`FSlateRHIRenderer::UpdateFullscreenState` (`0x287d750`, window mode changes) and
`RestoreSystemResolution` (`0x287d8e0`, which re-requests the resolution when the window is
activated in exclusive fullscreen), `UGameEngine` window creation (`0x2f2d360`), and two
functions in game code (`0x12d6880`, `0x16bea10`, purpose unknown).

### Windowed fullscreen replaces the view rect (STATIC)

In `ULocalPlayer::CalcSceneView`, after `GetProjectionData` (which applied the device's
`AdjustViewRect`), Square Enix added: if `Viewport->GetWindowMode()` (FViewport slot 30,
returns the field at `+0xC4`) is 1 (windowed fullscreen), `ViewRect` and
`ConstrainedViewRect` become `(0, 0, GSystemResolution.ResX, ResY)`. In that window mode
both eyes would render the full target. The mod makes the `jne` at RVA `0x3018fb8`
unconditional while stereo renders (signature `CalcSceneView windowed-fullscreen view
rect`). LIVE: that patch is not enough. In windowed fullscreen both eyes still come out
broken (left eye black, right eye shrunk into a corner, `captures/stereo/runS1/g_modes.png`):
`GSystemResolution.WindowMode` (`0x53e3ae8`) is compared with 1 in about twenty more
renderer functions (`0x22fe350` to `0x23217b0`, all of the same shape, plus `0x228bde0` in
the post-processing code), which then use other rectangles. The mod therefore switches the
game to a normal window while VR renders (`docs/engine-module.md`, "Window modes").

Exclusive fullscreen: `RestoreSystemResolution` (`0x287d8e0`, run when the window is
activated) calls `FSystemResolution::RequestResolutionChange` (`0x3326eb0`) with
`GSystemResolution.ResX/ResY` and sets `bForceRefresh` (`0x53e3aec`) when the window mode is
0, so a reactivation while the stereo override is active would ask for the eye target size
as a display mode. The `r.SetRes` sink (`0x3310ee0`) compares the parsed `r.SetRes` with
`GSystemResolution` and re-requests the resolution when they differ or when
`bForceRefresh` is set; it runs whenever a console variable changes, which with the
override active re-requests the window's own size (harmless: no resize). LIVE: switching
modes with `r.SetRes` while stereo rendered made the game call `ResizeBuffers`; with the
mod's old desktop mirror (which kept a view of the back buffer) that call failed with
`DXGI_ERROR_INVALID_CALL` and the game terminated.

### Frame pipeline (LIVE)

The game thread can be up to two engine frames ahead of the Present that shows a frame:
the rendering thread runs one frame behind the game thread, and the RHI thread, which
executes the recorded commands and presents, up to one more (UE 4.18 waits on the previous
frame's RHI fence in `EndDrawingViewport`). Anything paired per frame between the game
thread and Present therefore has to carry the frame's identity along the command stream
rather than rely on counting Presents. Switching the eye target on or off
(`bForceSeparateRenderTarget`) makes `FSceneViewport::UpdateViewportRHI` suspend and
restart the rendering thread (LIVE: `RenderTexture_RenderThread` comes from a new thread id
after every stereo on/off), which costs a hitch; the device keeps rendering stereo with the
last views when the XR session misses a frame instead of flipping to mono.

### Separate render target format (STATIC)

When `AllocateRenderTargetTexture` returns false, `FSceneViewport::InitDynamicRHI`
allocates the separate target with `RHICreateTargetableShaderResource2D(..., Format = *(uint8*)0x53e1c78, ...)`
(signature `FSceneViewport separate target format`).

### In-game UI render target and composite (STATIC)

- `0x2541670` (render thread) allocates the pooled target `InGameUIRenderTarget` once (it
  is kept at `this+0x110` and only allocated while that is null). Size: 1920x1080, or
  3840x2160 when `GSystemResolution.ResX > 1920`, replaced by `r.InGameUI.FixedWidth` x
  `r.InGameUI.FixedHeight` when both are non-zero (it reads the two cvars' data directly,
  `0x582899c` / `0x58289a0`). The size is stored at `this+0x240/+0x244`.
- `0x1508810` returns the UI layout rectangle with the same rule (1920x1080 / 3840x2160 /
  the fixed cvars); its callers lay out SE's UI canvas with it.
- SE added `InGameUITexture` / `InGameUITextureSampler` to the scene texture shader
  parameters (`FSceneTextureShaderParameters::Bind`, `0x2548ed0`), so the UI is
  composited into each view by a post-process material reading the scene textures, not
  by C++ code. How that material maps view pixels to UI pixels decides what each eye
  shows (see section 6 and the measurements below). The whole pipeline, including who
  calls this and how the binding is gated, is in section 9.

### What the UI composite does with an eye view (LIVE, per-eye captures)

Measured with 2064x2208 eyes (Null backend captures, gameplay HUD: distance bar at the top,
area name banner at the top left, "Commands Menu" prompt at the bottom left):

- The composite always maps the full UI height to the view height and the UI width as if
  the UI were 16:9 ("cover"): each eye shows the central `2064 / (2208 x 16/9) = 52.6 %` of
  the UI width at the correct aspect. The distance bar is 2.04x its 1920x1080 size (2208 /
  1080); the banner and the prompt are outside the visible part (only the end of the
  prompt's line reaches the left edge). Same with the default size rule and with
  `r.InGameUI.FixedWidth/Height = 1920x1080`.
- With the fixed size at the eye's aspect (1032x1104, 1920x2054, 2064x2208, after a stereo
  off/on cycle so that the pooled UI target is reallocated) the UI is laid out on the
  narrow canvas, but the composite still treats it as 16:9: the central 52.6 % of the
  canvas is stretched about 1.9x horizontally (text visibly widened), the banner is still
  cut at the left edge and the prompt is not visible.
- So no combination of the two size variables shows the whole UI undistorted in an eye;
  the composite's mapping itself has to change (or the UI has to go to its own layer).
- The UI is at zero parallax (identical position in both eyes), drawn over the scene.

## 9. In-game UI pipeline

How this build draws its UMG UI (HUD, command menu, main and save menus, dialogue, markers)
and composites it into the views, and what the mod changes in stereo
(`src/engine/src/ui_layer.cpp`). Signature names in `tools/re/signatures.json`.

### Where the UI is drawn (STATIC, LIVE)

The UI is not drawn by Slate onto the back buffer. It is drawn inside the scene renderer,
after the scene and before post-processing, by
`FDeferredShadingSceneRenderer::Render` (`0x21e64a0`, signature
`FDeferredShadingSceneRenderer::Render`):

```
if (ViewFamily flag 0x80 at FSceneViewFamily+0x3C)                // renderer +0x4C
    for each view (FViewInfo, 0x28B0 bytes, Views.Data at renderer +0xD0, Num +0xD8):
        if (UI render delegates exist || view has UI elements (+0x1738)):
            if (BeginRenderingInGameUI(GSceneRenderTargets, RHICmdList, View)):
                RendererModule->vt[0x148](View, RHICmdList, SceneContext)   if vt[0x150]()
                (renderer flag bit 0) RenderXXX(renderer, View, RHICmdList)  (0x262d140)
                RendererModule->vt[0x168](View, RHICmdList, SceneContext)   if vt[0x170]()
                EndRenderingInGameUI(GSceneRenderTargets, RHICmdList)
    (with no delegates and no elements the loop only runs Begin/End: the target is cleared)
for each view: FPostProcessing::Process(View)                       (0x251c230, composite inside)
```

- `GSceneRenderTargets` (`0x5923ad0`) is the static `FSceneRenderTargets`;
  `+0x110` is `TRefCountPtr<IPooledRenderTarget> InGameUIRenderTarget`, `+0x240/+0x244`
  its size. `IPooledRenderTarget +8` is the targetable texture, `+0x10` the shader
  resource texture (the same `FRHITexture2D` here).
- `FSceneRenderTargets::BeginRenderingInGameUI` (`0x2541670`, names ours) allocates the
  target when `+0x110` is null (`FRenderTargetPool::FindFreeElement`, debug name
  `InGameUIRenderTarget`), binds its targetable texture with a clear and sets the
  viewport `(0, 0, W, H)`. It does not use the view. `EndRenderingInGameUI` (`0x2541910`)
  records a copy to the resolve target (the same texture).
- The UI is drawn by render delegates registered on the Renderer module
  (`GetRendererModule()`, cached at `0x5831000`; slots `+0x148/+0x150` and
  `+0x168/+0x170`, the shape of UE 4.18's post-opaque / overlay extension hooks).
- **Once per view.** In stereo the pass runs twice per frame, clearing and redrawing the
  same target; only the last view's result survives to post-processing.
- **The UI does not depend on the view** (LIVE): the UI texture after the first eye's pass
  and after the second eye's pass were compared in gameplay (3840x2160 dumps through
  `ui dump`); they are identical except for an animated glow around the area banner's
  icon (a 190 px area that changes between frames anyway). World-anchored elements are
  placed on the game thread, not from the eye views.

Format and size (LIVE, D3D11 descriptions read in the game): `DXGI_FORMAT_B8G8R8A8_TYPELESS`
(`PF_B8G8R8A8` with `TexCreate_SRGB`), written through the sRGB view, so it holds linear
values read through `B8G8R8A8_UNORM_SRGB`. 1920x1080 in a 1280x720 window, 3840x2160 while
the mod renders stereo (it sets `GSystemResolution` to the eye target, 4128 wide, so
`ResX > 1920`); the target is reallocated when stereo starts or stops (seen: 1920x1080 in
mono before stereo, 3840x2160 in stereo, 1920x1080 again in screen mode).

Alpha (LIVE): Unreal's inverted convention. The clear value is (0,0,0,1); 98.4 % of the
pixels of a gameplay HUD frame have alpha 255 (nothing drawn) and drawn pixels have lower
alpha. Colour is premultiplied: composite = background * a + rgb. Some glow pixels carry
colour at alpha 255 (additive light), which this formula handles. The community plugin's
"clear to (0,0,0,1) = empty" matches this.

### How the composite reads it (STATIC, LIVE)

About 320 material shader `SetParameters` instantiations bind the scene-texture parameter
`InGameUITexture` (added by Square Enix to `FSceneTextureShaderParameters`, `Bind` at
`0x2548ed0`) with this inlined code (`r14` = `View.Family`):

```
tex = GFallbackTexture->TextureRHI                          // FTexture* at 0x594c370, +0x30
if ((Family->flags[0x3C] & 0x80) && GSceneRenderTargets.InGameUIRenderTarget)
    tex = InGameUIRenderTarget->ShaderResourceTexture       // read as [0x5923be0] + 0x10
SetTexture(InGameUITexture, tex)
```

So the same family flag gates the UI pass and every binding; with the flag clear the
composite samples the fallback texture and leaves the view unchanged (LIVE: clearing it
after the UI pass removes the UI from both eye images, captures `captures/ui/run1`). The
fallback's colour was not read; its effect is "no UI" (INFERRED: the engine's black
texture with alpha 1). The composite maps the UI onto the view as described in section 8
("What the UI composite does with an eye view").

### Other UI paths

- Slate (`FSlateRHIRenderer::DrawWindow_RenderThread`) draws onto the back buffer after the
  frame; in gameplay and in the menus tested nothing visible comes from it (the window in
  stereo shows only the mirror plus what the mod draws). INFERRED from the captures.
- The title screen and main menu ("New Game / Continue") are InGameUI as well (LIVE: with
  the UI redirected from start-up the desktop mirror shows a black title screen until the
  mod draws the UI texture over it).
- Pre-rendered movies and loading screens: not checked. Loading screens are presented while
  the scene renderer does not run (the render module shows them on the virtual screen).

### What the mod does in stereo (LIVE)

`src/engine/src/ui_layer.cpp`, active only while the render module's UI layer is wanted
(stereo mode, an XR session, `[ui] layer = 1`) and the view is a stereo eye
(`FSceneView::StereoPass != 0`):

1. `BeginRenderingInGameUI` (inline hook): a later eye of a family whose flag the mod already
   cleared is refused (returns false, the engine skips the pass and `End`). One UI pass per
   frame instead of two.
2. `EndRenderingInGameUI` (inline hook): after the engine's code the family's flag `0x80` is
   cleared, so post-processing binds the fallback texture; an RHI command is appended that,
   on the RHI thread right after the UI pass executed and before the frame's Present, hands
   the native texture (`FRHITexture2D +0xA0`) to the render module, which copies it into a
   quad layer and draws it over the desktop window.

The pooled target is never replaced (the community plugin swaps the targetable texture for
its own; that needs reference counting on a pool element the engine may release). Mono
frames, screen mode, `-emulatestereo` without the render module and `[stereo] enabled = 0`
run the engine's code unchanged.

### World-anchored UI elements and the mono camera (LIVE, measured)

Markers, names and damage numbers are positioned on the game thread from the game camera,
which in stereo is not the eye camera. In the gameplay scene tested the projection of the
mono view (logged by `uihook proj`, the matrix the UI pass receives in screen mode) is
**50.0 x 29.4 degrees** (16:9, symmetric, near 10 cm); the eye views have the headset's FOV
(94 x 96 degrees, asymmetric, on the Null backend). The UI is laid out over that 50 degree
frustum, so on a quad an element lines up with its object (for the eye midpoint) only when
the quad covers the same angle: height `2 * distance * tan(14.7 deg)` = 1.57 m at 3 m. What
else separates them: the game camera's pitch (with decoupled pitch the eye views drop it,
the markers do not), head position, and the parallax between the quad's distance and the
object's. See `docs/render.md`, "UI layer", for the numbers at the default placement.

## 10. Post-processing in stereo: Square Enix's passes (LIVE, GPU trace)

Recorded with the engine module's one-frame GPU trace (`gpu trace`, see [Tools](#tools)) in
the first room, on the street outside and in the item shop, eyes 2064x2208 (target
4128x2208), Null backend. Each view runs the whole post-processing chain on its own, left
view first. Stock UE 4.18 passes keep each view at its own rectangle in every intermediate
target (the right eye at `x = 2064` full size, `x = 1032` at half size, and so on). Several
of Square Enix's passes do not: they put every view's data at the origin of their targets.

| Pass (pool names) | Layout | Per eye correct? |
|---|---|---|
| Subsurface scattering (`SubsurfaceSetup` / `SubsurfaceBlurX/Y`, half size) | each view at the origin `0 0 1032 1104`; the combine (`SubsurfaceColor`) writes back at the view's own rectangle | yes: the right view's blur holds the right eye's skin (read-backs differ by the eyes' parallax) |
| Bloom (`BloomReduce`, 10 levels, target size = scene buffer >> (level + 1); `BloomBlur`, 9 upsample/combine passes) | each view at the origin; the tonemapper reads the result at the origin | **no** (fixed by the mod), see below |
| Custom glare (`CustomGlare`, `CombinedExtraGlare`) | each view at the origin of one shared target | not exercised: no glare primitives in the scenes tested; see below |

### The right-eye ghost: the bloom's first pass (LIVE, fixed)

- The bloom chain is built by `0x2287ba0` (called from `FPostProcessing::Process`,
  `0x251c230`, when the view's float at `FViewInfo+0xE70` is above 0, which it was in every
  scene). Its first pass, `Bloom reduce pass Process` (`0x22861b0`, vtable `0x4d5fd90`
  slot 5), reduces the view's full-resolution input (the anti-aliased scene colour, a
  4128x2208 target holding both eyes) into level 0 at the origin. The C++ side gives it the
  view's source rectangle from `FViewInfo+0x70` (`2064 0 4128 2208` for the right eye): the
  vertex constants (`UVScaleBias = 2064 2208 2064 0`) and the pixel shader constants carry
  the offset.
- **The pixel shader ignores it**: the right view's level 0 is numerically the same as the
  left view's (mean absolute difference 0.06 of 255 on the read-backs, `captures/stereo/runK/k4`
  events 2798 and 2831). So the right eye's whole bloom chain is the left eye's image; the
  tonemapper adds it at the right eye's pixel positions: a soft copy of everything bright
  at the left eye's image positions.
- `stereo swap 1` (right eye rendered into the left half) showed the fault follows the
  position, not the view: the eye at the offset is wrong whichever it is.
- `re poke` of the `jae` at `0x251d847` (skips the bloom chain and the glare combine)
  removed the ghost, and so does `r.BloomQuality 0` (no bloom at all, a visibly flatter
  image); `ShowFlag.Bloom 0` does not.
- **Fix** (`src/engine/src/bloom_fix.cpp`): a hook on the reduce pass's Process (render
  thread) appends an RHI command for the first level of a view whose rectangle does not
  start at the origin; on the RHI thread the next DrawIndexed (the pass's draw) gets a
  scratch texture of the input's size and format, with the view's rectangle copied to its
  origin, bound as shader resource 0 for that draw only. After the fix the two views' level
  0 differ by the eyes' parallax (mean difference 20.9) and the right eye shows no ghost in
  the room, the street and the shop (captures in section "Evidence" of
  `docs/engine-module.md`).

### Custom glare (STATIC + LIVE, not exercised)

- Square Enix renders "custom glare" primitives (primitive view relevance bit `0x400`, list
  at `FViewInfo+0x1768`, count `+0x1770`) per view in a loop before post-processing
  (`0x262d250` from `0x21e64a0`): `FSceneRenderTargets::BeginRenderingCustomGlare`
  (`0x2544ed0`) allocates `CustomGlare` (member `+0x118` of the scene targets
  `0x5923ad0`, half size), clears the whole target and sets the viewport at the origin. The
  combine pass `FRCPassPostProcessCombineExtraGlare` (constructor `0x251b380`, Process
  `0x228b310`, shader parameters `ExtraGlare`, `ExtraGlareRectangle` = the view's half
  size at the origin) is added after the bloom chain when that target exists.
- With two views each view clears and redraws the same origin region, so only the last
  view's glare survives until post-processing: the left eye would get the right eye's
  glare. LIVE: in the three scenes tested the count was 0 for both views every frame, the
  target was never allocated and the combine never ran. A fix would give each view its own
  region (or target) the same way as the bloom fix; needs a scene with glare primitives to
  verify.

### Other FViewInfo fields seen (STATIC + LIVE)

`+0x70` the rectangle post-processing reads the view from (equals the view rect at 100 %
screen percentage), `+0xA0` view rect, `+0x970` stereo pass, `+0xE70` bloom/glare enable
(float), `+0xF44` / `+0xF48` bloom parameters, `+0x10AC` another post-processing enable
(float), `+0x1768` custom glare primitives.

## 11. The player's character and the camera (reflection, LIVE)

Found and used by `src/engine/src/player.cpp` (camera modes, `docs/engine-module.md`).
Nothing here needs a signature: objects and functions are found by name through the object
array and the name pool (section 2), and functions are called with `ProcessEvent`
(UObject vtable slot 64) on the game thread.

| Fact | Value | How to find it again | Status |
|---|---|---|---|
| Local player controller | `GEngine+0x1070` (GameInstance) `+0x38` (LocalPlayers data) `[0]` `+0x30` | section 5 offsets | LIVE (`fp status`: alive object) |
| Controlled pawn | `Controller.K2_GetPawn()` -> `APawn*` (params: return value at +0) | UFunction `K2_GetPawn`, outer class `Controller` | LIVE: `PC0000_00_Cloud_Standard_C` in the Sector 7 slums save; none while a level loads |
| Pawn location | `Actor.K2_GetActorLocation()` -> `FVector` at +0 | UFunction, outer `Actor` | LIVE: Z 101.1 standing on the street (capsule centre) |
| View target | `Controller.GetViewTarget()` -> `AActor*` at +0. Virtual: a player controller returns its camera manager's view target. `PlayerCameraManager` has no reflected `GetViewTarget` in this build; the other function of that name belongs to `CameraModifier` | `fp find GetViewTarget` | LIVE |
| Camera manager | one object of class `EndPlayerCameraManager` in `/Game/GameContents/Level/Game/EndGame.EndGame.PersistentLevel` | `fp classes CameraManager` | LIVE |
| View target in normal play | an actor **named** `EndCameraActor` whose class is the engine's `CameraActor` (class chain `CameraActor Actor Object`), **not** the pawn | `fp status`, `fp chain view` | LIVE (street, walking and standing) |
| Player classes | pawn `PC0000_00_Cloud_Standard_C` -> `EndCharacter` -> `Character` -> `Pawn` -> `Actor`; controller `EndPlayerControllerBP_C` -> `EndPlayerController` -> `PlayerController` | `fp chain pawn`, `fp chain pc` | LIVE |
| Head and eye bones | Cloud's body mesh `CharacterMesh0` (547 bones) has `C_Head_a` (72.0 cm above the pawn's location), `L_Eye` / `R_Eye` (74.6 / 74.9 cm above it, 5.6 cm apart) and `C_Forehead`. Found with `SkinnedMeshComponent.GetNumBones()` (int32 at +0) and `GetBoneName(int32)` (FName at +4), located with `SceneComponent.GetSocketLocation(FName)` (FVector at +8) | `fp bones head`, `fp bones eye` | LIVE (street, standing) |
| Mesh visibility | `SceneComponent.SetVisibility(bool bNewVisibility, bool bPropagateToChildren)` (bytes +0, +1), `SceneComponent.IsVisible()` -> bool at +0 | UFunctions, outer `SceneComponent` | LIVE (called without failures; effect: see the first-person captures in `docs/engine-module.md`) |
| `UStruct::SuperStruct` | `+0x30`, right after `UField::Next` (+0x28): 4.18 has no `FStructBaseChain`. `+0x40` is `PropertiesSize` (int32) followed by `MinAlignment` (int32): read as a pointer it gave `0x100000990` (size 0x990, alignment 1), the bad address of the earlier attempt | `fp chain pawn` prints the whole chain up to `Object` | LIVE |

### The follow camera's boom (LIVE)

Street outside the first room, camera yaw fixed, pitch changed with the mouse, camera
location from `stereo views`, pawn location from `fp status` (`captures/camera/runB`):

| Camera pitch | Camera Z | Horizontal distance to the pawn |
|---|---|---|
| +9.9 | 98.1 | 337.8 |
| -10.0 | 215.5 | 337.8 |
| -29.3 | 322.4 | about 300 |

`Z = 156.5 - 339 * sin(pitch)` fits all three within 0.3 cm, and the horizontal distance
follows `339 * cos(pitch)`: the camera sits on a 339 cm boom around a pivot 55.4 cm above
the pawn's location (at Z 156.5 here), looking at the pivot. This is what the level boom
(`[camera] pivot_height = 55`) undoes. The game's camera collision shortens the boom near
walls (not measured).

### Battle state (LIVE in exploration, not seen in a battle)

Found by listing reflected functions and properties by name (`fp funcs`, `fp props`) and by a
scan of the exe's identifier strings for `Battle` / `Combat` (165 names):

| Function (class) | Parameters (from its property children) | Value in exploration | Notes |
|---|---|---|---|
| `EndBattleAPI.GetBattleSceneID` (static, `/Script/EndGame.Default__EndBattleAPI`) | `ReturnValue` NameProperty only | `None` (index 0) | used as the battle signal: the current battle scene, by name and by the game's data model (`BattleSceneID`, `BattleScenePhase`) |
| `EndBattleAPI.GetBattleSceneCount` | `InName` (Name), `ReturnValue` (Int) | not called with a valid name | count per scene name |
| `EndBattleAPI.GetBattleSceneSituationType` / `SituationID` | `BattleSceneID` (Name), `ReturnValue` (Enum / Name) | not called with a valid ID | |
| `EndBattleAIController.GetBattleInSituation` | `ReturnValue` (Enum) | 0, on `PC0000_00_Cloud_Standard_AI_C` | the party member's AI controller; a second candidate |
| `EndBattleAIController.IsInDummyBattle` | `ReturnValue` (Bool) | false | |
| `EndBattleAIController.GetBattleScenePhase` / `IsBattleScenePhase` | Int / Bool | not called | |
| `EndMenuAPI.SetFieldMenuInBattle(bInInBattle)`, `SetNavimapInBattle(bInInBattle)` | Bool in | - | the game tells its menus a battle started; a setter |

No battle-related reflected function exists on `EndPlayerController`, the game mode or the
game instance; `EndPlayerController` has `BattleTalkOnEndBattle` (object) and its class.
Static functions are called on the class default object; `fp call auto <Function>` does that.

Cloud's sword is a separate actor `WE0000_01_Cloud_IronBlade_C` whose
`SkeletalMeshComponent0` is attached to the body mesh `CharacterMesh0`;
`SceneComponent.GetChildrenComponents(bool bIncludeAllDescendants, TArray& Children)`
(bool at +0, TArray at +8) on the body mesh returns it (LIVE).

### Battle and conversation objects (LIVE, exploration only)

Out of combat the object array already holds the battle data tables
(`/Game/GameContents/DataObject/Resident/Battle*`, classes `EndDataObjectBattle*`), a
`BattleTalkOnEndBattle_C` actor in the persistent level and a `BattleTalk_<name>_C`
component on each party member, and the conversation menu widgets
(`/Game/GameContents/Menu/Resident/Cinema/TalkMenu_Center`). Their existence is therefore not
a battle or conversation signal; a state inside one of them (or the camera manager's mode)
has to be compared in and out of a battle, which has not been reached yet.

## Tools

All in `tools/re/`, run with the repo's `.venv` Python. The exe is found through Steam's
library folders (or `FF7R_EXE`, or a path argument).

| Script | Purpose |
|---|---|
| `signatures.py [exe]` | Resolves every entry of `signatures.json` and prints value and status (ok / CHANGED / FAIL). `--make RVA` prints a unique signature starting at an instruction. |
| `signatures.json` | Machine-readable database: pattern, resolve rule (same arguments as `ff7vr::pattern::rip`), expected value for this build, description, how to find it again. |
| `stereo_callsites.py [exe]` | Lists every engine call through `GEngine->StereoRenderingDevice` grouped by vtable slot, plus calls on the returned render target manager / stereo layers. |
| `live_check.py` | Read-only inspection of a running game: GEngine, device and vtable, controller refcounts, XRSystem, viewport size and separate-target state, render target texture and its native resource, local player view states, a few cvars, `GSystemResolution`. |
| `pe_info.py [exe]` | Exe identity: hashes, PE header, sections with entropy, imports, TLS, packer/DRM markers. |
| `ff7re.py` | Library: image loading by RVA, string search, RIP-relative xref scan, `.pdata` function bounds, control-flow disassembly, vtable helpers. |
| `gpu_trace_view.py` | Views a one-frame GPU trace of the running game (dev pipe `gpu trace`): `sheet` makes contact sheets of the read-backs (optionally only one eye's half), `png` converts read-backs, `passes` sums GPU time per render target. |

### One-frame GPU trace (in the mod, dev pipe)

```
gpu names on                                   # label textures with the engine's pool names (before 'stereo on')
gpu trace <abs prefix>                         # next stereo frame: every draw/dispatch/clear/copy with its state
gpu trace <abs prefix> dump fullscreen scale 4 # plus a read-back of every full-screen pass and its constants
gpu status
python tools/re/gpu_trace_view.py sheet <prefix> out.png --from N --to M
```

The trace hooks the D3D11 immediate context's functions (inline hooks on the functions its
vtable points to: this game's RHI does not call through that vtable pointer, so patching
vtable slots sees nothing). Times between events are GPU timestamps, but the tracing itself
starves the GPU, so they are only useful for relative sizes within one trace.
`re peek <rva> <n>` and `re poke <rva> <bytes>` read and patch the game image to try a
patch before writing it (`stereo swap 1` renders the right eye into the left half).

Reproducing the live check: `tools/dev/launch.ps1 -NoMod -ExtraArgs '-emulatestereo'
-WaitSeconds 45 -Screenshot -KeepRunning`, then `python tools/re/live_check.py`, then
`tools/dev/stop.ps1`.
