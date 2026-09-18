// Scripted test runs: drive the game without a person or a controller.
//
// [test] script=<file> (relative to the game folder) enables it. The script
// is read once at startup; times are seconds since the first Present.
//
//   # comment
//   @2.0  press a            tap an Xbox control (150 ms)
//   @2.5  press start 400    hold for 400 ms
//   @3.0  stick left 0 1 1500    left stick x y (-1..1, y up) for 1500 ms
//   @3.0  stick right 1 0 500    right stick
//   @4.0  shot menu          save both eyes to screenshots\menu_L.png / _R.png
//   @4.5  hotkey F7          press a mod hotkey (add "shift" for Shift+key)
//   @5.0  log some text      write a marker into kkvr.log
//   @5.5  capture            deep frame dump + "trace:" lines for the next frame
//   @6.0  quit               end the game process
//   @0    press_until a 0.4 gameplay 120
//                            tap A every 0.4 s until the condition has held
//                            for 2 s (or 120 s passed); the script clock
//                            stops meanwhile, so later times count from then
//   @0    wait gameplay 60   the same without pressing anything
//
// Conditions come from the frame statistics:
//   gameplay  8+ render-target switches per frame (the level's post effects;
//             videos, loading screens and menus have 1)
//   menu      300+ draws with fewer than 8 target switches (3D menu backdrop)
//
// Control names are those of [controller] (kPadControlNames, config.h): a b x
// y lb rb back start ls rs lt rt dpad_up dpad_down dpad_left dpad_right.
// Script input goes through the same mapping as a real pad and is combined
// with it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace kkvr {

struct AutotestInput {
  uint32_t controls = 0;  // bit i = control i of kPadControlNames pressed
  bool left_stick = false;
  float lx = 0.0f, ly = 0.0f;  // -1..1, y up
  bool right_stick = false;
  float rx = 0.0f, ry = 0.0f;
};

// Commands for the device, due this frame.
struct AutotestAction {
  enum Kind { kScreenshot, kHotkey, kCapture, kQuit } kind;
  std::string name;  // screenshot name
  int vk = 0;        // hotkey
  bool shift = false;
};

// Loads the script named in [test] script, if any. Safe to call repeatedly.
void AutotestLoad();

// Test runs: the game ignores controller input while its window is inactive
// (it tracks WM_ACTIVATE / WM_ACTIVATEAPP; it imports no focus query). So a
// person using the PC during a run would silently break the script. This
// subclasses the game window and reports it as always active.
void AutotestKeepWindowActive(HWND window);
bool AutotestActive();

// Is a window of this process (the game's) in the foreground?
bool ProcessOwnsForeground();

// What the last frame looked like (for wait conditions).
struct AutotestFrame {
  uint32_t draws = 0;
  uint32_t render_target_switches = 0;
};

// Game thread, once per frame: advance the script clock, return due actions.
std::vector<AutotestAction> AutotestTick(const AutotestFrame& frame);

// Current scripted input (any thread that runs on the game thread's input
// path: the joystick hooks).
AutotestInput AutotestCurrentInput();

}  // namespace kkvr
