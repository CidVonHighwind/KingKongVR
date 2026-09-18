// Vertex arena: the game's dynamic vertex data, written once, kept until the
// eyes are rendered.
//
// Jade streams all geometry through a dynamic write-only vertex buffer: lock,
// write this draw's vertices, draw, and again for the next draw, overwriting
// what the previous draw used (census: ~850 locks per frame, up to 2,000 in
// heavy scenes). The eyes are rendered from the recording at Present and need
// every draw's vertices still intact. The first version kept a CPU mirror and
// copied each draw's bytes into the recording and then into an upload buffer:
// three copies of ~14 MB per heavy frame, about 4 ms of CPU.
//
// The arena removes most of the copies. Locks of the game's dynamic vertex
// buffers (hooked in lock_hooks.cpp) are redirected into one large dynamic
// buffer that is only appended to during a frame:
//   - Each game buffer owns a region of its size in the arena. A DISCARD lock
//     (or the first lock in a new arena generation) gives it a fresh region at
//     the end; NOOVERWRITE locks write into its current region.
//   - The game writes into a CPU shadow of its buffer (the lock returns the
//     shadow's pointer); Unlock copies the written bytes into the arena.
//   - Draws bind the arena at the region's offset (D3D9Device::BindArenaStreams),
//     and the recording records that binding. Every eye therefore reads the
//     very bytes the game wrote for that draw.
//   - The arena wraps (DISCARD) only at a frame boundary, after the frame's
//     eyes; if a frame outgrows it, a larger arena replaces it and the old one
//     stays alive through the recording's references.
// Data that outlives a wrap: the game may draw vertices it wrote frames ago
// without locking again (its post-effect quads, a 6.5 KB buffer written once
// per level). When a draw needs a buffer whose region was wrapped away,
// Resolve uploads the shadowed ranges into a new region.
// The shadow also serves the screen-quad test, which needs vertices.
#pragma once

#include <d3d9.h>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "device/lock_hooks.h"

namespace kkvr {

class VertexArena final : public VertexBufferRedirect {
 public:
  // verify: the arena is readable and every write is also kept in a full CPU
  // mirror per game buffer, for VerifyRange.
  VertexArena(IDirect3DDevice9* device, bool verify);
  ~VertexArena() override;
  VertexArena(const VertexArena&) = delete;
  VertexArena& operator=(const VertexArena&) = delete;

  // VertexBufferRedirect (called from the vtable hooks).
  bool Lock(IDirect3DVertexBuffer9* vb, const D3DVERTEXBUFFER_DESC* desc, UINT offset, UINT size,
            void** data, DWORD flags, HRESULT* result) override;
  bool Unlock(IDirect3DVertexBuffer9* vb, HRESULT* result) override;

  // Is `vb` a game buffer that lives in the arena?
  bool Redirected(IDirect3DVertexBuffer9* vb) const;
  // Where byte `offset` of game buffer `vb` is now. If its region was
  // wrapped away, its shadowed data is uploaded to a new region first. False
  // if not redirected or nothing to bind.
  bool Resolve(IDirect3DVertexBuffer9* vb, UINT offset, IDirect3DVertexBuffer9** arena,
               UINT* arena_offset);
  // CPU copy of `bytes` bytes at `offset` of game buffer `vb`, if a shadowed
  // write covered that range since its last DISCARD; null otherwise.
  const uint8_t* ShadowCopy(IDirect3DVertexBuffer9* vb, UINT offset, UINT bytes) const;

  // Verify mode: do `bytes` bytes at `arena_offset` of `arena` hold what the
  // game wrote at `game_offset` of `game`? Returns the number of differing
  // bytes (0 = correct), or -1 if the range could not be checked.
  long VerifyRange(IDirect3DVertexBuffer9* game, UINT game_offset,
                   IDirect3DVertexBuffer9* arena, UINT arena_offset, UINT bytes);

  // Verify mode diagnostics: the buffer's region and its last locks.
  std::string Describe(IDirect3DVertexBuffer9* vb) const;

  // Frame boundary (after the eyes): wrap if more than half is used.
  void BeginFrame();
  // Before Reset / destruction.
  void Release();

  struct Stats {
    uint64_t locks = 0, bytes = 0, grows = 0, wraps = 0, restores = 0;
    UINT capacity = 0, peak = 0;
  };
  Stats TakeStats();

 private:
  struct Region {
    UINT base = 0;
    UINT size = 0;
    uint32_t generation = 0;
  };
  struct Shadow {
    std::vector<uint8_t> bytes;                    // buffer size once used
    // Written ranges begin -> end, disjoint and non-adjacent (merged). A
    // sorted map: ~850 writes per frame made the old vector rebuild per write
    // quadratic (gameplay frames 15-20 ms once every write was shadowed).
    std::map<UINT, UINT> ranges;
  };
  bool Allocate(Region* region, UINT size);
  // Drop everything known about `vb` (a new buffer now has its address).
  void Forget(IDirect3DVertexBuffer9* vb);

  IDirect3DDevice9* dev_;
  IDirect3DVertexBuffer9* vb_ = nullptr;
  UINT capacity_ = 0;
  UINT append_ = 0;
  uint32_t generation_ = 1;
  bool discard_next_ = true;
  std::unordered_map<IDirect3DVertexBuffer9*, Region> regions_;
  std::unordered_map<IDirect3DVertexBuffer9*, UINT> sizes_;  // desc cache: 0 = not dynamic
  // A game lock not yet unlocked: the shadow is copied into the arena at Unlock.
  struct Pending {
    IDirect3DVertexBuffer9* game;
    IDirect3DVertexBuffer9* arena;  // the arena locked for it (a reference: it may grow meanwhile)
    uint8_t* pointer;  // arena memory to fill at Unlock
    UINT offset;  // in the game buffer
    UINT locked;  // bytes locked
  };
  std::vector<Pending> pending_;
  std::unordered_map<IDirect3DVertexBuffer9*, Shadow> shadows_;
  Stats stats_;
  bool failed_ = false;
  bool verify_ = false;
  std::unordered_map<IDirect3DVertexBuffer9*, std::vector<uint8_t>> mirrors_;  // verify only
  struct LockRecord {
    UINT offset, size;
    DWORD flags;
    uint32_t generation;
    UINT arena_base;
  };
  std::unordered_map<IDirect3DVertexBuffer9*, std::vector<LockRecord>> lock_history_;  // verify only
};

}  // namespace kkvr
