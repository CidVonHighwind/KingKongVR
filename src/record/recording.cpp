// The proxy: records the game's frame.
//
// Every state change and draw of the game updates the state model (game_,
// game_state.h) and is appended to the frame recording (record_, frame_record.h).
// Nothing of it is executed on the device while the game renders: at Present
// every eye is rendered from the recording (frame_render.cpp). The game's
// queries are answered from the model.
//
// Decisions that need the game's vertices are taken here, when the draw is
// recorded, and travel with the recorded draw:
//   - screen-filling quads (IsScreenQuad): kept on the window plane or
//     screen-aligned per eye;
//   - pre-transformed (XYZRHW) draws onto the backbuffer keep a copy of their
//     vertices, so the renderer can place them on the window;
//   - vertex arena bindings (BindArenaStreams): which arena region holds each
//     stream's data right now.
// A set of what is already set is neither recorded nor executed (the 2005
// engine re-sends most of its state for every draw); runs of state sets are
// coalesced to the last value before the next other command.

#include "device/wrapper.h"

#include "common/config.h"
#include "render/d3d_math.h"
#include "common/log.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
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

// Coalescing keys (FrameRecord::AddState): render states 0-255, sampler
// states 256 + slot * 14 + type, FVF 536, textures 537 + slot, texture stage
// states 557 + stage * 33 + type.
constexpr int kKeyFvf = 536;
int SamplerKey(int slot, DWORD type) { return 256 + slot * 14 + static_cast<int>(type); }
int TextureKey(int slot) { return 537 + slot; }
int StageKey(DWORD stage, DWORD type) {
  return 557 + static_cast<int>(stage) * 33 + static_cast<int>(type);
}

}  // namespace

RecordedCall& D3D9Device::Record(Cmd cmd) { return record_.Add(cmd); }

RecordedCall& D3D9Device::RecordState(Cmd cmd, int key) { return record_.AddState(cmd, key); }

void D3D9Device::Live(const RecordedCall& call) {
  if (execute_live_) renderer_->Execute(call, record_);
}

#define RECORD_ARGS(call, ...)                                                   \
  {                                                                              \
    const DWORD values[] = {__VA_ARGS__};                                        \
    for (size_t arg_ = 0; arg_ < sizeof(values) / sizeof(DWORD); ++arg_) (call).a[arg_] = values[arg_]; \
  }

// -- state -----------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  const DWORD s = static_cast<DWORD>(State);
  if (s < kModelRenderStates) {
    if (game_.rs_valid[s] && game_.rs[s] == Value) {
      ++filtered_calls_;
      return D3D_OK;
    }
    game_.rs[s] = Value;
    game_.rs_valid[s] = true;
  }
  RecordedCall& c = RecordState(Cmd::kSetRenderState, s < kModelRenderStates ? static_cast<int>(s) : -1);
  RECORD_ARGS(c, s, Value);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
  const DWORD s = static_cast<DWORD>(State);
  if (!pValue) return D3DERR_INVALIDCALL;
  if (s >= kModelRenderStates || !game_.rs_valid[s]) return D3DERR_INVALIDCALL;
  *pValue = game_.rs[s];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type,
                                                      DWORD Value) {
  const int slot = ModelSampler(Sampler);
  const DWORD t = static_cast<DWORD>(Type);
  const bool tracked = slot >= 0 && t < kModelSamplerStates;
  if (tracked) {
    if (game_.ss[slot][t] == Value) {
      ++filtered_calls_;
      return D3D_OK;
    }
    game_.ss[slot][t] = Value;
  }
  RecordedCall& c = RecordState(Cmd::kSetSamplerState, tracked ? SamplerKey(slot, t) : -1);
  RECORD_ARGS(c, Sampler, t, Value);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type,
                                                      DWORD* pValue) {
  const int slot = ModelSampler(Sampler);
  if (!pValue || slot < 0 || static_cast<DWORD>(Type) >= kModelSamplerStates) {
    return D3DERR_INVALIDCALL;
  }
  *pValue = game_.ss[slot][Type];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTextureStageState(DWORD Stage,
                                                           D3DTEXTURESTAGESTATETYPE Type,
                                                           DWORD Value) {
  const DWORD t = static_cast<DWORD>(Type);
  const bool tracked = Stage < kModelStages && t < kModelStageStates;
  if (tracked) {
    if (game_.tss[Stage][t] == Value) {
      ++filtered_calls_;
      return D3D_OK;
    }
    game_.tss[Stage][t] = Value;
  }
  RecordedCall& c = RecordState(Cmd::kSetTextureStageState, tracked ? StageKey(Stage, t) : -1);
  RECORD_ARGS(c, Stage, t, Value);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTextureStageState(DWORD Stage,
                                                           D3DTEXTURESTAGESTATETYPE Type,
                                                           DWORD* pValue) {
  if (!pValue || Stage >= kModelStages || static_cast<DWORD>(Type) >= kModelStageStates) {
    return D3DERR_INVALIDCALL;
  }
  *pValue = game_.tss[Stage][Type];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  const int slot = ModelSampler(Stage);
  if (slot >= 0) {
    if (game_.textures[slot].get() == pTexture) {
      ++filtered_calls_;
      return D3D_OK;
    }
    game_.textures[slot].Reset(pTexture);
  }
  RecordedCall& c = RecordState(Cmd::kSetTexture, slot >= 0 ? TextureKey(slot) : -1);
  c.a[0] = Stage;
  // A coalesced call may already hold the texture it had before.
  c.obj = record_.Hold(pTexture);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
  const int slot = ModelSampler(Stage);
  if (!ppTexture || slot < 0) return D3DERR_INVALIDCALL;
  *ppTexture = game_.textures[slot].get();
  if (*ppTexture) (*ppTexture)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State,
                                                   CONST D3DMATRIX* pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  // Not a switch: D3DTS_WORLD is D3DTS_WORLDMATRIX(0) == 256, outside the
  // D3DTRANSFORMSTATETYPE enumeration.
  const DWORD state = static_cast<DWORD>(State);
  if (state == D3DTS_VIEW) {
    game_.view = *pMatrix;
  } else if (state == D3DTS_PROJECTION) {
    game_.projection = *pMatrix;
  } else if (state == static_cast<DWORD>(D3DTS_WORLD)) {
    game_.world = *pMatrix;
  } else if (state >= D3DTS_TEXTURE0 && state < D3DTS_TEXTURE0 + kModelStages) {
    game_.texture_matrix[state - D3DTS_TEXTURE0] = *pMatrix;
  } else {
    game_.extra_world[state] = *pMatrix;
  }
  RecordedCall& c = Record(Cmd::kSetTransform);
  c.a[0] = state;
  c.data = record_.Store(pMatrix, sizeof(D3DMATRIX));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
  if (!pMatrix) return D3DERR_INVALIDCALL;
  const DWORD state = static_cast<DWORD>(State);
  if (state == D3DTS_VIEW) {
    *pMatrix = game_.view;
  } else if (state == D3DTS_PROJECTION) {
    *pMatrix = game_.projection;
  } else if (state == static_cast<DWORD>(D3DTS_WORLD)) {
    *pMatrix = game_.world;
  } else if (state >= D3DTS_TEXTURE0 && state < D3DTS_TEXTURE0 + kModelStages) {
    *pMatrix = game_.texture_matrix[state - D3DTS_TEXTURE0];
  } else {
    const auto it = game_.extra_world.find(state);
    *pMatrix = it != game_.extra_world.end() ? it->second : IdentityMatrix();
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetFVF(DWORD FVF) {
  if (!game_.declaration && game_.fvf == FVF) {
    ++filtered_calls_;
    return D3D_OK;
  }
  game_.fvf = FVF;
  game_.declaration.Reset();  // SetFVF replaces a vertex declaration
  RecordedCall& c = RecordState(Cmd::kSetFVF, kKeyFvf);
  c.a[0] = FVF;
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetFVF(DWORD* pFVF) {
  if (!pFVF) return D3DERR_INVALIDCALL;
  *pFVF = game_.fvf;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
  game_.declaration.Reset(pDecl);
  if (pDecl) game_.fvf = 0;  // a declaration replaces the FVF
  RecordedCall& c = Record(Cmd::kSetVertexDeclaration);
  c.obj = record_.Hold(pDecl);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = game_.declaration.get();
  if (*ppDecl) (*ppDecl)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSource(UINT StreamNumber,
                                                      IDirect3DVertexBuffer9* pStreamData,
                                                      UINT OffsetInBytes, UINT Stride) {
  if (StreamNumber >= kModelStreams) {
    RecordedCall& c = Record(Cmd::kSetStreamSource);
    RECORD_ARGS(c, StreamNumber, OffsetInBytes, Stride);
    c.obj = record_.Hold(pStreamData);
    Live(c);
    return D3D_OK;
  }
  ModelStream& s = game_.streams[StreamNumber];
  if (s.vb.get() == pStreamData && s.offset == OffsetInBytes && s.stride == Stride) {
    ++filtered_calls_;
    return D3D_OK;
  }
  s.vb.Reset(pStreamData);
  s.offset = OffsetInBytes;
  s.stride = Stride;
  // Buffers in the vertex arena are bound at the next draw, at the region
  // that holds their data then (BindArenaStreams).
  if (pStreamData && arena_->Redirected(pStreamData)) return D3D_OK;
  Binding& recorded = recorded_streams_[StreamNumber];
  if (recorded.vb == pStreamData && recorded.offset == OffsetInBytes && recorded.stride == Stride) {
    return D3D_OK;
  }
  recorded = {pStreamData, OffsetInBytes, Stride};
  RecordedCall& c = Record(Cmd::kSetStreamSource);
  RECORD_ARGS(c, StreamNumber, OffsetInBytes, Stride);
  c.obj = record_.Hold(pStreamData);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSource(UINT StreamNumber,
                                                      IDirect3DVertexBuffer9** ppStreamData,
                                                      UINT* pOffsetInBytes, UINT* pStride) {
  if (!ppStreamData || StreamNumber >= kModelStreams) return D3DERR_INVALIDCALL;
  const ModelStream& s = game_.streams[StreamNumber];
  *ppStreamData = s.vb.get();
  if (*ppStreamData) (*ppStreamData)->AddRef();
  if (pOffsetInBytes) *pOffsetInBytes = s.offset;
  if (pStride) *pStride = s.stride;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
  if (StreamNumber < kModelStreams) game_.streams[StreamNumber].frequency = Setting;
  RecordedCall& c = Record(Cmd::kSetStreamSourceFreq);
  RECORD_ARGS(c, StreamNumber, Setting);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
  game_.indices.Reset(pIndexData);
  RecordedCall& c = Record(Cmd::kSetIndices);
  c.obj = record_.Hold(pIndexData);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
  if (!ppIndexData) return D3DERR_INVALIDCALL;
  *ppIndexData = game_.indices.get();
  if (*ppIndexData) (*ppIndexData)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader) {
  game_.vertex_shader.Reset(pShader);
  RecordedCall& c = Record(Cmd::kSetVertexShader);
  c.obj = record_.Hold(pShader);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = game_.vertex_shader.get();
  if (*ppShader) (*ppShader)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader) {
  game_.pixel_shader.Reset(pShader);
  RecordedCall& c = Record(Cmd::kSetPixelShader);
  c.obj = record_.Hold(pShader);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader) {
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = game_.pixel_shader.get();
  if (*ppShader) (*ppShader)->AddRef();
  return D3D_OK;
}

namespace {
template <typename T>
void StoreConstants(T* model, int model_count, UINT start, const T* data, UINT count, int per) {
  if (!data || start >= static_cast<UINT>(model_count)) return;
  const UINT n = (std::min)(count, static_cast<UINT>(model_count) - start);
  std::memcpy(model + start * per, data, n * per * sizeof(T));
}
}  // namespace

#define RECORD_CONSTANTS(command, type, per)                                   \
  if (!pConstantData) return D3DERR_INVALIDCALL;                               \
  RecordedCall& c = Record(Cmd::command);                                      \
  c.a[0] = StartRegister;                                                      \
  c.a[1] = count;                                                              \
  c.data = record_.Store(pConstantData, count * per * sizeof(type));           \
  Live(c);                                                                     \
  return D3D_OK;

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantF(UINT StartRegister,
                                                               CONST float* pConstantData,
                                                               UINT Vector4fCount) {
  const UINT count = Vector4fCount;
  StoreConstants(&game_.vs_float[0][0], kModelVsConstF, StartRegister, pConstantData, count, 4);
  RECORD_CONSTANTS(kSetVertexShaderConstantF, float, 4);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantF(UINT StartRegister,
                                                               float* pConstantData,
                                                               UINT Vector4fCount) {
  if (!pConstantData || StartRegister + Vector4fCount > static_cast<UINT>(kModelVsConstF)) {
    return D3DERR_INVALIDCALL;
  }
  std::memcpy(pConstantData, game_.vs_float[StartRegister], Vector4fCount * 4 * sizeof(float));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantI(UINT StartRegister,
                                                               CONST int* pConstantData,
                                                               UINT Vector4iCount) {
  const UINT count = Vector4iCount;
  StoreConstants(&game_.vs_int[0][0], kModelConstIB, StartRegister, pConstantData, count, 4);
  RECORD_CONSTANTS(kSetVertexShaderConstantI, int, 4);
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantB(UINT StartRegister,
                                                               CONST BOOL* pConstantData,
                                                               UINT BoolCount) {
  const UINT count = BoolCount;
  StoreConstants(game_.vs_bool, kModelConstIB, StartRegister, pConstantData, count, 1);
  RECORD_CONSTANTS(kSetVertexShaderConstantB, BOOL, 1);
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantF(UINT StartRegister,
                                                              CONST float* pConstantData,
                                                              UINT Vector4fCount) {
  const UINT count = Vector4fCount;
  StoreConstants(&game_.ps_float[0][0], kModelPsConstF, StartRegister, pConstantData, count, 4);
  RECORD_CONSTANTS(kSetPixelShaderConstantF, float, 4);
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantI(UINT StartRegister,
                                                              CONST int* pConstantData,
                                                              UINT Vector4iCount) {
  const UINT count = Vector4iCount;
  StoreConstants(&game_.ps_int[0][0], kModelConstIB, StartRegister, pConstantData, count, 4);
  RECORD_CONSTANTS(kSetPixelShaderConstantI, int, 4);
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantB(UINT StartRegister,
                                                              CONST BOOL* pConstantData,
                                                              UINT BoolCount) {
  const UINT count = BoolCount;
  StoreConstants(game_.ps_bool, kModelConstIB, StartRegister, pConstantData, count, 1);
  RECORD_CONSTANTS(kSetPixelShaderConstantB, BOOL, 1);
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight) {
  if (!pLight) return D3DERR_INVALIDCALL;
  ModelLight& light = game_.lights[Index];
  light.light = *pLight;
  light.defined = true;
  RecordedCall& c = Record(Cmd::kSetLight);
  c.a[0] = Index;
  c.data = record_.Store(pLight, sizeof(*pLight));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight) {
  const auto it = game_.lights.find(Index);
  if (!pLight || it == game_.lights.end() || !it->second.defined) return D3DERR_INVALIDCALL;
  *pLight = it->second.light;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  ModelLight& light = game_.lights[Index];
  if (!light.defined) {
    // Direct3D 9 creates a default directional light.
    light.light = D3DLIGHT9{};
    light.light.Type = D3DLIGHT_DIRECTIONAL;
    light.light.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
    light.light.Direction = {0.0f, 0.0f, 1.0f};
    light.defined = true;
  }
  light.enabled = Enable != FALSE;
  RecordedCall& c = Record(Cmd::kLightEnable);
  RECORD_ARGS(c, Index, static_cast<DWORD>(Enable));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable) {
  const auto it = game_.lights.find(Index);
  if (!pEnable || it == game_.lights.end()) return D3DERR_INVALIDCALL;
  *pEnable = it->second.enabled ? TRUE : FALSE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  game_.material = *pMaterial;
  RecordedCall& c = Record(Cmd::kSetMaterial);
  c.data = record_.Store(pMaterial, sizeof(*pMaterial));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial) {
  if (!pMaterial) return D3DERR_INVALIDCALL;
  *pMaterial = game_.material;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane) {
  if (!pPlane) return D3DERR_INVALIDCALL;
  if (Index < kModelClipPlanes) std::memcpy(game_.clip_planes[Index], pPlane, 4 * sizeof(float));
  RecordedCall& c = Record(Cmd::kSetClipPlane);
  c.a[0] = Index;
  c.data = record_.Store(pPlane, 4 * sizeof(float));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetClipPlane(DWORD Index, float* pPlane) {
  if (!pPlane || Index >= kModelClipPlanes) return D3DERR_INVALIDCALL;
  std::memcpy(pPlane, game_.clip_planes[Index], 4 * sizeof(float));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetNPatchMode(float nSegments) {
  RecordedCall& c = Record(Cmd::kSetNPatchMode);
  c.f = nSegments;
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware) {
  RecordedCall& c = Record(Cmd::kSetSoftwareVertexProcessing);
  c.a[0] = static_cast<DWORD>(bSoftware);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber) {
  RecordedCall& c = Record(Cmd::kSetCurrentTexturePalette);
  c.a[0] = PaletteNumber;
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  if (CaptureThisFrame()) {
    Trace("SetViewport(%lu,%lu %lux%lu)", pViewport->X, pViewport->Y, pViewport->Width,
          pViewport->Height);
  }
  game_.viewport = *pViewport;
  RecordedCall& c = Record(Cmd::kSetViewport);
  c.data = record_.Store(pViewport, sizeof(*pViewport));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetViewport(D3DVIEWPORT9* pViewport) {
  if (!pViewport) return D3DERR_INVALIDCALL;
  *pViewport = game_.viewport;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetScissorRect(CONST RECT* pRect) {
  if (!pRect) return D3DERR_INVALIDCALL;
  game_.scissor = *pRect;
  RecordedCall& c = Record(Cmd::kSetScissorRect);
  c.data = record_.Store(pRect, sizeof(*pRect));
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetScissorRect(RECT* pRect) {
  if (!pRect) return D3DERR_INVALIDCALL;
  *pRect = game_.scissor;
  return D3D_OK;
}

// -- targets ---------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderTarget(DWORD RenderTargetIndex,
                                                      IDirect3DSurface9* pRenderTarget) {
  if (RenderTargetIndex == 0 && !pRenderTarget) return D3DERR_INVALIDCALL;
  if (RenderTargetIndex < kModelTargets) game_.targets[RenderTargetIndex].Reset(pRenderTarget);
  if (CaptureThisFrame()) {
    char desc[96];
    DescribeSurface(pRenderTarget, desc, sizeof(desc));
    Trace("SetRenderTarget(%lu, %s)", RenderTargetIndex, desc);
  }
  if (RenderTargetIndex == 0) {
    game_.ResetViewportTo(pRenderTarget);  // Direct3D 9 does this
    D3DSURFACE_DESC d{};
    rt0_backbuffer_ = SUCCEEDED(pRenderTarget->GetDesc(&d)) && d.Width == backbuffer_w_ &&
                      d.Height == backbuffer_h_;
    ++stats_.render_target_switches;
  }
  RecordedCall& c = Record(Cmd::kSetRenderTarget);
  c.a[0] = RenderTargetIndex;
  c.obj = record_.Hold(pRenderTarget);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTarget(DWORD RenderTargetIndex,
                                                      IDirect3DSurface9** ppRenderTarget) {
  if (!ppRenderTarget || RenderTargetIndex >= kModelTargets) return D3DERR_INVALIDCALL;
  *ppRenderTarget = game_.targets[RenderTargetIndex].get();
  if (!*ppRenderTarget) return D3DERR_NOTFOUND;
  (*ppRenderTarget)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
  game_.depth_stencil.Reset(pNewZStencil);
  RecordedCall& c = Record(Cmd::kSetDepthStencilSurface);
  c.obj = record_.Hold(pNewZStencil);
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
  if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
  *ppZStencilSurface = game_.depth_stencil.get();
  if (!*ppZStencilSurface) return D3DERR_NOTFOUND;
  (*ppZStencilSurface)->AddRef();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::BeginScene() {
  Live(Record(Cmd::kBeginScene));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::EndScene() {
  Live(Record(Cmd::kEndScene));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags,
                                            D3DCOLOR Color, float Z, DWORD Stencil) {
  ++stats_.clears;
  if (CaptureThisFrame()) {
    Trace("Clear(%lu rects, flags %lu, color %08lX)%s", Count, Flags, Color,
          Count && pRects ? "" : " full");
    for (DWORD i = 0; i < Count && pRects && i < 4; ++i) {
      Trace("  rect %ld,%ld-%ld,%ld", pRects[i].x1, pRects[i].y1, pRects[i].x2, pRects[i].y2);
    }
  }
  RecordedCall& c = Record(Cmd::kClear);
  c.a[0] = pRects ? Count : 0;
  c.a[1] = Flags;
  c.a[2] = Color;
  c.a[3] = Stencil;
  c.f = Z;
  if (pRects && Count) {
    c.data = record_.Store(pRects, Count * sizeof(D3DRECT));
    c.size = Count * sizeof(D3DRECT);
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface,
                                                  CONST RECT* pSourceRect,
                                                  IDirect3DSurface9* pDestSurface,
                                                  CONST RECT* pDestRect,
                                                  D3DTEXTUREFILTERTYPE Filter) {
  if (CaptureThisFrame()) {
    char src[96], dst[96], rs[48] = "all", rd[48] = "all";
    DescribeSurface(pSourceSurface, src, sizeof(src));
    DescribeSurface(pDestSurface, dst, sizeof(dst));
    if (pSourceRect) snprintf(rs, sizeof(rs), "%ld,%ld-%ld,%ld", pSourceRect->left, pSourceRect->top, pSourceRect->right, pSourceRect->bottom);
    if (pDestRect) snprintf(rd, sizeof(rd), "%ld,%ld-%ld,%ld", pDestRect->left, pDestRect->top, pDestRect->right, pDestRect->bottom);
    Trace("StretchRect(%s [%s] -> %s [%s])", src, rs, dst, rd);
  }
  RecordedCall& c = Record(Cmd::kStretchRect);
  c.obj = record_.Hold(pSourceSurface);
  c.obj2 = record_.Hold(pDestSurface);
  c.a[0] = static_cast<DWORD>(Filter);
  if (pSourceRect) {
    c.data = record_.Store(pSourceRect, sizeof(RECT));
    c.size = sizeof(RECT);
  }
  if (pDestRect) {
    c.data2 = record_.Store(pDestRect, sizeof(RECT));
    c.size2 = sizeof(RECT);
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect,
                                                D3DCOLOR color) {
  RecordedCall& c = Record(Cmd::kColorFill);
  c.obj = record_.Hold(pSurface);
  c.a[0] = color;
  if (pRect) {
    c.data = record_.Store(pRect, sizeof(RECT));
    c.size = sizeof(RECT);
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface,
                                                    CONST RECT* pSourceRect,
                                                    IDirect3DSurface9* pDestinationSurface,
                                                    CONST POINT* pDestPoint) {
  RecordedCall& c = Record(Cmd::kUpdateSurface);
  c.obj = record_.Hold(pSourceSurface);
  c.obj2 = record_.Hold(pDestinationSurface);
  if (pSourceRect) {
    c.data = record_.Store(pSourceRect, sizeof(RECT));
    c.size = sizeof(RECT);
  }
  if (pDestPoint) {
    c.data2 = record_.Store(pDestPoint, sizeof(POINT));
    c.size2 = sizeof(POINT);
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture,
                                                    IDirect3DBaseTexture9* pDestinationTexture) {
  RecordedCall& c = Record(Cmd::kUpdateTexture);
  c.obj = record_.Hold(pSourceTexture);
  c.obj2 = record_.Hold(pDestinationTexture);
  Live(c);
  return D3D_OK;
}

// -- resources (Direct3D9Ex has no managed pool) ----------------------------------

HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels,
                                                    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                    IDirect3DTexture9** ppTexture,
                                                    HANDLE* pSharedHandle) {
  if (d3d9ex_ && Pool == D3DPOOL_MANAGED) {
    // A dynamic default-pool texture can be locked like a managed one and
    // never gets lost on an Ex device.
    Pool = D3DPOOL_DEFAULT;
    if (!(Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) Usage |= D3DUSAGE_DYNAMIC;
  }
  const HRESULT hr =
      dev_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
  if (SUCCEEDED(hr) && ppTexture && *ppTexture && (Usage & D3DUSAGE_RENDERTARGET)) {
    render_target_textures_.insert(*ppTexture);
    Logf("CreateTexture %ux%u levels=%u usage=0x%X fmt=%d (render target)", Width, Height,
         Levels, Usage, static_cast<int>(Format));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF,
                                                         D3DPOOL Pool,
                                                         IDirect3DVertexBuffer9** ppVertexBuffer,
                                                         HANDLE* pSharedHandle) {
  if (d3d9ex_ && Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_DEFAULT;
  return dev_->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                                                        D3DPOOL Pool,
                                                        IDirect3DIndexBuffer9** ppIndexBuffer,
                                                        HANDLE* pSharedHandle) {
  if (d3d9ex_ && Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_DEFAULT;
  return dev_->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage,
                                                        D3DFORMAT Format, D3DPOOL Pool,
                                                        IDirect3DCubeTexture9** ppCubeTexture,
                                                        HANDLE* pSharedHandle) {
  if (d3d9ex_ && Pool == D3DPOOL_MANAGED) {
    Pool = D3DPOOL_DEFAULT;
    if (!(Usage & D3DUSAGE_RENDERTARGET)) Usage |= D3DUSAGE_DYNAMIC;
  }
  return dev_->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture,
                                 pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
    D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) {
  if (d3d9ex_ && Pool == D3DPOOL_MANAGED) {
    Pool = D3DPOOL_DEFAULT;
    Usage |= D3DUSAGE_DYNAMIC;
  }
  return dev_->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool,
                                   ppVolumeTexture, pSharedHandle);
}

// -- draws -------------------------------------------------------------------------

// Stream 0's vertices for a draw, from the vertex arena's CPU shadow (null if
// the game did not write them through the arena).
const uint8_t* D3D9Device::StreamVertices(UINT first_vertex, UINT count) {
  const ModelStream& s = game_.streams[0];
  if (!s.vb || !s.stride || !arena_->Redirected(s.vb.get())) return nullptr;
  return arena_->ShadowCopy(s.vb.get(), s.offset + first_vertex * s.stride, count * s.stride);
}

// Every stream bound to a game buffer in the vertex arena is bound at the
// region where the buffer's data is now; the recording gets the binding, so
// every eye reads the bytes the game wrote for this draw. The first draw of a
// frame always records its bindings (recorded_streams_ is cleared in
// BeginFrame), so a recording never depends on bindings of earlier frames.
void D3D9Device::BindArenaStreams() {
  for (UINT i = 0; i < kModelStreams; ++i) {
    const ModelStream& s = game_.streams[i];
    if (!s.vb || !arena_->Redirected(s.vb.get())) continue;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0;
    if (!arena_->Resolve(s.vb.get(), s.offset, &vb, &offset)) {
      static int reported = 0;
      if (reported++ < 10) {
        Logf("vertex arena: NO DATA for game vb %p (stream %u) at frame %llu",
             static_cast<void*>(s.vb.get()), i, frame_);
      }
      continue;
    }
    Binding& recorded = recorded_streams_[i];
    if (recorded.vb == vb && recorded.offset == offset && recorded.stride == s.stride) continue;
    recorded = {vb, offset, s.stride};
    RecordedCall& c = Record(Cmd::kSetStreamSource);
    RECORD_ARGS(c, i, offset, s.stride);
    c.obj = record_.Hold(vb);
    Live(c);
  }
}

// Verify mode: the bytes the draw's recorded arena binding points at must be
// what the game wrote. Every 8th frame (reading the arena back per draw is
// slow; checking every frame slowed menus to ~25 fps).
void D3D9Device::VerifyDrawVertices(UINT first_vertex, UINT vertices) {
  if (!verify_ || frame_ % 8 != 0) return;
  const int streams = game_.declaration ? kModelStreams : 1;
  for (int i = 0; i < streams; ++i) {
    const ModelStream& g = game_.streams[i];
    const Binding& d = recorded_streams_[i];
    if (!g.vb || !arena_->Redirected(g.vb.get()) || !g.stride) continue;
    const UINT bytes = vertices * g.stride;
    const long bad = arena_->VerifyRange(g.vb.get(), g.offset + first_vertex * g.stride, d.vb,
                                         d.offset + first_vertex * d.stride, bytes);
    ++verify_stats_.draws;
    if (bad < 0) {
      ++verify_stats_.unchecked;
    } else if (bad > 0) {
      if (verify_stats_.bad_draws++ < 20) {
        Logf("verify: WRONG vertex data: draw %u, stream %d, %ld of %u bytes differ (game vb %p "
             "offset %u, arena %p offset %u)", stats_.draws, i, bad, bytes,
             static_cast<void*>(g.vb.get()), g.offset + first_vertex * g.stride,
             static_cast<void*>(d.vb), d.offset + first_vertex * d.stride);
        Logf("verify:   frame %llu, game vb %s", frame_, arena_->Describe(g.vb.get()).c_str());
      }
      verify_stats_.bad_bytes += static_cast<uint64_t>(bad);
    }
  }
}

// The game draws its full-screen layers as small fixed-function meshes with
// the perspective projection still set: the post-effect composite back onto
// the backbuffer, the pause menu's dark backdrop, the title screen's fades.
// Moved per eye they would no longer cover the screen (strips of the raw
// scene or black boxes at the edges; F2 captures 2026-09-17), so they are
// placed on the window plane (or kept screen-aligned if they sample a render
// target, which already holds the eye's image).
// Test: at most 8 XYZ vertices, all in front of the camera, together spanning
// the whole screen in both directions (NDC -0.98..0.98). Spanning one
// direction was tried for the 16:9 language menu video and tore that menu
// apart: its other layers sit at the same depth and must move together.
bool D3D9Device::IsScreenQuad(const void* vertices, UINT stride, UINT count, float* quad_w,
                              bool* copy) {
  if (!vertices || count < 3 || count > 8 || stride < 12 || game_.vertex_shader ||
      game_.declaration) {
    return false;
  }
  if ((game_.fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ || !IsPerspective(game_.projection)) {
    return false;
  }
  const D3DMATRIX m = Multiply(Multiply(game_.world, game_.view), game_.projection);
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  float w_min = 1e9f, w_max = -1e9f;
  const auto* bytes = static_cast<const BYTE*>(vertices);
  for (UINT i = 0; i < count; ++i) {
    float p[3];
    std::memcpy(p, bytes + i * stride, sizeof(p));
    const float w = p[0] * m._14 + p[1] * m._24 + p[2] * m._34 + m._44;
    if (w <= 1e-6f) {
      if (CaptureThisFrame()) Trace("  small draw, %u vertices: vertex %u behind the camera (w %.3f)", count, i, w);
      return false;
    }
    w_min = (std::min)(w_min, w);
    w_max = (std::max)(w_max, w);
    const float x = (p[0] * m._11 + p[1] * m._21 + p[2] * m._31 + m._41) / w;
    const float y = (p[0] * m._12 + p[1] * m._22 + p[2] * m._32 + m._42) / w;
    x0 = (std::min)(x0, x);
    x1 = (std::max)(x1, x);
    y0 = (std::min)(y0, y);
    y1 = (std::max)(y1, y);
  }
  const bool full = x0 <= -0.98f && x1 >= 0.98f && y0 <= -0.98f && y1 >= 0.98f;
  *quad_w = 0.5f * (w_min + w_max);
  IDirect3DBaseTexture9* tex0 = game_.textures[0].get();
  *copy = tex0 && render_target_textures_.count(tex0) != 0;
  if (CaptureThisFrame()) {
    Trace("  small draw, %u vertices: NDC x %.2f..%.2f y %.2f..%.2f -> %s", count, x0, x1, y0, y1,
          full ? "screen quad (no eye offset)" : "scene");
  }
  return full;
}

// Screen-quad decision and, for pre-transformed draws onto the backbuffer, a
// copy of the vertices (see the renderer's window layers).
void D3D9Device::RecordDraw(RecordedCall& call, const void* vertices, UINT stride, UINT count) {
  float quad_w = 1.0f;
  bool copy = false;
  const bool quad = count <= 8 && IsScreenQuad(vertices, stride, count, &quad_w, &copy);
  call.quad = quad ? static_cast<uint8_t>(1 | (copy ? 2 : 0)) : 0;
  call.quad_w = quad_w;
}

void D3D9Device::CountDraw() {
  ++stats_.draws;
  ++trace_draws_;
  if (game_.vertex_shader) ++stats_.draws_shader;
  if (CaptureThisFrame()) {
    // One line per draw: where the object sits in view space (WORLD * VIEW
    // origin, +z forward, game units), its scale and the depth/blend state.
    const D3DMATRIX wv = Multiply(game_.world, game_.view);
    const float scale = std::sqrt(wv._11 * wv._11 + wv._12 * wv._12 + wv._13 * wv._13);
    const D3DMATRIX& p = game_.projection;
    Logf("trace:   draw %u: %s fvf 0x%lx origin %.3f %.3f %.3f scale %.3f z %lu/%lu/%lu "
         "blend %lu tex0 %p proj _11 %.3f _43 %.3f tci0 0x%lx", stats_.draws,
         game_.vertex_shader ? "shader" : "fixed", game_.fvf, wv._41, wv._42, wv._43, scale,
         game_.rs[D3DRS_ZENABLE], game_.rs[D3DRS_ZWRITEENABLE], game_.rs[D3DRS_ZFUNC],
         game_.rs[D3DRS_ALPHABLENDENABLE], static_cast<void*>(game_.textures[0].get()), p._11,
         p._43, game_.tss[0][D3DTSS_TEXCOORDINDEX]);
    if (!IsPerspective(p)) {
      Logf("trace:     non-perspective PROJECTION rows: %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | "
           "%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g; viewport %lu,%lu %lux%lu",
           p._11, p._12, p._13, p._14, p._21, p._22, p._23, p._24, p._31, p._32, p._33, p._34,
           p._41, p._42, p._43, p._44, game_.viewport.X, game_.viewport.Y,
           game_.viewport.Width, game_.viewport.Height);
    }
  }
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                    UINT StartVertex, UINT PrimitiveCount) {
  CountDraw();
  const UINT count = VertexCountOf(PrimitiveType, PrimitiveCount);
  BindArenaStreams();
  VerifyDrawVertices(StartVertex, count);
  const bool rhw = (game_.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && !game_.declaration &&
                   !game_.vertex_shader;
  const uint8_t* vertices = (count <= 8 || (rhw && rt0_backbuffer_)) && count > 0
                                ? StreamVertices(StartVertex, count)
                                : nullptr;
  RecordedCall& c = Record(Cmd::kDrawPrimitive);
  RECORD_ARGS(c, static_cast<DWORD>(PrimitiveType), StartVertex, PrimitiveCount);
  if (PrimitiveCount <= 2) RecordDraw(c, vertices, game_.streams[0].stride, count);
  if (rhw && rt0_backbuffer_ && vertices) {
    const UINT bytes = count * game_.streams[0].stride;
    c.data = record_.Store(vertices, bytes);
    c.size = bytes;
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                           INT BaseVertexIndex,
                                                           UINT MinVertexIndex, UINT NumVertices,
                                                           UINT startIndex, UINT primCount) {
  CountDraw();
  BindArenaStreams();
  const UINT first = static_cast<UINT>(BaseVertexIndex + static_cast<INT>(MinVertexIndex));
  VerifyDrawVertices(first, NumVertices);
  RecordedCall& c = Record(Cmd::kDrawIndexedPrimitive);
  RECORD_ARGS(c, static_cast<DWORD>(PrimitiveType), static_cast<DWORD>(BaseVertexIndex),
              MinVertexIndex, NumVertices, startIndex, primCount);
  if (primCount <= 2 && NumVertices <= 8) {
    RecordDraw(c, StreamVertices(first, NumVertices), game_.streams[0].stride, NumVertices);
  }
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                                      UINT PrimitiveCount,
                                                      CONST void* pVertexStreamZeroData,
                                                      UINT VertexStreamZeroStride) {
  if (!pVertexStreamZeroData) return D3DERR_INVALIDCALL;
  CountDraw();
  const UINT count = VertexCountOf(PrimitiveType, PrimitiveCount);
  RecordedCall& c = Record(Cmd::kDrawPrimitiveUP);
  RECORD_ARGS(c, static_cast<DWORD>(PrimitiveType), PrimitiveCount, VertexStreamZeroStride);
  const size_t bytes = static_cast<size_t>(count) * VertexStreamZeroStride;
  c.data = record_.Store(pVertexStreamZeroData, bytes);
  c.size = static_cast<uint32_t>(bytes);
  if (PrimitiveCount <= 2) RecordDraw(c, pVertexStreamZeroData, VertexStreamZeroStride, count);
  // Direct3D 9 unbinds stream 0 after an UP draw.
  game_.streams[0] = ModelStream();
  recorded_streams_[0] = Binding();
  Live(c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData,
    UINT VertexStreamZeroStride) {
  if (!pVertexStreamZeroData || !pIndexData) return D3DERR_INVALIDCALL;
  CountDraw();
  const size_t index_bytes = static_cast<size_t>(VertexCountOf(PrimitiveType, PrimitiveCount)) *
                             (IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2);
  // Vertices are referenced from index 0 up to min_index + vertices.
  const size_t vertex_bytes = static_cast<size_t>(MinVertexIndex + NumVertices) * VertexStreamZeroStride;
  RecordedCall& c = Record(Cmd::kDrawIndexedPrimitiveUP);
  RECORD_ARGS(c, static_cast<DWORD>(PrimitiveType), MinVertexIndex, NumVertices, PrimitiveCount,
              static_cast<DWORD>(IndexDataFormat), VertexStreamZeroStride);
  c.data = record_.Store(pVertexStreamZeroData, vertex_bytes);
  c.size = static_cast<uint32_t>(vertex_bytes);
  c.data2 = record_.Store(pIndexData, index_bytes);
  c.size2 = static_cast<uint32_t>(index_bytes);
  if (PrimitiveCount <= 2 && NumVertices <= 8) {
    RecordDraw(c, static_cast<const BYTE*>(pVertexStreamZeroData) + MinVertexIndex * VertexStreamZeroStride,
               VertexStreamZeroStride, NumVertices);
  }
  game_.streams[0] = ModelStream();
  game_.indices.Reset();
  recorded_streams_[0] = Binding();
  Live(c);
  return D3D_OK;
}

// -- capture traces ------------------------------------------------------------------

// Capture frames only: one "trace:" line per event, with the draws since the
// previous event. Shows the frame's pass structure (targets, viewports,
// copies, clears) for reverse engineering.
void D3D9Device::Trace(const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (trace_draws_) Logf("trace:   %u draws", trace_draws_);
  trace_draws_ = 0;
  Logf("trace: %s", text);
}

void D3D9Device::DescribeSurface(IDirect3DSurface9* surface, char* out, size_t size) {
  D3DSURFACE_DESC d{};
  if (!surface || FAILED(surface->GetDesc(&d))) {
    snprintf(out, size, "%p", static_cast<void*>(surface));
    return;
  }
  IDirect3DSurface9* bb = nullptr;
  bool is_bb = false;
  if (SUCCEEDED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    is_bb = bb == surface;
    bb->Release();
  }
  snprintf(out, size, "%s%p %ux%u fmt%d ms%d", is_bb ? "BACKBUFFER " : "",
           static_cast<void*>(surface), d.Width, d.Height, static_cast<int>(d.Format),
           static_cast<int>(d.MultiSampleType));
}

}  // namespace kkvr
