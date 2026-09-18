// Glitch catcher ([render] glitch_catch=1): finds single-frame rendering
// glitches automatically, since a person cannot press F2 in that frame.
//
// Two tests, both on 240x180 grey thumbnails of the eye images:
//   1. Renderer vs live reference ([render] execute_live=1): the left eye
//      rendered from the recording at Present is compared with the same calls
//      executed on the device while they were recorded. A difference is an
//      error in the recording or the start state (frame_render.cpp).
//   2. Temporal outlier, per eye: frame B (the previous one) is a glitch if it
//      differs clearly from both its neighbours A and C while A and C are
//      close to each other. Catches glitches from any source, live included.
// Each hit saves full-resolution images into game\captures\glitch_<time>\ and
// a log line ("glitch:"), at most once per 2 s.

#include "device/wrapper.h"

#include "common/config.h"
#include "common/log.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace kkvr {
namespace {

constexpr UINT kThumbW = 240;
constexpr UINT kThumbH = 180;

double MeanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  uint64_t sum = 0;
  for (size_t i = 0; i < a.size(); ++i) sum += static_cast<uint64_t>(std::abs(int(a[i]) - int(b[i])));
  return static_cast<double>(sum) / a.size();
}

// Bounding box (in 0..1 image coordinates) of pixels differing by > 24.
std::string ChangedBox(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  UINT x0 = kThumbW, y0 = kThumbH, x1 = 0, y1 = 0;
  for (UINT y = 0; y < kThumbH; ++y) {
    for (UINT x = 0; x < kThumbW; ++x) {
      const size_t i = y * kThumbW + x;
      if (std::abs(int(a[i]) - int(b[i])) <= 24) continue;
      x0 = (std::min)(x0, x); y0 = (std::min)(y0, y);
      x1 = (std::max)(x1, x); y1 = (std::max)(y1, y);
    }
  }
  char box[64];
  snprintf(box, sizeof(box), "box x %.2f-%.2f y %.2f-%.2f", double(x0) / kThumbW,
           double(x1 + 1) / kThumbW, double(y0) / kThumbH, double(y1 + 1) / kThumbH);
  return box;
}

// Share of thumbnail pixels differing by more than 24 grey levels.
double ChangedShare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  size_t changed = 0;
  for (size_t i = 0; i < a.size(); ++i) changed += std::abs(int(a[i]) - int(b[i])) > 24;
  return static_cast<double>(changed) / a.size();
}

}  // namespace

// Grey thumbnail of `source` (any render target surface) into `out`.
bool D3D9Device::Thumbnail(IDirect3DSurface9* source, std::vector<uint8_t>* out) {
  if (!source) return false;
  if (!glitch_thumb_rt_ &&
      FAILED(dev_->CreateRenderTarget(kThumbW, kThumbH, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                      FALSE, &glitch_thumb_rt_, nullptr))) {
    return false;
  }
  if (!glitch_thumb_sys_ &&
      FAILED(dev_->CreateOffscreenPlainSurface(kThumbW, kThumbH, D3DFMT_X8R8G8B8,
                                               D3DPOOL_SYSTEMMEM, &glitch_thumb_sys_, nullptr))) {
    return false;
  }
  if (FAILED(dev_->StretchRect(source, nullptr, glitch_thumb_rt_, nullptr, D3DTEXF_LINEAR)) ||
      FAILED(dev_->GetRenderTargetData(glitch_thumb_rt_, glitch_thumb_sys_))) {
    return false;
  }
  D3DLOCKED_RECT locked{};
  if (FAILED(glitch_thumb_sys_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return false;
  out->resize(kThumbW * kThumbH);
  for (UINT y = 0; y < kThumbH; ++y) {
    const auto* row = static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
    for (UINT x = 0; x < kThumbW; ++x) {
      const uint8_t* p = row + x * 4;
      (*out)[y * kThumbW + x] = static_cast<uint8_t>((p[0] + 2 * p[1] + p[2]) / 4);
    }
  }
  glitch_thumb_sys_->UnlockRect();
  return true;
}

IDirect3DSurface9* D3D9Device::GlitchCopy(IDirect3DTexture9** texture, UINT width, UINT height) {
  if (*texture) {
    // The eye image size follows the window (eye_projection.h): a copy of the
    // old size would compare two different framings.
    D3DSURFACE_DESC d{};
    if (FAILED((*texture)->GetLevelDesc(0, &d)) || d.Width != width || d.Height != height) {
      (*texture)->Release();
      *texture = nullptr;
    }
  }
  if (!*texture &&
      FAILED(dev_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                 D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, texture, nullptr))) {
    return nullptr;
  }
  IDirect3DSurface9* surface = nullptr;
  (*texture)->GetSurfaceLevel(0, &surface);
  return surface;  // AddRef'd
}

void D3D9Device::SaveGlitch(const char* kind, const char* detail,
                            IDirect3DSurface9* images[], const char* names[], int count) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  const double seconds = static_cast<double>(now.QuadPart) / freq.QuadPart;
  ++glitch_count_;
  if (seconds - glitch_last_save_ < 2.0 || glitch_saved_ >= 30) {
    Logf("glitch: %s at frame %llu (%s) - not saved (rate limit)", kind, frame_, detail);
    return;
  }
  glitch_last_save_ = seconds;
  ++glitch_saved_;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[96];
  snprintf(name, sizeof(name), "glitch_%02u-%02u-%02u_%03u_%s", t.wHour, t.wMinute, t.wSecond,
           t.wMilliseconds, kind);
  const std::string root = ModuleDir() + "captures\\";
  CreateDirectoryA(root.c_str(), nullptr);
  const std::string dir = root + name + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  for (int i = 0; i < count; ++i) {
    const HRESULT hr = SaveSurface(dir + names[i] + ".png", kImagePng, images[i]);
    if (FAILED(hr)) Logf("glitch: saving %s failed (0x%08lx)", names[i], hr);
  }
  Logf("glitch: %s at frame %llu (%s) -> %s", kind, frame_, detail, dir.c_str());
}

// execute_live: the backbuffer holds the live reference render of the frame
// (the recording executed while the game recorded it). Keep a copy before the
// eye renders overwrite it.
void D3D9Device::CopyLiveForGlitchCheck() {
  glitch_live_valid_ = false;
  IDirect3DSurface9* backbuffer = nullptr;
  if (FAILED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
  if (IDirect3DSurface9* copy = GlitchCopy(&glitch_live_copy_, renderer_->output_width(),
                                           renderer_->output_height())) {
    glitch_live_valid_ = SUCCEEDED(dev_->StretchRect(renderer_->DeviceSurface(backbuffer), nullptr,
                                                     copy, nullptr, D3DTEXF_NONE));
    copy->Release();
  }
  backbuffer->Release();
}

// Called in Present after both eye images exist.
void D3D9Device::CatchGlitches() {
  if (++glitch_frames_ % 600 == 0) {
    Logf("glitch: %llu frames checked, %llu glitches found; renderer vs live mean difference "
         "over the last 600 frames %.3f (0 = identical)", glitch_frames_, glitch_count_,
         glitch_mean_sum_ / 600.0);
    glitch_mean_sum_ = 0.0;
  }

  // 1. Renderer vs live reference (left eye, execute_live only): the frame
  // rendered from its recording and start state must equal the same calls
  // executed while they were recorded. Differences mean the recording or the
  // start state misses something.
  IDirect3DSurface9* rendered_left = EyeSurface(0);
  IDirect3DSurface9* live_copy = nullptr;
  if (glitch_live_valid_ && glitch_live_copy_) glitch_live_copy_->GetSurfaceLevel(0, &live_copy);
  std::vector<uint8_t> live_thumb, rendered_thumb;
  if (rendered_left && live_copy && Thumbnail(live_copy, &live_thumb) &&
      Thumbnail(rendered_left, &rendered_thumb)) {
    const double share = ChangedShare(live_thumb, rendered_thumb);
    glitch_mean_sum_ += MeanDiff(live_thumb, rendered_thumb);
    if (share > 0.002) {
      char detail[160];
      snprintf(detail, sizeof(detail), "%.2f%% of the image differs, mean %.2f, %s",
               100.0 * share, MeanDiff(live_thumb, rendered_thumb),
               ChangedBox(live_thumb, rendered_thumb).c_str());
      IDirect3DSurface9* images[] = {live_copy, rendered_left};
      const char* names[] = {"live_left", "replayed_left"};
      SaveGlitch("replay", detail, images, names, 2);
    }
  }
  if (live_copy) live_copy->Release();
  if (rendered_left) rendered_left->Release();

  // Frame summaries for the temporal test's log (A, B, C as below).
  char summary[160];
  snprintf(summary, sizeof(summary),
           "frame %llu: %u draws (%u shader), %u target switches, %u clears, %zu recorded calls",
           frame_, last_.draws, last_.draws_shader, last_.render_target_switches, last_.clears,
           record_.calls().size());
  glitch_draws_[0] = glitch_draws_[1];
  glitch_draws_[1] = glitch_draws_[2];
  glitch_draws_[2] = last_.draws;
  // A frame next to one with (almost) no draws (loading screens, fades from black) is
  // a scene transition, not a glitch: the level's first frame is often half
  // built (seen 2026-09-17, frame after a black loading frame; also after a
  // loading frame with a single draw).
  const bool transition = glitch_draws_[0] <= 2 || glitch_draws_[1] <= 2 || glitch_draws_[2] <= 2;
  glitch_summaries_[0].swap(glitch_summaries_[1]);
  glitch_summaries_[1].swap(glitch_summaries_[2]);
  glitch_summaries_[2] = summary;

  // 2. Temporal outlier per eye: B (previous frame) vs A (before) and C (now).
  for (int eye = 0; eye < 2; ++eye) {
    IDirect3DSurface9* current = EyeSurface(eye);
    std::vector<uint8_t> c;
    if (!current || !Thumbnail(current, &c)) {
      if (current) current->Release();
      continue;
    }
    std::vector<uint8_t>& a = glitch_history_[eye][0];
    std::vector<uint8_t>& b = glitch_history_[eye][1];
    if (!a.empty() && !b.empty() && !transition) {
      const double ab = MeanDiff(a, b), bc = MeanDiff(b, c), ac = MeanDiff(a, c);
      if (ab > 2.0 && bc > 2.0 && ac < 0.35 * (std::min)(ab, bc)) {
        char detail[128];
        snprintf(detail, sizeof(detail), "%s eye: previous frame differs from both neighbours "
                 "(%.2f, %.2f) while they match (%.2f)", eye ? "right" : "left", ab, bc, ac);
        IDirect3DSurface9* previous = GlitchCopy(&glitch_prev_[eye], renderer_->output_width(), renderer_->output_height());
        IDirect3DSurface9* images[] = {previous, current};
        const char* names[] = {"glitched_frame", "next_frame"};
        SaveGlitch(eye ? "temporal_right" : "temporal_left", detail, images, names, 2);
        Logf("glitch:   before  %s", glitch_summaries_[0].c_str());
        Logf("glitch:   GLITCH  %s", glitch_summaries_[1].c_str());
        Logf("glitch:   after   %s", glitch_summaries_[2].c_str());
        if (previous) previous->Release();
      }
    }
    a.swap(b);
    b.swap(c);
    // Keep the full image of this frame: it is "previous" next time.
    if (IDirect3DSurface9* keep = GlitchCopy(&glitch_prev_[eye], renderer_->output_width(), renderer_->output_height())) {
      dev_->StretchRect(current, nullptr, keep, nullptr, D3DTEXF_NONE);
      keep->Release();
    }
    current->Release();
  }
}

void D3D9Device::ReleaseGlitchCatcher() {
  for (IDirect3DTexture9*& t : glitch_prev_) {
    if (t) t->Release();
    t = nullptr;
  }
  if (glitch_live_copy_) glitch_live_copy_->Release();
  glitch_live_copy_ = nullptr;
  if (glitch_thumb_rt_) glitch_thumb_rt_->Release();
  glitch_thumb_rt_ = nullptr;
  if (glitch_thumb_sys_) glitch_thumb_sys_->Release();
  glitch_thumb_sys_ = nullptr;
  for (auto& h : glitch_history_) {
    h[0].clear();
    h[1].clear();
  }
}

}  // namespace kkvr
