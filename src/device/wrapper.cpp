#include "device/wrapper.h"

#include "game/aspect.h"
#include "debug/autotest.h"
#include "device/lock_hooks.h"
#include "common/config.h"
#include "game/joystick.h"
#include "game/timing.h"
#include "common/log.h"

#include <cmath>
#include <cstring>

namespace kkvr {
namespace {

// Frames between the periodic summary lines ("timing:", "headset output:").
// Detailed dumps ("trace:") are written only for a frame capture (F2).
constexpr uint64_t kReportInterval = 300;

// A 4x4 matrix across four lines with a label.
void LogMatrix(const char* label, const D3DMATRIX& m) {
  Logf("%s", label);
  for (int r = 0; r < 4; ++r) {
    Logf("    % 12.4f % 12.4f % 12.4f % 12.4f", m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]);
  }
}

// Swap exclusive fullscreen for a window. Returns true if pp was changed.
bool ForceWindowed(D3DPRESENT_PARAMETERS* pp) {
  if (!GetConfig().windowed || !pp || pp->Windowed) return false;
  pp->Windowed = TRUE;
  pp->FullScreen_RefreshRateInHz = 0;  // must be 0 when windowed
  return true;
}

// The game creates a fullscreen-sized popup; turn it into a normal window,
// centred in the work area. Present stretches the backbuffer over the whole
// client area, so the client gets the backbuffer's aspect, as large as fits
// in [display] window_width x window_height (a 16:9 client showed the 4:3
// picture squashed to 16:9).
void StyleGameWindow(HWND hwnd, UINT backbuffer_w, UINT backbuffer_h) {
  const Config& c = GetConfig();
  if (!c.windowed || !hwnd) return;

  int client_w = c.window_width;
  int client_h = c.window_height;
  if (backbuffer_w > 0 && backbuffer_h > 0) {
    const int fit_h = static_cast<int>(static_cast<int64_t>(client_w) * backbuffer_h / backbuffer_w);
    if (fit_h <= client_h) {
      client_h = fit_h;
    } else {
      client_w = static_cast<int>(static_cast<int64_t>(client_h) * backbuffer_w / backbuffer_h);
    }
  }

  const LONG style = c.borderless ? (WS_POPUP | WS_VISIBLE)
                                  : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  const LONG ex_style = GetWindowLongA(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
  SetWindowLongA(hwnd, GWL_STYLE, style);
  SetWindowLongA(hwnd, GWL_EXSTYLE, ex_style);

  RECT r{0, 0, client_w, client_h};
  AdjustWindowRectEx(&r, style, FALSE, ex_style);
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;

  RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + ((work.right - work.left) - w) / 2;
  int y = work.top + ((work.bottom - work.top) - h) / 2;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;

  SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  Logf("window %p restyled: client %dx%d at %d,%d%s", static_cast<void*>(hwnd),
       client_w, client_h, x, y,
       c.borderless ? " (borderless)" : "");
}

}  // namespace

// ---------------------------------------------------------------------------
// FrameLimiter
// ---------------------------------------------------------------------------

FrameLimiter::FrameLimiter() {
  QueryPerformanceFrequency(&freq_);
  // High-resolution timers exist from Windows 10 1803; fall back to a plain
  // one (about 1 ms granularity, with the spin below covering the rest).
  timer_ = CreateWaitableTimerExA(nullptr, nullptr,
                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
  if (!timer_) {
    timer_ = CreateWaitableTimerExA(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
}

FrameLimiter::~FrameLimiter() {
  if (timer_) CloseHandle(timer_);
}

void FrameLimiter::Wait(int fps) {
  if (fps <= 0) return;
  const LONGLONG period = freq_.QuadPart / fps;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  // Fixed deadlines keep the average rate exact; after a hitch (loading),
  // resynchronise instead of racing to catch up.
  if (next_ == 0 || now.QuadPart - next_ > period) next_ = now.QuadPart;
  const LONGLONG deadline = next_;
  next_ += period;

  // Sleep until ~2 ms before the deadline, then spin for precision.
  const LONGLONG margin = freq_.QuadPart / 500;
  const LONGLONG remaining = deadline - now.QuadPart;
  if (timer_ && remaining > margin) {
    LARGE_INTEGER due;
    due.QuadPart = -((remaining - margin) * 10000000LL / freq_.QuadPart);
    if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer_, INFINITE);
    }
  }
  for (;;) {
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= deadline) break;
    YieldProcessor();
  }
}

// ---------------------------------------------------------------------------
// D3D9Device
// ---------------------------------------------------------------------------

D3D9Device::D3D9Device(IDirect3DDevice9* real, const D3DPRESENT_PARAMETERS& pp, HWND window)
    : dev_(real), window_(window) {
  const Config& config = GetConfig();
  backbuffer_w_ = pp.BackBufferWidth;
  backbuffer_h_ = pp.BackBufferHeight;
  window_width_ = config.screen_width;
  window_distance_ = config.screen_distance;
  aspect_patched_ =
      config.aspect_fix != kAspectFixOff && PatchScreenAspect(backbuffer_w_, backbuffer_h_);
  // The engine never steps less than 0.01 s per frame, so above 100 fps it
  // runs fast. Lower the floor to half a frame at the configured cap.
  if (config.fix_frame_step && (config.fps_limit == 0 || config.fps_limit > 100)) {
    const float cap = config.fps_limit > 0 ? static_cast<float>(config.fps_limit) : 1000.0f;
    PatchMinimumFrameStep(0.5f / cap);
  }
  Logf("device wrapper created (real=%p)", real);
  {
    IDirect3DDevice9Ex* ex = nullptr;
    d3d9ex_ = SUCCEEDED(real->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                             reinterpret_cast<void**>(&ex))) && ex;
    if (ex) ex->Release();
    Logf("device is %s", d3d9ex_ ? "Direct3D9Ex (managed pool mapped to default)" : "Direct3D9");
    shared_output_ = d3d9ex_;
    if (shared_output_) Logf("headset output: shared textures (zero-copy)");
  }
  InstallVertexBufferLockHooks(real);
  verify_ = config.verify;
  arena_ = std::make_unique<VertexArena>(real, config.verify);
  if (verify_) Logf("render: VERIFY mode (slow): the vertex bytes of every 8th frame are checked");
  SetVertexBufferRedirect(arena_.get());
  // The game starts from the device's state as created.
  game_.ReadFromDevice(dev_);
  if (IDirect3DSurface9* rt0 = game_.targets[0].get()) {
    D3DSURFACE_DESC d{};
    rt0_backbuffer_ = SUCCEEDED(rt0->GetDesc(&d)) && d.Width == backbuffer_w_ &&
                      d.Height == backbuffer_h_;
  }
  renderer_ = std::make_unique<EyeRenderer>(dev_, arena_.get(), &render_target_textures_);
  execute_live_ = config.execute_live;
  Logf("render: the game's frames are recorded and every eye is rendered from the recording%s",
       execute_live_ ? " (execute_live: also executed while recorded, as a reference)" : "");
  AutotestLoad();
  AutotestKeepWindowActive(window_);
  if (config.vr_enabled) {
    xr_ = std::make_unique<XrPresenter>();
    xr_->SetScreenDistance(window_distance_);
  }
  UpdateWindowEyes();
  BeginFrame();
}

// -- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9Device::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
    AddRef();
    *ppvObj = this;
    return S_OK;
  }
  // Anything else (IDirect3DDevice9Ex, etc.) we do not wrap; hand back the
  // real object rather than lying about support.
  return dev_->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE D3D9Device::AddRef() {
  return ++refs_;
}

ULONG STDMETHODCALLTYPE D3D9Device::Release() {
  const ULONG remaining = --refs_;
  if (remaining == 0) {
    Logf("device wrapper released after %llu frames", frame_);
    ReleaseRendering();
    game_.Clear();
    xr_.reset();
    dev_->Release();
    delete this;
  }
  return remaining;
}

// -- frame structure --------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9Device::Present(CONST RECT* pSourceRect,
                                              CONST RECT* pDestRect,
                                              HWND hDestWindowOverride,
                                              CONST RGNDATA* pDirtyRegion) {
  frame_times_.PresentStart();
  EndFrameAndReport();
  FinishFrame();  // both eyes rendered from the recording
  HookDriverUpdate();
  PollHotkeys();
  RunAutotestActions();
  FinishFrameCaptureEyes();
  const Config& config = GetConfig();
  // With a headset every eye pair is paced by the headset's frame tick and
  // handed over as soon as the GPU has finished it.
  const bool headset_active = xr_ && xr_->WantsFrames();
  if (headset_active && shared_output_) SubmitShared();
  ComposePreview();  // the left eye in the game window
  FinishFrameCaptureDesktop();
  frame_times_.SubmitDone();
  // Returns how many headset frames passed while waiting (1 = on time).
  // An fps_limit below the headset rate waits that many ticks per frame
  // (60 on a 120 Hz headset: every second tick).
  const auto pace = [&]() -> int {
    if (headset_active) {
      int need = 1;
      const double period = xr_->TickPeriodMs();
      if (config.fps_limit > 0 && period > 0.0) {
        need = static_cast<int>(std::lround(1000.0 / config.fps_limit / period));
        if (need < 1) need = 1;
      }
      int total = 0;
      while (total < need) {
        const int passed = xr_->WaitForFrameTick(&xr_tick_, 100);
        if (passed <= 0) break;
        total += passed;
      }
      if (total >= need) return total - need + 1;
    }
    limiter_.Wait(config.fps_limit);
    return 1;
  };
  frame_times_.WaitDone(pace());
  // More eye pairs from the same game frame, each from a newly sampled head
  // pose (future games below the headset rate). The game itself continues
  // only after the last pair.
  int pairs = config.replays_per_frame;
  if (pairs == 0) {
    // auto: as many pairs as headset frames fit in one game frame.
    double period = headset_active ? xr_->TickPeriodMs() : 0.0;
    if (period <= 0.0) period = config.fps_limit > 0 ? 1000.0 / config.fps_limit : 1000.0 / 60;
    pairs = static_cast<int>(std::lround(1000.0 / config.game_fps / period));
    if (pairs < 1) pairs = 1;
    if (pairs > 8) pairs = 8;
  }
  for (int k = 1; k < pairs; ++k) {
    UpdateWindowEyes();
    RenderEyes();
    ++render_stats_.extra_pairs;
    if (!frame_capture_pairs_dir_.empty()) {
      const float* l = window_views_[0].position;
      const float* r = window_views_[1].position;
      Logf("  pair %d eyes (metres): L %.4f %.4f %.4f  R %.4f %.4f %.4f", k, l[0], l[1], l[2],
           r[0], r[1], r[2]);
      SaveBackbufferImages(frame_capture_pairs_dir_ + "pair" + std::to_string(k), true);
    }
    if (headset_active && shared_output_) SubmitShared();
    if (k + 1 == pairs) ComposePreview();
    const int passed = pace();
    if (passed > 1) {
      // Headset frames were missed: give the time back to the game instead
      // of rendering more pairs of this frame late.
      render_stats_.skipped_pairs += pairs - 1 - k;
      break;
    }
  }
  frame_capture_pairs_dir_.clear();
  // The pose for the next frame, sampled as late as possible (this also picks
  // up the headset's resolution once the session runs).
  UpdateWindowEyes();
  const HRESULT hr =
      dev_->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  BeginFrame();
  frame_times_.PresentEnd(frame_);
  return hr;
}

void D3D9Device::RunAutotestActions() {
  AutotestLoad();
  AutotestFrame frame;
  frame.draws = last_.draws;
  frame.render_target_switches = last_.render_target_switches;
  for (const AutotestAction& action : AutotestTick(frame)) {
    switch (action.kind) {
      case AutotestAction::kScreenshot:
        SaveScreenshot(action.name);
        break;
      case AutotestAction::kHotkey:
        Logf("autotest: hotkey %s%s", action.shift ? "Shift+" : "",
             ("F" + std::to_string(action.vk - VK_F1 + 1)).c_str());
        HandleHotkey(action.vk, action.shift);
        break;
      case AutotestAction::kCapture:
        Logf("autotest: capture");
        StartFrameCapture();
        break;
      case AutotestAction::kQuit:
        Logf("autotest: quit");
        LogShutdown();
        TerminateProcess(GetCurrentProcess(), 0);
        break;
    }
  }
}

HRESULT SaveSurface(const std::string& path, DWORD format, IDirect3DSurface9* surface) {
  using SaveFn = HRESULT(WINAPI*)(const char*, DWORD, IDirect3DSurface9*, const PALETTEENTRY*,
                                  const RECT*);
  static const SaveFn save = [] {
    HMODULE d3dx = GetModuleHandleA("d3dx9_26.dll");
    if (!d3dx) d3dx = LoadLibraryA("d3dx9_26.dll");
    const SaveFn fn = d3dx ? reinterpret_cast<SaveFn>(
                                 GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA"))
                           : nullptr;
    if (!fn) Logf("screenshot: d3dx9_26.dll!D3DXSaveSurfaceToFileA not available");
    return fn;
  }();
  return save && surface ? save(path.c_str(), format, surface, nullptr, nullptr) : E_FAIL;
}

// Both eyes in <game>\screenshots (see SaveBackbufferImages).
void D3D9Device::SaveScreenshot(const std::string& name) {
  const std::string dir = ModuleDir() + "screenshots\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  SaveBackbufferImages(dir + name, true);
}

// Image files in CaptureFormat() (SaveSurface). The eye textures and the
// backbuffer may be multisampled, so they are resolved into a plain target
// first.
void D3D9Device::SaveBackbufferImages(const std::string& prefix, bool eyes) {
  IDirect3DSurface9* backbuffer = nullptr;
  if (FAILED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
  // Saved at the size the eyes are rendered at, so a capture shows every
  // rendered pixel (a downscale hides thin artefacts). The projection check
  // compares images of one size, which this keeps.
  IDirect3DSurface9* resolve = nullptr;
  if (FAILED(dev_->CreateRenderTarget(renderer_->output_width(), renderer_->output_height(),
                                      D3DFMT_X8R8G8B8,
                                      D3DMULTISAMPLE_NONE, 0, FALSE, &resolve,
                                      nullptr))) {
    backbuffer->Release();
    return;
  }
  IDirect3DSurface9* sources[2] = {nullptr, nullptr};
  if (eyes) {
    sources[0] = EyeSurface(0);
    sources[1] = EyeSurface(1);
  } else {
    // What the renderer drew into the backbuffer (its scaled copy at render
    // scale), resolved at its own size first: a multisampled source cannot be
    // stretched.
    IDirect3DSurface9* rendered = renderer_->DeviceSurface(backbuffer);
    if (SUCCEEDED(dev_->CreateRenderTarget(renderer_->output_width(), renderer_->output_height(),
                                           D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                           &sources[0], nullptr)) &&
        FAILED(dev_->StretchRect(rendered, nullptr, sources[0], nullptr, D3DTEXF_NONE))) {
      sources[0]->Release();
      sources[0] = nullptr;
    }
  }
  for (int eye = 0; eye < 2; ++eye) {
    if (!sources[eye]) continue;
    if (FAILED(dev_->StretchRect(sources[eye], nullptr, resolve, nullptr, D3DTEXF_LINEAR))) {
      continue;
    }
    const std::string suffix = !eyes ? "" : eye ? "_R" : "_L";
    const std::string path = prefix + suffix + CaptureExtension();
    const HRESULT hr = SaveSurface(path, CaptureFormat(), resolve);
    Logf("screenshot: %s %s", path.c_str(), SUCCEEDED(hr) ? "saved" : "FAILED");
  }
  for (IDirect3DSurface9* source : sources) {
    if (source) source->Release();
  }
  resolve->Release();
  backbuffer->Release();
}

// One pass of the frame ([test] dump_passes): the target as the renderer left
// it. A multisampled target cannot be saved directly, so it is resolved into a
// plain one of the same size first.
void D3D9Device::SavePassImage(const std::string& prefix, IDirect3DSurface9* surface) {
  if (!surface) return;
  D3DSURFACE_DESC d{};
  if (FAILED(surface->GetDesc(&d))) return;
  IDirect3DSurface9* plain = nullptr;
  if (FAILED(dev_->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                      FALSE, &plain, nullptr))) {
    return;
  }
  if (SUCCEEDED(dev_->StretchRect(surface, nullptr, plain, nullptr, D3DTEXF_NONE))) {
    // Always uncompressed: a frame has ~25 passes, and encoding them as PNG
    // would stall the game for a minute.
    SaveSurface(prefix + ".tga", kImageTga, plain);
  }
  plain->Release();
}

// Frame capture: everything about one frame in game\captures\<date_time>\
//   frame.txt   the frame's trace (every render-target switch, viewport,
//               StretchRect and Clear, one line per draw), the fixed-function
//               matrices, texture wrapping, stereo/window state
//   eye_L, eye_R  the two eyes as rendered (.bmp, .png in test runs)
//   headset<n>_L, _R  the images the headset gets
//   desktop     the backbuffer after the desktop preview was composed
// Timeline: armed in Present N (hotkey), frame N+1 is traced (capture_next_
// makes CaptureThisFrame true), and in Present N+1 the report is written,
// then the images are saved before and after the preview.
void D3D9Device::StartFrameCapture() {
  if (frame_capture_ != FrameCapture::kIdle) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[64];
  snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth,
           t.wDay, t.wHour, t.wMinute, t.wSecond);
  const std::string root = ModuleDir() + "captures\\";
  CreateDirectoryA(root.c_str(), nullptr);
  frame_capture_dir_ = root + name + "\\";
  CreateDirectoryA(frame_capture_dir_.c_str(), nullptr);
  LogTeeBegin(frame_capture_dir_ + "frame.txt");
  Logf("frame capture into %s (frame %llu is traced)", frame_capture_dir_.c_str(),
       frame_ + 1);
  Logf("  window %.3f m wide, %.3f m away, eyes from %s", window_width_, window_distance_,
       window_eyes_from_headset_ ? "headset" : "default view");
  Logf("  backbuffer %ux%u, aspect_fix %d", backbuffer_w_, backbuffer_h_,
       GetConfig().aspect_fix);
  capture_next_ = true;
  frame_capture_ = FrameCapture::kArmed;
}

void D3D9Device::FinishFrameCaptureEyes() {
  if (frame_capture_ != FrameCapture::kTracing) return;
  const float* l = window_views_[0].position;
  const float* r = window_views_[1].position;
  Logf("  window eyes of this frame (metres, window frame): L %.4f %.4f %.4f  "
       "R %.4f %.4f %.4f", l[0], l[1], l[2], r[0], r[1], r[2]);
  SaveBackbufferImages(frame_capture_dir_ + "eye", true);
}

void D3D9Device::FinishFrameCaptureDesktop() {
  if (frame_capture_ == FrameCapture::kArmed) {
    // End of Present N: the next frame is the traced one.
    frame_capture_ = FrameCapture::kTracing;
    return;
  }
  if (frame_capture_ != FrameCapture::kTracing) return;
  SaveBackbufferImages(frame_capture_dir_ + "desktop", false);
  // Replay mode: the extra eye pairs of this game frame are saved as well.
  frame_capture_pairs_dir_ = frame_capture_dir_;
  Logf("frame capture done: %s", frame_capture_dir_.c_str());
  LogTeeEnd();
  frame_capture_ = FrameCapture::kIdle;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Reset(D3DPRESENT_PARAMETERS* pp) {
  if (pp) {
    Logf("Reset: %ux%u windowed=%d fmt=%d present_interval=%u",
         pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed,
         static_cast<int>(pp->BackBufferFormat), pp->PresentationInterval);
  }
  if (ForceWindowed(pp)) Logf("  forcing windowed");
  // Everything we created in D3DPOOL_DEFAULT must go before Reset, and the
  // state model's references to the game's resources.
  ReleaseRendering();
  game_.Clear();
  const HRESULT hr = dev_->Reset(pp);
  if (FAILED(hr)) Logf("Reset FAILED hr=0x%08X", hr);
  if (SUCCEEDED(hr) && pp) {
    StyleGameWindow(window_, pp->BackBufferWidth, pp->BackBufferHeight);
    backbuffer_w_ = pp->BackBufferWidth;
    backbuffer_h_ = pp->BackBufferHeight;
    aspect_patched_ = GetConfig().aspect_fix != kAspectFixOff &&
                      PatchScreenAspect(backbuffer_w_, backbuffer_h_);
  }
  if (SUCCEEDED(hr)) {
    // Reset restores the default device state: the game continues from there.
    game_.ReadFromDevice(dev_);
    rt0_backbuffer_ = true;
    for (Binding& b : recorded_streams_) b = Binding();
    BeginFrame();
  }
  return hr;
}

// Frame capture: the fixed-function matrices and texture wrapping at the end
// of the traced frame.
void D3D9Device::ReportCapture() {
  Logf("---- capture @ frame %llu ----", frame_);
  LogMatrix("  VIEW (fixed-function):", game_.view);
  LogMatrix("  PROJECTION (fixed-function):", game_.projection);
  LogMatrix("  WORLD (last set):", game_.world);
  // Texture coordinate wrapping leaves a seam through the middle of a
  // full-screen quad, and the address modes decide what happens at the edges.
  Logf("  WRAP0..3 %lu %lu %lu %lu | sampler 0 address u/v %lu/%lu, sampler 1 %lu/%lu",
       game_.rs[D3DRS_WRAP0], game_.rs[D3DRS_WRAP1], game_.rs[D3DRS_WRAP2], game_.rs[D3DRS_WRAP3],
       game_.ss[0][D3DSAMP_ADDRESSU], game_.ss[0][D3DSAMP_ADDRESSV],
       game_.ss[1][D3DSAMP_ADDRESSU], game_.ss[1][D3DSAMP_ADDRESSV]);
  Logf("---- end capture ----");
}

bool D3D9Device::CaptureThisFrame() const { return capture_next_; }

void D3D9Device::EndFrameAndReport() {
  last_ = stats_;
  stats_ = FrameStats();
  ++frame_;

  if (frame_ <= 3 || capture_next_) {
    Logf("frame %llu: draws=%u (shader=%u) rt_switches=%u clears=%u", frame_, last_.draws,
         last_.draws_shader, last_.render_target_switches, last_.clears);
  }
  if (capture_next_) {
    ReportCapture();
    capture_next_ = false;
  }
  if (frame_ % kReportInterval == 0) {
    // Frame rate and game speed: the game clock should advance as fast as
    // real time (1.00) unless the game itself slows or pauses time.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float game = GameClockSeconds();
    if (report_time_.QuadPart != 0) {
      const double real = static_cast<double>(now.QuadPart - report_time_.QuadPart) /
                          static_cast<double>(freq.QuadPart);
      const double advanced = static_cast<double>(game) - report_game_clock_;
      Logf("timing: %.1f fps, game clock %.2f s in %.2f s real (speed %.2f)",
           kReportInterval / real, advanced, real, real > 0 ? advanced / real : 0.0);
    }
    report_time_ = now;
    report_game_clock_ = game;
  }
  if (frame_ % kReportInterval == 0 && handover_count_ > 0) {
    Logf("headset output: %u frames handed over, %.2f ms per frame waiting for the GPU, "
         "%u skipped (no free slot)", handover_count_, handover_block_ms_ / kReportInterval,
         handover_skipped_);
    handover_block_ms_ = 0.0;
    handover_count_ = 0;
    handover_skipped_ = 0;
  }
}

// -- hotkeys ----------------------------------------------------------------

void D3D9Device::PollHotkeys() {
  // Xbox (Guide) button: recentre, like F8. Checked before the foreground
  // test: with a headset on, the game window is often not in the foreground.
  const bool guide = GetConfig().guide_recenter && JoystickGuideButton();
  if (guide && !guide_down_) {
    Logf("[Xbox button] recentre screen");
    if (xr_) xr_->RequestRecenter();
  }
  guide_down_ = guide;
  if (!ProcessOwnsForeground()) return;
  // Turn the simulated head in small steps ([test] sim_head_step), to see how
  // an artefact moves with the view: F6 yaw, F7 pitch, F9 roll (Shift = the
  // other way), F10 = straight ahead again. The numpad does the same for
  // keyboards that have one.
  static constexpr int kKeys[] = {VK_F8,      VK_F3,      VK_F4,      VK_F2,
                                  VK_F6,      VK_F7,      VK_F9,      VK_F10,
                                  VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD8, VK_NUMPAD2,
                                  VK_NUMPAD7, VK_NUMPAD9, VK_NUMPAD5, VK_F11,
                                  VK_F12,     VK_OEM_MINUS, VK_OEM_PLUS};
  static_assert(sizeof(kKeys) / sizeof(kKeys[0]) == kHotkeyCount,
                "kHotkeyCount must match the key list");
  const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  for (int i = 0; i < kHotkeyCount; ++i) {
    const bool down = (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
    const bool pressed = down && !key_down_[i];
    key_down_[i] = down;
    if (pressed) HandleHotkey(kKeys[i], shift);
  }
}

void D3D9Device::HandleHotkey(int vk, bool shift) {
  switch (vk) {
    case VK_F2:
      Logf("[key F2] frame capture");
      StartFrameCapture();
      break;
    case VK_F8:
      Logf("[key F8] recentre screen");
      if (xr_) xr_->RequestRecenter();
      break;
    case VK_F3:
      window_width_ = shift ? window_width_ / 1.1f : window_width_ * 1.1f;
      OnScreenSettingsChanged(shift ? "Shift+F3" : "F3");
      break;
    case VK_F4:
      window_distance_ = shift ? window_distance_ * 1.1f : window_distance_ / 1.1f;
      if (window_distance_ < 0.3f) window_distance_ = 0.3f;
      OnScreenSettingsChanged(shift ? "Shift+F4" : "F4");
      break;
    case VK_F11:
    case VK_F12:
    case VK_OEM_MINUS:
    case VK_OEM_PLUS: {
      // Bisect an artefact by hiding a range of draws while the rest of the
      // frame, including the post effects that put the picture on screen,
      // still runs: '-' and '=' move the hidden range through the frame, F11
      // and Shift+F11 by one draw, F12 changes how many draws are hidden.
      // Developer keys only ([test] debug_keys): '-' and '=' are ordinary
      // keys a player may press.
      if (!GetConfig().debug_keys) break;
      const int drawn = renderer_ ? renderer_->draws_done() : 0;
      if (drawn <= 0) break;
      const char* name = "=";
      if (vk == VK_F12) {
        static constexpr int kSizes[] = {0, 10, 50, 150, 1, 3};
        int i = 0;
        while (i + 1 < static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0])) &&
               kSizes[i] != skip_count_) {
          ++i;
        }
        skip_count_ = kSizes[(i + 1) % (sizeof(kSizes) / sizeof(kSizes[0]))];
        name = "F12";
      } else {
        int step = 0;
        switch (vk) {
          case VK_F11: step = shift ? 1 : -1; name = "F11"; break;
          case VK_OEM_MINUS: step = -25; name = "-"; break;
          default: step = 25; break;  // '='
        }
        if (skip_count_ == 0) skip_count_ = 50;  // start hiding something
        skip_from_ += step;
        if (skip_from_ < 0) skip_from_ = 0;
        if (skip_from_ > drawn) skip_from_ = drawn;
      }
      if (skip_count_ == 0) {
        Logf("[key %s] all %d draws shown", name, drawn);
      } else {
        Logf("[key %s] hiding draws %d..%d of %d", name, skip_from_ + 1,
             skip_from_ + skip_count_, drawn);
      }
      break;
    }
    case VK_F6:
    case VK_F7:
    case VK_F9:
    case VK_F10:
    case VK_NUMPAD4:
    case VK_NUMPAD6:
    case VK_NUMPAD8:
    case VK_NUMPAD2:
    case VK_NUMPAD7:
    case VK_NUMPAD9:
    case VK_NUMPAD5: {
      if (!xr_) break;
      const float step = GetConfig().sim_head_step;
      const float back = shift ? -step : step;  // Shift turns the other way
      switch (vk) {
        case VK_F6: xr_->AddManualHead(back, 0.0f, 0.0f); break;         // yaw
        case VK_F7: xr_->AddManualHead(0.0f, back, 0.0f); break;         // pitch
        case VK_F9: xr_->AddManualHead(0.0f, 0.0f, back); break;         // roll
        case VK_NUMPAD4: xr_->AddManualHead(step, 0.0f, 0.0f); break;    // yaw left
        case VK_NUMPAD6: xr_->AddManualHead(-step, 0.0f, 0.0f); break;   // yaw right
        case VK_NUMPAD8: xr_->AddManualHead(0.0f, step, 0.0f); break;    // pitch up
        case VK_NUMPAD2: xr_->AddManualHead(0.0f, -step, 0.0f); break;   // pitch down
        case VK_NUMPAD7: xr_->AddManualHead(0.0f, 0.0f, step); break;    // roll left
        case VK_NUMPAD9: xr_->AddManualHead(0.0f, 0.0f, -step); break;   // roll right
        default: xr_->ResetManualHead(); break;                          // F10, numpad 5
      }
      float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
      xr_->ManualHead(&yaw, &pitch, &roll);
      Logf("[head] simulated head: yaw %.2f, pitch %.2f, roll %.2f deg (step %.2f)", yaw, pitch,
           roll, step);
      break;
    }
  }
}

void D3D9Device::OnScreenSettingsChanged(const char* key_name) {
  Logf("[key %s] screen %.2f m wide, %.2f m away", key_name, window_width_, window_distance_);
  if (xr_) xr_->SetScreenDistance(window_distance_);
  SaveScreenSettings(window_width_, window_distance_);
}

// -- everything else is a straight pass-through -----------------------------

#define D3D9_METHOD(ret, name, params, args)                              \
  ret STDMETHODCALLTYPE D3D9Device::name params { return dev_->name args; }
#define D3D9_METHOD_CUSTOM(ret, name, params, args)  // hand-written
#include "device/generated/idirect3ddevice9_methods.inl"
#undef D3D9_METHOD
#undef D3D9_METHOD_CUSTOM

// ---------------------------------------------------------------------------
// D3D9
// ---------------------------------------------------------------------------

D3D9::D3D9(IDirect3D9* real) : d3d_(real) {}

HRESULT STDMETHODCALLTYPE D3D9::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
    AddRef();
    *ppvObj = this;
    return S_OK;
  }
  return d3d_->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE D3D9::AddRef() {
  return ++refs_;
}

ULONG STDMETHODCALLTYPE D3D9::Release() {
  const ULONG remaining = --refs_;
  if (remaining == 0) {
    d3d_->Release();
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE D3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
                                             HWND hFocusWindow, DWORD BehaviorFlags,
                                             D3DPRESENT_PARAMETERS* pPresentationParameters,
                                             IDirect3DDevice9** ppReturnedDeviceInterface) {
  if (pPresentationParameters) {
    Logf("CreateDevice: %ux%u windowed=%d fmt=%d refresh=%u interval=%u "
         "flags=0x%08X msaa=%d",
         pPresentationParameters->BackBufferWidth,
         pPresentationParameters->BackBufferHeight,
         pPresentationParameters->Windowed,
         static_cast<int>(pPresentationParameters->BackBufferFormat),
         pPresentationParameters->FullScreen_RefreshRateInHz,
         pPresentationParameters->PresentationInterval, BehaviorFlags,
         static_cast<int>(pPresentationParameters->MultiSampleType));
  }

  // Keep the game's request so a failed windowed attempt can fall back.
  D3DPRESENT_PARAMETERS requested{};
  if (pPresentationParameters) requested = *pPresentationParameters;
  const bool forced = ForceWindowed(pPresentationParameters);

  HRESULT hr = d3d_->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                  BehaviorFlags, pPresentationParameters,
                                  ppReturnedDeviceInterface);
  if (FAILED(hr) && forced) {
    Logf("windowed CreateDevice failed hr=0x%08X, retrying as requested", hr);
    *pPresentationParameters = requested;
    hr = d3d_->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                            pPresentationParameters, ppReturnedDeviceInterface);
  }
  if (FAILED(hr) || !ppReturnedDeviceInterface || !*ppReturnedDeviceInterface) {
    Logf("CreateDevice FAILED hr=0x%08X", hr);
    return hr;
  }

  // CreateDevice fills in a 0x0 backbuffer size for windowed mode, so read
  // the parameters after the call.
  D3DPRESENT_PARAMETERS pp{};
  if (pPresentationParameters) pp = *pPresentationParameters;
  const HWND window = pp.hDeviceWindow ? pp.hDeviceWindow : hFocusWindow;
  if (pp.Windowed) StyleGameWindow(window, pp.BackBufferWidth, pp.BackBufferHeight);
  auto* device = new D3D9Device(*ppReturnedDeviceInterface, pp, window);
  IDirect3D9Ex* ex = nullptr;
  if (SUCCEEDED(d3d_->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&ex))) &&
      ex) {
    LUID luid{};
    if (SUCCEEDED(ex->GetAdapterLUID(Adapter, &luid))) {
      device->SetAdapterLuid(static_cast<int64_t>(
          (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart));
    }
    ex->Release();
  }
  *ppReturnedDeviceInterface = device;
  return hr;
}

#define D3D9_METHOD(ret, name, params, args) \
  ret STDMETHODCALLTYPE D3D9::name params { return d3d_->name args; }
#define D3D9_METHOD_CUSTOM(ret, name, params, args)  // hand-written above
#include "device/generated/idirect3d9_methods.inl"
#undef D3D9_METHOD
#undef D3D9_METHOD_CUSTOM

}  // namespace kkvr
