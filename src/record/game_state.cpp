#include "record/game_state.h"

namespace kkvr {

const D3DMATRIX& IdentityMatrix() {
  static const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  return identity;
}

GameState::GameState() {
  world = view = projection = IdentityMatrix();
  for (D3DMATRIX& m : texture_matrix) m = IdentityMatrix();
}

void GameState::ReadFromDevice(IDirect3DDevice9* dev) {
  Clear();
  for (DWORD i = 0; i < kModelRenderStates; ++i) {
    rs_valid[i] = SUCCEEDED(dev->GetRenderState(static_cast<D3DRENDERSTATETYPE>(i), &rs[i]));
  }
  for (DWORD stage = 0; stage < kModelStages; ++stage) {
    for (DWORD type = 1; type < kModelStageStates; ++type) {
      dev->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type),
                                &tss[stage][type]);
    }
    dev->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage),
                      &texture_matrix[stage]);
  }
  for (int slot = 0; slot < kModelSamplers; ++slot) {
    const DWORD sampler = SamplerNumber(slot);
    for (DWORD type = 1; type < kModelSamplerStates; ++type) {
      dev->GetSamplerState(sampler, static_cast<D3DSAMPLERSTATETYPE>(type), &ss[slot][type]);
    }
  }
  dev->GetTransform(D3DTS_WORLD, &world);
  dev->GetTransform(D3DTS_VIEW, &view);
  dev->GetTransform(D3DTS_PROJECTION, &projection);
  dev->GetFVF(&fvf);
  dev->GetVertexShaderConstantF(0, &vs_float[0][0], kModelVsConstF);
  dev->GetViewport(&viewport);
  dev->GetScissorRect(&scissor);
  dev->GetMaterial(&material);
  for (int i = 0; i < kModelTargets; ++i) {
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(i, &surface)) && surface) {
      targets[i].Reset(surface);
      surface->Release();
    }
  }
  IDirect3DSurface9* ds = nullptr;
  if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) {
    depth_stencil.Reset(ds);
    ds->Release();
  }
}

void GameState::Clear() {
  for (auto& t : textures) t.Reset();
  declaration.Reset();
  vertex_shader.Reset();
  pixel_shader.Reset();
  for (ModelStream& s : streams) s = ModelStream();
  indices.Reset();
  for (auto& t : targets) t.Reset();
  depth_stencil.Reset();
  lights.clear();
  extra_world.clear();
}

void GameState::ResetViewportTo(IDirect3DSurface9* target) {
  D3DSURFACE_DESC d{};
  if (!target || FAILED(target->GetDesc(&d))) return;
  viewport = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
  scissor = {0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
}

}  // namespace kkvr
