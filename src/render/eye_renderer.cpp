#include "render/eye_renderer.h"

#include "common/config.h"
#include "render/d3d_math.h"
#include "render/eye_projection.h"
#include "common/log.h"
#include "record/vertex_arena.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kkvr {
namespace {

UINT VertexCountOf(D3DPRIMITIVETYPE type, UINT primitives) {
  switch (type) {
    case D3DPT_POINTLIST: return primitives;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    case D3DPT_TRIANGLELIST: return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return primitives + 2;
    default: return 0;
  }
}

bool SameMatrix(const D3DMATRIX& a, const D3DMATRIX& b) {
  return std::memcmp(&a, &b, sizeof(D3DMATRIX)) == 0;
}

// Bounding box of pre-transformed (XYZRHW) positions, in pixels.
struct PixelBounds {
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
};

PixelBounds BoundsOf(const uint8_t* vertices, UINT count, UINT stride) {
  PixelBounds b;
  for (UINT i = 0; i < count; ++i) {
    float p[2];
    std::memcpy(p, vertices + i * stride, sizeof(p));
    b.x0 = (std::min)(b.x0, p[0]);
    b.x1 = (std::max)(b.x1, p[0]);
    b.y0 = (std::min)(b.y0, p[1]);
    b.y1 = (std::max)(b.y1, p[1]);
  }
  return b;
}

}  // namespace

EyeRenderer::EyeRenderer(IDirect3DDevice9* device, VertexArena* arena,
                         const std::unordered_set<IDirect3DBaseTexture9*>* render_target_textures)
    : dev_(device), arena_(arena), rt_textures_(render_target_textures) {}

void EyeRenderer::SetTargetSize(UINT width, UINT height) {
  if (width == target_width_ && height == target_height_) return;
  target_width_ = width;
  target_height_ = height;
  Release();  // the copies of the game's targets have the old size
}

// The game's resolution comes with every eye (EyeContext), so a Reset to
// another size cannot leave the renderer with the old one.
void EyeRenderer::UpdateScale() {
  if (eye_.backbuffer_width != backbuffer_width_ || eye_.backbuffer_height != backbuffer_height_) {
    Release();  // copies of targets of the old size
    backbuffer_width_ = eye_.backbuffer_width;
    backbuffer_height_ = eye_.backbuffer_height;
  }
  const UINT width = output_width(), height = output_height();
  scale_x_ = backbuffer_width_ ? static_cast<float>(width) / backbuffer_width_ : 1.0f;
  scale_y_ = backbuffer_height_ ? static_cast<float>(height) / backbuffer_height_ : 1.0f;
  scaling_ = width != backbuffer_width_ || height != backbuffer_height_;
}

void EyeRenderer::Release() {
  state_.Clear();
  for (auto& m : scaled_surfaces_) m.clear();
  for (auto& m : scaled_textures_) m.clear();
  for (IDirect3DSurface9*& t : dev_targets_) t = nullptr;
  dev_depth_ = nullptr;
  for (IDirect3DBaseTexture9*& t : dev_textures_) t = nullptr;
}

void EyeRenderer::Render(const FrameRecord& record, const GameState& start,
                         const EyeContext& eye) {
  Begin(start, eye);
  for (const RecordedCall& call : record.calls()) Execute(call, record);
  End();
}

void EyeRenderer::Begin(const GameState& start, const EyeContext& eye) {
  eye_ = eye;
  UpdateScale();
  state_ = start;
  draws_done_ = 0;
  pass_ = 0;
  pass_draws_ = 0;
  ApplyAll(start);
}

void EyeRenderer::End() {
  FinishDraw();
  FinishPass();
}

// The target the frame has been drawing into is done: hand it to the dump.
void EyeRenderer::FinishPass() {
  if (pass_dump_ && pass_draws_ > 0) pass_dump_(pass_, pass_draws_, dev_targets_[0]);
  ++pass_;
  pass_draws_ = 0;
}

// -- device cache ------------------------------------------------------------

// Every value of `s` onto the device, unconditionally; the cache then holds
// exactly the device's values.
void EyeRenderer::ApplyAll(const GameState& s) {
  // Targets first: SetRenderTarget resets the viewport and scissor rectangle.
  for (DWORD i = 0; i < kModelTargets; ++i) {
    if (i == 0 || s.targets[i] || dev_targets_[i]) {
      dev_targets_[i] = DeviceSurface(s.targets[i].get());
      dev_->SetRenderTarget(i, dev_targets_[i]);
    }
  }
  rt0_backbuffer_ = TargetIsBackbuffer(s.targets[0].get());
  target_scaled_ = dev_targets_[0] != s.targets[0].get();
  dev_depth_ = DeviceSurface(s.depth_stencil.get());
  dev_->SetDepthStencilSurface(dev_depth_);
  dev_viewport_ = DeviceViewport(s.viewport);
  dev_->SetViewport(&dev_viewport_);
  dev_scissor_ = DeviceRect(s.scissor);
  dev_->SetScissorRect(&dev_scissor_);
  layer_depth_ = false;

  for (DWORD i = 0; i < kModelRenderStates; ++i) {
    if (!s.rs_valid[i]) continue;
    dev_->SetRenderState(static_cast<D3DRENDERSTATETYPE>(i), s.rs[i]);
    dev_rs_[i] = s.rs[i];
  }
  for (DWORD stage = 0; stage < kModelStages; ++stage) {
    for (DWORD type = 1; type < kModelStageStates; ++type) {
      dev_->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type),
                                 s.tss[stage][type]);
      dev_tss_[stage][type] = s.tss[stage][type];
    }
    dev_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage),
                       &s.texture_matrix[stage]);
    dev_texture_matrix_[stage] = s.texture_matrix[stage];
  }
  for (int slot = 0; slot < kModelSamplers; ++slot) {
    const DWORD sampler = SamplerNumber(slot);
    for (DWORD type = 1; type < kModelSamplerStates; ++type) {
      dev_->SetSamplerState(sampler, static_cast<D3DSAMPLERSTATETYPE>(type), s.ss[slot][type]);
      dev_ss_[slot][type] = s.ss[slot][type];
    }
    dev_textures_[slot] = DeviceTexture(s.textures[slot].get());
    dev_->SetTexture(sampler, dev_textures_[slot]);
  }
  dev_->SetTransform(D3DTS_WORLD, &s.world);
  dev_world_ = s.world;
  dev_->SetTransform(D3DTS_VIEW, &s.view);
  dev_view_ = s.view;
  dev_->SetTransform(D3DTS_PROJECTION, &s.projection);
  dev_projection_ = s.projection;
  for (const auto& kv : s.extra_world) {
    dev_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(kv.first), &kv.second);
  }
  if (s.declaration) {
    dev_->SetVertexDeclaration(s.declaration.get());
    dev_fvf_ = 0;
  } else {
    dev_->SetFVF(s.fvf);
    dev_fvf_ = s.fvf;
  }
  dev_declaration_ = s.declaration.get();
  dev_->SetVertexShader(s.vertex_shader.get());
  dev_vs_ = s.vertex_shader.get();
  dev_->SetPixelShader(s.pixel_shader.get());
  dev_ps_ = s.pixel_shader.get();
  dev_->SetVertexShaderConstantF(0, &s.vs_float[0][0], kModelVsConstF);
  std::memcpy(dev_vs_float_, s.vs_float, sizeof(dev_vs_float_));
  dev_->SetVertexShaderConstantI(0, &s.vs_int[0][0], kModelConstIB);
  dev_->SetVertexShaderConstantB(0, s.vs_bool, kModelConstIB);
  if (s.pixel_shader) {
    dev_->SetPixelShaderConstantF(0, &s.ps_float[0][0], kModelPsConstF);
    dev_->SetPixelShaderConstantI(0, &s.ps_int[0][0], kModelConstIB);
    dev_->SetPixelShaderConstantB(0, s.ps_bool, kModelConstIB);
  }
  for (UINT i = 0; i < kModelStreams; ++i) {
    const ModelStream& st = s.streams[i];
    // Game buffers that live in the vertex arena are never bound themselves:
    // the recording binds their arena region before every draw that uses it.
    IDirect3DVertexBuffer9* vb =
        st.vb && arena_ && arena_->Redirected(st.vb.get()) ? nullptr : st.vb.get();
    dev_->SetStreamSource(i, vb, vb ? st.offset : 0, vb ? st.stride : 0);
    dev_streams_[i] = {vb, vb ? st.offset : 0, vb ? st.stride : 0};
    dev_->SetStreamSourceFreq(i, st.frequency);
  }
  dev_->SetIndices(s.indices.get());
  dev_indices_ = s.indices.get();
  for (const auto& kv : s.lights) {
    if (kv.second.defined) dev_->SetLight(kv.first, &kv.second.light);
    dev_->LightEnable(kv.first, kv.second.enabled);
  }
  dev_->SetMaterial(&s.material);
  for (DWORD i = 0; i < kModelClipPlanes; ++i) dev_->SetClipPlane(i, s.clip_planes[i]);
}

void EyeRenderer::DevRenderState(DWORD state, DWORD value) {
  if (state < kModelRenderStates) {
    if (dev_rs_[state] == value) return;
    dev_rs_[state] = value;
  }
  dev_->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
}

void EyeRenderer::DevStageState(DWORD stage, DWORD type, DWORD value) {
  if (stage < kModelStages && type < kModelStageStates) {
    if (dev_tss_[stage][type] == value) return;
    dev_tss_[stage][type] = value;
  }
  dev_->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), value);
}

void EyeRenderer::DevSamplerState(int slot, DWORD type, DWORD value) {
  if (slot < 0) return;
  if (type < kModelSamplerStates) {
    if (dev_ss_[slot][type] == value) return;
    dev_ss_[slot][type] = value;
  }
  dev_->SetSamplerState(SamplerNumber(slot), static_cast<D3DSAMPLERSTATETYPE>(type), value);
}

void EyeRenderer::DevTexture(int slot, IDirect3DBaseTexture9* game_texture) {
  if (slot < 0) return;
  IDirect3DBaseTexture9* texture = DeviceTexture(game_texture);
  if (dev_textures_[slot] == texture) return;
  dev_textures_[slot] = texture;
  dev_->SetTexture(SamplerNumber(slot), texture);
}

void EyeRenderer::DevTransform(DWORD state, const D3DMATRIX& m) {
  D3DMATRIX* cached = nullptr;
  if (state == D3DTS_VIEW) {
    cached = &dev_view_;
  } else if (state == D3DTS_PROJECTION) {
    cached = &dev_projection_;
  } else if (state == static_cast<DWORD>(D3DTS_WORLD)) {
    cached = &dev_world_;
  } else if (state >= D3DTS_TEXTURE0 && state < D3DTS_TEXTURE0 + kModelStages) {
    cached = &dev_texture_matrix_[state - D3DTS_TEXTURE0];
  }
  if (cached) {
    if (SameMatrix(*cached, m)) return;
    *cached = m;
  }
  dev_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(state), &m);
}

void EyeRenderer::DevFvf(DWORD fvf) {
  if (!dev_declaration_ && dev_fvf_ == fvf) return;
  dev_->SetFVF(fvf);
  dev_fvf_ = fvf;
  dev_declaration_ = nullptr;
}

void EyeRenderer::DevDeclaration(IDirect3DVertexDeclaration9* declaration) {
  if (dev_declaration_ == declaration && declaration) return;
  dev_->SetVertexDeclaration(declaration);
  dev_declaration_ = declaration;
  dev_fvf_ = 0;
}

void EyeRenderer::DevVertexShader(IDirect3DVertexShader9* shader) {
  if (dev_vs_ == shader) return;
  dev_->SetVertexShader(shader);
  dev_vs_ = shader;
}

void EyeRenderer::DevPixelShader(IDirect3DPixelShader9* shader) {
  if (dev_ps_ == shader) return;
  dev_->SetPixelShader(shader);
  dev_ps_ = shader;
}

void EyeRenderer::DevVsFloat(UINT start, const float* data, UINT count) {
  if (start < kModelVsConstF && start + count <= kModelVsConstF) {
    if (std::memcmp(dev_vs_float_[start], data, count * 4 * sizeof(float)) == 0) return;
    std::memcpy(dev_vs_float_[start], data, count * 4 * sizeof(float));
  }
  dev_->SetVertexShaderConstantF(start, data, count);
}

void EyeRenderer::DevStream(UINT index, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
  if (index < kModelStreams) {
    DevStreamBinding& d = dev_streams_[index];
    if (d.vb == vb && d.offset == offset && d.stride == stride) return;
    d = {vb, offset, stride};
  }
  dev_->SetStreamSource(index, vb, offset, stride);
}

void EyeRenderer::DevIndices(IDirect3DIndexBuffer9* indices) {
  if (dev_indices_ == indices) return;
  dev_->SetIndices(indices);
  dev_indices_ = indices;
}

// The game's viewport (scaled here for a scaled target).
void EyeRenderer::DevViewport(const D3DVIEWPORT9& game_viewport) {
  const D3DVIEWPORT9 vp = DeviceViewport(game_viewport);
  if (std::memcmp(&dev_viewport_, &vp, sizeof(vp)) == 0) return;
  dev_->SetViewport(&vp);
  dev_viewport_ = vp;
}

void EyeRenderer::DevScissor(const RECT& game_rect) {
  const RECT r = DeviceRect(game_rect);
  if (std::memcmp(&dev_scissor_, &r, sizeof(r)) == 0) return;
  dev_->SetScissorRect(&r);
  dev_scissor_ = r;
}

void EyeRenderer::DevTarget(DWORD index, IDirect3DSurface9* game_target) {
  IDirect3DSurface9* target = DeviceSurface(game_target);
  if (index == 0) target_scaled_ = target != game_target;
  if (index < kModelTargets && dev_targets_[index] == target && index != 0) return;
  dev_->SetRenderTarget(index, target);
  if (index < kModelTargets) dev_targets_[index] = target;
  if (index == 0) {
    // Direct3D 9 resets the viewport and scissor rectangle to the target.
    D3DSURFACE_DESC d{};
    if (target && SUCCEEDED(target->GetDesc(&d))) {
      dev_viewport_ = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
      dev_scissor_ = {0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
    }
  }
}

void EyeRenderer::DevDepthStencil(IDirect3DSurface9* game_surface) {
  IDirect3DSurface9* surface = DeviceSurface(game_surface);
  if (dev_depth_ == surface) return;
  dev_->SetDepthStencilSurface(surface);
  dev_depth_ = surface;
}

void EyeRenderer::RestoreVertexFormat() {
  if (state_.declaration) {
    DevDeclaration(state_.declaration.get());
  } else {
    DevFvf(state_.fvf);
  }
}

bool EyeRenderer::TargetIsBackbuffer(IDirect3DSurface9* target) const {
  D3DSURFACE_DESC d{};
  return target && SUCCEEDED(target->GetDesc(&d)) && d.Width == backbuffer_width_ &&
         d.Height == backbuffer_height_;
}

// -- executing the recording ----------------------------------------------------

void EyeRenderer::Execute(const RecordedCall& c, const FrameRecord& record) {
  const auto data = [&](uint32_t offset) { return record.Data(offset); };
  switch (c.cmd) {
    case Cmd::kSetRenderState:
      if (c.a[0] < kModelRenderStates) state_.rs[c.a[0]] = c.a[1];
      DevRenderState(c.a[0], c.a[1]);
      break;
    case Cmd::kSetSamplerState: {
      const int slot = ModelSampler(c.a[0]);
      if (slot >= 0 && c.a[1] < kModelSamplerStates) state_.ss[slot][c.a[1]] = c.a[2];
      if (slot >= 0) {
        DevSamplerState(slot, c.a[1], c.a[2]);
      } else {
        dev_->SetSamplerState(c.a[0], static_cast<D3DSAMPLERSTATETYPE>(c.a[1]), c.a[2]);
      }
      break;
    }
    case Cmd::kSetTextureStageState:
      if (c.a[0] < kModelStages && c.a[1] < kModelStageStates) state_.tss[c.a[0]][c.a[1]] = c.a[2];
      DevStageState(c.a[0], c.a[1], c.a[2]);
      break;
    case Cmd::kSetTexture: {
      const int slot = ModelSampler(c.a[0]);
      auto* texture = static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(c.obj));
      if (slot >= 0) {
        state_.textures[slot].Reset(texture);
        DevTexture(slot, texture);
      } else {
        dev_->SetTexture(c.a[0], DeviceTexture(texture));
      }
      break;
    }
    case Cmd::kSetTransform: {
      const DWORD state = c.a[0];
      const auto* m = reinterpret_cast<const D3DMATRIX*>(data(c.data));
      if (state == D3DTS_VIEW) {
        state_.view = *m;  // set per draw (window layers replace it)
      } else if (state == D3DTS_PROJECTION) {
        state_.projection = *m;  // eye-dependent: set per draw
      } else if (state >= D3DTS_TEXTURE0 && state < D3DTS_TEXTURE0 + kModelStages) {
        state_.texture_matrix[state - D3DTS_TEXTURE0] = *m;
        DevTransform(state, *m);
      } else if (state == static_cast<DWORD>(D3DTS_WORLD)) {
        state_.world = *m;
        DevTransform(state, *m);
      } else {
        state_.extra_world[state] = *m;
        dev_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(state), m);
      }
      break;
    }
    case Cmd::kSetFVF:
      state_.fvf = c.a[0];
      state_.declaration.Reset();
      DevFvf(c.a[0]);
      break;
    case Cmd::kSetStreamSource:
      if (c.a[0] < kModelStreams) {
        ModelStream& s = state_.streams[c.a[0]];
        s.vb.Reset(static_cast<IDirect3DVertexBuffer9*>(c.obj));
        s.offset = c.a[1];
        s.stride = c.a[2];
      }
      DevStream(c.a[0], static_cast<IDirect3DVertexBuffer9*>(c.obj), c.a[1], c.a[2]);
      break;
    case Cmd::kSetVertexDeclaration: {
      auto* declaration = static_cast<IDirect3DVertexDeclaration9*>(c.obj);
      state_.declaration.Reset(declaration);
      if (declaration) state_.fvf = 0;
      DevDeclaration(declaration);
      break;
    }
    case Cmd::kSetIndices:
      state_.indices.Reset(static_cast<IDirect3DIndexBuffer9*>(c.obj));
      DevIndices(static_cast<IDirect3DIndexBuffer9*>(c.obj));
      break;
    case Cmd::kSetVertexShader:
      state_.vertex_shader.Reset(static_cast<IDirect3DVertexShader9*>(c.obj));
      DevVertexShader(static_cast<IDirect3DVertexShader9*>(c.obj));
      break;
    case Cmd::kSetPixelShader:
      state_.pixel_shader.Reset(static_cast<IDirect3DPixelShader9*>(c.obj));
      DevPixelShader(static_cast<IDirect3DPixelShader9*>(c.obj));
      break;
    case Cmd::kSetVertexShaderConstantF: {
      const auto* values = reinterpret_cast<const float*>(data(c.data));
      const UINT start = c.a[0], count = c.a[1];
      if (start < kModelVsConstF) {
        const UINT n = (std::min)(count, static_cast<UINT>(kModelVsConstF) - start);
        std::memcpy(state_.vs_float[start], values, n * 4 * sizeof(float));
      }
      DevVsFloat(start, values, count);  // c0..c3 are rebuilt per draw if needed
      break;
    }
    case Cmd::kSetPixelShaderConstantF:
      dev_->SetPixelShaderConstantF(c.a[0], reinterpret_cast<const float*>(data(c.data)), c.a[1]);
      break;
    case Cmd::kSetVertexShaderConstantI:
      dev_->SetVertexShaderConstantI(c.a[0], reinterpret_cast<const int*>(data(c.data)), c.a[1]);
      break;
    case Cmd::kSetVertexShaderConstantB:
      dev_->SetVertexShaderConstantB(c.a[0], reinterpret_cast<const BOOL*>(data(c.data)), c.a[1]);
      break;
    case Cmd::kSetPixelShaderConstantI:
      dev_->SetPixelShaderConstantI(c.a[0], reinterpret_cast<const int*>(data(c.data)), c.a[1]);
      break;
    case Cmd::kSetPixelShaderConstantB:
      dev_->SetPixelShaderConstantB(c.a[0], reinterpret_cast<const BOOL*>(data(c.data)), c.a[1]);
      break;
    case Cmd::kSetLight:
      dev_->SetLight(c.a[0], reinterpret_cast<const D3DLIGHT9*>(data(c.data)));
      break;
    case Cmd::kLightEnable:
      dev_->LightEnable(c.a[0], static_cast<BOOL>(c.a[1]));
      break;
    case Cmd::kSetMaterial:
      dev_->SetMaterial(reinterpret_cast<const D3DMATERIAL9*>(data(c.data)));
      break;
    case Cmd::kSetViewport:
      state_.viewport = *reinterpret_cast<const D3DVIEWPORT9*>(data(c.data));
      DevViewport(state_.viewport);
      break;
    case Cmd::kSetScissorRect:
      state_.scissor = *reinterpret_cast<const RECT*>(data(c.data));
      DevScissor(state_.scissor);
      break;
    case Cmd::kSetRenderTarget: {
      auto* target = static_cast<IDirect3DSurface9*>(c.obj);
      if (c.a[0] < kModelTargets) state_.targets[c.a[0]].Reset(target);
      if (c.a[0] == 0) {
        if (pass_dump_) FinishPass();  // the previous target is finished
        state_.ResetViewportTo(target);
        rt0_backbuffer_ = TargetIsBackbuffer(target);
      }
      DevTarget(c.a[0], target);
      break;
    }
    case Cmd::kSetDepthStencilSurface:
      state_.depth_stencil.Reset(static_cast<IDirect3DSurface9*>(c.obj));
      DevDepthStencil(static_cast<IDirect3DSurface9*>(c.obj));
      break;
    case Cmd::kClear: {
      DevViewport(state_.viewport);
      const auto* rects = c.size ? reinterpret_cast<const D3DRECT*>(data(c.data)) : nullptr;
      if (rects) {
        // The game paints its letterbox bars twice: two quads and a clear of
        // the same two rectangles. Both go ([render] hide_letterbox), or the
        // bands stay black whatever happens to the quads.
        std::vector<D3DRECT> keep;
        keep.reserve(c.a[0]);
        for (DWORD i = 0; i < c.a[0]; ++i) {
          const RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2};
          if (eye_.hide_letterbox && IsLetterboxRect(r)) {
            ++letterbox_draws_;
            continue;
          }
          const RECT d = target_scaled_ ? DeviceRect(r) : r;
          keep.push_back(D3DRECT{d.left, d.top, d.right, d.bottom});
        }
        if (!keep.empty()) {
          dev_->Clear(static_cast<DWORD>(keep.size()), keep.data(), c.a[1], c.a[2], c.f, c.a[3]);
        } else if (c.a[0] == 0) {
          dev_->Clear(0, nullptr, c.a[1], c.a[2], c.f, c.a[3]);
        }
      } else {
        dev_->Clear(c.a[0], nullptr, c.a[1], c.a[2], c.f, c.a[3]);
      }
      break;
    }
    case Cmd::kStretchRect: {
      // Between the scaled copies, with their rectangles scaled.
      auto* source = static_cast<IDirect3DSurface9*>(c.obj);
      auto* dest = static_cast<IDirect3DSurface9*>(c.obj2);
      IDirect3DSurface9* device_source = DeviceSurface(source);
      IDirect3DSurface9* device_dest = DeviceSurface(dest);
      RECT sr{}, dr{};
      if (c.size) {
        sr = *reinterpret_cast<const RECT*>(data(c.data));
        if (device_source != source) sr = DeviceRect(sr);
      }
      if (c.size2) {
        dr = *reinterpret_cast<const RECT*>(data(c.data2));
        if (device_dest != dest) dr = DeviceRect(dr);
      }
      dev_->StretchRect(device_source, c.size ? &sr : nullptr, device_dest,
                        c.size2 ? &dr : nullptr, static_cast<D3DTEXTUREFILTERTYPE>(c.a[0]));
      break;
    }
    case Cmd::kColorFill: {
      auto* surface = static_cast<IDirect3DSurface9*>(c.obj);
      IDirect3DSurface9* device_surface = DeviceSurface(surface);
      RECT r{};
      if (c.size) {
        r = *reinterpret_cast<const RECT*>(data(c.data));
        if (device_surface != surface) r = DeviceRect(r);
      }
      dev_->ColorFill(device_surface, c.size ? &r : nullptr, c.a[0]);
      break;
    }
    case Cmd::kBeginScene:
      dev_->BeginScene();
      break;
    case Cmd::kEndScene:
      dev_->EndScene();
      break;
    case Cmd::kSetClipPlane:
      dev_->SetClipPlane(c.a[0], reinterpret_cast<const float*>(data(c.data)));
      break;
    case Cmd::kDrawPrimitive: {
      if (SkipDraw()) break;
      const auto type = static_cast<D3DPRIMITIVETYPE>(c.a[0]);
      if (c.size && IsLetterboxDraw(type, c.a[2], data(c.data), state_.streams[0].stride)) break;
      if (c.size && IsWindowLayerDraw() &&
          DrawWindowLayer(type, c.a[2], data(c.data), state_.streams[0].stride)) {
        break;
      }
      PrepareDraw(c);
      if (c.size && target_scaled_ && IsRhw()) {
        DrawScaledRhw(type, c.a[2], data(c.data), state_.streams[0].stride);
        const ModelStream& s0 = state_.streams[0];
        DevStream(0, s0.vb.get(), s0.offset, s0.stride);
      } else {
        dev_->DrawPrimitive(type, c.a[1], c.a[2]);
      }
      FinishDraw();
      break;
    }
    case Cmd::kDrawIndexedPrimitive:
      if (SkipDraw()) break;
      if (target_scaled_ && IsRhw()) ++unscaled_rhw_draws_;
      PrepareDraw(c);
      dev_->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(c.a[0]), static_cast<INT>(c.a[1]),
                                 c.a[2], c.a[3], c.a[4], c.a[5]);
      FinishDraw();
      break;
    case Cmd::kDrawPrimitiveUP: {
      if (SkipDraw()) break;
      const auto type = static_cast<D3DPRIMITIVETYPE>(c.a[0]);
      if (IsLetterboxDraw(type, c.a[1], data(c.data), c.a[2])) {
        state_.streams[0] = ModelStream();
        dev_streams_[0] = {};
        break;
      }
      if (!(IsWindowLayerDraw() && DrawWindowLayer(type, c.a[1], data(c.data), c.a[2]))) {
        PrepareDraw(c);
        if (target_scaled_ && IsRhw()) {
          DrawScaledRhw(type, c.a[1], data(c.data), c.a[2]);
        } else {
          dev_->DrawPrimitiveUP(type, c.a[1], data(c.data), c.a[2]);
        }
        FinishDraw();
      }
      // Direct3D 9 unbinds stream 0 after an UP draw.
      state_.streams[0] = ModelStream();
      dev_streams_[0] = {};
      break;
    }
    case Cmd::kDrawIndexedPrimitiveUP:
      if (SkipDraw()) break;
      if (target_scaled_ && IsRhw()) ++unscaled_rhw_draws_;
      PrepareDraw(c);
      dev_->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(c.a[0]), c.a[1], c.a[2], c.a[3],
                                   data(c.data2), static_cast<D3DFORMAT>(c.a[4]), data(c.data),
                                   c.a[5]);
      FinishDraw();
      state_.streams[0] = ModelStream();
      dev_streams_[0] = {};
      state_.indices.Reset();
      dev_indices_ = nullptr;
      break;
    case Cmd::kSetStreamSourceFreq:
      if (c.a[0] < kModelStreams) state_.streams[c.a[0]].frequency = c.a[1];
      dev_->SetStreamSourceFreq(c.a[0], c.a[1]);
      break;
    case Cmd::kSetNPatchMode:
      dev_->SetNPatchMode(c.f);
      break;
    case Cmd::kSetSoftwareVertexProcessing:
      dev_->SetSoftwareVertexProcessing(static_cast<BOOL>(c.a[0]));
      break;
    case Cmd::kSetCurrentTexturePalette:
      dev_->SetCurrentTexturePalette(c.a[0]);
      break;
    case Cmd::kUpdateSurface:
      dev_->UpdateSurface(static_cast<IDirect3DSurface9*>(c.obj),
                          c.size ? reinterpret_cast<const RECT*>(data(c.data)) : nullptr,
                          static_cast<IDirect3DSurface9*>(c.obj2),
                          c.size2 ? reinterpret_cast<const POINT*>(data(c.data2)) : nullptr);
      break;
    case Cmd::kUpdateTexture:
      dev_->UpdateTexture(
          static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(c.obj)),
          static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(c.obj2)));
      break;
  }
}

// -- the eye's view -----------------------------------------------------------

// Camera and projection for the upcoming draw. VIEW stays the game's; the
// eye's camera and its projection through the window replace PROJECTION.
void EyeRenderer::PrepareDraw(const RecordedCall& draw) {
  const bool screen_quad = (draw.quad & 1) != 0;
  const bool screen_quad_copy = (draw.quad & 2) != 0;
  const float screen_quad_w = draw.quad_w;
  const D3DMATRIX& game_view = state_.view;
  const D3DMATRIX& game_proj = state_.projection;
  const bool perspective_target = rt0_backbuffer_ && IsPerspective(game_proj);
  const bool main_view = perspective_target && !screen_quad;
  const bool window = eye_.mode == EyeMode::kWindow;

  // Fallback: PatchScreenAspect (aspect.h) makes the engine project for the
  // real aspect. Only if the patch could not be applied (another game build)
  // is the projection 16:9 at any resolution; then it is matched to the
  // viewport here ([display] aspect_fix).
  D3DMATRIX base = game_proj;
  bool adjusted = false;
  if (main_view && !eye_.aspect_patched && eye_.aspect_fix != kAspectFixOff &&
      eye_.backbuffer_height > 0) {
    const D3DVIEWPORT9& vp = state_.viewport;
    const float target = vp.Height > 0 ? static_cast<float>(vp.Width) / vp.Height
                                       : static_cast<float>(eye_.backbuffer_width) /
                                             eye_.backbuffer_height;
    const float current = std::fabs(base._22) / base._11;  // width / height
    if (std::fabs(current - target) > 0.01f) {
      if (eye_.aspect_fix == kAspectFixCrop) {
        base._11 *= current / target;
      } else {
        base._22 *= target / current;
      }
      adjusted = true;
    }
  }

  D3DMATRIX proj = base;
  float layer_depth = -1.0f;
  const float scale = eye_.world_scale;

  // How far this shot is moved onto the window (eye_projection.h). Wide lenses
  // (gameplay at 85 degrees, the menu scene at 57) are left alone and are a
  // window into the world; a long lens is filmed from closer than it implies,
  // so its subject would sit between the window and the viewer, often behind
  // their head. The amount follows the shot's field of view continuously, so a
  // zoom moves gradually instead of switching modes: nothing above
  // 1.2 x [vr] cinematic_fov (54 degrees by default, below the menu scene's
  // 57), kFlattenMax at the threshold itself.
  constexpr float kFlattenMax = 0.97f;  // 1 would put every surface at one depth
  float flatten = 0.0f;
  if (eye_.cinematic_fov > 0.0f && perspective_target) {
    const float shot_fov =
        114.59156f * std::atan(1.0f / (base._11 > 1e-3f ? base._11 : 1e-3f));
    const float narrow = eye_.cinematic_fov, wide = 1.2f * eye_.cinematic_fov;
    const float t = (shot_fov - narrow) / (wide - narrow);  // 0 at narrow, 1 at wide
    flatten = kFlattenMax * (1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t)));
  }
  // A flattened shot is a picture, and a cutscene's framing fills the window
  // with a face: show it smaller, the way a cinema screen is smaller than the
  // room ([vr] cinematic_scale). Follows the same ramp, so nothing jumps.
  const float picture = 1.0f - flatten * (1.0f - eye_.cinematic_scale);

  if (window && perspective_target && !(screen_quad && screen_quad_copy)) {
    // Native eye view (eye_projection.h). Scene geometry goes through the
    // eye's camera; screen layers (menus, fades) are first placed on the
    // window plane. The post-effect composite (a screen quad sampling a render
    // target, which already holds this eye's image) stays screen-aligned.
    const float plane = base._11 * 0.5f * eye_.window_width * scale;
    const D3DMATRIX camera = EyeViewMatrix(eye_.view, scale, plane, flatten, picture);
    const D3DMATRIX eye_proj = EyeProjection(eye_.render_tangents, base._33, base._43);
    if (!screen_quad) {
      // The eye's camera goes into PROJECTION, not VIEW. Everything after the
      // view space is matrix multiplication, so the image is the same, but
      // camera space stays the game's: the coordinates the pipeline generates
      // there (the characters' irises), its lighting and its fog are exactly
      // what the game set up, and nothing has to be corrected per eye. With
      // the camera in VIEW they all had to be, and a flattened shot could not
      // be (the camera has perspective in it, which a texture matrix cannot
      // undo - it left Ann's eyes blank, 2026-09-17).
      proj = Multiply(camera, eye_proj);
    } else {
      // Clip position -> the point on the window plane where it was shown:
      // (x/w * plane/P11, y/w * plane/P22, plane), with w the quad's depth.
      const float w0 = screen_quad_w > 1e-6f ? screen_quad_w : 1.0f;
      D3DMATRIX to_plane{};
      to_plane._11 = plane / (w0 * base._11);
      to_plane._22 = plane / (w0 * base._22);
      to_plane._43 = plane / w0;
      to_plane._44 = 1.0f / w0;
      proj = Multiply(Multiply(Multiply(base, to_plane), camera), eye_proj);
    }
    adjusted = true;
  } else if (window && rt0_backbuffer_ && !IsPerspective(game_proj)) {
    // Orthographic 2D layers (menu text and panels: pixel or 0..1
    // coordinates, 2026-09-17 captures). Where the game would show a point on
    // the screen, it is placed on the window plane instead, then seen through
    // the eye's camera. NDC (x, y) -> game view (x * hw, -y * hh) on the plane.
    const WindowLayer layer = WindowLayerTransform();
    D3DMATRIX to_plane{};
    to_plane._11 = layer.hw;
    to_plane._22 = -layer.hh;
    to_plane._43 = layer.plane;
    to_plane._44 = 1.0f;
    proj = Multiply(Multiply(Multiply(base, to_plane), layer.view), layer.projection);
    adjusted = true;
    // Depth: the game gives these layers a constant depth in front of
    // everything (z = _43 for vertices at z 0) and draws them after the 3D
    // menu scene with the depth test on. At the window plane they would be
    // tested against that scene, which may stand in front of the window (the
    // language list vanished behind the menu's Kong picture, headset test
    // 2026-09-17). Keep the game's depth instead.
    const D3DVIEWPORT9& vp = state_.viewport;
    layer_depth = vp.MinZ + base._43 * (vp.MaxZ - vp.MinZ);
  } else if (main_view && eye_.mode == EyeMode::kWindowReference) {
    // Reference render for the projection check (frame captures): the eye's
    // image of the window rectangle, off-axis, with the same 1:1 model as the
    // native eye render (eye `distance` metres in front of the window plane).
    // Camera at p (view space: +x right, +y down, +z forward), plane at c:
    //   x_clip = q.x * P11 * d/c + q.z * P11 * p.x/c   (q = v - p, d = c - p.z)
    // The move to p goes into PROJECTION as well (camera space stays the
    // game's, as in the window mode).
    const float* e = eye_.view.position;
    const float c = base._11 * 0.5f * eye_.window_width * scale;
    const float distance = e[2] > 0.05f ? e[2] : 0.05f;
    const float p[3] = {e[0] * scale, -e[1] * scale, c - distance * scale};
    const float k = (c - p[2]) / c;
    proj._11 *= k;
    proj._22 *= k;
    proj._31 += base._11 * p[0] / c;
    proj._32 += base._22 * p[1] / c;
    D3DMATRIX to_eye = IdentityMatrix();
    to_eye._41 = -p[0];
    to_eye._42 = -p[1];
    to_eye._43 = -p[2];
    proj = Multiply(to_eye, proj);
    adjusted = true;
  }

  DevTransform(D3DTS_VIEW, game_view);
  DevTransform(D3DTS_PROJECTION, proj);

  if (layer_depth >= 0.0f) {
    D3DVIEWPORT9 vp = state_.viewport;
    vp.MinZ = vp.MaxZ = (std::min)(1.0f, layer_depth);
    DevViewport(vp);
    layer_depth_ = true;
  }

  // Shader draws (the skinned characters): c0..c3 is W*V*P or its transpose,
  // and has to be rebuilt from the eye's matrices or the character keeps the
  // game's camera - drawn in the wrong place and at a depth that does not
  // match the rest, so it can vanish behind the scene (Ann was missing in the
  // opening cinematic, 2026-09-17).
  //
  // The world matrix in those constants is not always the one SetTransform
  // holds: in cinematics the engine sets only the shader's. So it is recovered
  // instead of guessed - c0..c3 times the inverse of the game's V*P is that
  // world matrix, and the right form is the one whose result is affine (last
  // column 0,0,0,1). Both forms are tried; if neither is affine, c0..c3 is
  // something else and stays untouched.
  if (!state_.vertex_shader) return;
  const float* c0 = &state_.vs_float[0][0];
  D3DMATRIX constants;
  std::memcpy(&constants, c0, sizeof(constants));
  D3DMATRIX world{};
  int form = 0;
  D3DMATRIX view_projection_inverse;
  if (adjusted && Invert(Multiply(game_view, game_proj), &view_projection_inverse)) {
    const auto affine = [](const D3DMATRIX& m) {
      return std::fabs(m._14) < 1e-3f && std::fabs(m._24) < 1e-3f &&
             std::fabs(m._34) < 1e-3f && std::fabs(m._44 - 1.0f) < 1e-3f;
    };
    const D3DMATRIX straight = Multiply(constants, view_projection_inverse);
    const D3DMATRIX transposed = Multiply(Transpose(constants), view_projection_inverse);
    if (affine(straight)) {
      form = 1;
      world = straight;
    } else if (affine(transposed)) {
      form = 2;
      world = transposed;
    }
  }
  if (form != 0) {
    D3DMATRIX m = Multiply(Multiply(world, game_view), proj);
    if (form == 2) m = Transpose(m);
    DevVsFloat(0, &m.m[0][0], 4);
  } else {
    DevVsFloat(0, c0, 4);
  }
}

void EyeRenderer::FinishDraw() {
  if (!layer_depth_) return;
  DevViewport(state_.viewport);
  layer_depth_ = false;
}

// 2D layers are placed on the window plane (half width hw, half height hh,
// at distance `plane` in game view space; any distance works, the plane and
// the camera agree) and drawn through the eye's camera.
EyeRenderer::WindowLayer EyeRenderer::WindowLayerTransform() const {
  const float scale = eye_.world_scale;
  WindowLayer layer;
  layer.hw = 0.5f * eye_.window_width * scale;
  layer.hh = 0.5f * eye_.window_height * scale;
  layer.plane = layer.hw;
  layer.view = EyeViewMatrix(eye_.view, scale, layer.plane);
  layer.projection = EyeProjection(eye_.render_tangents, 1.0f, -0.05f);
  return layer;
}

// -- pre-transformed 2D layers --------------------------------------------------

// Some of the game's 2D draws use pre-transformed vertices (D3DFVF_XYZRHW,
// positions in render-target pixels): the loading screen video, menu fades,
// letterbox bars. No matrix reaches them, so they are re-issued with the
// positions rewritten: the pixel where the game would show a vertex becomes
// the same point on the window plane, drawn through the eye's camera.
// Stay screen-aligned (they work on the eye's own image): draws into anything
// but the backbuffer (the post-effect chain), draws sampling a render-target
// texture (post-effect composites), untextured quads covering the whole
// viewport (tints, fades; inside the window the result is the same).
bool EyeRenderer::IsWindowLayerDraw() const {
  if (eye_.mode != EyeMode::kWindow || state_.vertex_shader || state_.declaration ||
      !rt0_backbuffer_) {
    return false;
  }
  if ((state_.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return false;
  for (int slot = 0; slot < 16; ++slot) {
    IDirect3DBaseTexture9* t = state_.textures[slot].get();
    if (t && rt_textures_ && rt_textures_->count(t)) return false;  // composite
  }
  return true;
}

bool EyeRenderer::DrawWindowLayer(D3DPRIMITIVETYPE type, UINT primitives,
                                  const uint8_t* vertices, UINT stride) {
  const UINT count = VertexCountOf(type, primitives);
  if (!vertices || stride < 16 || count == 0) return false;
  const D3DVIEWPORT9& viewport = state_.viewport;
  if (!state_.textures[0]) {
    // Untextured and covering the whole viewport: a tint or fade, keep it.
    // (Letterbox bars do not get here: IsLetterboxDraw has dropped them.)
    const PixelBounds b = BoundsOf(vertices, count, stride);
    const float vx = static_cast<float>(viewport.X), vy = static_cast<float>(viewport.Y);
    const float vw = static_cast<float>(viewport.Width), vh = static_cast<float>(viewport.Height);
    const bool full_width = b.x0 <= vx + 1.0f && b.x1 >= vx + vw - 1.5f;
    if (full_width && b.y0 <= vy + 1.0f && b.y1 >= vy + vh - 1.5f) return false;
  }

  // Pixel -> NDC -> window plane (game view space: +x right, +y down; the
  // window spans the render target). D3D9 pixel centres sit at integer
  // coordinates, so the target's left edge is at x = -0.5.
  const WindowLayer layer = WindowLayerTransform();
  const float vx = static_cast<float>(viewport.X), vy = static_cast<float>(viewport.Y);
  const float vw = viewport.Width ? static_cast<float>(viewport.Width) : 1.0f;
  const float vh = viewport.Height ? static_cast<float>(viewport.Height) : 1.0f;
  const UINT out_stride = stride - 4;  // XYZRHW (4 floats) -> XYZ (3 floats)
  layer_bytes_.resize(static_cast<size_t>(count) * out_stride);
  for (UINT i = 0; i < count; ++i) {
    const uint8_t* in = vertices + i * stride;
    uint8_t* out = layer_bytes_.data() + i * out_stride;
    float p[4];
    std::memcpy(p, in, sizeof(p));
    const float ndc_x = (p[0] + 0.5f - vx) / vw * 2.0f - 1.0f;
    const float ndc_y = 1.0f - (p[1] + 0.5f - vy) / vh * 2.0f;
    const float q[3] = {ndc_x * layer.hw, -ndc_y * layer.hh, layer.plane};
    std::memcpy(out, q, sizeof(q));
    std::memcpy(out + 12, in + 16, stride - 16);
  }

  const DWORD fvf = state_.fvf;
  DevRenderState(D3DRS_LIGHTING, FALSE);  // pre-transformed vertices were never lit
  DevTransform(D3DTS_WORLD, IdentityMatrix());
  DevTransform(D3DTS_VIEW, layer.view);
  DevTransform(D3DTS_PROJECTION, layer.projection);
  DevFvf((fvf & ~D3DFVF_POSITION_MASK) | D3DFVF_XYZ);
  // The game's own depth for the layer (the pre-transformed z of the first
  // vertex), not the window plane's: see the orthographic layers.
  float z0 = 0.0f;
  std::memcpy(&z0, vertices + 8, sizeof(z0));
  D3DVIEWPORT9 vp = viewport;
  vp.MinZ = vp.MaxZ = (std::max)(0.0f, (std::min)(1.0f, z0));
  DevViewport(vp);
  dev_->DrawPrimitiveUP(type, primitives, layer_bytes_.data(), out_stride);
  dev_streams_[0] = {};  // UP draws unbind stream 0

  // Back to the game's state.
  DevViewport(viewport);
  DevFvf(fvf);
  DevRenderState(D3DRS_LIGHTING, state_.rs[D3DRS_LIGHTING]);
  DevTransform(D3DTS_WORLD, state_.world);
  const ModelStream& s0 = state_.streams[0];
  DevStream(0, s0.vb.get(), s0.offset, s0.stride);
  ++window_layer_draws_;
  return true;
}

// -- render scale ----------------------------------------------------------------

IDirect3DSurface9* EyeRenderer::DeviceSurface(IDirect3DSurface9* game) {
  if (!game) return game;
  const int eye = (eye_.index == 1 && !eye_.share_targets) ? 1 : 0;
  // Without a render scale only the second eye needs its own copies.
  if (!scaling_ && eye == 0) return game;
  auto& surfaces = scaled_surfaces_[eye];
  auto it = surfaces.find(game);
  if (it != surfaces.end()) return it->second.scaled ? it->second.scaled.get() : game;

  ScaledSurface entry;
  entry.game.Reset(game);
  D3DSURFACE_DESC d{};
  if (SUCCEEDED(game->GetDesc(&d)) && d.Width == backbuffer_width_ &&
      d.Height == backbuffer_height_) {
    IDirect3DSurface9* scaled = nullptr;
    IDirect3DTexture9* container = nullptr;
    if (SUCCEEDED(game->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&container))) && container) {
      // A level of a render-target texture: that level of the scaled texture.
      auto* scaled_texture = static_cast<IDirect3DTexture9*>(DeviceTexture(container));
      if (scaled_texture != container) {
        for (DWORD level = 0; level < container->GetLevelCount(); ++level) {
          IDirect3DSurface9* s = nullptr;
          if (FAILED(container->GetSurfaceLevel(level, &s))) continue;
          const bool match = s == game;
          s->Release();
          if (match) {
            scaled_texture->GetSurfaceLevel(level, &scaled);
            break;
          }
        }
      }
      container->Release();
    } else if (d.Usage & D3DUSAGE_DEPTHSTENCIL) {
      dev_->CreateDepthStencilSurface(ScaledX(d.Width), ScaledY(d.Height), d.Format,
                                      d.MultiSampleType, d.MultiSampleQuality, FALSE, &scaled,
                                      nullptr);
    } else if (d.Usage & D3DUSAGE_RENDERTARGET) {
      dev_->CreateRenderTarget(ScaledX(d.Width), ScaledY(d.Height), d.Format, d.MultiSampleType,
                               d.MultiSampleQuality, FALSE, &scaled, nullptr);
    }
    Logf("render scale: %ux%u %s (format %d, multisample %d) -> %ux%u %s", d.Width, d.Height,
         (d.Usage & D3DUSAGE_DEPTHSTENCIL) ? "depth buffer" : "render target",
         static_cast<int>(d.Format), static_cast<int>(d.MultiSampleType), ScaledX(d.Width),
         ScaledY(d.Height), scaled ? "created" : "FAILED (drawn at game resolution)");
    if (scaled) {
      entry.scaled.Reset(scaled);
      scaled->Release();
    }
  }
  IDirect3DSurface9* result = entry.scaled ? entry.scaled.get() : game;
  surfaces.emplace(game, std::move(entry));
  return result;
}

IDirect3DBaseTexture9* EyeRenderer::DeviceTexture(IDirect3DBaseTexture9* game) {
  if (!game || !rt_textures_ || !rt_textures_->count(game)) return game;
  const int eye = (eye_.index == 1 && !eye_.share_targets) ? 1 : 0;
  if (!scaling_ && eye == 0) return game;
  auto& textures = scaled_textures_[eye];
  auto it = textures.find(game);
  if (it != textures.end()) return it->second.scaled ? it->second.scaled.get() : game;

  ScaledTexture entry;
  entry.game.Reset(game);
  // render_target_textures_ holds only IDirect3DTexture9 (CreateTexture hook).
  auto* texture = static_cast<IDirect3DTexture9*>(game);
  D3DSURFACE_DESC d{};
  if (SUCCEEDED(texture->GetLevelDesc(0, &d)) && d.Width == backbuffer_width_ &&
      d.Height == backbuffer_height_) {
    IDirect3DTexture9* scaled = nullptr;
    const UINT levels = (d.Usage & D3DUSAGE_AUTOGENMIPMAP) ? 0 : texture->GetLevelCount();
    const HRESULT hr = dev_->CreateTexture(ScaledX(d.Width), ScaledY(d.Height), levels, d.Usage,
                                           d.Format, D3DPOOL_DEFAULT, &scaled, nullptr);
    Logf("render scale: %ux%u render target texture (format %d) -> %ux%u %s", d.Width, d.Height,
         static_cast<int>(d.Format), ScaledX(d.Width), ScaledY(d.Height),
         SUCCEEDED(hr) ? "created" : "FAILED (drawn at game resolution)");
    if (SUCCEEDED(hr)) {
      entry.scaled.Reset(scaled);
      scaled->Release();
    }
  }
  IDirect3DBaseTexture9* result = entry.scaled ? entry.scaled.get() : game;
  textures.emplace(game, std::move(entry));
  return result;
}

D3DVIEWPORT9 EyeRenderer::DeviceViewport(const D3DVIEWPORT9& v) const {
  if (!target_scaled_) return v;
  D3DVIEWPORT9 d = v;
  d.X = static_cast<DWORD>(std::lround(v.X * scale_x_));
  d.Y = static_cast<DWORD>(std::lround(v.Y * scale_y_));
  d.Width = static_cast<DWORD>(std::lround((v.X + v.Width) * scale_x_)) - d.X;
  d.Height = static_cast<DWORD>(std::lround((v.Y + v.Height) * scale_y_)) - d.Y;
  return d;
}

RECT EyeRenderer::DeviceRect(const RECT& r) const {
  if (!target_scaled_) return r;
  return {std::lround(r.left * scale_x_), std::lround(r.top * scale_y_),
          std::lround(r.right * scale_x_), std::lround(r.bottom * scale_y_)};
}

// [test] debug_keys (bisecting an artefact): hide a range of draws. The state
// of the hidden draws is still applied, and the rest of the frame, including
// the post effects that put the picture on the screen, still runs.
bool EyeRenderer::SkipDraw() {
  ++draws_done_;
  ++pass_draws_;
  return eye_.skip_count > 0 && draws_done_ > eye_.skip_from &&
         draws_done_ <= eye_.skip_from + eye_.skip_count;
}

// A pre-transformed, untextured draw that is a letterbox bar ([render]
// hide_letterbox). The game letterboxes cinematics to 16:9 with two black bars
// inside the 4:3 frame and renders the picture behind them; the window is a
// fixed 4:3 frame, so the bars would only make it smaller. Checked before any
// other handling of the draw, in every mode.
bool EyeRenderer::IsLetterboxDraw(D3DPRIMITIVETYPE type, UINT primitives,
                                  const uint8_t* vertices, UINT stride) {
  if (!eye_.hide_letterbox || !vertices || stride < 16 || state_.textures[0] || !IsRhw()) {
    return false;
  }
  const UINT count = VertexCountOf(type, primitives);
  if (count == 0 || count > 8) return false;
  const PixelBounds b = BoundsOf(vertices, count, stride);
  if (IsLetterboxRect(RECT{static_cast<LONG>(b.x0), static_cast<LONG>(b.y0),
                           static_cast<LONG>(b.x1), static_cast<LONG>(b.y1)})) {
    ++letterbox_draws_;
    return true;
  }
  return false;
}

// A letterbox bar: the full width of the viewport, against its top or bottom
// edge, and a band rather than the whole frame.
bool EyeRenderer::IsLetterboxRect(const RECT& r) const {
  const D3DVIEWPORT9& vp = state_.viewport;
  const LONG vx = static_cast<LONG>(vp.X), vy = static_cast<LONG>(vp.Y);
  const LONG vw = static_cast<LONG>(vp.Width), vh = static_cast<LONG>(vp.Height);
  const bool full_width = r.left <= vx + 1 && r.right >= vx + vw - 2;
  const bool at_edge = r.top <= vy + 1 || r.bottom >= vy + vh - 2;
  return full_width && at_edge && (r.bottom - r.top) < (45 * vh) / 100;
}

bool EyeRenderer::IsRhw() const {
  return !state_.vertex_shader && !state_.declaration &&
         (state_.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

// A pre-transformed draw into a scaled target: positions scaled.
//
// Positions are multiplied by the scale and nothing else. Source texture and
// target are scaled by the same factor, so for a quad x' = a*x + b the texel
// the pass reads for a pixel comes out shifted by exactly -b: any half-pixel
// term makes every pass shift the picture instead of filtering it. The game's
// post-effect chain draws its quads at 0..W-0.5 rather than -0.5..W-0.5, so
// each copy blends two texels (a cheap blur); with b = 0 that relationship is
// the same at any render scale.
//
// Measured on the user's captures (2026-09-17, scale 1.9 x 2.17): b = (s-1)/2
// drifted the picture by +1 pixel per pass, b = -(s-1)/2 by -1 pixel per pass,
// over six blur passes each time. The composite then blended displaced copies,
// which is what showed up as seams in the headset.
void EyeRenderer::DrawScaledRhw(D3DPRIMITIVETYPE type, UINT primitives, const uint8_t* vertices,
                                UINT stride) {
  const UINT count = VertexCountOf(type, primitives);
  if (!vertices || stride < 16 || count == 0) return;
  rhw_bytes_.assign(vertices, vertices + static_cast<size_t>(count) * stride);
  for (UINT i = 0; i < count; ++i) {
    uint8_t* v = rhw_bytes_.data() + i * stride;
    float p[2];
    std::memcpy(p, v, sizeof(p));
    p[0] *= scale_x_;
    p[1] *= scale_y_;
    std::memcpy(v, p, sizeof(p));
  }
  dev_->DrawPrimitiveUP(type, primitives, rhw_bytes_.data(), stride);
  dev_streams_[0] = {};  // UP draws unbind stream 0
}

}  // namespace kkvr
