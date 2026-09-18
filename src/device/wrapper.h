// COM wrappers around IDirect3D9 / IDirect3DDevice9.
//
// Every method is declared from the generated method lists so that the
// pure-virtual COM interface is guaranteed fully implemented -- if a method is
// missing the class simply will not compile. Methods tagged CUSTOM in
// tools/gen_wrapper.py are hand-written; the rest are mechanical
// pass-throughs.
//
// The render path:
//   recording.cpp     the proxy: the game's state changes and draws update the
//                     state model (game_state.h) and are recorded
//                     (frame_record.h), never executed while the game renders
//   frame_render.cpp  at Present: every eye rendered from the recording by
//                     the EyeRenderer (eye_renderer.h), eye images, checks
//   shared_output.cpp eye images to the headset (shared textures)
//   preview.cpp       the left eye in the game window; eye poses per frame
//   wrapper.cpp       device lifetime, Present, Reset, reports, captures,
//                     hotkeys
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "render/eye_renderer.h"
#include "debug/frame_times.h"
#include "record/game_state.h"
#include "record/frame_record.h"
#include "record/vertex_arena.h"
#include "xr/xr.h"

namespace kkvr {

// Number of hotkeys polled in PollHotkeys (checked there at compile time).
constexpr int kHotkeyCount = 19;

// Per-frame counts of the game's calls (autotest wait conditions, glitch
// catcher summaries).
struct FrameStats {
  uint32_t draws = 0;            // game draw calls
  uint32_t draws_shader = 0;     // issued while a vertex shader was bound
  uint32_t render_target_switches = 0;
  uint32_t clears = 0;
};

// Caps Present to a fixed rate ([display] fps_limit). Needed because vsync
// does not hold the game in a window; see timing.h for why speed stays
// correct above 100 fps.
class FrameLimiter {
 public:
  FrameLimiter();
  ~FrameLimiter();
  FrameLimiter(const FrameLimiter&) = delete;
  FrameLimiter& operator=(const FrameLimiter&) = delete;
  void Wait(int fps);

 private:
  LARGE_INTEGER freq_{};
  LONGLONG next_ = 0;
  HANDLE timer_ = nullptr;
};

// Image file formats of D3DXSaveSurfaceToFileA (D3DXIMAGE_FILEFORMAT).
constexpr DWORD kImageBmp = 0, kImageTga = 2, kImagePng = 3;

// D3DXSaveSurfaceToFileA of the game's own d3dx9_26.dll (E_FAIL without it).
HRESULT SaveSurface(const std::string& path, DWORD format, IDirect3DSurface9* surface);

class D3D9Device final : public IDirect3DDevice9 {
 public:
  D3D9Device(IDirect3DDevice9* real, const D3DPRESENT_PARAMETERS& pp, HWND window);

#define D3D9_METHOD(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
#define D3D9_METHOD_CUSTOM(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
#include "device/generated/idirect3ddevice9_methods.inl"
#undef D3D9_METHOD
#undef D3D9_METHOD_CUSTOM

  void SetAdapterLuid(int64_t luid) { adapter_luid_ = luid; }

 private:
  // -- recording.cpp: the proxy ------------------------------------------------
  // The game's state as it set it (answers Get*, feeds the recording).
  GameState game_;
  // The recording of the current frame and the state it started from.
  FrameRecord record_;
  GameState start_;
  // [render] execute_live (checks only): also execute every recorded call on
  // the device right away, from the frame's start state, as a reference image
  // for the renderer (glitch_catch compares them).
  bool execute_live_ = false;
  void Live(const RecordedCall& call);
  RecordedCall& Record(Cmd cmd);
  RecordedCall& RecordState(Cmd cmd, int key);
  bool rt0_backbuffer_ = false;  // the game's render target 0 is backbuffer-sized
  std::unordered_set<IDirect3DBaseTexture9*> render_target_textures_;
  uint64_t filtered_calls_ = 0;  // sets of what was already set, not recorded

  // Screen-filling quads, decided when a draw is recorded (the renderer only
  // has the decision, not the vertices): see IsScreenQuad.
  bool IsScreenQuad(const void* vertices, UINT stride, UINT count, float* quad_w, bool* copy);
  void RecordDraw(RecordedCall& call, const void* vertices, UINT stride, UINT count);
  void CountDraw();  // statistics and the capture trace line
  // Vertex arena: game buffers bound on the device are their arena regions.
  std::unique_ptr<VertexArena> arena_;
  struct Binding {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
  };
  Binding recorded_streams_[kModelStreams];  // device bindings recorded this frame
  void BindArenaStreams();
  const uint8_t* StreamVertices(UINT first_vertex, UINT count);  // CPU copy, or null

  // [render] verify: the vertex bytes each draw will read ("verify:" lines).
  bool verify_ = false;
  struct VerifyStats {
    uint64_t draws = 0, bad_draws = 0, bad_bytes = 0, unchecked = 0;
  } verify_stats_;
  void VerifyDrawVertices(UINT first_vertex, UINT vertices);

  // Capture frames log every target switch, viewport, copy and clear
  // ("trace:" lines) with the number of draws in between.
  void Trace(const char* format, ...);
  void DescribeSurface(IDirect3DSurface9* surface, char* out, size_t size);
  uint32_t trace_draws_ = 0;

  // -- frame_render.cpp: rendering the eyes ------------------------------------
  std::unique_ptr<EyeRenderer> renderer_;
  IDirect3DTexture9* eye_out_[2] = {};
  EyeContext EyeContextFor(int eye) const;
  void BeginFrame();          // end of Present: the next game frame starts
  void FinishFrame();         // start of Present: render the eyes, checks
  void RenderEyes();          // both eyes into eye_out_
  void RenderEye(int eye);
  void CopyBackbufferToEye(int eye);
  IDirect3DSurface9* EyeSurface(int eye);  // AddRef'd, or null
  void ReleaseRendering();
  struct RenderStats {
    uint64_t calls = 0, bytes = 0;
    uint32_t frames = 0, eyes = 0;
    uint32_t extra_pairs = 0, skipped_pairs = 0;
    double render_ms = 0.0;
  } render_stats_;

  // -- glitch_catcher.cpp ([render] glitch_catch) -------------------------------
  void CatchGlitches();
  void CopyLiveForGlitchCheck();
  void ReleaseGlitchCatcher();
  bool Thumbnail(IDirect3DSurface9* source, std::vector<uint8_t>* out);
  IDirect3DSurface9* GlitchCopy(IDirect3DTexture9** texture, UINT width, UINT height);
  void SaveGlitch(const char* kind, const char* detail, IDirect3DSurface9* images[],
                  const char* names[], int count);
  IDirect3DTexture9* glitch_prev_[2] = {};
  IDirect3DTexture9* glitch_live_copy_ = nullptr;
  bool glitch_live_valid_ = false;
  IDirect3DSurface9* glitch_thumb_rt_ = nullptr;
  IDirect3DSurface9* glitch_thumb_sys_ = nullptr;
  std::vector<uint8_t> glitch_history_[2][2];
  double glitch_last_save_ = 0.0;
  uint32_t glitch_saved_ = 0;
  uint64_t glitch_count_ = 0;
  uint64_t glitch_frames_ = 0;
  double glitch_mean_sum_ = 0.0;
  std::string glitch_summaries_[3];
  uint32_t glitch_draws_[3] = {1, 1, 1};  // draws of frames A, B, C (temporal test)

  // -- preview.cpp ----------------------------------------------------------------
  void ComposePreview();
  void UpdateWindowEyes();     // once per frame, as late as possible
  // Where the picture goes in the backbuffer so it keeps its aspect after
  // Present stretches the backbuffer over the window. {x0, y0, x1, y1};
  // returns true if that is not the whole backbuffer (bars needed).
  bool DesktopOutputRect(float out[4]) const;

  // The eyes' views for this frame (positions in metres, window frame: +x
  // right, +y up, +z towards the viewer).
  bool window_eyes_from_headset_ = false;
  XrPresenter::EyeView window_views_[2];
  // The frustum each eye is rendered for (window bounds, eye_projection.h) and
  // the eye image size: the headset pixels the window covers x render_scale.
  float window_render_tangents_[2][4] = {{-1, 1, 1, -1}, {-1, 1, 1, -1}};
  int skip_from_ = 0;   // draws skipped while playing ('-'/'=' move, F12 sizes)
  int skip_count_ = 0;
  UINT eye_target_w_ = 0, eye_target_h_ = 0;
  void UpdateEyeTargetSize();  // preview.cpp, when the window or headset changes
  void ReleaseEyeTextures();   // frame_render.cpp
  // Frame captures write PNG while an autotest script runs (the tools read
  // PNG, and simulated eye images are small) and BMP otherwise: encoding a
  // PNG of a headset-sized eye image stalls the game for seconds.
  bool CapturePng() const { return !GetConfig().test_script.empty(); }
  const char* CaptureExtension() const { return CapturePng() ? ".png" : ".bmp"; }
  DWORD CaptureFormat() const { return CapturePng() ? kImagePng : kImageBmp; }
  // The window's height in metres (its aspect is the game's).
  float WindowHeight() const {
    return window_width_ * static_cast<float>(backbuffer_h_) /
           static_cast<float>(backbuffer_w_ ? backbuffer_w_ : 1);
  }
  int64_t window_pose_time_ = 0;  // XrTime the eyes are predicted for
  // Screen / window placement, live-tunable (F3 size, F4 distance).
  float window_width_ = 0.0f;    // metres ([vr] screen_width at start)
  float window_distance_ = 0.0f; // metres ([vr] screen_distance at start)
  // Game units per metre for the current window size. Resizing the window
  // (F3) scales the world with it: the game camera keeps its distance behind
  // the window in game units and everything keeps its place relative to the
  // window (user's request, 2026-09-17: a bigger 3D menu logo popped far out
  // of a bigger window). Moving the window (F4) stays physical.
  float WorldScale() const {
    const Config& c = GetConfig();
    return c.world_scale * c.world_scale_width / (window_width_ > 0.01f ? window_width_ : 0.01f);
  }

  // -- shared_output.cpp: eye images for the headset ------------------------------
  std::unique_ptr<XrPresenter> xr_;
  static constexpr int kHandoverSlots = 8;
  static_assert(kHandoverSlots <= 32, "slots are handed back as a 32-bit mask");
  struct SharedSlot {
    IDirect3DTexture9* eye[2] = {};
    HANDLE handle[2] = {};
    bool in_use = false;
  };
  SharedSlot shared_slots_[kHandoverSlots];
  IDirect3DSurface9* shared_sync_ = nullptr;
  UINT shared_w_ = 0, shared_h_ = 0;
  bool shared_output_ = false;  // the device is Direct3D9Ex (sharing possible)
  bool d3d9ex_ = false;
  void SubmitShared();
  // [test] output_grid: a grid of lines over `dest` (shared_output.cpp).
  void DrawTestGrid(IDirect3DSurface9* dest, UINT width, UINT height, D3DCOLOR colour);
  // The window's outline of eye_out_[eye] into `dest` (alpha 0 outside).
  void DrawWindowOutline(int eye, IDirect3DSurface9* dest, UINT width, UINT height);
  void ReleaseSharedOutput();
  int64_t adapter_luid_ = 0;
  bool shared_adapter_checked_ = false;
  // Hand-overs to the headset ("headset output:" every kReportInterval frames).
  uint32_t handover_count_ = 0;
  uint32_t handover_skipped_ = 0;   // no free slot
  double handover_block_ms_ = 0.0;  // time spent waiting for the GPU
  bool handover_failed_ = false;    // shared textures failed: output disabled
  uint64_t xr_tick_ = 0;  // last headset frame tick seen (XrPresenter::WaitForFrameTick)

  // -- wrapper.cpp -------------------------------------------------------------------
  void EndFrameAndReport();    // called from Present
  bool CaptureThisFrame() const;  // is this frame traced (frame capture)?
  void ReportCapture();        // end of the traced frame: matrices, wrapping
  void RunAutotestActions();   // test script: screenshots, hotkeys, quit
  void SaveScreenshot(const std::string& name);
  // Resolve and save the eye images (or the backbuffer): `prefix` + "_L"/"_R",
  // or `prefix` for the backbuffer, + CaptureExtension().
  void SaveBackbufferImages(const std::string& prefix, bool eyes);
  // One render target of the frame, as the renderer finished it ([test]
  // dump_passes). Multisampled targets are resolved first.
  void SavePassImage(const std::string& prefix, IDirect3DSurface9* surface);
  // Frame capture (F2, autotest "capture"): see StartFrameCapture.
  void StartFrameCapture();
  void FinishFrameCaptureEyes();     // in Present, before the desktop view
  void FinishFrameCaptureDesktop();  // in Present, after it
  enum class FrameCapture { kIdle, kArmed, kTracing };
  FrameCapture frame_capture_ = FrameCapture::kIdle;
  std::string frame_capture_dir_;
  std::string frame_capture_pairs_dir_;  // save this frame's extra pairs
  void PollHotkeys();
  void HandleHotkey(int vk, bool shift);  // also used by test scripts
  void OnScreenSettingsChanged(const char* key_name);
  bool key_down_[kHotkeyCount] = {};
  bool guide_down_ = false;  // Xbox button state (PollHotkeys)

  IDirect3DDevice9* dev_ = nullptr;
  ULONG refs_ = 1;
  HWND window_ = nullptr;      // device window, restyled after Reset
  FrameLimiter limiter_;
  FrameTimes frame_times_;     // per-frame timing log (frame_times.h)

  uint64_t frame_ = 0;
  FrameStats stats_;
  FrameStats last_;
  bool capture_next_ = false;

  UINT backbuffer_w_ = 0;
  UINT backbuffer_h_ = 0;
  bool aspect_patched_ = false;  // PatchScreenAspect succeeded (aspect.h)

  // Frame rate / game speed report (every kReportInterval frames).
  LARGE_INTEGER report_time_{};
  double report_game_clock_ = 0.0;
};

class D3D9 final : public IDirect3D9 {
 public:
  explicit D3D9(IDirect3D9* real);

#define D3D9_METHOD(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
#define D3D9_METHOD_CUSTOM(ret, name, params, args) ret STDMETHODCALLTYPE name params override;
#include "device/generated/idirect3d9_methods.inl"
#undef D3D9_METHOD
#undef D3D9_METHOD_CUSTOM

 private:
  IDirect3D9* d3d_ = nullptr;
  ULONG refs_ = 1;
};

}  // namespace kkvr
