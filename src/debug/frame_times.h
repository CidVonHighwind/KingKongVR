// Per-frame timing log ([display] frame_times=1, on by default).
//
// Averages hide the single slow frames that are felt as stutter, so every
// frame is written to kkvr_frametimes.csv next to the DLL (overwritten at
// start) and every 300 frames kkvr.log gets a "frametime:" line with the
// median, 99th percentile and worst frame and how many headset frames were
// missed.
//
// Sections of a frame (milliseconds), measured in D3D9Device::Present:
//   interval  Present start to Present start (the frame time)
//   game      end of the previous Present to this Present (game code, draws)
//   submit    both eyes rendered from the recording, the hand-over to the
//             headset (waiting for the GPU) and the desktop preview
//   wait      waiting for the headset's next frame (or the frame limiter)
//   present   the rest: extra eye pairs, pose update, the real Present
//   missed    headset frames that passed while waiting beyond the expected
//             one (0 = on time)
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace kkvr {

class FrameTimes {
 public:
  FrameTimes();
  ~FrameTimes();
  FrameTimes(const FrameTimes&) = delete;
  FrameTimes& operator=(const FrameTimes&) = delete;

  void PresentStart();   // first thing in Present
  void SubmitDone();     // after the headset hand-over and preview
  void WaitDone(int headset_frames_passed);  // after pacing (1 = on time)
  void PresentEnd(uint64_t frame);          // last thing in Present

 private:
  double Ms(LONGLONG from, LONGLONG to) const;

  bool enabled_ = false;
  FILE* csv_ = nullptr;
  LARGE_INTEGER freq_{};
  LONGLONG start_ = 0, prev_start_ = 0, prev_end_ = 0, submit_ = 0, wait_ = 0;
  int passed_ = 1;
  std::vector<double> window_;  // intervals of the current 300-frame window
  uint32_t missed_ = 0, missed_frames_ = 0;
  double game_sum_ = 0, submit_sum_ = 0, wait_sum_ = 0, present_sum_ = 0;
  double game_max_ = 0, submit_max_ = 0;
};

}  // namespace kkvr
