// OpenXR output: shows the game's eye images as a projection layer.
//
// A dedicated thread owns everything OpenXR and D3D11. It runs the frame loop
// at the headset's rate, submits the layer, and between frames copies the
// newest game frame if there is one; otherwise the last images are shown
// again. It also publishes the eyes' views relative to the window, which the
// game renders each eye from.
//
// Frames are handed over as shared D3D9Ex textures from a small ring of
// "slots": the XR thread opens and copies them on the GPU and hands each slot
// back once it is no longer needed (TakeReleasedSlots). InvalidateFrames makes
// the XR thread let go of all handles before the game frees the textures.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace kkvr {

class XrPresenter {
 public:
  XrPresenter();
  ~XrPresenter();  // stops and joins the thread; never call from DllMain
  XrPresenter(const XrPresenter&) = delete;
  XrPresenter& operator=(const XrPresenter&) = delete;

  // True once a session is running and frames are wanted. Lets the game
  // thread skip the headset output when nobody is looking.
  bool WantsFrames() const { return wants_frames_.load(); }

  // One eye as the headset sees it, predicted for a display time.
  struct EyeView {
    float position[3] = {};                   // window frame, metres
    float orientation[4] = {0, 0, 0, 1};      // window frame, quaternion x y z w
    float tangents[4] = {-1, 1, 1, -1};       // tan of fov: left, right, up, down
    float local_position[3] = {};             // runtime LOCAL space (for the layer)
    float local_orientation[4] = {0, 0, 0, 1};
    float fov[4] = {};                        // angles: left, right, up, down (radians)
  };

  struct Frame {
    int width = 0;
    int height = 0;
    float quad_width = 0.0f;   // window width in metres (resolution log)
    int slot = -1;             // shared slot, returned by TakeReleasedSlots
    // Shared D3D9Ex textures (DXGI B8G8R8A8). The XR thread opens them with
    // D3D11 and copies them on the GPU. The game thread has waited for the
    // GPU to finish them before submitting.
    void* shared[2] = {};
    // The views the images were rendered for (submitted with them as a
    // projection layer) and the time they were predicted for (XrTime, ns).
    EyeView views[2];
    int64_t pose_time = 0;
    // One pixel of the left eye (centre, BGRA as read by the game thread) to
    // check that the content arrived.
    uint8_t probe[4] = {};
  };

  // Game thread: offer the newest frame. A frame that was offered but not
  // yet taken is dropped and its slot released.
  void SubmitFrame(const Frame& frame);

  // Game thread: slots (bit mask) whose pixels the XR thread no longer uses.
  uint32_t TakeReleasedSlots();

  // Game thread: wait for any copy in progress, then drop every frame so
  // all slots are released. Call before freeing the slot surfaces.
  void InvalidateFrames();

  // Place the screen in front of the current head position on the next frame.
  void RequestRecenter() { recenter_.store(true); }

  // Distance from the recentre point to the screen, in metres. Changing it
  // slides the screen along its axis.
  void SetScreenDistance(float metres) { screen_distance_.store(metres); }
  float ScreenDistance() const { return screen_distance_.load(); }

  // Latest eye views in the window's frame (positions in metres: origin at
  // the window centre, +x right, +y up, +z towards the viewer) and the time
  // they are predicted for (XrTime, ns). False until the headset is tracking.
  bool GetWindowViews(EyeView views[2], int64_t* pose_time) const;

  // Simulated headset: the head is turned by these angles (degrees) on top of
  // its own motion. Hotkeys add to them (wrapper.cpp), the XR thread reads
  // them every frame.
  void AddManualHead(float yaw, float pitch, float roll) {
    manual_yaw_.store(manual_yaw_.load() + yaw);
    manual_pitch_.store(manual_pitch_.load() + pitch);
    manual_roll_.store(manual_roll_.load() + roll);
  }
  void ResetManualHead() {
    manual_yaw_.store(0.0f);
    manual_pitch_.store(0.0f);
    manual_roll_.store(0.0f);
  }
  void ManualHead(float* yaw, float* pitch, float* roll) const {
    *yaw = manual_yaw_.load();
    *pitch = manual_pitch_.load();
    *roll = manual_roll_.load();
  }

  // Eye image size the runtime recommends (0 until known).
  int RecommendedWidth() const { return recommended_width_.load(); }
  int RecommendedHeight() const { return recommended_height_.load(); }
  void SetRecommendedSize(int w, int h) {
    recommended_width_.store(w);
    recommended_height_.store(h);
  }

  // XR thread only: if a new frame was submitted, make it current (releasing
  // the previous one) and call `upload` with it while holding the upload
  // lock. Returns true if a frame was uploaded.
  template <typename UploadFn>
  bool UploadNewFrame(UploadFn upload) {
    std::lock_guard<std::mutex> upload_lock(upload_mutex_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!has_pending_) return false;
      if (current_.slot >= 0) released_ |= 1u << current_.slot;
      current_ = pending_;
      has_pending_ = false;
    }
    upload(current_);
    return true;
  }
  // XR thread only: publish eye views (see GetWindowViews); `views` may be
  // null when `valid` is false. Also the headset's frame tick: once per
  // headset frame, right after xrWaitFrame.
  void PublishWindowViews(const EyeView* views, bool valid, int64_t pose_time);

  // Game thread: wait until the headset starts a frame after `*last_tick`
  // (the number of the tick seen last; updated). Returns how many ticks
  // passed (more than 1: headset frames were missed), 0 on timeout.
  // Replay mode renders one eye pair per tick with the pose published then.
  int WaitForFrameTick(uint64_t* last_tick, int timeout_ms);
  // Average time between ticks (ms), 0 until measured.
  double TickPeriodMs() const { return tick_period_ms_.load(); }

  // LUID of the adapter the XR runtime renders on (0 = not known yet). Shared
  // textures only work on the same adapter as the game's device.
  int64_t AdapterLuid() const { return adapter_luid_.load(); }
  void SetAdapterLuid(int64_t luid) { adapter_luid_.store(luid); }

  // Bumped by InvalidateFrames: shared texture handles seen before are stale.
  uint32_t SharedGeneration() const { return shared_generation_.load(); }

 private:
  void ThreadMain();

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> wants_frames_{false};
  std::atomic<bool> recenter_{true};
  std::atomic<float> screen_distance_;  // [vr] screen_distance at start
  std::atomic<uint32_t> shared_generation_{0};
  std::atomic<double> tick_period_ms_{0.0};
  std::atomic<int64_t> adapter_luid_{0};
  int64_t last_tick_qpc_ = 0;  // guarded by eyes_mutex_

  std::mutex upload_mutex_;  // held while the XR thread copies current_
  std::mutex mutex_;         // pending_, current_ slot bookkeeping, released_
  Frame pending_;
  bool has_pending_ = false;
  Frame current_;
  uint32_t released_ = 0;

  mutable std::mutex eyes_mutex_;
  EyeView window_views_[2];
  int64_t window_views_time_ = 0;
  bool window_views_valid_ = false;
  std::atomic<float> manual_yaw_{0.0f};
  std::atomic<float> manual_pitch_{0.0f};
  std::atomic<float> manual_roll_{0.0f};
  std::atomic<int> recommended_width_{0};
  std::atomic<int> recommended_height_{0};
  uint64_t tick_ = 0;                 // guarded by eyes_mutex_
  std::condition_variable tick_cv_;
};

}  // namespace kkvr
