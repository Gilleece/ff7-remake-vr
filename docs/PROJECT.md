# ff7-remake-vr

A dedicated, native VR mod for **Final Fantasy VII Remake Intergrade** (PC, Steam).

The game can already be played in VR through UEVR, but the result is visually poor and slow. This project replaces it with a lean, game-specific mod.

## Goals

1. **Main goal:** native stereo VR camera for the normal third-person game, with performance clearly better than UEVR on the same machine and headset resolution. Input stays the normal gamepad/keyboard input. No motion controllers.
2. **Stretch goal:** a first-person camera that can be toggled, on by default outside combat, switching automatically to third person in combat.

"Better than UEVR" has to be shown with numbers, so a repeatable benchmark against UEVR is part of the project (milestone M4).

## Target

| Item | Value |
|---|---|
| Game | Final Fantasy VII Remake Intergrade, Steam app 1462040 |
| Game exe | `End\Binaries\Win64\ff7remake_.exe` (96 MB). `ff7remake.exe` in the install root is a small launcher |
| Renderer | D3D11 (launch option `-d3d11`). D3D11 is the only renderer we target |
| Engine | Unreal Engine 4.18, heavily modified by Square Enix. No PDB, no source |
| VR API | OpenXR. Primary setup: Quest 3 through Virtual Desktop (VDXR). Headless testing: SteamVR with its null driver |
| Toolchain | Visual Studio 2026 (MSVC 14.50, bundled CMake and Ninja), Windows SDK 10.0.26100, Python 3.13 for tooling |

### What UEVR's behaviour on this game tells us

- UEVR runs this game on D3D11 with **Native Stereo** plus its native stereo fix. So the engine's own stereo path works in this build.
- The usual UEVR profile sets `VR_DisableHZBOcclusion=true` and `VR_DisableInstanceCulling=true`. Both cost performance. Fixing the underlying stereo problem instead of disabling them is a candidate win.
- The community UEVR plugin for this game logs `Found GSystemResolution` and `Found light flag bit manipulation ... Patched`. So lights break in stereo without a patch, and resolution handling needs help.
- A separate movie-fix plugin exists, so pre-rendered movies need handling. There is also a community "disable vignette" mod.
- The game imports XInput 1.3.

## Architecture

### Rendering approach: the engine's own stereo path

We install our own stereo rendering device into the engine (`GEngine->StereoRenderingDevice`), the same mechanism UEVR's Native Stereo uses, but written for this one binary. The engine then renders both eyes in one frame into a double-wide target, sharing game-thread work and shadow passes between the eyes. Per-eye view offsets and projection come from the XR runtime.

Reasons for choosing this over alternate-eye rendering as the primary mode:
- It is proven on this exact game through UEVR.
- It gives correct stereo every frame with no temporal ghosting.
- An alternate-eye ("sequential") mode can be added later on the same hooks as a performance option.

Where the performance win over UEVR is expected to come from:
- Keeping HZB occlusion and instance culling on by fixing them for stereo.
- Fixed foveated rendering through NVAPI variable rate shading on D3D11.
- A VR-specific preset of engine cvars, and removing passes that are wasted in VR.
- Fewer frame copies and no generic framework overhead.
- Later: DLSS through coexistence with the Luma mod.

### Modules

```
src/loader/   the DLL the game loads (proxy DLL). Bootstraps everything else
src/core/     logging, config, hook wrappers, pattern scanner, crash handler
src/engine/   UE4.18/FF7R bindings: signatures, offsets, GEngine, stereo device, camera
src/xr/       XR backend abstraction: OpenXR D3D11 backend and a Null backend
src/render/   D3D11 hooks, engine render target to XR swapchain, capture to PNG
src/dev/      debug command channel, scripted input, capture commands
tools/dev/    PowerShell: build, deploy, launch, kill, restore, SteamVR null setup
tools/re/     Python reverse-engineering scripts
tools/xr_smoke/  standalone D3D11 test app for the XR layer
docs/         this file and findings (docs/re/)
third_party/  fetched dependencies
_ref/         reference clones such as UEVR source (gitignored, read-only)
```

The XR layer has two backends behind one interface:
- **OpenXR backend** (D3D11): the real thing. Headless tests use SteamVR with the null driver.
- **Null backend**: no runtime at all. Fixed resolution and FOV, scripted head poses, and it can dump each eye to PNG. This allows engine-side development and visual checks without a headset.

### Conventions

- C++20, CMake with Ninja, MSVC x64, static CRT (`/MT`) so the DLL has no redistributable dependency.
- Dependencies are pinned and fetched into `third_party/` (CMake FetchContent or a fetch script). Nothing is installed system-wide.
- CMake targets: `ff7vr_core`, `ff7vr_xr`, `ff7vr_engine`, `ff7vr_render` (static libs), `ff7vr` (the DLL), `xr_smoke` (test exe).
- Log file: `ff7vr.log` next to the DLL in the game's `End\Binaries\Win64\`. Config: `ff7vr.ini` in the same place.
- Engine addresses are found by signature scan at runtime, with the known RVA for this exe build as a cross-check, never by hardcoded absolute address.
- Nothing machine-specific is hardcoded: the game install, save folder and runtime paths are detected or configured.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Foundations | Mod DLL builds and loads in the game with a log line. Dev harness can launch, kill and restore without manual steps. XR layer passes a standalone smoke test on both backends. Engine facts needed for M1 are found and verified |
| M1 | Stereo in the engine | Our stereo device is installed, the game renders side-by-side stereo in gameplay, and per-eye PNGs captured through the Null backend look correct. Scripted input can get from launch into a loaded save |
| M2 | OpenXR end to end | Per-eye images go to the OpenXR runtime with correct poses, projection and frame pacing. Verified headless on SteamVR null. Recenter and world scale work |
| M3 | Correctness | Known stereo bugs fixed (lights, vignette, movies). HUD and menus are readable (quad layer or equivalent). Cutscenes behave |
| M4 | Performance | Benchmark harness compares the mod with UEVR at the same resolution and scene. Performance features land until we are clearly ahead |
| M5 | Stretch | First-person camera toggle, automatic third person in combat |
| M6 | Packaging | Install and uninstall scripts, user README, Virtual Desktop defaults, a first-run test checklist |
