// Desktop view and the per-frame eye poses.
//
// Desktop: the game window shows the left eye image, letterboxed to keep its
// aspect, drawn into the game's backbuffer just before Present. The device
// state is not restored: the eye renderer applies its complete state before
// every eye.
//
// Eye poses: every draw of a frame is rendered from the same eye views,
// snapshotted once per frame (UpdateWindowEyes). With a headset (or the
// simulated headset, [vr] simulate_headset) they come from the XR thread.
// Without one, a fixed default view is used: both eyes straight in front of
// the window at [vr] screen_distance, so the desktop build renders through the
// same per-eye path as the headset.
//
// (Until 2026-09-17 this file also held side-by-side, anaglyph and wiggle
// previews and a simulated window view.)

#include "device/wrapper.h"

#include "common/config.h"
#include "render/eye_projection.h"
#include "common/log.h"

#include <cmath>

namespace kkvr {
namespace {

struct ViewVertex {
  float x, y, z, rhw;
  float u, v;
};
constexpr DWORD kViewFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

// Default eye views without a headset: 64 mm apart, a Quest-like field of
// view (the simulated headset uses the same angles).
constexpr float kDefaultIpd = 0.064f;
constexpr float kDefaultFov[4] = {-0.82f, 0.82f, 0.85f, -0.85f};  // radians: l, r, u, d

}  // namespace

// Snapshot the eye views for the next frame: from the headset when it tracks,
// otherwise the default view. Called as late as possible before the frame.
void D3D9Device::UpdateWindowEyes() {
  window_eyes_from_headset_ = xr_ && xr_->GetWindowViews(window_views_, &window_pose_time_);
  if (!window_eyes_from_headset_) {
    window_pose_time_ = 0;
    const float y = -GetConfig().screen_height_offset;
    for (int eye = 0; eye < 2; ++eye) {
      XrPresenter::EyeView& v = window_views_[eye];
      v.position[0] = (eye ? 0.5f : -0.5f) * kDefaultIpd;
      v.position[1] = y;
      v.position[2] = window_distance_;
      for (int k = 0; k < 3; ++k) v.local_position[k] = v.position[k];
      v.orientation[0] = v.orientation[1] = v.orientation[2] = 0.0f;
      v.orientation[3] = 1.0f;
      for (int k = 0; k < 4; ++k) {
        v.fov[k] = kDefaultFov[k];
        v.tangents[k] = std::tan(kDefaultFov[k]);
      }
    }
  }
  // Render each eye for the window's frustum only.
  const float height = WindowHeight();
  for (int eye = 0; eye < 2; ++eye) {
    WindowRenderTangents(window_views_[eye], window_width_, height,
                         window_render_tangents_[eye]);
  }
  UpdateEyeTargetSize();
}

// The eye image size: the headset pixels the window covers seen head-on from
// the window distance, times [render] render_scale (1 = one rendered pixel per
// headset pixel inside the window). It does not follow the head, so the cost
// per frame is constant and the targets are not recreated while playing; it
// changes only with the window size or distance (F3/F4) and when the headset
// reports its resolution.
void D3D9Device::UpdateEyeTargetSize() {
  int recommended_w = 0, recommended_h = 0;
  if (xr_) {
    recommended_w = xr_->RecommendedWidth();
    recommended_h = xr_->RecommendedHeight();
  }
  UINT w = 0, h = 0;
  if (recommended_w > 0 && recommended_h > 0) {
    int px = 0, py = 0;
    WindowPixelSize(window_views_[0], window_width_, WindowHeight(), window_distance_,
                    recommended_w, recommended_h, &px, &py);
    const float scale = GetConfig().render_scale;
    w = static_cast<UINT>((std::max)(64.0f, px * scale));
    h = static_cast<UINT>((std::max)(64.0f, py * scale));
  } else {
    // No headset yet: the game's own resolution.
    w = static_cast<UINT>(backbuffer_w_ * GetConfig().render_scale);
    h = static_cast<UINT>(backbuffer_h_ * GetConfig().render_scale);
  }
  w = (std::min)(w, 4096u) & ~7u;
  h = (std::min)(h, 4096u) & ~7u;
  if (w == eye_target_w_ && h == eye_target_h_) return;
  Logf("render: eye images %ux%u (game %ux%u, render_scale %.2f%s)", w, h, backbuffer_w_,
       backbuffer_h_, GetConfig().render_scale,
       recommended_w > 0 ? ", headset pixels the window covers" : ", no headset yet");
  eye_target_w_ = w;
  eye_target_h_ = h;
  if (renderer_) renderer_->SetTargetSize(w, h);
  ReleaseEyeTextures();
}

// Windowed, Present stretches the backbuffer over the client area whatever
// its shape (a destination rectangle is ignored with the multisampled DISCARD
// swap chain the game uses). So when the window has another aspect than the
// backbuffer, the picture is drawn into a narrower or flatter rectangle of the
// backbuffer, with black bars, which the stretch then brings back to the
// right shape. Example: 1920x1440 in a maximised 2560x1360 client -> the
// picture spans 1920*(1360*4/3)/2560 = 1360 px of the backbuffer's width.
bool D3D9Device::DesktopOutputRect(float out[4]) const {
  const float w = static_cast<float>(backbuffer_w_);
  const float h = static_cast<float>(backbuffer_h_);
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = w;
  out[3] = h;
  RECT client{};
  if (!GetConfig().windowed || !window_ || w <= 0.0f || h <= 0.0f ||
      !GetClientRect(window_, &client) || client.right <= 0 || client.bottom <= 0) {
    return false;
  }
  const float client_aspect = static_cast<float>(client.right) / client.bottom;
  const float scale = (w / h) / client_aspect;  // picture width / backbuffer width
  if (std::fabs(scale - 1.0f) < 0.005f) return false;
  if (scale < 1.0f) {  // window wider than the picture: bars left and right
    out[0] = std::floor(0.5f * w * (1.0f - scale));
    out[2] = w - out[0];
  } else {             // window taller: bars top and bottom
    out[1] = std::floor(0.5f * h * (1.0f - 1.0f / scale));
    out[3] = h - out[1];
  }
  return true;
}

// Draw the left eye image into the backbuffer (letterboxed if needed). The
// device state is left as it is: the eye renderer applies its complete state
// before every eye (eye_renderer.h).
void D3D9Device::ComposePreview() {
  if (!eye_out_[0]) return;
  float out[4];
  const bool letterbox = DesktopOutputRect(out);

  IDirect3DSurface9* backbuffer = nullptr;
  if (FAILED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;

  const UINT w = backbuffer_w_;
  const UINT h = backbuffer_h_;
  dev_->SetRenderTarget(0, backbuffer);
  dev_->SetDepthStencilSurface(nullptr);
  dev_->SetVertexShader(nullptr);
  dev_->SetPixelShader(nullptr);
  dev_->SetFVF(kViewFvf);
  dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev_->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev_->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  dev_->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev_->SetRenderState(D3DRS_COLORWRITEENABLE,
                       D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                           D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
  dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev_->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  dev_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dev_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev_->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  // Zoomed: point sampling, so what is on screen is the rendered pixel.
  const float zoom = GetConfig().preview_zoom;
  const DWORD filter = zoom > 1.0f ? D3DTEXF_POINT : D3DTEXF_LINEAR;
  dev_->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
  dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
  dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  const D3DVIEWPORT9 full{0, 0, w, h, 0.0f, 1.0f};
  dev_->SetViewport(&full);

  if (SUCCEEDED(dev_->BeginScene())) {
    if (letterbox) dev_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    // -0.5: D3D9 pixel centres sit on integer coordinates.
    // The part of the eye image to show ([display] preview_zoom around
    // preview_zoom_x/y), clamped to stay inside the image.
    const float half = 0.5f / zoom;
    const float cx = (std::min)(1.0f - half, (std::max)(half, GetConfig().preview_zoom_x));
    const float cy = (std::min)(1.0f - half, (std::max)(half, GetConfig().preview_zoom_y));
    const float u0 = cx - half, u1 = cx + half, v0 = cy - half, v1 = cy + half;
    const ViewVertex v[4] = {
        {out[0] - 0.5f, out[1] - 0.5f, 0.0f, 1.0f, u0, v0},
        {out[2] - 0.5f, out[1] - 0.5f, 0.0f, 1.0f, u1, v0},
        {out[0] - 0.5f, out[3] - 0.5f, 0.0f, 1.0f, u0, v1},
        {out[2] - 0.5f, out[3] - 0.5f, 0.0f, 1.0f, u1, v1},
    };
    dev_->SetTexture(0, eye_out_[0]);
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ViewVertex));
    dev_->EndScene();
  }

  backbuffer->Release();
}

}  // namespace kkvr
