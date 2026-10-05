# Dev harness

How to build the mod, get it into the game, run the game without anyone at the
keyboard, see what happened, and put everything back afterwards.

Everything lives in `tools/dev/`. The scripts are plain Windows PowerShell 5.1
(the one that ships with Windows); run them with

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\<script>.ps1 [options]
```

Every script has a help block: `Get-Help tools\dev\launch.ps1 -Full`.

## Quick start

```
# 1. Build (configures on first use)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1

# 2. Start the game with the mod, wait for the title screen, take a PNG, stop and clean up
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -Until title -Screenshot

# 3. Same, but load the latest save and leave the game running in the world
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -Until gameplay -Screenshot -KeepRunning
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\screenshot.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\stop.ps1
```

After `launch.ps1` (without `-KeepRunning`) or `stop.ps1` the machine is back
to its resting state: no game process, the mod removed from the game folder,
Luma/ReShade's `dxgi.dll` back in place, the game lock released. The mod's log
of each run is kept in `captures\runs\<time>\ff7vr.log`.

## Requirements

- Visual Studio 2022 or later with the "Desktop development with C++" workload
  and the "C++ CMake tools for Windows" component. The build uses the CMake
  and Ninja bundled with Visual Studio; nothing needs to be on `PATH`. Visual
  Studio is found with `vswhere`.
- Steam with FINAL FANTASY VII REMAKE INTERGRADE (app 1462040) installed. The
  install folder is found from the Steam registry key and
  `steamapps\libraryfolders.vdf`.
- Network access on the first configure (dependencies are fetched into
  `third_party/_fetched`, shared by all build directories).
- At least one save game, for `-Until gameplay`.

## Configuration

Nothing machine-specific is stored in the repository. Everything is detected;
these environment variables override the detection:

| Variable | Meaning | Default |
|---|---|---|
| `FF7VR_DEV_NAME` | Your name for this checkout: default build directory `build\<name>` and the owner name written into the game lock | `dev` |
| `FF7VR_GAME_DIR` | Game install folder (the one containing `End\` and `Engine\`) | from Steam |
| `FF7VR_SAVE_ROOT` | The game's user-data folder | `%USERPROFILE%\Documents\My Games\FINAL FANTASY VII REMAKE` |
| `FF7VR_BACKUP_DIR` | Where save backups go | `%USERPROFILE%\ff7-remake-vr-backups` |
| `FF7VR_VS_PATH` | Visual Studio installation folder | from `vswhere` |

## Repository layout produced by the harness

| Path | What | In git |
|---|---|---|
| `build\<name>\` | Build directory per developer or configuration | no |
| `third_party\_fetched\` | Fetched dependencies, shared by all build directories | no |
| `captures\*.png` | Screenshots | no |
| `captures\runs\<time>\` | `ff7vr.log` and crash dumps of each run | no |
| `captures\runs\<time>-<pid>-steps\` | One PNG per screen change while reaching gameplay | no |
| `dist\` | Packages built by `tools\package\package.ps1` | no |
| `.locks\game`, `.locks\steamvr` | Run locks (see below) | no |
| `.locks\state\` | Small state files of the scripts | no |

## Building: `build.ps1`

Enters the Visual Studio x64 developer environment, configures with Ninja if
needed, and builds.

```
build.ps1                                   # build\<FF7VR_DEV_NAME or dev>, RelWithDebInfo
build.ps1 -BuildDir build\debug -Config Debug
build.ps1 -Target ff7vr                     # only the DLL and what it needs
build.ps1 -Without xr,xr_smoke              # leave optional modules out
build.ps1 -With xr                          # put them back
build.ps1 -Clean                            # delete the build directory first
build.ps1 -Reconfigure
```

Output: `<BuildDir>\src\loader\xinput1_3.dll` (the mod), `xinput1_3.pdb`, and
`ff7vr.ini` next to it.

CMake structure:

- `CMakeLists.txt` sets C++20, the static CRT (`/MT`) for every target, and
  `FETCHCONTENT_BASE_DIR=third_party/_fetched`.
- `cmake/ff7vr_deps.cmake`: pinned dependencies (MinHook 1.3.4 by commit hash).
- `cmake/ff7vr_warnings.cmake`: `ff7vr_set_warnings(<target>)` for our own
  targets (`/W4 /permissive-`, warnings as errors by default; turn that off
  with `-DFF7VR_WARNINGS_AS_ERRORS=OFF`).
- `cmake/ff7vr_buildinfo.cmake`: generates `ff7vr_buildinfo.h` (git commit,
  build time) so the log identifies the exact binary.
- `src/core` and `src/loader` are always built. `src/xr`, `tools/xr_smoke`,
  `src/engine`, `src/render` and `src/dev` are added when they contain a
  `CMakeLists.txt`, each behind an option `FF7VR_WITH_<NAME>` (default ON).
  If one of them does not compile, build the rest with `-Without <name>`.
  The loader links `ff7vr_engine`, `ff7vr_render`, `ff7vr_xr` and `ff7vr_dev`
  when those targets exist and defines `FF7VR_HAVE_<NAME>` accordingly.
- `CMakePresets.json` has `release` and `debug` presets for IDEs; they expect
  a developer environment (cl on `PATH`).

Core self-tests (not deployed):

```
build\<name>\src\core\ff7vr_core_tests.exe                 # unit tests, exit 0 = pass
build\<name>\src\core\ff7vr_core_tests.exe bench "<game>\End\Binaries\Win64\ff7remake_.exe"
build\<name>\src\core\ff7vr_core_tests.exe crash           # crash handler writes .log and .dmp next to the exe
```

The benchmark maps the game executable and runs 40 scans over its 64 MB of
code: a scan that stops after a few hits takes about 1-15 ms, a scan that has
to cover the whole image (no match, or checking uniqueness) about 50 ms.

## The mod DLL

### Why `xinput1_3.dll`

`ff7remake_.exe` imports `XINPUT1_3.dll` statically (ordinals 2 and 3,
`XInputGetState` and `XInputSetState`). `xinput1_3.dll` is not a KnownDLL, so
the Windows loader takes the copy in the executable's folder
(`End\Binaries\Win64`) before the one in `System32`. The game therefore loads
our DLL at process start without any injector, whether it is started directly
or through Steam.

`dxgi.dll` is not an option: ReShade with the Luma addon is installed under
that name and has to keep working alongside the mod.

The proxy exports exactly what the system `xinput1_3.dll` (DirectX June 2010
redistributable) exports, with the same ordinals, including the unnamed
ordinals 100-103. Each export forwards to the real DLL, which is loaded by
absolute path from the system directory on first use (never from `DllMain`).
If `xinput1_3.dll` is missing there, `xinput1_4.dll` (always present since
Windows 8) is used instead; if an export is missing the call reports "no
controller connected". A broken or absent system DLL therefore cannot crash
the game.

### What it does at start-up

`DllMain` only starts one thread. That thread:

1. reads `ff7vr.ini` next to the DLL (missing file = defaults),
2. opens `ff7vr.log` next to the DLL (truncated per run, one `WriteFile` per
   line, so nothing is lost on a crash or kill),
3. installs the crash handler,
4. logs the build identity, DLL path, exe path, exe file version, size and PE
   timestamp, module base and size, process id, command line, and the
   environment variables `XR_RUNTIME_JSON`, `SteamAppId`, `SteamGameId`,
   `SteamClientLaunch`,
5. resolves the real XInput DLL and initialises MinHook,
6. calls `ff7vr::loader::start_modules()` and logs `ff7vr: initialised, idle`.

Example:

```
2026-10-05 12:41:20.238 [tid 19240] INFO  ff7vr 0.1.0 loaded (commit 24c5ff5..., built ... RelWithDebInfo)
2026-10-05 12:41:20.238 [tid 19240] INFO  exe: file version 1.0.0.7, file size 96927560 bytes, PE timestamp 0x698ba49c
2026-10-05 12:41:20.239 [tid 19240] INFO  env: XR_RUNTIME_JSON=(unset)
2026-10-05 12:41:20.239 [tid 19240] INFO  ff7vr: initialised, idle
```

### Where new code plugs in

`src/loader/startup.cpp`, function `start_modules(const StartupContext&)`, is
the single place where engine hooks, rendering and XR initialisation are
started. The comment at the top of that file describes the steps: expose one
`start()` function in your module, the loader links it automatically when the
target exists, call it inside the matching `#if FF7VR_HAVE_<NAME>` block, gate
it with an ini switch. `start_modules` runs very early (before the engine has
created its window or D3D device), so modules install hooks there and do their
real work from those hooks.

### `ff7vr.ini`

Copied next to the DLL by the build and by `deploy.ps1`. Every key is optional.

| Key | Default in code | Meaning |
|---|---|---|
| `[log] level` | `info` | `trace`, `debug`, `info`, `warn`, `error` |
| `[crash] enabled` | `1` | Install the crash handler |
| `[crash] first_chance` | `1` | Report fatal exceptions when they are raised (see below) |
| `[crash] max_reports`, `max_dumps` | `8`, `2` | Caps per process |
| `[crash] full_memory` | `0` | Full-memory minidumps (several GB) |
| `[dev] pipe` | `0` | Serve `\\.\pipe\ff7vr-dev` (ping, log markers, virtual pad) |
| `[dev] virtual_pad` | `0` | Merge an injected pad state into XInput user 0 |
| `[debug] crash_after_seconds` | `0` | Crash on purpose N seconds after start (tests the crash handler) |

Use another ini for a run with `launch.ps1 -Ini path\to\test.ini`.

### Crash handler

UE4 catches crashes on its own threads with `__try/__except` and runs its own
reporter, so a top-level exception filter alone would rarely see anything. The
handler therefore also reports fatal exceptions (access violation, illegal
instruction, stack overflow, heap corruption, ...) first-chance, from a
vectored handler. The report is written by a dedicated thread created at
start-up, so it also works on stack overflow. It logs the exception code, the
faulting address as `module+RVA`, the registers and up to 32 stack frames as
`module+RVA`, and writes `ff7vr-crash-<time>-<pid>-<n>.dmp` next to the log.
Then the exception continues to the game's own handling unchanged.

Verified in the game with `[debug] crash_after_seconds = 20`:

```
FATAL CRASH (first-chance): exception 0xC0000005 ACCESS_VIOLATION at XINPUT1_3.dll+0x10387 (thread 20288, report 1/8)
FATAL   write of address 0x0
FATAL   stack (3 frames):
FATAL     #00 XINPUT1_3.dll+0x10387
FATAL     #01 KERNEL32.DLL+0x2cd87
FATAL     #02 ntdll.dll+0xacaec
FATAL   minidump written: ...\End\Binaries\Win64\ff7vr-crash-20261005-123631-19396-0.dmp
```

During normal runs (title screen, menus, loading, gameplay) no first-chance
report was produced. A first-chance report of an exception that the game
handles itself is possible in principle; it is marked "first-chance" and
capped. Set `[crash] first_chance = 0` if that ever becomes noisy.

## How the game is launched, and why

Facts established on this game build (exe file version 1.0.0.7):

- `ff7remake.exe` in the install root is a small launcher. Steam runs it, and
  it starts `End\Binaries\Win64\ff7remake_.exe`, staying alive as its parent.
- Starting `ff7remake_.exe` directly without anything else gets as far as a
  dialog "Please launch the game via the Steam client. Exiting FINAL FANTASY
  VII REMAKE." The Steam API refuses a launch that did not come from Steam.
- Starting it directly with the environment variables `SteamAppId=1462040`
  and `SteamGameId=1462040` works: the Steam API accepts the launch, the game
  runs normally (the Steam overlay comes up as usual) and there is no
  relaunch through Steam. This is what `launch.ps1` does by default
  (`-Via direct`).
- With a direct start the game gets exactly our command line and inherits the
  script's environment. Verified: `launch.ps1 -GameEnv "XR_RUNTIME_JSON=X:\probe\runtime.json"`
  shows `env: XR_RUNTIME_JSON=X:\probe\runtime.json` in `ff7vr.log`. This is
  how a specific OpenXR runtime is selected for a run without touching the
  system default runtime.
- `launch.ps1 -Via steam` runs `steam.exe -applaunch 1462040 <args>`. Steam
  starts the root launcher, which starts the game with
  `ff7remake_.exe End <our args> <launch options from the Steam properties>`
  (Steam appends the user's launch options, for example a second `-d3d11`).
  No confirmation dialog appeared. The game's environment comes from Steam,
  not from the script, so `-GameEnv` and `XR_RUNTIME_JSON` set in the shell
  do NOT reach the game this way.
- Steam must be running for either way (the Steam API talks to the client).
  `launch.ps1` starts Steam with `-silent` if it is not running and waits for
  it. (Not exercised yet: Steam was always running during testing.)
- `-windowed -ResX=<w> -ResY=<h>` on the command line give a window of that
  client size without changing the game's saved settings (checked: no file in
  the save folder or `Saved\Config` changed apart from the UE4 crash reporter
  ini the engine writes on every start). `launch.ps1` uses 1280x720 windowed
  by default; `-Width/-Height` change it, `-Fullscreen` drops the switches.
- `-d3d11` is always passed (the only renderer the project targets).
- Start-up times on the development machine: window after about 4-5 s, title
  screen after about 11 s, in the world after about 32-38 s.

## Scripts

### `launch.ps1`: run the game unattended

```
launch.ps1 [-Until none|title|gameplay] [-Screenshot] [-ScreenshotPath <png>]
           [-WaitLog <regex>] [-WaitSeconds <n>] [-KeepRunning]
           [-BuildDir <dir>] [-Ini <file>] [-NoMod] [-KeepLuma]
           [-Width 1280 -Height 720 | -Fullscreen] [-ExtraArgs "<args>"]
           [-GameEnv "NAME=value;NAME2=value2"] [-Via direct|steam]
           [-StartTimeout 120] [-UntilTimeout 240] [-LockWaitSeconds 900]
```

Steps: take the game lock (waits up to `-LockWaitSeconds` if someone else
holds it); back up saves if no backup exists yet; rename `dxgi.dll` to
`dxgi.dll.vr-disabled` (unless `-KeepLuma`); deploy the mod (unless
`-NoMod`); start the game; wait for its window and for `ff7vr: initialised`
in the log; optionally wait for the title screen or gameplay, a log regex
and/or a number of seconds; optionally capture a PNG; then, unless
`-KeepRunning`, stop the game and undo everything. It fails (exit 1) if a
step times out, the game exits, the screenshot is blank, or the log contains
a crash report. The cleanup runs in every case.

```
# Does the mod still load and does the game reach its title screen?
launch.ps1 -Until title -Screenshot

# Wait for a message our code logs, then capture
launch.ps1 -Until gameplay -WaitLog "stereo device installed" -Screenshot

# Vanilla game for comparison
launch.ps1 -NoMod -Until title -Screenshot

# Point the game at a specific OpenXR runtime
launch.ps1 -Until gameplay -GameEnv "XR_RUNTIME_JSON=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json"

# Test the crash handler
launch.ps1 -Ini my-crash-test.ini -WaitSeconds 40
```

Exit codes: 0 success, 1 a step failed, 3 lock not obtained or the game was
already running.

### `stop.ps1`: stop and clean up

Kills `ff7remake_.exe` and helper processes from the game folder (the root
launcher, `CrashReportClient`), waits for them to exit, undeploys the mod
(moving `ff7vr.log` and dumps to `captures\runs\<time>\`), renames
`dxgi.dll.vr-disabled` back to `dxgi.dll`, and releases the game lock. Safe to
run at any time and any number of times. If the lock belongs to someone else
it does nothing and exits 1 (`-Force` overrides), so it cannot kill another
person's run.

```
stop.ps1
stop.ps1 -Force
```

### `screenshot.ps1`: capture the game window

```
screenshot.ps1                           # captures\<time>-shot.png
screenshot.ps1 -Path captures\title.png
screenshot.ps1 -Method screen
```

The default method uses `PrintWindow` with `PW_RENDERFULLCONTENT`, which asks
the compositor for the window's content. It returns the real game image for
the game's D3D11 flip-model swap chain, also when the window is behind other
windows. A plain `BitBlt` of the window DC is not reliable for flip-model swap chains and often gives a black image.
`-Method screen` copies the screen area after bringing the window to the
front. The script prints the size, mean brightness, the detected screen state
and exits 2 if the image is blank (black or flat).

Note: the Steam overlay's "Access Steam features" notification appears in the
bottom right corner for a few seconds after start-up and is part of the
capture.

### `send-input.ps1`: input for menus

```
send-input.ps1 -Keys enter
send-input.ps1 -Keys "down*2,wait:500,enter"
send-input.ps1 -Keys "w" -HoldMs 2000            # walk forward for two seconds
send-input.ps1 -Pipe "ping;mark before-test"
```

`-Keys` sends key presses with `SendInput` (scan codes). The game reads the
keyboard only while its window is in the foreground; the script brings it
there first. Keys verified with the default bindings: Enter confirms in the title screen,
title menu and dialogs; W moves the character forward.

`-Pipe` talks to the mod's command pipe `\\.\pipe\ff7vr-dev` (enabled by
`[dev] pipe = 1`): `ping` (replies with the number of XInput polls seen),
`mark <text>` (writes `MARK <text>` into `ff7vr.log`, handy to line up log
output with test steps), and virtual gamepad commands (`tap A`, `hold LB`,
`stick L 0 1 500`, `trigger R 1 200`, `release`; see
`src/loader/dev_input.h`).

Finding: without a physical controller this game build never calls
`XInputGetState` (the counter stays at 0 through the title screen, menus,
loading and gameplay, and also after posting `WM_DEVICECHANGE`). The virtual
pad, which works by changing what `XInputGetState` returns, therefore has no
effect at the moment. Keyboard input is what the harness uses.

### `reach-gameplay.ps1` and `launch.ps1 -Until gameplay`: into a loaded save

Goes from the title screen into the most recent save using only load
operations:

```
title ("PRESS ANY BUTTON")  --Enter-->  title menu, cursor on Continue
                            --Enter-->  "Resume playing from where you left off?"  Yes selected
                            --Enter-->  loading screen  -->  in the world
```

The script presses a key only after a capture shows the expected screen. The
screen is recognised from colour statistics of a few fixed regions of the
client area (relative coordinates, so it works at any 16:9 window size; tested
at 1280x720 and 960x540): the title text, the highlighted Continue entry, the
blue dialog box with Yes highlighted, the loading screen's blue glow. Gameplay
counts as reached when the loading screen has been seen and something else has
been on screen for 6 seconds. If the mod's dev pipe answers and stereo is
switched on (`[stereo] start_in_stereo = 1`), stereo is switched off through the
pipe while the menus are driven and on again once gameplay is reached: in stereo
the window shows a crop of an eye, which the classifier does not recognise. If the menu cursor is not on Continue the script
presses Escape instead of Enter, and it stops at any other dialog, so it never
starts a new game and never saves. A PNG of every screen change is stored in
`captures\runs\<time>-<pid>-steps\` (time to the millisecond plus the script's
process id, so runs started in the same second do not share a folder).

```
launch.ps1 -Until gameplay -Screenshot -KeepRunning
# or, with the game already on the title screen
reach-gameplay.ps1
```

Reliability so far: 5 of 5 scripted attempts reached gameplay (32-38 s from
launch), with the mod loaded, at two window sizes (1280x720 and 960x540). Known limits:

- It needs at least one save; "Continue" loads the newest one, which may be
  an autosave. Where you land depends on that save.
- The classifier is calibrated on the English UI. Other languages may need new
  thresholds (`$script:StateRegions` and `Get-FrameState` in `common.ps1`).
- A save that resumes into a cutscene counts as "gameplay" once the cutscene
  runs for 6 seconds.
- Keyboard input needs the game window in the foreground, so do not use the
  desktop while it runs. If the window cannot be brought forward the script
  warns and the step times out.
- The game autosaves at checkpoints while it runs. Saves are backed up before
  the first run (see below); nothing in the harness ever saves on purpose.

### `deploy.ps1` / `undeploy.ps1`

```
deploy.ps1                                # from build\<name>\src\loader
deploy.ps1 -BuildDir build\debug -Ini my.ini
undeploy.ps1
undeploy.ps1 -Force                       # also remove deployed files that were modified
```

`deploy.ps1` copies `xinput1_3.dll` and `ff7vr.ini` into
`End\Binaries\Win64\` and records name, size and SHA-256 of each in
`End\Binaries\Win64\ff7vr.deploy-manifest.json`. It refuses to overwrite a
file it did not deploy itself, and refuses while the game runs (the DLL is
locked then). `undeploy.ps1` moves the mod's runtime files (`ff7vr.log`,
`ff7vr-crash-*.dmp`) to `captures\runs\<time>\`, then deletes exactly the
files in the manifest and the manifest itself. Nothing else in the game folder
is touched. `launch.ps1` and `stop.ps1` call these for you.

### `backup-saves.ps1` / `restore-saves.ps1`

The game writes autosaves while it runs, so the save folder is backed up
before the first run. `launch.ps1` does this automatically; it does nothing
when a complete backup already exists.

```
backup-saves.ps1            # back up if there is no backup yet, print its folder
backup-saves.ps1 -Verify    # re-hash the newest backup against its manifest
backup-saves.ps1 -New       # take another backup even if one exists
```

A backup is `%USERPROFILE%\ff7-remake-vr-backups\saves-<yyyyMMdd-HHmmss>\`
holding `Steam\<account id>\*.sav` (every account folder) and
`Saved\Config\`, plus `backup-manifest.json` with the SHA-256 of every file.
Each file is compared with its source after copying; the manifest is written
last, so a folder without one is an incomplete backup and is ignored.

```
restore-saves.ps1 -List
restore-saves.ps1 -Yes                                  # newest backup -> real save folder
restore-saves.ps1 -BackupDir <folder> -SaveRoot <scratch copy> -Yes
```

`restore-saves.ps1` overwrites the current saves, so it does nothing without
`-Yes` and refuses while the game runs. It verifies the backup first, takes a
safety copy of the current state (`pre-restore-<time>\`, same format), removes
files inside `Steam\` and `Saved\Config\` that are not in the backup, copies
the backup back and checks every restored file by hash. Try it on a scratch
copy with `-SaveRoot` (and `-BackupRoot` to keep its safety copy out of your
real backup folder).

### `lock.ps1`: the game lock

Only one person or script may run the game (or SteamVR) at a time on a
machine. The lock is the directory `.locks\game` (`.locks\steamvr` for
SteamVR). Creating a directory is atomic, so whoever creates it owns it;
`owner.txt` inside records `<owner> <time> pid=<pid>`. A lock is stale when it
is older than 20 minutes and no `ff7remake_` process runs; the next acquire
removes a stale lock. Waiting callers poll every 30 seconds.

```
lock.ps1 -Status
lock.ps1 -Acquire -WaitSeconds 600     # before running the game by hand or in a debugger
lock.ps1 -Release
lock.ps1 -Acquire -Name steamvr
```

`launch.ps1` and `stop.ps1` take and release the game lock themselves.

### `common.ps1`

Shared functions dot-sourced by every script: path detection (Steam
libraries, game folder, save folder), the lock, Luma rename and restore,
deploy manifest, save backup, window capture, screen-state classifier,
keyboard input, dev pipe client, game start and stop. Useful for one-off
experiments:

```
powershell -NoProfile -Command ". .\tools\dev\common.ps1; Get-GameRoot; (Get-ScreenState).State"
```

## Packaging for players: `tools\package\`

`package.ps1` makes a Release build (`build\release`) and assembles
`dist\ff7vr-<date>-<commit>\` (plus a zip): the DLL, the player's ini
(`tools\package\ff7vr.ini`), the launcher (`tools\package\launcher\`) and the
user guide (`README.md`). The package does not use this repository or these
scripts: the launcher finds the game itself, records its changes in
`End\Binaries\Win64\ff7vr.session.json` and undoes them after the session or with
`restore`. It waits until the game and its helpers (root launcher, crash
reporter) have completely exited before tidying up, never starts a second game
while one is still closing, and lets only one launcher instance work on the game
folder at a time (named mutex `Local\ff7vr-launcher`). It keeps no handle to the
game process once started. `collect-diagnostics.cmd` (`ff7vr-launcher.ps1 diagnostics`) zips
the last session's log folder, the ini, `VERSION.txt` and a `system.txt`
(Windows, GPU and driver, OpenXR runtimes, game folder state, the key lines of
the log) into `diagnostics-<time>.zip` next to the launcher; it changes nothing.
The launcher's session record is separate from `ff7vr.deploy-manifest.json`; each
refuses to overwrite files the other put there. On a development machine take the
game lock (`lock.ps1 -Acquire`) before running the launcher by hand.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1 [-Clean] [-NoZip]
```

Keep the checkout at a short path: the OpenXR loader's build fails with
"Cannot open compiler generated file" when its object paths pass 260 characters.

## Troubleshooting

- **"Game install not found"**: set `FF7VR_GAME_DIR`.
- **"... exists and was not deployed by these scripts"**: an `xinput1_3.dll`
  or `ff7vr.ini` from somewhere else is in the game folder. Move it away by
  hand; the scripts never overwrite foreign files.
- **"Both dxgi.dll and dxgi.dll.vr-disabled exist"**: ReShade was reinstalled
  during a run. Decide by hand which one to keep.
- **The lock is held and nothing runs**: wait (a stale lock clears after 20
  minutes), or check `.locks\game\owner.txt` and release it with
  `lock.ps1 -Release -Owner <name>`.
- **A run failed half-way**: `stop.ps1` puts everything back.
- **What did the mod do?**: `captures\runs\<time>\ff7vr.log` for each run.
