# King Kong VR

A free VR mod for **Peter Jackson's King Kong: The Official Game of the Movie**
(PC, 2005).

The game appears as a **window into its world**, hanging in front of you: each
eye sees the scene from its own position, so you get real stereo depth, and
moving your head lets you look around the window's edges, the way you would
through a real one. The game's own camera and controls are untouched — you
play with a controller, as on a monitor.

> Not affiliated with Ubisoft or Universal. You need your own copy of the game;
> this mod contains none of its files.

## Requirements

- The PC version of the game that includes `kingkong9d.exe` (tested with the
  Signature Edition).
- Windows 10 or 11.
- A PCVR headset with an **OpenXR runtime that provides a 32-bit runtime** —
  the game is a 32-bit program. Tested: Meta Quest 3 through **Virtual
  Desktop**. SteamVR also registers a 32-bit OpenXR runtime (untested).
- An Xbox controller (or any XInput pad).
- A graphics card that can render two eyes at your headset's rate. Tested on
  an RTX 5070 Ti: 120 Hz with room to spare.

## Install

1. Copy `d3d9.dll` and `PlayKingKong.bat` from this package into the game's
   folder — the one that contains `kingkong9d.exe`.
2. Recommended game settings (in the game's `SettingsApplication.exe`):
   - resolution **1920x1440** (a 4:3 resolution suits the window best),
   - anti-aliasing **4x** (8x can drop below 120 fps),
   - any display mode — the black bands of "4/3 Black bands" are removed.
3. Start your headset's streaming (Virtual Desktop: set it to **120 Hz**).
4. Start the game with **`PlayKingKong.bat`**. The game's own shortcut may
   point to a folder that no longer exists, and the game refuses to start
   without the `KingKongTheGame.bf` argument the batch file passes.

On the first start the mod writes `kkvr.ini` next to the game with the
settings you are likely to change.

## Controls

| Key | |
|---|---|
| **Xbox button** or **F8** | recentre the window in front of you |
| **F3** / **Shift+F3** | window bigger / smaller |
| **F4** / **Shift+F4** | window closer / farther |
| **F2** | save a frame capture (`captures\` in the game folder) — useful for bug reports |

Window size and distance are saved to `kkvr.ini`. The game itself is played
with the controller (button layout: see *More settings* below).

## Settings

All in `kkvr.ini` (created on first start; delete it to get the defaults back).

| Setting | Default | |
|---|---|---|
| `[display] fps_limit` | 120 | game frames per second: your headset's refresh rate, or half of it if your PC cannot keep up (the headset still runs at its own rate and follows your head) |
| `[vr] screen_width`, `screen_distance` | 1.2, 0.6 | the window, in metres (F3/F4 change them) |
| `[vr] cinematic_scale` | 0.6 | how much of the window a cutscene fills (0.1 to 1) |
| `[render] render_scale` | 1.0 | sharpness: 1.0 = one rendered pixel per headset pixel; lower it if the game stutters |

### More settings

Not in the file by default; add a line under the section to change one.

| Setting | Default | |
|---|---|---|
| `[vr] screen_height_offset` | 0.0 | window height above eye level, in metres |
| `[vr] world_scale` | 1.0 | how big the game world is relative to the window |
| `[vr] cinematic_fov` | 45 | cutscenes filmed with a narrower lens than this (degrees) go onto the window; 0 = never |
| `[vr] refresh_rate` | 120 | headset refresh rate to request, in Hz (0 = the runtime's default) |
| `[vr] enabled` | 1 | 0 = play on the monitor only |
| `[render] hide_letterbox` | 1 | remove the game's black bands |
| `[display] window_width`, `window_height` | 1280, 720 | size of the game window on the desktop |
| `[display] borderless` | 0 | 1 = no title bar |
| `[controller] guide_recenter` | 1 | 0 = the Xbox button does not recentre |
| `[controller] a`, `b`, `x`, `y`, `lb`, `rb`, `lt`, `rt`, `back`, `start`, `ls`, `rs`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | see below | which button of the game's console pad each Xbox button presses |

Button layout: each Xbox button is set to a button of the console pad the game
expects, by position (-1 = none): 0 bottom (confirm), 1 top, 2 left, 3 right
(back), 4 black, 5 white, 6 left trigger, 7 right trigger, 8 back, 9 start,
10 left stick click, 11 right stick click, 12 up, 13 right, 14 down, 15 left.
Defaults: `a=0 b=3 x=2 y=1 lb=4 rb=5 lt=6 rt=7 back=8 start=9 ls=10 rs=11
dpad_up=12 dpad_right=13 dpad_down=14 dpad_left=15`.

## Troubleshooting

- **Everything is in `kkvr.log`** in the game folder — the first lines say
  which headset and runtime were found, and every few seconds it logs the
  frame rate and frame times. Include it with any bug report.
- **Nothing in the headset:** start the VR runtime's streaming before the
  game, and make sure it provides a 32-bit OpenXR runtime (Virtual Desktop
  does).
- **Missed frames / stutter:** lower `[render] render_scale` (e.g. 0.8) or the
  game's anti-aliasing, or set `[display] fps_limit` to half the headset's
  rate (60 at 120 Hz).
- **Stuck at an invisible wall, or unable to climb a slope:** set
  `[display] fps_limit=60` in `kkvr.ini` and restart the game. Some of the
  game's movement may not work at 120 fps; the headset keeps its own rate.
- **The settings application does not start** ("not properly installed"): a
  copied game is missing its install location in the registry. Copy
  `install_registry.bat` from this package into the game folder, right-click
  it and choose *Run as administrator*. `uninstall_registry.bat` removes the
  entries again (only use it if you ran `install_registry.bat`).

## Uninstall

Delete `d3d9.dll`, `kkvr.ini` and `kkvr.log` from the game folder. The game
had no `d3d9.dll` of its own.

## How it works

The mod is a `d3d9.dll` that the game loads instead of Windows' own. The game
thinks it is talking to Direct3D 9; the mod passes its calls on, but not
straight away:

```
 game ──Direct3D 9 calls──▶ record the frame ──at Present──▶ render it per eye ──▶ OpenXR thread ──▶ headset
                            (nothing drawn yet)              (left, right)         (shared GPU textures)
```

1. **Record.** Every draw call and state change of a frame is recorded, not
   executed. The game's queries ("which texture is bound?") are answered from
   a model of the device state the mod keeps. Vertex data the game streams
   through one reused buffer is redirected into an append-only buffer, so
   every recorded draw still finds its vertices later.
2. **Render per eye.** When the game presents the frame, the mod renders the
   recording twice, once from each eye's real position, taken from the
   headset's pose. Each eye looks through the window: an off-axis projection
   from the eye through the window's four corners, at the headset's pixel
   density and only for the part of the view the window covers. The game's
   own camera stays as it is; the eye's offset goes into the projection, so
   lighting, fog and reflections still work in the game's camera space.
   Shader draws, 2D layers (menus, HUD), full-screen post effects and
   long-lens cutscenes each get their own treatment. Each eye has its own
   copies of the game's render targets, so effects that reuse the previous
   frame do not mix the two eyes.
3. **Hand over.** Both eye images go to a separate OpenXR thread as shared
   GPU textures (no copy through the CPU). That thread runs at the headset's
   rate and submits them as a projection layer; the runtime corrects each
   frame for the latest head rotation.

A few patches in the game's executable make this work: the frame-step floor
(the engine would otherwise run fast above 100 fps), the screen aspect table
(a real 4:3 picture instead of a squeezed 16:9 one) and the controller input
(XInput instead of the game's old joystick bindings).

Source layout (`src/`, includes are relative to it):

| Folder | What is in it |
|---|---|
| `device/` | the `d3d9.dll` export and the wrapped Direct3D 9 device: Present, Reset, windowed mode, hotkeys, lock hooks |
| `record/` | the game's frame as a recording, the model of its device state, the vertex arena |
| `render/` | each eye rendered from the recording, the window maths, the hand-over to the headset, the desktop view |
| `xr/` | the OpenXR thread |
| `game/` | patches of the game executable: frame step, screen aspect, controller |
| `common/` | settings (`kkvr.ini`), log, crash reports |
| `debug/` | test automation, glitch catcher, frame time statistics |

Every byte of the game executable the mod relies on is listed, with its
address and meaning, in `PATCH_SITES` in `tools/re/kkre.py`;
`python tools/re/kkre.py verify` checks an executable against it.

## Building from source

Visual Studio 2022 or newer (C++), CMake 3.20+, Python 3 for the tools.

```
cmake -S . -B build -A Win32        # the game is 32-bit
cmake --build build --config Release
powershell -File tools\package.ps1  # dist\KingKongVR-<version>.zip
```

Developer tools: `tools/autotest/suite.py` (regression suite: one game start
with a simulated headset, checks projection, rendering and frame times),
`tools/autotest/run.ps1` (scripted runs, script format in
`src/debug/autotest.h`) and F2 frame captures of every render pass.
