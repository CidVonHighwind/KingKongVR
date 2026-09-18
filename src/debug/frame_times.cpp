#include "debug/frame_times.h"

#include "common/config.h"
#include "common/log.h"

#include <algorithm>
#include <string>

namespace kkvr {

FrameTimes::FrameTimes() {
  QueryPerformanceFrequency(&freq_);
  enabled_ = GetConfig().frame_times;
  if (!enabled_) return;
  const std::string path = ModuleDir() + "kkvr_frametimes.csv";
  csv_ = std::fopen(path.c_str(), "w");
  if (csv_) {
    std::fprintf(csv_, "frame,time_s,interval_ms,game_ms,submit_ms,wait_ms,present_ms,missed\n");
    Logf("frametime: per-frame times in %s", path.c_str());
  }
  window_.reserve(300);
}

FrameTimes::~FrameTimes() {
  if (csv_) std::fclose(csv_);
}

double FrameTimes::Ms(LONGLONG from, LONGLONG to) const {
  return from && to ? 1000.0 * static_cast<double>(to - from) / freq_.QuadPart : 0.0;
}

void FrameTimes::PresentStart() {
  if (!enabled_) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  prev_start_ = start_;
  start_ = now.QuadPart;
  submit_ = wait_ = 0;
  passed_ = 1;
}

void FrameTimes::SubmitDone() {
  if (!enabled_) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  submit_ = now.QuadPart;
}

void FrameTimes::WaitDone(int headset_frames_passed) {
  if (!enabled_) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  wait_ = now.QuadPart;
  passed_ = headset_frames_passed;
}

void FrameTimes::PresentEnd(uint64_t frame) {
  if (!enabled_) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double interval = Ms(prev_start_, start_);
  const double game = Ms(prev_end_, start_);
  const double submit = Ms(start_, submit_);
  const double wait = Ms(submit_, wait_);
  const double present = Ms(wait_, now.QuadPart);
  const int missed = passed_ > 1 ? passed_ - 1 : 0;
  prev_end_ = now.QuadPart;
  if (prev_start_ == 0) return;

  if (csv_) {
    std::fprintf(csv_, "%llu,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", frame,
                 static_cast<double>(start_) / freq_.QuadPart, interval, game, submit, wait,
                 present, missed);
  }
  window_.push_back(interval);
  missed_ += static_cast<uint32_t>(missed);
  missed_frames_ += missed > 0;
  game_sum_ += game;
  submit_sum_ += submit;
  wait_sum_ += wait;
  present_sum_ += present;
  game_max_ = (std::max)(game_max_, game);
  submit_max_ = (std::max)(submit_max_, submit);

  if (window_.size() >= 300) {
    const size_t n = window_.size();
    std::vector<double> sorted = window_;
    std::sort(sorted.begin(), sorted.end());
    size_t over_10 = 0, over_17 = 0;
    for (double v : sorted) {
      over_10 += v > 10.0;
      over_17 += v > 16.7;
    }
    Logf("frametime: %zu frames: median %.2f ms, p99 %.2f, worst %.2f | %zu over 10 ms, %zu "
         "over 16.7 ms | headset frames missed %u (in %u frames) | avg game %.2f (max %.2f), "
         "submit %.2f (max %.2f), wait %.2f, present %.2f",
         n, sorted[n / 2], sorted[n * 99 / 100], sorted[n - 1], over_10, over_17, missed_,
         missed_frames_, game_sum_ / n, game_max_, submit_sum_ / n, submit_max_, wait_sum_ / n,
         present_sum_ / n);
    if (csv_) std::fflush(csv_);
    window_.clear();
    missed_ = missed_frames_ = 0;
    game_sum_ = submit_sum_ = wait_sum_ = present_sum_ = 0;
    game_max_ = submit_max_ = 0;
  }
}

}  // namespace kkvr
