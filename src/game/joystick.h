// Xbox controller support.
//
// How the game reads a controller (kingkong9d.exe, from disassembly):
//
//   The input driver's update (0xa00750, called through the table entry
//   0x3de9368 with the button buffer 0x3de9280) fills one byte per button,
//   0xff = pressed. The indices follow the console pad the game's logic was
//   written for (original Xbox order):
//
//      0 A / confirm   4 Black / L2   8 Back / Select  12 D-pad up
//      1 Y (top)       5 White / R2   9 Start          13 D-pad right
//      2 X (left)      6 L trigger   10 L stick click  14 D-pad down
//      3 B / back      7 R trigger   11 R stick click  15 D-pad left
//
//   (by pad position; 3 = menu back and 0 = confirm are verified by the
//   autotest, 1 vs. 2 is not)
//
//   Evidence: Esc is hard-wired to 9 (Start); the POV hat is decoded to
//   12..15 in that order; the HKCU\JADE_Joy\USBPS2Version setting swaps
//   exactly 9/11 or 10/11 (differences between PS2 USB adapters); the
//   keyboard handler (0x9ff0a0) puts fire on 7; in play, index 0 confirms in
//   menus (a first guess of 0 = Triangle had Y selecting) and only index 3
//   goes back (an earlier 1 = back left B dead in menus).
//
//   The joystick itself comes from winmm joyGetPosEx (0x9ffae0). Buttons 0..7
//   only reach the buffer through the registry "Joy bindings", which can
//   produce just a few of those indices per character; raw buttons 8..15 are
//   copied straight to indices 8..15. Axes X/Y are movement, Z/R the camera.
//
// What we do:
//   - Patch the game's winmm imports: joyGetPosEx reports the sticks from
//     XInput (left on X/Y, right on Z/R) and no
//     buttons, so the registry bindings never fire.
//   - Wrap the driver update: after the game's own update (keyboard included),
//     set the buttons from XInput through the [controller] mapping in
//     kkvr.ini (Xbox control -> button index).
#pragma once

namespace kkvr {

// Called from DllMain; patches the game's winmm joystick imports.
void InstallJoystickHooks();

// Wraps the driver update once the game has installed it. Called from the
// joystick poll and from Present (the game skips polling if winmm reports no
// joystick); cheap once done.
void HookDriverUpdate();

// Is the Xbox (Guide) button of the active pad held? The documented XInput
// API does not report it; XInput 1.4's undocumented XInputGetStateEx (export
// ordinal 100) does (bit 0x0400). False if that export is missing. Windows may
// also open the Game Bar on this button ("Open Xbox Game Bar using this
// button on a controller" in the Windows settings).
bool JoystickGuideButton();

}  // namespace kkvr
