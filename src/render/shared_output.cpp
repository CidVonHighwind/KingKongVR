// Zero-copy output to the headset.
//
// The game's device is a Direct3D9Ex device (proxy.cpp), which can create
// textures shared with other D3D devices; a plain Direct3D9 device refuses
// (share probe: D3DERR_INVALIDCALL). Each eye image is drawn on the GPU
// through the window's outline (DrawWindowOutline) into a slot of shared
// A8R8G8B8 render target textures (DXGI B8G8R8A8, the same
// format family as the headset's sRGB swapchain), and the XR thread copies
// them into its swapchain images with D3D11 CopyResource. No pixel crosses
// the CPU.
//
// The two devices share no command queue, so before submitting, the game
// thread makes sure the GPU has finished the copies: it reads one pixel of
// the left eye through a 1x1 lockable surface (a blocking lock waits for all
// queued work). The pixel doubles as a content check on the XR side.
//
// A slot is in use from submission until the XR thread releases it
// (XrPresenter::TakeReleasedSlots).

#include "device/wrapper.h"

#include "common/config.h"
#include "render/eye_projection.h"
#include "common/log.h"

#include <string>

namespace kkvr {

void D3D9Device::SubmitShared() {
  if (handover_failed_ || !xr_) return;
  if (!shared_adapter_checked_ && xr_->AdapterLuid() != 0) {
    shared_adapter_checked_ = true;
    if (adapter_luid_ != 0 && adapter_luid_ != xr_->AdapterLuid()) {
      Logf("shared output: the game (LUID %llx) and the headset runtime (LUID %llx) use "
           "different adapters, which cannot share textures: nothing is sent to the headset",
           adapter_luid_, xr_->AdapterLuid());
      ReleaseSharedOutput();
      shared_output_ = false;
      return;
    }
    Logf("shared output: game and headset runtime share adapter LUID %llx", adapter_luid_);
  }
  const uint32_t released = xr_->TakeReleasedSlots();
  for (int i = 0; i < kHandoverSlots; ++i) {
    if (released & (1u << i)) shared_slots_[i].in_use = false;
  }

  // The headset's own eye image size.
  if (xr_->RecommendedWidth() <= 0) return;
  const UINT cw = static_cast<UINT>(xr_->RecommendedWidth());
  const UINT ch = static_cast<UINT>(xr_->RecommendedHeight());
  if (shared_w_ != cw || shared_h_ != ch) {
    ReleaseSharedOutput();
    shared_w_ = cw;
    shared_h_ = ch;
  }

  SharedSlot* slot = nullptr;
  int index = -1;
  for (int i = 0; i < kHandoverSlots; ++i) {
    if (!shared_slots_[i].in_use) {
      slot = &shared_slots_[i];
      index = i;
      break;
    }
  }
  if (!slot) {
    ++handover_skipped_;
    return;
  }
  auto fail = [this](const char* what, HRESULT hr) {
    Logf("shared output: %s failed hr=0x%08X, headset output disabled", what,
         static_cast<unsigned>(hr));
    handover_failed_ = true;
  };
  HRESULT hr = D3D_OK;
  for (int eye = 0; eye < 2; ++eye) {
    if (slot->eye[eye]) continue;
    HANDLE handle = nullptr;
    if (FAILED(hr = dev_->CreateTexture(cw, ch, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &slot->eye[eye], &handle))) {
      return fail("CreateTexture (shared)", hr);
    }
    slot->handle[eye] = handle;
  }
  if (!shared_sync_ &&
      FAILED(hr = dev_->CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                           &shared_sync_, nullptr))) {
    return fail("CreateRenderTarget (sync)", hr);
  }

  for (int eye = 0; eye < 2; ++eye) {
    IDirect3DSurface9* dest = nullptr;
    slot->eye[eye]->GetSurfaceLevel(0, &dest);
    if (dest) DrawWindowOutline(eye, dest, cw, ch);
    if (dest) dest->Release();
  }
  const std::string& capture_dir = frame_capture_ == FrameCapture::kTracing
                                       ? frame_capture_dir_
                                       : frame_capture_pairs_dir_;
  if (!capture_dir.empty()) {
    // F2: the images exactly as the headset gets them.
    const std::string prefix = capture_dir + "headset" + std::to_string(handover_count_ % 1000);
    using SaveFn = HRESULT(WINAPI*)(const char*, DWORD, IDirect3DBaseTexture9*, const PALETTEENTRY*);
    static SaveFn save_texture = [] {
      HMODULE d3dx = GetModuleHandleA("d3dx9_26.dll");
      return d3dx ? reinterpret_cast<SaveFn>(GetProcAddress(d3dx, "D3DXSaveTextureToFileA")) : nullptr;
    }();
    if (save_texture) {
      const std::string extension = CaptureExtension();
      save_texture((prefix + "_L" + extension).c_str(), CaptureFormat(), slot->eye[0], nullptr);
      save_texture((prefix + "_R" + extension).c_str(), CaptureFormat(), slot->eye[1], nullptr);
      Logf("screenshot: %s_L/_R%s saved (headset images)", prefix.c_str(), extension.c_str());
    }
  }

  // Wait for the GPU (see top) and take the probe pixel.
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  XrPresenter::Frame frame;
  {
    IDirect3DSurface9* left = nullptr;
    slot->eye[0]->GetSurfaceLevel(0, &left);
    const RECT centre{static_cast<LONG>(cw / 2), static_cast<LONG>(ch / 2),
                      static_cast<LONG>(cw / 2 + 1), static_cast<LONG>(ch / 2 + 1)};
    dev_->StretchRect(left, &centre, shared_sync_, nullptr, D3DTEXF_NONE);
    left->Release();
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(shared_sync_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      std::memcpy(frame.probe, locked.pBits, 4);
      shared_sync_->UnlockRect();
    }
  }
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  handover_block_ms_ += 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart;

  frame.shared[0] = slot->handle[0];
  frame.shared[1] = slot->handle[1];
  frame.width = static_cast<int>(cw);
  frame.height = static_cast<int>(ch);
  frame.quad_width = window_width_;
  frame.slot = index;
  frame.views[0] = window_views_[0];
  frame.views[1] = window_views_[1];
  frame.pose_time = window_pose_time_;
  slot->in_use = true;
  xr_->SubmitFrame(frame);
  ++handover_count_;
}

// Projection output for one eye: transparent everywhere, and inside the
// window's outline (WindowOutline, eye_projection.h) the eye's rendered view.
// The outline is needed in two frames: the headset image covers the eye's
// whole field of view (where the polygon goes), while eye_out_ covers only the
// window's frustum (where its texture comes from).
void D3D9Device::DrawWindowOutline(int eye, IDirect3DSurface9* dest, UINT width, UINT height) {
  dev_->ColorFill(dest, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
  if (!eye_out_[eye]) return;
  float screen[16][2], uv[16][2];
  const float window_height = WindowHeight();
  const XrPresenter::EyeView& view = window_views_[eye];
  const int n = WindowOutline(view, window_width_, window_height, view.tangents, screen);
  if (n < 3) return;
  WindowOutline(view, window_width_, window_height, window_render_tangents_[eye], uv);

  struct Vertex {
    float x, y, z, rhw, u, v;
  } vertices[16];
  for (int i = 0; i < n; ++i) {
    // Both images are this eye's view (the headset image a wider one), so a
    // point's texture coordinate is linear in its headset image position: no
    // perspective correction, rhw 1.
    vertices[i] = {screen[i][0] * width - 0.5f, screen[i][1] * height - 0.5f,
                   0.0f, 1.0f, uv[i][0], uv[i][1]};
  }
  // No state is saved or restored: the eye renderer applies its complete
  // state before every eye (eye_renderer.h).
  dev_->SetRenderTarget(0, dest);
  dev_->SetDepthStencilSurface(nullptr);
  const D3DVIEWPORT9 full{0, 0, width, height, 0.0f, 1.0f};
  dev_->SetViewport(&full);
  dev_->SetVertexShader(nullptr);
  dev_->SetPixelShader(nullptr);
  dev_->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev_->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev_->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  dev_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev_->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
  dev_->SetTexture(0, eye_out_[eye]);
  dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  dev_->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  dev_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dev_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  if (SUCCEEDED(dev_->BeginScene())) {
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, static_cast<UINT>(n - 2), vertices, sizeof(Vertex));
    dev_->EndScene();
  }
  // Display space: fixed to the view, whatever the head does.
  if (GetConfig().output_grid) DrawTestGrid(dest, width, height, D3DCOLOR_ARGB(255, 255, 40, 40));
}

// [test] output_grid: evenly spaced lines over the whole surface, to see
// which stage deforms the image (config.h). Drawn with the same state the
// outline draw sets up, so it must follow one.
void D3D9Device::DrawTestGrid(IDirect3DSurface9* dest, UINT width, UINT height, D3DCOLOR colour) {
  constexpr UINT kSpacing = 256;  // pixels between lines
  constexpr float kThickness = 3.0f;
  struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR colour;
  };
  dev_->SetRenderTarget(0, dest);
  dev_->SetDepthStencilSurface(nullptr);
  const D3DVIEWPORT9 full{0, 0, width, height, 0.0f, 1.0f};
  dev_->SetViewport(&full);
  dev_->SetTexture(0, nullptr);
  dev_->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  if (FAILED(dev_->BeginScene())) return;
  for (UINT x = kSpacing; x < width; x += kSpacing) {
    const float x0 = static_cast<float>(x) - 0.5f * kThickness;
    const Vertex v[4] = {{x0, 0.0f, 0.0f, 1.0f, colour},
                         {x0 + kThickness, 0.0f, 0.0f, 1.0f, colour},
                         {x0, static_cast<float>(height), 0.0f, 1.0f, colour},
                         {x0 + kThickness, static_cast<float>(height), 0.0f, 1.0f, colour}};
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
  }
  for (UINT y = kSpacing; y < height; y += kSpacing) {
    const float y0 = static_cast<float>(y) - 0.5f * kThickness;
    const Vertex v[4] = {{0.0f, y0, 0.0f, 1.0f, colour},
                         {0.0f, y0 + kThickness, 0.0f, 1.0f, colour},
                         {static_cast<float>(width), y0, 0.0f, 1.0f, colour},
                         {static_cast<float>(width), y0 + kThickness, 0.0f, 1.0f, colour}};
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
  }
  dev_->EndScene();
}

void D3D9Device::ReleaseSharedOutput() {
  // The XR thread drops frames and its opened handles first (InvalidateFrames
  // bumps the shared generation).
  if (xr_) {
    xr_->InvalidateFrames();
    xr_->TakeReleasedSlots();
  }
  for (SharedSlot& slot : shared_slots_) {
    for (IDirect3DTexture9*& t : slot.eye) {
      if (t) t->Release();
      t = nullptr;
    }
    slot = SharedSlot();
  }
  if (shared_sync_) shared_sync_->Release();
  shared_sync_ = nullptr;
  shared_w_ = shared_h_ = 0;
}

}  // namespace kkvr
