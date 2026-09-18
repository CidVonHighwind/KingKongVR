// Game frame timing (kingkong9d.exe, from disassembly).
//
// Every frame the engine (0xa051a0) takes the real time since the last frame
// and clamps it to [0.01 s, 0.2667 s] before advancing the game clock
// (0xf357a0, scaled by the game-speed factor). So the game runs at correct
// speed up to 100 fps and too fast above it: 120 fps is 20% fast, and an
// uncapped window at ~180 fps was 1.8x. We point the clamp's lower bound at
// our own value so higher frame rates keep real-time speed.
#pragma once

namespace kkvr {

// Lower the minimum frame step to `seconds`. Checks the exact instruction
// bytes first; another build of the game is left alone. Returns true if
// patched.
bool PatchMinimumFrameStep(float seconds);

// Game clock in seconds (the value the engine accumulates), or a negative
// value if unavailable.
float GameClockSeconds();

}  // namespace kkvr
