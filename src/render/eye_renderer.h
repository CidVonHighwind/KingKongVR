// Renders one eye from a recorded frame.
//
// The EyeRenderer is the only code that sets render state on the device for
// the game's frame. It starts every eye from the frame's recorded start state
// (game_state.h), executes the recorded calls (frame_record.h) and applies the
// eye's view where the game's image depends on the camera:
//   - scene draws: the eye's camera and field of view seen through the window
//     (eye_projection.h) go into PROJECTION, so camera space - and everything
//     the pipeline generates there: texture coordinates for the characters'
//     irises, lighting, fog - stays the game's; shader draws get c0..c3 =
//     W*V*P rebuilt, from a world matrix recovered from the constants;
//   - shots with a long lens (cinematics) are moved onto the window gradually
//     and shown smaller ([vr] cinematic_fov, cinematic_scale);
//   - each eye has its own copies of the game's render-target textures: its
//     post effects carry content from one render into the next;
//   - screen-filling quads (post-effect composites, fades): kept on the window
//     plane or screen-aligned (decided when the frame was recorded);
//   - 2D layers (orthographic projections, pre-transformed XYZRHW vertices):
//     placed on the window plane with the game's own depth.
// The eyes render at their own resolution (SetTargetSize: the headset pixels
// the window covers, eye_projection.h), not the game's. Every target the size
// of the backbuffer (the backbuffer, its depth buffer, the post-effect
// textures) is replaced by a copy of that size the renderer owns, and every
// value in pixels (viewports, scissor, Clear, StretchRect and ColorFill
// rectangles, pre-transformed vertex positions) is scaled on its way to the
// device, horizontally and vertically by their own factor. Matrices need no
// change: clip space does not know the target size. Smaller targets (128x128
// shadow maps) stay as they are.
// A device-state cache skips calls that set what is already set (the engine
// re-sends most of its state for every draw). The cache is filled by a full
// apply of the start state at the beginning of every eye, so nothing done to
// the device between eyes (headset copies, desktop view) can leak in.
//
// The renderer never calls the proxy's hooks: a recording is executed straight
// on the real device.
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "record/game_state.h"
#include "record/frame_record.h"
#include "xr/xr.h"

namespace kkvr {

class VertexArena;

// How an eye is rendered.
enum class EyeMode {
  kWindow,           // the eye's view through the window (the mod)
  kFlat,             // [test] flat: the game's own camera (ground truth)
  kWindowReference,  // projection check: the window rectangle, off-axis
};

// Everything about one eye that the recording does not know.
struct EyeContext {
  XrPresenter::EyeView view;       // headset view in the window frame
  float render_tangents[4] = {-1, 1, 1, -1};  // frustum to render (window bounds)
  EyeMode mode = EyeMode::kWindow;
  float window_width = 1.0f;       // metres
  float window_height = 1.0f;      // metres (the game's aspect)
  float world_scale = 1.0f;        // game units per metre
  UINT backbuffer_width = 0;       // the game's resolution (the scaled copies
  UINT backbuffer_height = 0;      // replace targets of this size)
  int aspect_fix = 0;              // [display] aspect_fix
  bool aspect_patched = false;     // PatchScreenAspect succeeded: no fallback
  int index = 0;                   // which eye (its own copies of the game's targets)
  bool share_targets = false;      // [test] share_eye_targets: one set for both
  int skip_from = 0;               // skip draws skip_from+1 .. skip_from+skip_count
  int skip_count = 0;              // 0 = skip nothing (the frame stays complete)
  bool hide_letterbox = true;      // [render] hide_letterbox: drop cinematic bars
  float cinematic_fov = 45.0f;     // [vr] cinematic_fov: fully flattened below this (0 = off)
  float cinematic_scale = 0.6f;    // [vr] cinematic_scale: its size on the window
};

class EyeRenderer {
 public:
  EyeRenderer(IDirect3DDevice9* device, VertexArena* arena,
              const std::unordered_set<IDirect3DBaseTexture9*>* render_target_textures);

  // The size the eyes are rendered at (0 = the game's). Changing it, or the
  // game's resolution (EyeContext, after a Reset), drops the copies of the
  // game's targets, so they are created again at the new size.
  void SetTargetSize(UINT width, UINT height);

  // Frame capture with [test] dump_passes: called with every render target the
  // frame finishes with (pass number, draws in it, the surface as rendered), so
  // the pass where an artefact appears can be found. Empty = off.
  using PassDump = std::function<void(int pass, int draws, IDirect3DSurface9* target)>;
  void SetPassDump(PassDump dump) { pass_dump_ = std::move(dump); }

  // Render a whole recorded frame for one eye (Begin, Execute each call, End).
  void Render(const FrameRecord& record, const GameState& start, const EyeContext& eye);

  // Step by step (the live reference executes calls while they are recorded).
  void Begin(const GameState& start, const EyeContext& eye);
  void Execute(const RecordedCall& call, const FrameRecord& record);
  void End();

  // Drop references held by the current state and the scaled targets (before
  // Reset; the game recreates its targets after it).
  void Release();

  // Where the renderer really draws what the game draws into `game_surface`
  // (its scaled copy, or the surface itself). Not AddRef'd.
  IDirect3DSurface9* DeviceSurface(IDirect3DSurface9* game_surface);
  // Draws in the last rendered eye (the bisect's upper end).
  int draws_done() const { return draws_done_; }
  // Size of the eye images.
  UINT output_width() const { return target_width_ ? target_width_ : backbuffer_width_; }
  UINT output_height() const { return target_height_ ? target_height_ : backbuffer_height_; }

  // Indexed pre-transformed draws into scaled targets (positions not scaled;
  // the game has none, census 2026-09-17).
  uint64_t TakeUnscaledRhwDraws() {
    const uint64_t n = unscaled_rhw_draws_;
    unscaled_rhw_draws_ = 0;
    return n;
  }

  uint64_t TakeLetterboxDraws() {
    const uint64_t n = letterbox_draws_;
    letterbox_draws_ = 0;
    return n;
  }

  uint64_t TakeWindowLayerDraws() {
    const uint64_t n = window_layer_draws_;
    window_layer_draws_ = 0;
    return n;
  }

 private:
  // -- device cache (values on the device) --
  void ApplyAll(const GameState& s);
  void DevRenderState(DWORD state, DWORD value);
  void DevStageState(DWORD stage, DWORD type, DWORD value);
  void DevSamplerState(int slot, DWORD type, DWORD value);
  void DevTexture(int slot, IDirect3DBaseTexture9* texture);
  void DevTransform(DWORD state, const D3DMATRIX& m);
  void DevFvf(DWORD fvf);
  void DevDeclaration(IDirect3DVertexDeclaration9* declaration);
  void DevVertexShader(IDirect3DVertexShader9* shader);
  void DevPixelShader(IDirect3DPixelShader9* shader);
  void DevVsFloat(UINT start, const float* data, UINT count);
  void DevStream(UINT index, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
  void DevIndices(IDirect3DIndexBuffer9* indices);
  void DevViewport(const D3DVIEWPORT9& vp);
  void DevScissor(const RECT& r);
  void DevTarget(DWORD index, IDirect3DSurface9* target);
  void DevDepthStencil(IDirect3DSurface9* surface);
  void RestoreVertexFormat();  // the state's FVF or declaration

  // -- per draw --
  void PrepareDraw(const RecordedCall& draw);
  void FinishDraw();
  // The window plane that 2D layers are placed on (game view units: half
  // width, half height, distance) and the eye's view and projection of it.
  struct WindowLayer {
    float hw, hh, plane;
    D3DMATRIX view, projection;
  };
  WindowLayer WindowLayerTransform() const;
  bool IsWindowLayerDraw() const;
  bool DrawWindowLayer(D3DPRIMITIVETYPE type, UINT primitives, const uint8_t* vertices,
                       UINT stride);
  bool TargetIsBackbuffer(IDirect3DSurface9* target) const;

  // -- render scale --
  void UpdateScale();  // from eye_ and the target size, at the start of every eye
  UINT ScaledX(UINT v) const { return static_cast<UINT>(v * scale_x_ + 0.5f); }
  UINT ScaledY(UINT v) const { return static_cast<UINT>(v * scale_y_ + 0.5f); }
  IDirect3DBaseTexture9* DeviceTexture(IDirect3DBaseTexture9* game_texture);
  D3DVIEWPORT9 DeviceViewport(const D3DVIEWPORT9& v) const;
  RECT DeviceRect(const RECT& r) const;
  bool SkipDraw();     // [test] debug_keys: the hidden draw range
  bool IsLetterboxRect(const RECT& r) const;  // [render] hide_letterbox
  bool IsLetterboxDraw(D3DPRIMITIVETYPE type, UINT primitives, const uint8_t* vertices,
                       UINT stride);
  bool IsRhw() const;  // pre-transformed vertices (fixed function)
  void DrawScaledRhw(D3DPRIMITIVETYPE type, UINT primitives, const uint8_t* vertices, UINT stride);

  IDirect3DDevice9* dev_;
  VertexArena* arena_;
  const std::unordered_set<IDirect3DBaseTexture9*>* rt_textures_;
  UINT backbuffer_width_ = 0, backbuffer_height_ = 0;  // the game's, from eye_ (Begin)
  UINT target_width_ = 0, target_height_ = 0;  // the size the eyes render at (0: the game's)
  float scale_x_ = 1.0f, scale_y_ = 1.0f;  // target / backbuffer
  bool scaling_ = false;                // the target size differs from the game's
  // Scaled copies, keyed by the game's object (held, so its address cannot be
  // reused by another object; null: the object has no scaled copy). The game
  // creates its targets once per device (census 2026-09-17).
  struct ScaledSurface {
    ComRef<IDirect3DSurface9> game, scaled;
  };
  struct ScaledTexture {
    ComRef<IDirect3DBaseTexture9> game, scaled;
  };
  // Per eye: the game's post effects carry content from one render into the
  // next (motion blur, glow), so sharing its render-target textures between
  // the eyes let each read what the other left - a ghost of the other eye's
  // picture, offset (2026-09-18). Eye 0 uses the game's own textures unless
  // the render scale needs copies anyway.
  std::unordered_map<IDirect3DSurface9*, ScaledSurface> scaled_surfaces_[2];
  std::unordered_map<IDirect3DBaseTexture9*, ScaledTexture> scaled_textures_[2];
  bool target_scaled_ = false;  // render target 0 on the device is a scaled copy
  std::vector<uint8_t> rhw_bytes_;
  EyeContext eye_;
  GameState state_;  // the game's state as executed so far

  // Device values (valid after ApplyAll). Viewport, scissor and surfaces are
  // the device's (scaled) ones.
  DWORD dev_rs_[kModelRenderStates] = {};
  DWORD dev_tss_[kModelStages][kModelStageStates] = {};
  DWORD dev_ss_[kModelSamplers][kModelSamplerStates] = {};
  IDirect3DBaseTexture9* dev_textures_[kModelSamplers] = {};
  D3DMATRIX dev_world_{}, dev_view_{}, dev_projection_{};
  D3DMATRIX dev_texture_matrix_[kModelStages] = {};
  DWORD dev_fvf_ = 0;
  IDirect3DVertexDeclaration9* dev_declaration_ = nullptr;
  IDirect3DVertexShader9* dev_vs_ = nullptr;
  IDirect3DPixelShader9* dev_ps_ = nullptr;
  float dev_vs_float_[kModelVsConstF][4] = {};
  struct DevStreamBinding {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
  } dev_streams_[kModelStreams];
  IDirect3DIndexBuffer9* dev_indices_ = nullptr;
  D3DVIEWPORT9 dev_viewport_{};
  RECT dev_scissor_{};
  IDirect3DSurface9* dev_targets_[kModelTargets] = {};
  IDirect3DSurface9* dev_depth_ = nullptr;

  bool rt0_backbuffer_ = false;
  bool layer_depth_ = false;  // viewport depth range pinned for this draw
  std::vector<uint8_t> layer_bytes_;
  uint64_t window_layer_draws_ = 0;
  int draws_done_ = 0;  // draws of this eye so far (the hidden draw range)
  PassDump pass_dump_;
  int pass_ = 0;
  int pass_draws_ = 0;
  void FinishPass();  // hand the current target to pass_dump_
  uint64_t unscaled_rhw_draws_ = 0;
  uint64_t letterbox_draws_ = 0;
};

}  // namespace kkvr
