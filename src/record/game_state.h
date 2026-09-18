// The Direct3D 9 state as the game set it.
//
// The proxy never executes the game's state changes and draws on the device
// while the game renders a frame; it records them (frame_record.h) and keeps
// this model of what the game believes is set. The model answers the game's
// Get* calls, and a copy taken at the start of each frame is where every eye
// render starts from (eye_renderer.h). So a recording is self-contained: it
// does not depend on what happens to be on the device.
//
// Objects in the model hold a reference (ComRef): the game may release a
// texture it still has "bound", and a copy of the model (the frame's start
// state) must keep its objects alive until the frame is rendered.
//
// Direct3D 9 behaviours the model reproduces because the game can observe
// them: SetRenderTarget(0) resets the viewport and scissor rectangle to the
// whole target; DrawPrimitiveUP unbinds stream 0 and DrawIndexedPrimitiveUP
// also the index buffer; SetFVF replaces a vertex declaration and vice versa.
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <map>

namespace kkvr {

// Reference-counting pointer for COM objects in the model.
template <typename T>
class ComRef {
 public:
  ComRef() = default;
  explicit ComRef(T* p) : p_(p) {
    if (p_) p_->AddRef();
  }
  ComRef(const ComRef& other) : p_(other.p_) {
    if (p_) p_->AddRef();
  }
  ComRef& operator=(const ComRef& other) {
    Reset(other.p_);
    return *this;
  }
  ~ComRef() {
    if (p_) p_->Release();
  }
  void Reset(T* p = nullptr) {
    if (p == p_) return;
    if (p) p->AddRef();
    if (p_) p_->Release();
    p_ = p;
  }
  T* get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

constexpr int kModelRenderStates = 256;
constexpr int kModelStages = 8;          // texture stage states
constexpr int kModelStageStates = 33;
constexpr int kModelSamplers = 20;       // 0-15 pixel, 16-19 vertex (D3DVERTEXTEXTURESAMPLER0..3)
constexpr int kModelSamplerStates = 14;
constexpr int kModelVsConstF = 256;
constexpr int kModelPsConstF = 32;
constexpr int kModelConstIB = 16;
constexpr int kModelStreams = 4;
constexpr int kModelTargets = 4;
constexpr int kModelClipPlanes = 6;

// Model slot of a sampler number (-1: not tracked).
inline int ModelSampler(DWORD sampler) {
  if (sampler < 16) return static_cast<int>(sampler);
  if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3) {
    return 16 + static_cast<int>(sampler - D3DVERTEXTEXTURESAMPLER0);
  }
  return -1;
}
inline DWORD SamplerNumber(int slot) {
  return slot < 16 ? static_cast<DWORD>(slot)
                   : D3DVERTEXTEXTURESAMPLER0 + static_cast<DWORD>(slot - 16);
}

const D3DMATRIX& IdentityMatrix();

struct ModelStream {
  ComRef<IDirect3DVertexBuffer9> vb;
  UINT offset = 0;
  UINT stride = 0;
  UINT frequency = 1;
};

struct ModelLight {
  D3DLIGHT9 light{};
  bool defined = false;
  bool enabled = false;
};

struct GameState {
  GameState();

  DWORD rs[kModelRenderStates] = {};
  bool rs_valid[kModelRenderStates] = {};  // the device accepts this render state
  DWORD tss[kModelStages][kModelStageStates] = {};
  DWORD ss[kModelSamplers][kModelSamplerStates] = {};
  ComRef<IDirect3DBaseTexture9> textures[kModelSamplers];

  // D3DTS_WORLD is D3DTS_WORLDMATRIX(0); other world matrices (vertex
  // blending) are kept in extra_world.
  D3DMATRIX world, view, projection;
  D3DMATRIX texture_matrix[kModelStages];
  std::map<DWORD, D3DMATRIX> extra_world;

  DWORD fvf = 0;
  ComRef<IDirect3DVertexDeclaration9> declaration;
  ComRef<IDirect3DVertexShader9> vertex_shader;
  ComRef<IDirect3DPixelShader9> pixel_shader;
  float vs_float[kModelVsConstF][4] = {};
  int vs_int[kModelConstIB][4] = {};
  BOOL vs_bool[kModelConstIB] = {};
  float ps_float[kModelPsConstF][4] = {};
  int ps_int[kModelConstIB][4] = {};
  BOOL ps_bool[kModelConstIB] = {};

  ModelStream streams[kModelStreams];
  ComRef<IDirect3DIndexBuffer9> indices;

  D3DVIEWPORT9 viewport{};
  RECT scissor{};
  ComRef<IDirect3DSurface9> targets[kModelTargets];
  ComRef<IDirect3DSurface9> depth_stencil;

  std::map<DWORD, ModelLight> lights;
  D3DMATERIAL9 material{};
  float clip_planes[kModelClipPlanes][4] = {};

  // The device's state right after CreateDevice or Reset: the game starts
  // from there. Called while nothing else has changed the device.
  void ReadFromDevice(IDirect3DDevice9* dev);
  // Drop all references (before Reset).
  void Clear();

  // Direct3D 9 side effect of SetRenderTarget(0, target): viewport and scissor
  // cover the whole target.
  void ResetViewportTo(IDirect3DSurface9* target);
};

}  // namespace kkvr
