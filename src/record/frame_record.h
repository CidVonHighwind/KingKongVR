// Frame recording: the game's Direct3D commands of one frame, rendered later
// once per eye.
//
// Why: the game renders a frame once, for one camera. A window into the game
// world needs it from each eye's position. The proxy records every command
// (recording.cpp) instead of executing it, and at Present EyeRenderer renders
// the recording for each eye from the frame's start state (game_state.h).
//
// What makes King Kong's frames recordable (measured with a call census):
//   - its frames never read GPU results back (no queries, no
//     GetRenderTargetData, no read locks) and change no textures;
//   - all geometry is streamed through one dynamic, write-only vertex buffer,
//     about 850 locks per frame. Those locks are redirected into an
//     append-only arena (vertex_arena.h), so every recorded draw still finds
//     its vertices when the eyes are rendered.
// Everything else is state: recorded as values, with references held on the
// resources a command uses until the record is dropped.
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace kkvr {

enum class Cmd : uint8_t {
  kSetRenderState, kSetSamplerState, kSetTextureStageState, kSetTexture,
  kSetTransform, kSetFVF, kSetStreamSource, kSetVertexDeclaration, kSetIndices,
  kSetVertexShader, kSetPixelShader, kSetVertexShaderConstantF,
  kSetPixelShaderConstantF, kSetVertexShaderConstantI, kSetVertexShaderConstantB,
  kSetPixelShaderConstantI, kSetPixelShaderConstantB, kSetLight, kLightEnable,
  kSetMaterial, kSetViewport, kSetScissorRect, kSetRenderTarget,
  kSetDepthStencilSurface, kClear, kStretchRect, kColorFill, kBeginScene, kEndScene,
  kSetClipPlane, kDrawPrimitive, kDrawIndexedPrimitive, kDrawPrimitiveUP,
  kDrawIndexedPrimitiveUP, kSetStreamSourceFreq, kSetNPatchMode,
  kSetSoftwareVertexProcessing, kSetCurrentTexturePalette, kUpdateSurface,
  kUpdateTexture,
};

// One recorded call. Scalars in a[], one COM object in obj/obj2 (held with a
// reference), variable data (matrices, constants, vertex bytes, rects) in the
// record's byte arena at [data, data + size).
struct RecordedCall {
  Cmd cmd;
  DWORD a[6] = {};
  float f = 0.0f;
  // Draws: screen-quad decision of the live frame (bit 0 quad, bit 1 copies a
  // render target) and the quad's clip w.
  uint8_t quad = 0;
  float quad_w = 1.0f;
  IUnknown* obj = nullptr;
  IUnknown* obj2 = nullptr;
  uint32_t data = 0;
  uint32_t size = 0;
  uint32_t data2 = 0;  // second blob (StretchRect dest rect, indexed UP indices)
  uint32_t size2 = 0;
};

class FrameRecord {
 public:
  FrameRecord() = default;
  FrameRecord(const FrameRecord&) = delete;
  FrameRecord& operator=(const FrameRecord&) = delete;
  ~FrameRecord() { Clear(); }

  RecordedCall& Add(Cmd cmd) {
    ++epoch_;  // any other command ends the run of state sets before it
    calls_.emplace_back();
    calls_.back().cmd = cmd;
    return calls_.back();
  }

  // State sets are coalesced: setting the same state `key` again before any
  // other command reuses the earlier call (the caller overwrites its value),
  // so only the last value before a draw is replayed. Independent states in
  // one run commute, so their order does not matter.
  static constexpr int kStateKeys = 1024;
  RecordedCall& AddState(Cmd cmd, int key) {
    if (key >= 0 && key < kStateKeys && pending_epoch_[key] == epoch_) {
      return calls_[pending_index_[key]];
    }
    calls_.emplace_back();
    calls_.back().cmd = cmd;
    if (key >= 0 && key < kStateKeys) {
      pending_epoch_[key] = epoch_;
      pending_index_[key] = static_cast<uint32_t>(calls_.size() - 1);
    }
    return calls_.back();
  }
  // Keep `object` alive until Clear (null is fine).
  IUnknown* Hold(IUnknown* object) {
    if (object) {
      object->AddRef();
      held_.push_back(object);
    }
    return object;
  }
  // Copy `size` bytes into the arena; returns the offset.
  uint32_t Store(const void* bytes, size_t size) {
    const uint32_t offset = static_cast<uint32_t>(arena_.size());
    if (size) {
      arena_.resize(arena_.size() + size);
      std::memcpy(arena_.data() + offset, bytes, size);
    }
    return offset;
  }
  const uint8_t* Data(uint32_t offset) const { return arena_.data() + offset; }

  void Clear() {
    ++epoch_;
    for (IUnknown* object : held_) object->Release();
    held_.clear();
    calls_.clear();
    arena_.clear();
  }

  const std::vector<RecordedCall>& calls() const { return calls_; }
  size_t arena_bytes() const { return arena_.size(); }

 private:
  std::vector<RecordedCall> calls_;
  std::vector<IUnknown*> held_;
  std::vector<uint8_t> arena_;
  uint32_t epoch_ = 1;
  uint32_t pending_epoch_[kStateKeys] = {};
  uint32_t pending_index_[kStateKeys] = {};
};

}  // namespace kkvr
