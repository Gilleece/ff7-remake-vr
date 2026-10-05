# ff7vr: VR for FINAL FANTASY VII REMAKE INTERGRADE (PC)

ff7vr is a VR mod for the Steam version of FINAL FANTASY VII REMAKE INTERGRADE. It
makes the game's own engine render a separate image for each eye at the headset's
resolution, so you sit inside the normal third-person game in 3D. You keep playing
with the gamepad or keyboard and mouse; motion controllers are not used.

It is written for this one game (the Steam build, file version 1.0.0.7, Direct3D 11)
and talks to the headset through OpenXR. The target setup is a Meta Quest 3 through
Virtual Desktop.

## State: new, and not yet tried in a headset

**Everything described here was built and tested on a PC without a headset.** The
3D rendering, the floating HUD panel, the virtual screen and the hand-over between
them were checked with a simulated headset (an internal test mode and SteamVR's
"null" driver, a virtual headset that shows the images in a window) and with
screenshots of each eye. Nobody has looked at it through a lens yet. Expect rough
edges, and read [Known problems and what has not been tested](#known-problems-and-what-has-not-been-tested)
before the first session.

## Requirements

- FINAL FANTASY VII REMAKE INTERGRADE on Steam, file version 1.0.0.7 (the current
  Steam build). Another build is refused by the mod: the game then runs without VR.
- Steam running (the game asks the Steam client whether it may start).
- A headset reachable through OpenXR. The settings are prepared for **Virtual
  Desktop**: the Virtual Desktop Streamer app on the PC and the headset connected in
  Virtual Desktop. Virtual Desktop's OpenXR runtime (VDXR) is chosen for this game
  only; the PC's default OpenXR runtime is neither used nor changed.
- An NVIDIA graphics card for foveated rendering (below). On other cards the mod
  works without it.
- Windows 10 or 11 with Windows PowerShell 5.1 (part of Windows).

## Starting a session and getting back to normal

The package folder (`ff7vr-<date>-<commit>`) can sit anywhere, for example on the
desktop. It contains:

| File | What it is |
|---|---|
| `start-vr.cmd` | **double-click to play in VR** |
| `restore.cmd` | double-click to put the game folder back to normal if anything went wrong |
| `collect-diagnostics.cmd` | double-click after a problem: zips the last session's log, the settings and basic system facts to send back |
| `ff7vr.ini` | the settings (open it in Notepad) |
| `xinput1_3.dll` | the mod itself |
| `ff7vr-launcher.ps1` | the script the `.cmd` files run |
| `README.md`, `VERSION.txt` | this guide; which version of the mod this is |
| `logs\` | one folder per session with the mod's log (created on the first session) |

**To play:** start Virtual Desktop's Streamer and connect the headset in Virtual
Desktop (it can also be done later), then double-click `start-vr.cmd`. The launcher:

1. finds the game through Steam and refuses politely if it is already running;
2. puts things back first if an earlier session was interrupted;
3. checks that Steam runs (it starts Steam if not) and warns if the Virtual Desktop
   Streamer does not seem to run. That is only a warning: the game then runs flat on
   the monitor, and the mod keeps trying to reach the headset every 5 seconds, so
   connecting it later is enough;
4. sets ReShade/Luma aside for the session (it renames `dxgi.dll` in the game's
   `End\Binaries\Win64` folder to `dxgi.dll.vr-disabled`; the two have not been
   tested together);
5. copies `xinput1_3.dll` and `ff7vr.ini` into that folder and records every change
   in `ff7vr.session.json` there;
6. starts the game (with `-d3d11`) and waits;
7. when you quit the game: keeps the session's log in `logs\<date-time>\`, removes
   the mod's files and puts `dxgi.dll` back.

Leave the launcher's window open while you play. If it gets closed anyway, or the PC
restarts during a session, double-click `restore.cmd` (or just start the next session:
it tidies up first). `restore.cmd` is harmless when nothing needs doing.

Options, from a command prompt in the package folder:

```
start-vr.cmd -KeepInstalled     leave the mod installed after the game exits (Luma stays set aside),
                                so the game can also be started from Steam with the mod.
                                restore.cmd removes it again.
start-vr.cmd -KeepLuma          do not set ReShade/Luma aside. Does not work yet: with ReShade/Luma
                                loaded the mod does not reach the headset and the game runs flat
start-vr.cmd -ExtraArgs "..."   extra arguments for the game
powershell -NoProfile -ExecutionPolicy Bypass -File ff7vr-launcher.ps1 status
                                show what is installed, change nothing
```

The launcher starts the game executable directly instead of through Steam, so the
launch options in the game's Steam properties are not applied; `-d3d11` is always
passed. Your save games are not touched by the mod or the launcher (the game
autosaves as usual).

## What you should see

- **On the monitor:** the game starts as usual. As soon as the 3D view is running in
  the headset, the mod switches the game to a 1280x720 window (the 3D rendering does
  not work in the game's fullscreen modes); the window shows a cropped view of the
  left eye with the HUD drawn over it.
- **In the headset, title screen, menus and loading screens:** the game on a flat
  virtual screen, 1.8 m wide, 2 m in front of you at eye height.
- **In the headset, in the world:** the 3D scene around you, from behind the
  character as in the normal game. The horizon stays level: the game camera turns
  you left and right, but looking up and down is done with your head. Leaning moves
  the view.
- **HUD and menus in the world:** on a flat panel about 3 m in front of you, 3.56 x
  2 m, that stays in place when you turn your head. Whatever the game shows full
  screen (main menu, command menu, dialogue) is on that panel too.
- Whenever the game stops delivering 3D images (a menu that stops the 3D view, a
  loading screen, a long hitch), the headset switches to the virtual screen and back
  without a black frame.
- The view is centred once, when the headset connects: face the direction you want
  to play in at that moment.

## Controls

The game's own controls are unchanged. The mod adds:

| Input | Effect |
|---|---|
| **Home** key (game window in focus) | switch between third and first person |
| Gamepad **View/Back + right stick click** together | the same; the game does not see this combination |

First person puts the view at the character's head, hides the character's body,
and blends over in about a third of a second. It only applies while the normal
follow camera is active: during a scripted camera shot the game's camera is used as
it is. A manual switch lasts until the next automatic one.

There is no recenter button in the mod (see known problems).

## Settings worth knowing

All settings are in `ff7vr.ini` in the package folder; each line has a comment. The
launcher copies the file into the game folder for every session, so edit the one in
the package folder. The ones you are most likely to touch on day one:

| Setting | Default | What it does |
|---|---|---|
| `[stereo] enabled` | `1` | `0` = no 3D: the whole game is shown on the virtual screen. The fallback if 3D misbehaves |
| `[xr] resolution_scale` | `1.0` | per-eye render size relative to what Virtual Desktop asks for. `0.8` renders 64 % of the pixels: faster, softer |
| `[foveation] preset` | `quality` | lower detail at the outer edges of each eye to save GPU time: `quality` (barely visible), `balanced`, `performance` (visibly blocky edges), `off` |
| `[ui] distance`, `[ui] size` | `3.0`, `2.0` | distance and height in metres of the HUD/menu panel. `size = 1.57` lines the markers over enemies up with the enemies; `2.0` is easier to read |
| `[ui] follow_head` | `0` | `1` = the panel follows your head |
| `[stereo] world_scale` | `1.0` | above 1 the world looks smaller (you become a giant), below 1 larger |
| `[first_person] enabled` | `1` | `0` = first person cannot be switched on at all |
| `[first_person] toggle_key` | `36` | the keyboard key for the switch, as a Windows virtual-key code (36 = Home, 35 = End, 45 = Insert, 0 = none) |
| `[first_person] default` | `0` | `1` = start in first person. Off because battles are not detected yet |
| `[camera] boom` | `level` | `game` = in third person, follow the game camera's height as it tilts (the eyes rise and sink) |
| `[stereo] decoupled_pitch` | `1` | `0` = apply the game camera's tilt to the view too (the horizon tilts) |
| `[screen] distance`, `width` | `2.0`, `1.8` | the virtual screen, in metres |
| `[log] level` | `debug` | how much goes into the log; `info` keeps it shorter |

## Performance

How it performs in a headset has not been measured: no headset was used during
development. Judge it on your own setup, against whatever you compare it with.

The mod logs its own frame timing. Every 10 seconds `ff7vr.log` gets a `timing:`
block: the frame rate over those 10 seconds, the mode (`screen` = virtual screen,
`stereo` = 3D), how many frames went to the headset, `errors`, and the GPU time of
the 3D scene with foveated rendering. A Quest 3 at 90 Hz needs a frame every
11.1 ms (80 Hz: 12.5 ms, 120 Hz: 8.3 ms).

For orientation only, two numbers from the development PC (RTX 5080, Ryzen 7
5800X3D), first room of one save game, standing still, without a headset:

- 3D at 2 x 2500x2600 pixels per eye, without a VR runtime (internal test mode, the
  game's frame cap lifted): 8.6 ms per frame for both eyes.
- Foveated rendering, GPU time of the 3D scene at 2 x 2496x2592 per eye with the
  simulated headset: 7.71 ms off, 6.76 ms `quality`, 6.38 ms `balanced`, 6.12 ms
  `performance`.

Busy scenes (combat, open areas) cost more than a quiet room, and Virtual
Desktop's video encoding comes on top.

## First-session checklist

Check in this order; each step depends on the ones before it. The log is
`logs\<date-time>\ff7vr.log` in the package folder after the session (during the
session it is `ff7vr.log` in the game's `End\Binaries\Win64` folder).

1. **The launcher ran through.** Its window says the mod was installed and the game
   started, and after quitting: "The game folder is back to normal".
2. **The mod loaded.** The log starts with `ff7vr 0.1.0 loaded` and contains
   `engine: stereo device installed`. If it says `engine: stereo will NOT be enabled`,
   the reason follows on the same line.
3. **The headset was reached.** `xr: session created ... runtime 'VirtualDesktopXR'`
   and `xr: frame loop running`. If you see `xr: no session (SystemUnavailable ...)`,
   the headset was not connected in Virtual Desktop at that moment; connecting it is
   enough, the mod retries every 5 seconds.
4. **Title screen on the virtual screen** in front of you at eye height, sharp, not
   too dark or washed out compared with the monitor.
5. **Load a save. The world in 3D.** Correct depth, both eyes aligned (no double
   vision when looking at a near object), the horizon level, the scene stable when
   you turn your head. The log shows `stereo: rendering STEREO ... (eye WxH ...)` with
   the per-eye size Virtual Desktop chose.
6. **Smoothness.** Turning your head must feel smooth. The log's `timing:` lines
   every 10 seconds show the frame rate and `errors 0`.
7. **HUD panel.** Complete to its corners, sharp, comfortable to read; open the
   command menu (Space or the pad's command button). Adjust `[ui] size` / `distance`
   if needed.
8. **Walking and turning the camera** with the stick or mouse: comfortable? Does the
   camera height stay steady when you tilt the game camera?
9. **First person** with Home or View + right stick click: the view at head height,
   the body hidden, back to third person with the same input.
10. **Foveated rendering.** Look straight ahead and let your eyes wander to the edges:
    it should look as sharp as before up to well beyond comfortable eye movement.
    Compare with `[foveation] enabled = 0`.
11. **Things the developer could not reach:** combat, a conversation, a real-time
    cutscene, a loading screen between areas, a pre-rendered movie, the pause menu.
    See the list below for what to look for.
12. **Quit the game** normally: the headset returns to Virtual Desktop's own view,
    the launcher tidies up. Afterwards ReShade/Luma works again in the flat game.

## Known problems and what has not been tested

Not tested at all:

- **A real headset.** Nothing has been seen through a lens: not the image sharpness,
  comfort, scale, stereo depth, the HUD panel's size and distance, or how foveated
  rendering looks behind the lenses. With Virtual Desktop and no headset connected,
  only this was checked: the game runs flat and the mod retries every 5 seconds.
- **Combat, conversations, real-time cutscenes, loading screens between areas,
  pre-rendered movies, the pause menu.** None of these was reached in testing. What
  to look for: scripted camera shots should be shown as the game frames them (the
  mod then uses the game's camera); a battle is played in third person only if you
  are in third person (battles are not detected yet); the camera may cut between
  shots, which can be uncomfortable in a headset.
- **Pre-rendered movies** are very likely wrong: movie detection exists but is off
  (`[stereo] movie_screen = 0`) because it was never seen to work. A movie may be
  shown inside the 3D scene or the HUD panel instead of on the virtual screen.
  Setting `[stereo] movie_screen = 1` is worth a try if movies look broken.
- **A connected gamepad.** The gamepad combination was tested only with a simulated
  button state; the game reads the pad only while one is connected.
- **Disconnecting and reconnecting the headset** during a session, and quitting from
  Virtual Desktop's menu, were tested with SteamVR's virtual headset only.
- **Other save games and areas** than the first rooms of the save used in testing
  (Sector 7 slums). Sunlit areas, fog and big open areas are untested in 3D.
- **ReShade/Luma together with the mod.** Not tested; the launcher sets it aside.
- **The game's HDR output.** The virtual screen assumes normal (SDR) output; with HDR
  switched on in the game it will look wrong.

Known problems:

- **No recenter button.** The view is centred once, when the headset connects. The
  Quest's own recenter (holding the Meta button) has not been tried with the mod;
  if the view ends up off to the side, restarting the game re-centres it.
- **Smooth turning only.** The game camera turns you smoothly, as in the flat game;
  there is no snap turn. This can be uncomfortable for some players.
- **Markers over enemies and objects are slightly off** on the default HUD panel
  (3 to 6 degrees towards the edges). `[ui] size = 1.57` lines them up; the larger
  default is easier to read.
- **The game window becomes 1280x720 while 3D runs** if the game was in a fullscreen
  mode, and is not switched back when the game exits. Whether the game remembers the
  small window for the next flat session is untested; if it does, set the display
  mode again in the game's options.
- **First person without battle detection:** if you are in first person when a
  battle starts, you stay in first person. Switch with Home or View + right stick.
- **The desktop window** shows a crop of the left eye, not the full picture.
- **Some effects stay as in the flat game:** the vignette (darker image corners),
  and the game's depth of field in cutscenes. Camera motion blur and chromatic
  aberration are switched off while 3D runs.
- **Square Enix's lens glare effect** may appear in the wrong eye in scenes that use
  it (not seen in testing).
- Frame pacing with Virtual Desktop is unknown: with SteamVR's virtual headset the
  runtime sometimes blocked for 6 to 7 ms per frame during the first 15 to 20
  seconds of a session.

## When something is wrong: what to send

1. **The diagnostics zip.** After quitting the game, double-click
   `collect-diagnostics.cmd`. It writes `diagnostics-<date-time>.zip` into the
   package folder with the last session's log folder (`ff7vr.log`, and a
   `ff7vr-crash-*.dmp` if the game crashed), your `ff7vr.ini`, `VERSION.txt` and a
   `system.txt` (Windows version, graphics card and driver, the OpenXR runtimes
   and which one the mod used, the state of the game folder). It changes nothing.
   If you ran several sessions since the problem, send that session's folder from
   `logs\` as well.
2. A sentence on what you saw in the headset and when (the log has times; note the
   time on the clock), and a screenshot of the monitor if it shows the problem.

To narrow a problem down, change one setting at a time and start a new session:

| If the problem is... | Try |
|---|---|
| anything in 3D (image, depth, flicker, crash in the world) | `[stereo] enabled = 0`: if the virtual screen works, the 3D rendering is at fault |
| HUD or menus missing, cut off, doubled | `[ui] layer = 0` (the HUD goes back into the 3D image, cropped) |
| blocky or shimmering edges, any odd shading | `[foveation] enabled = 0` |
| uncomfortable camera height or movement | `[camera] boom = game`, then `[stereo] decoupled_pitch = 0` |
| anything in first person | `[first_person] enabled = 0` |
| stutter or low frame rate | `[xr] resolution_scale = 0.8`, then `[foveation] preset = balanced` |
| the game does not start or crashes at once | `restore.cmd`, then start the game from Steam without the mod to rule out the game itself |

## Building from source

Visual Studio 2022 or later with the C++ workload; everything else is fetched on the
first build. One command builds a release and assembles the package in `dist\`:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1
```

Keep the checkout at a short path (for example `C:\src\ff7-remake-vr`): the
dependency build can exceed Windows' path length limit otherwise. Developer
documentation: `docs/dev-harness.md` (build, run, test without a headset),
`docs/engine-module.md`, `docs/render.md`, `docs/benchmarking.md`.

## What the launcher changes on the PC

Only the game's `End\Binaries\Win64` folder, and only for the session:
`dxgi.dll` is renamed to `dxgi.dll.vr-disabled`, and `xinput1_3.dll`, `ff7vr.ini`,
`ff7vr.session.json` are added (the mod writes `ff7vr.log` there while it runs).
Afterwards those are removed and `dxgi.dll` gets its name back. No game file is
modified, nothing is installed, the PC's OpenXR settings are not changed, and the
Steam launch options are not touched. The mod's name `xinput1_3.dll` is how the game
loads it: the game loads a file of that name from its own folder before the one in
Windows.
