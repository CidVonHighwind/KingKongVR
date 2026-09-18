// User settings from kkvr.ini beside the DLL. The file is created with
// defaults on first run so there is something to edit.
#pragma once

#include <string>

namespace kkvr {

// Xbox controller inputs, in the order of Config::pad_buttons: their names
// in [controller] and in test scripts (autotest.h).
constexpr int kPadControlCount = 16;
inline constexpr const char* kPadControlNames[kPadControlCount] = {
    "a",  "b",  "x",  "y",  "lb",      "rb",        "back",      "start",
    "ls", "rs", "lt", "rt", "dpad_up", "dpad_down", "dpad_left", "dpad_right"};

struct Config {
  bool windowed = true;       // force windowed instead of fullscreen exclusive
  bool borderless = false;    // popup window without title bar and border
  int window_width = 1280;    // client area; the backbuffer is scaled into it
  int window_height = 720;
  int fps_limit = 120;        // 0 = off; the headset's refresh rate
  bool fix_frame_step = true; // lower the engine's 0.01 s step floor (timing.h)
  int aspect_fix = 1;          // AspectFix: projection vs. backbuffer aspect

  bool vr_enabled = true;     // show the game in the headset via OpenXR
  float screen_distance = 0.6f;       // metres in front of the head
  float screen_width = 1.2f;          // metres; height follows the aspect
  float screen_height_offset = 0.0f;  // metres above eye level
  float world_scale = 1.0f;           // window mode: game units per metre ...
  float world_scale_width = 1.1f;     // ... at this window width (metres)
  // [vr] cinematic_fov: a shot with a narrower horizontal field of view than
  // this (degrees) is a cinematic, filmed from closer than its lens implies,
  // and is shown on the window instead of through it (eye_renderer.cpp).
  // Gameplay is 85 degrees, the menu scene 57, cinematics 29. 0 = never.
  float cinematic_fov = 45.0f;
  // [vr] cinematic_scale: how much of the window such a shot fills. A cutscene
  // is framed for a monitor, so filling the window puts a face a metre tall an
  // arm's length away; 0.6 shows it like a cinema screen instead.
  float cinematic_scale = 0.6f;
  bool guide_recenter = true;         // [controller] Xbox button recentres (like F8)
  bool frame_times = true;            // [display] frame_times: kkvr_frametimes.csv + log lines
  float refresh_rate = 120.0f;        // headset Hz to request (0 = default)
  bool simulate_headset = false;      // run a synthetic headset instead of OpenXR

  // [render] replays_per_frame: eye pairs per game frame (replay mode). The
  // limiter paces pairs at fps_limit, so the game runs at fps_limit / this.
  int replays_per_frame = 1;  // 0 = auto: headset rate / game_fps
  int game_fps = 60;
  // [render] verify: check the vertex arena against ground truth (slow;
  // "verify:" log lines): every 8th frame, the bytes each draw reads are
  // compared with a CPU copy of everything the game wrote.
  bool verify = false;
  // [render] execute_live (checks): also execute every recorded call while it
  // is recorded, as a reference image for the renderer (glitch_catch).
  bool execute_live = false;
  // [render] render_scale: the eyes render at this multiple of the game's
  // resolution (eye_renderer.h); 1 = game resolution. 0.5 to 3.
  float render_scale = 1.0f;
  // [render] hide_letterbox: the game letterboxes its cinematics to 16:9 with
  // black bars inside the 4:3 frame; the window is 4:3, so they only make the
  // picture smaller (eye_renderer.cpp).
  bool hide_letterbox = true;
  bool glitch_catch = false;  // [render] glitch_catch: save single-frame glitches (glitch_catcher.cpp)
  // [test] synthetic head for [vr] simulate_headset.
  float sim_head_amplitude = 0.10f;  // metres
  float sim_head_frequency = 1.0f;   // Hz
  float sim_head_rotation = 15.0f;   // degrees of yaw sway (pitch 0.6x, roll 0.3x)
  // [test] sim_headset_width/height, sim_headset_fov: the eye image the
  // simulated headset asks for and its field of view in degrees. The defaults
  // are small and fast; set 3072x3264 / 94 to mimic a Quest 3 through Virtual
  // Desktop (what the user runs), so captures have the real pixel geometry.
  int sim_headset_width = 1344;
  int sim_headset_height = 1440;
  float sim_headset_fov = 94.0f;
  // [test] output_grid: draw a test grid, to find which stage deforms the
  // image. Green in the eye image (part of the game's picture, moves with the
  // window), red in the headset image (display space, fixed to the view). A
  // kink or step in the green grid is ours; in the red grid the compositor's.
  bool output_grid = false;
  // [display] preview_zoom (with preview_zoom_x/y, 0..1 in the eye image): the
  // desktop view shows this magnification of the eye image, with point
  // sampling, so single rendered pixels are visible without a headset.
  float preview_zoom = 1.0f;
  float preview_zoom_x = 0.5f;
  float preview_zoom_y = 0.5f;
  // [test] sim_head_step: degrees per numpad press for the simulated head
  // (Shift = 4x). With sim_head_amplitude and sim_head_rotation 0 the head
  // only moves when a key is pressed, so two captures differ by exactly one
  // step.
  float sim_head_step = 0.5f;
  // [test] debug_keys: the frame-bisect keys ('-', '=', F11, F12). Off for
  // players - '-' and '=' are ordinary keys.
  bool debug_keys = false;
  // [test] dump_passes: a frame capture (F2) also writes every render target
  // of the left eye as it is finished, numbered in the order the frame draws
  // them, so the pass where an artefact appears can be found.
  bool dump_passes = false;
  // [test] share_eye_targets: both eyes use one set of the game's render
  // target textures, as before 2026-09-18. The game's post effects carry
  // content from one render into the next, so each eye then reads the other's
  // picture: a ghost, offset. Here to measure that difference again.
  bool share_eye_targets = false;
  // [test] flat: render the frame with the game's own camera instead of the
  // eye's, i.e. what the game would put on a monitor. Ground truth for
  // "is this missing because of our projection, or does the game not draw it".
  bool flat = false;
  std::string test_script;           // [test] script: autotest file (autotest.h)

  // [controller]: game button index (0..15, -1 = none) for each Xbox input,
  // in the order a b x y lb rb back start ls rs lt rt dpad_up dpad_down
  // dpad_left dpad_right. See joystick.h for the indices.
  int pad_buttons[kPadControlCount] = {0, 3, 2, 1, 4, 5, 8, 9,
                                       10, 11, 6, 7, 12, 14, 15, 13};
};

// [display] aspect_fix. The game projects for 16:9 at any resolution.
enum AspectFix {
  kAspectFixOff,     // keep the game's projection (stretched at non-16:9)
  kAspectFixCrop,    // keep vertical view, narrow horizontal (safe for culling)
  kAspectFixExpand,  // keep horizontal view, show more vertically
};

// Settings as read at startup. Live changes are not reflected here.
const Config& GetConfig();

// Write the live-tunable screen placement back to kkvr.ini.
void SaveScreenSettings(float width, float distance);

}  // namespace kkvr
