# ff7vr: VR for FINAL FANTASY VII REMAKE INTERGRADE (PC)

ff7vr is a VR mod for the Steam version of FINAL FANTASY VII REMAKE INTERGRADE. It
makes the game's own engine render a separate image for each eye at the headset's
resolution: first person while exploring, third person in battles, switchable at any
time. You play with the gamepad or keyboard and mouse; motion controllers are not
used. It talks to the headset through OpenXR.

This is an unofficial fan project, not affiliated with or endorsed by Square Enix.
The game is not included: you need your own copy on Steam.

The details (what the launcher does, a first-session checklist, the full lists of
known problems and untested areas, troubleshooting, performance) are in
`docs/guide.md`, which is `GUIDE.md` in the package folder.

## State

Played on a Meta Quest 3 through Virtual Desktop at 3072x3264 pixels per eye, 72 and
90 Hz, on one PC (RTX 5080), in the first areas of one save game (Sector 7 slums).
Confirmed there: 3D at the headset's resolution, the HUD on its own floating panel,
first and third person, and the fixes for a right-eye ghost and for lighting faults
on skin indoors. Not tried yet: battles, conversations, cutscenes, movies, a real
gamepad, other headsets and other PCs. Expect rough edges.

## Requirements

- FINAL FANTASY VII REMAKE INTERGRADE on Steam, file version 1.0.0.7 (the current
  Steam build). Another build is refused by the mod: the game then runs without VR.
- Steam running.
- A PC VR headset with an OpenXR runtime (Virtual Desktop, SteamVR, Meta Quest Link,
  ...). The mod picks the runtime that has a headset connected by itself; to pin one,
  set `[xr] runtime` in `ff7vr.ini`. The PC's default OpenXR runtime is not changed.
- An NVIDIA graphics card for foveated rendering; on other cards the mod works
  without it.
- Windows 10 or 11.

## Playing

Start your headset's PC app (for example the Virtual Desktop Streamer or SteamVR) and
connect the headset (this can also be done later), then double-click `start-vr.cmd` in the package folder. The launcher sets
ReShade/Luma aside, copies the mod into the game folder, starts the game and waits;
when the game exits it puts everything back and keeps the session's log in `logs\`.
Leave its window open while you play.

Title screen, menus and loading screens appear on a flat virtual screen; in the world
you are in 3D, with the HUD on a panel in front of you. The view is centred when the
headset connects (End recenters later). While 3D runs, the game on the monitor
becomes a 1280x720 window for that session only.

If a session was interrupted or anything looks wrong in the game folder, double-click
`restore.cmd` (harmless when nothing needs doing). Save games are not touched.

Without the launcher: unzip the `-dropin.zip` into the game's `End\Binaries\Win64`
folder (beside `ff7remake_.exe`) and start the game from Steam, or with
`ff7vr-start.cmd` there. The log is `ff7vr.log` in that folder; to remove the mod,
delete the files the zip added. Details (ReShade/Luma, DLSS): `GUIDE.md`,
"Installing without the launcher".

## Controls

The game's own controls are unchanged. The mod adds these; the keys work while the
game window has the focus, and on the gamepad you **hold View/Back and press** the
second button, or click both sticks together (the game does not see these
combinations):

| Keyboard | Gamepad | Effect |
|---|---|---|
| **Home** | **both stick clicks together (L3+R3)**, or hold View/Back + right stick click | switch between first and third person |
| **End** | hold View/Back + left stick click | **recenter**: the direction you face now becomes forward, and the HUD panel and the virtual screen move in front of you |
| **Insert** | hold View/Back + Menu/Start | 3D off (the game on the virtual screen) and on again. If the headset has lost the game (for example after Virtual Desktop or SteamVR was closed and opened again), the same key reconnects it |
| **Page Down** / **Page Up** | hold View/Back + D-pad down / up | HUD/menu panel 0.25 m nearer / farther |

## Settings worth knowing

All settings are in `ff7vr.ini` in the package folder, each with a comment. The
launcher copies it into the game folder for every session, so edit the one in the
package folder.

| Setting | Default | What it does |
|---|---|---|
| `[stereo] enabled` | `1` | `0` = no 3D: the whole game is shown on the virtual screen. The fallback if 3D misbehaves |
| `[xr] runtime` | `auto` | which OpenXR runtime drives the headset. `auto` takes the first one with a headset connected (runtimes whose PC app runs first, then the PC's default); pin one with `virtualdesktop`, `steamvr`, `system` or the path of a runtime's `.json` |
| `[xr] resolution_scale` | `1.0` | per-eye render size relative to what the headset's runtime asks for. `0.8` renders 64 % of the pixels: faster, softer |
| `[foveation] preset` | `performance` | lower detail at the outer edges of each eye to save GPU time: `performance` (not noticeable on a Quest 3 in play), `balanced`, `quality` (smaller saving), `off` |
| `[foveation] eye_tracking` | `0` | `1`: the full-detail area follows your eyes on a headset with eye tracking (untested with a real one) |
| `[ui] distance`, `[ui] size` | `3.0`, `2.0` | distance and height in metres of the HUD/menu panel (Page Down/Up change the distance during play, until the game is restarted). `size = 1.57` lines the markers over enemies up with the enemies; `2.0` is easier to read |
| `[ui] follow_head` | `0` | `1` = the panel follows your head |
| `[stereo] world_scale` | `1.0` | above 1 the world looks smaller (you become a giant), below 1 larger |
| `[first_person] default` | `1` | `0` = third person from the start and after every battle |
| `[first_person] enabled` | `1` | `0` = first person is off completely (no key, no pad combination, no automatic switch) |
| `[first_person] auto_combat` | `1` | `0` = no automatic third person in battles |
| `[first_person] battle_signal` | see the ini | how a battle is detected. Empty (`battle_signal =`) if battles stay in first person or exploration switches to third person by itself |
| `[first_person] toggle_key`, `[controls] ..._key` | Home, End, Insert, Page Down/Up | the keys of "Controls", as Windows virtual-key codes (0 = none) |
| `[controls] pad` | `1` | `0` = no gamepad combinations for recenter, 3D on/off and the panel distance |
| `[controls] fp_toggle_chord` | `L3+R3` | the gamepad buttons pressed together that switch first/third person; empty = off. A single stick click still reaches the game, at most 150 ms (`fp_toggle_chord_ms`) late |
| `[camera] boom` | `level` | `game` = in third person, follow the game camera's height as it tilts (the eyes rise and sink) |
| `[stereo] decoupled_pitch` | `1` | `0` = apply the game camera's tilt to the view too (the horizon tilts) |
| `[screen] distance`, `width` | `2.0`, `1.8` | the virtual screen, in metres |
| `[stereo] movie_screen` | `0` | `1` = pre-rendered movies on the virtual screen; try it if movies look broken in 3D |
| `[stereo] ao_fix` | `1` | corrects the right eye's ambient occlusion (without it the right eye shows a dark ghost of nearby objects). `0` only to compare |
| `[stereo] light_fix` | `1` | corrects indoor lamp lighting in 3D (without it skin shows white blocks indoors). `0` only to compare |
| `[picture] brightness`, `contrast`, `saturation`, `gamma`, `black_level` | `0`, `1`, `1`, `1`, `0` (no change) | colour of the 3D view and the virtual screen in the headset (the HUD panel stays as drawn). A starting point for a washed-out look: `-0.05`, `1.15`, `1.1`, `1`, `-0.01`. `[controls] brightness_up_key` / `brightness_down_key` change the brightness during play |
| `[stereo_cvars]` | four level-of-detail lines | the game's console variables, `name = value`, applied only while 3D renders. The shipped lines push the engine's detail levels further out (the wide per-eye view makes it drop detail much nearer than in the flat game). Uncommenting `r.BloomQuality = 0` makes the picture a little less washed out |
| `[log] level` | `debug` | how much goes into the log; `info` keeps it shorter |

## Known problems

- The picture looks washed out and too bright in the headset; cause unknown. Bloom
  off (`r.BloomQuality = 0` in `[stereo_cvars]`) helps a little.
- Smooth turning only; there is no snap turn.
- Markers over enemies are 3 to 6 degrees off on the default HUD panel size
  (`[ui] size = 1.57` lines them up).
- The game window becomes 1280x720 while 3D runs, until the game exits.
- With ReShade/Luma loaded the game runs in 3D, but the right eye shows only a strip
  of the scene; the launcher sets it aside for each session (rename its `dxgi.dll`
  for a drop-in install).
- Battle detection has never been seen in a real battle; switch by hand with Home if
  needed.
- Pre-rendered movies are very likely wrong (`[stereo] movie_screen = 1` is worth a
  try).
- In third person the eyes can end up right behind an obstacle or a person
  (`[camera] boom = game` is the alternative).

## If something is wrong

After quitting the game, double-click `collect-diagnostics.cmd`: it zips the last
session's log, your settings and basic system facts into the package folder and
changes nothing. Send that with a sentence on what you saw and when. To narrow it
down, change one setting at a time:

- anything in 3D: `[stereo] enabled = 0` (if the virtual screen works, 3D is at fault)
- HUD or menus missing or cut off: `[ui] layer = 0`
- blocky edges or odd shading: `[foveation] enabled = 0`
- stutter: `[xr] resolution_scale = 0.8`

The full table is in the guide.

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

## Licence

MIT, see `LICENSE`. The third-party components the mod is built with, and their
licences, are listed in `THIRD-PARTY-NOTICES.md`.
