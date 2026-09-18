// Rendering the recorded frame for every eye.
//
// Frame timeline:
//   BeginFrame   (end of Present) the state model is copied as the new
//                frame's start state; the recording starts empty; the vertex
//                arena may wrap; with execute_live the renderer starts the
//                live reference from the start state.
//   game frame   the proxy records (recording.cpp).
//   FinishFrame  (start of Present) both eyes are rendered from the recording
//                into eye_out_; with execute_live, glitch_catch compares the
//                left eye with the live reference; frame captures also get
//                the projection check's reference image.
// Extra eye pairs for the same frame (future games below the headset rate)
// render the same recording again with newer eye poses.

#include "device/wrapper.h"

#include "common/config.h"
#include "render/eye_projection.h"
#include "common/log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace kkvr {
namespace {

double NowMs() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

}  // namespace

EyeContext D3D9Device::EyeContextFor(int eye) const {
  EyeContext c;
  c.index = eye;
  c.share_targets = GetConfig().share_eye_targets;
  c.view = window_views_[eye];
  for (int i = 0; i < 4; ++i) c.render_tangents[i] = window_render_tangents_[eye][i];
  c.mode = GetConfig().flat ? EyeMode::kFlat : EyeMode::kWindow;
  c.window_width = window_width_;
  c.window_height = WindowHeight();
  c.world_scale = WorldScale();
  c.backbuffer_width = backbuffer_w_;
  c.backbuffer_height = backbuffer_h_;
  c.aspect_fix = GetConfig().aspect_fix;
  c.aspect_patched = aspect_patched_;
  c.skip_from = skip_from_;
  c.skip_count = skip_count_;
  c.hide_letterbox = GetConfig().hide_letterbox;
  c.cinematic_fov = GetConfig().cinematic_fov;
  c.cinematic_scale = GetConfig().cinematic_scale;
  return c;
}

void D3D9Device::BeginFrame() {
  record_.Clear();
  start_ = game_;
  // Every recording binds the arena regions it uses itself.
  for (Binding& b : recorded_streams_) b = Binding();
  arena_->BeginFrame();
  if (execute_live_) renderer_->Begin(start_, EyeContextFor(0));
}

void D3D9Device::RenderEye(int eye) {
  const double t0 = NowMs();
  // Frame capture with [test] dump_passes: every render target of the left eye
  // as the frame finishes it, numbered in drawing order (pass000, pass001 ...).
  const bool dump = eye == 0 && GetConfig().dump_passes &&
                    frame_capture_ == FrameCapture::kTracing && !frame_capture_dir_.empty();
  if (dump) {
    const std::string dir = frame_capture_dir_;
    renderer_->SetPassDump([this, dir](int pass, int draws, IDirect3DSurface9* target) {
      char name[64];
      snprintf(name, sizeof(name), "pass%03d_%ddraws", pass, draws);
      SavePassImage(dir + name, target);
      Logf("  pass %d: %d draws -> %s", pass, draws, name);
    });
  }
  renderer_->Render(record_, start_, EyeContextFor(eye));
  if (dump) renderer_->SetPassDump({});
  CopyBackbufferToEye(eye);
  // Part of the game's picture: moves and rotates with the window.
  if (GetConfig().output_grid && eye_out_[eye]) {
    if (IDirect3DSurface9* surface = EyeSurface(eye)) {
      DrawTestGrid(surface, renderer_->output_width(), renderer_->output_height(),
                   D3DCOLOR_ARGB(255, 40, 255, 40));
      surface->Release();
    }
  }
  render_stats_.render_ms += NowMs() - t0;
  ++render_stats_.eyes;
}

void D3D9Device::RenderEyes() {
  RenderEye(0);
  RenderEye(1);
}

void D3D9Device::FinishFrame() {
  if (execute_live_) {
    renderer_->End();
    if (GetConfig().glitch_catch) CopyLiveForGlitchCheck();
  }
  RenderEyes();
  if (GetConfig().glitch_catch) CatchGlitches();

  // The projection check's reference render costs a second render and a PNG
  // the size of an eye image, which is slow at headset resolution: only while
  // an autotest script runs (tools/autotest/check_projection.py needs it).
  if (frame_capture_ == FrameCapture::kTracing && !GetConfig().test_script.empty()) {
    // Frame capture: the left eye once more as an image of just the window
    // rectangle (off-axis, the same 1:1 model), plus the window outline in
    // the eye image, so tools/autotest/check_projection.py can warp one onto
    // the other and compare: they must agree inside the window.
    EyeContext reference = EyeContextFor(0);
    reference.mode = EyeMode::kWindowReference;
    renderer_->Render(record_, start_, reference);
    SaveBackbufferImages(frame_capture_dir_ + "window_reference_L", false);
    float uv[16][2];
    const int n = WindowOutline(window_views_[0], window_width_, WindowHeight(),
                                window_render_tangents_[0], uv);
    std::string line;
    for (int i = 0; i < n; ++i) {
      char pt[48];
      snprintf(pt, sizeof(pt), " %.6f,%.6f", uv[i][0], uv[i][1]);
      line += pt;
    }
    Logf("  projection outline L (uv, corners from top-left clockwise):%s", line.c_str());
  }

  render_stats_.calls += record_.calls().size();
  render_stats_.bytes += record_.arena_bytes();
  ++render_stats_.frames;
  if (render_stats_.frames >= 300) {
    if (const uint64_t bars = renderer_->TakeLetterboxDraws()) {
      Logf("render: %llu cinematic letterbox bars dropped ([render] hide_letterbox)", bars);
    }
    if (const uint64_t unscaled = renderer_->TakeUnscaledRhwDraws()) {
      Logf("render scale: %llu indexed pre-transformed draws were not scaled", unscaled);
    }
    Logf("render: %.0f calls and %.0f KB recorded per frame; %.2f ms per eye render; %u extra "
         "pairs, %u skipped (late); %.0f redundant calls filtered per frame; %llu window layer "
         "draws",
         static_cast<double>(render_stats_.calls) / render_stats_.frames,
         static_cast<double>(render_stats_.bytes) / render_stats_.frames / 1024.0,
         render_stats_.eyes ? render_stats_.render_ms / render_stats_.eyes : 0.0,
         render_stats_.extra_pairs, render_stats_.skipped_pairs,
         static_cast<double>(filtered_calls_) / render_stats_.frames,
         renderer_->TakeWindowLayerDraws());
    filtered_calls_ = 0;
    if (verify_) {
      Logf("verify: %llu draws checked, %llu with wrong vertex data (%llu bytes), %llu not "
           "checkable", verify_stats_.draws,
           verify_stats_.bad_draws, verify_stats_.bad_bytes, verify_stats_.unchecked);
      verify_stats_ = VerifyStats();
    }
    const VertexArena::Stats a = arena_->TakeStats();
    Logf("vertex arena: %llu locks, %.1f MB written, capacity %u MB, peak %.1f MB, %llu wraps, "
         "%llu grows, %llu regions restored from shadow", a.locks, a.bytes / 1048576.0,
         a.capacity >> 20, a.peak / 1048576.0, a.wraps, a.grows, a.restores);
    render_stats_ = RenderStats();
  }
}

// Resolve the rendered backbuffer (at render scale: its scaled copy) into
// eye_out_[eye] (plain render target texture of the same size).
void D3D9Device::CopyBackbufferToEye(int eye) {
  const UINT w = renderer_->output_width(), h = renderer_->output_height();
  if (!eye_out_[eye] &&
      FAILED(dev_->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                 D3DPOOL_DEFAULT, &eye_out_[eye], nullptr))) {
    Logf("render: eye texture %ux%u could not be created", w, h);
    return;
  }
  IDirect3DSurface9* backbuffer = nullptr;
  IDirect3DSurface9* dest = nullptr;
  if (SUCCEEDED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
      SUCCEEDED(eye_out_[eye]->GetSurfaceLevel(0, &dest))) {
    dev_->StretchRect(renderer_->DeviceSurface(backbuffer), nullptr, dest, nullptr, D3DTEXF_NONE);
  }
  if (dest) dest->Release();
  if (backbuffer) backbuffer->Release();
}

// The image of one eye as rendered this frame (eye texture), AddRef'd or null.
IDirect3DSurface9* D3D9Device::EyeSurface(int eye) {
  IDirect3DSurface9* surface = nullptr;
  if (eye_out_[eye]) eye_out_[eye]->GetSurfaceLevel(0, &surface);
  return surface;
}

void D3D9Device::ReleaseEyeTextures() {
  for (IDirect3DTexture9*& t : eye_out_) {
    if (t) t->Release();
    t = nullptr;
  }
}

void D3D9Device::ReleaseRendering() {
  ReleaseGlitchCatcher();
  if (renderer_) renderer_->Release();
  record_.Clear();
  start_.Clear();
  arena_->Release();
  ReleaseEyeTextures();
}

}  // namespace kkvr
