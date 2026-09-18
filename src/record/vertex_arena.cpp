#include "record/vertex_arena.h"

#include "common/log.h"

#include <algorithm>
#include <cstring>

namespace kkvr {
namespace {

constexpr UINT kInitialCapacity = 32u << 20;  // heavy KK frames use ~16 MB

UINT Align4(UINT v) { return (v + 3u) & ~3u; }

}  // namespace

VertexArena::VertexArena(IDirect3DDevice9* device, bool verify)
    : dev_(device), verify_(verify) {}

VertexArena::~VertexArena() { Release(); }

void VertexArena::Release() {
  for (const Pending& p : pending_) {
    p.arena->Unlock();
    p.arena->Release();
  }
  pending_.clear();
  if (vb_) vb_->Release();
  vb_ = nullptr;
  capacity_ = append_ = 0;
  ++generation_;
  regions_.clear();
  sizes_.clear();
  shadows_.clear();
  discard_next_ = true;
}

void VertexArena::Forget(IDirect3DVertexBuffer9* vb) {
  sizes_.erase(vb);
  regions_.erase(vb);
  shadows_.erase(vb);
  mirrors_.erase(vb);
  lock_history_.erase(vb);
}

bool VertexArena::Redirected(IDirect3DVertexBuffer9* vb) const {
  auto it = sizes_.find(vb);
  return it != sizes_.end() && it->second != 0;
}

bool VertexArena::Resolve(IDirect3DVertexBuffer9* vb, UINT offset,
                          IDirect3DVertexBuffer9** arena, UINT* arena_offset) {
  auto known = sizes_.find(vb);
  if (known == sizes_.end() || known->second == 0) return false;
  Region& region = regions_[vb];
  if (region.generation != generation_ || !vb_) {
    // Wrapped away (or never in this arena): bring back what we shadowed.
    auto shadow = shadows_.find(vb);
    if (shadow == shadows_.end() || shadow->second.ranges.empty()) return false;
    if (!Allocate(&region, known->second)) return false;
    ++stats_.restores;
    for (const auto& range : shadow->second.ranges) {
      void* data = nullptr;
      const DWORD flags = discard_next_ ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
      discard_next_ = false;
      if (SUCCEEDED(vb_->Lock(region.base + range.first, range.second - range.first, &data,
                              flags)) && data) {
        std::memcpy(data, shadow->second.bytes.data() + range.first,
                    range.second - range.first);
        vb_->Unlock();
      }
    }
  }
  *arena = vb_;
  *arena_offset = region.base + offset;
  return true;
}

const uint8_t* VertexArena::ShadowCopy(IDirect3DVertexBuffer9* vb, UINT offset,
                                       UINT bytes) const {
  auto it = shadows_.find(vb);
  if (it == shadows_.end()) return nullptr;
  const auto& ranges = it->second.ranges;
  auto range = ranges.upper_bound(offset);  // first range starting after offset
  if (range == ranges.begin()) return nullptr;
  --range;
  return offset + bytes <= range->second ? it->second.bytes.data() + offset : nullptr;
}

bool VertexArena::Allocate(Region* region, UINT size) {
  const UINT aligned = Align4(size);
  if (!vb_ || append_ + aligned > capacity_) {
    // New, larger arena. Draws already recorded keep the old one alive.
    UINT capacity = capacity_ ? capacity_ * 2 : kInitialCapacity;
    while (capacity < aligned) capacity *= 2;
    IDirect3DVertexBuffer9* vb = nullptr;
    const DWORD usage = verify_ ? D3DUSAGE_DYNAMIC : (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
    const HRESULT hr = dev_->CreateVertexBuffer(capacity, usage, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    if (FAILED(hr)) {
      Logf("vertex arena: CreateVertexBuffer(%u) failed hr=0x%08X, arena disabled", capacity,
           static_cast<unsigned>(hr));
      failed_ = true;
      return false;
    }
    if (vb_) {
      vb_->Release();
      ++stats_.grows;
      Logf("vertex arena: grown to %u MB", capacity >> 20);
    }
    vb_ = vb;
    capacity_ = capacity;
    append_ = 0;
    ++generation_;
    discard_next_ = true;
  }
  region->base = append_;
  region->size = size;
  region->generation = generation_;
  append_ += aligned;
  if (append_ > stats_.peak) stats_.peak = append_;
  return true;
}

bool VertexArena::Lock(IDirect3DVertexBuffer9* vb, const D3DVERTEXBUFFER_DESC* desc, UINT offset,
                       UINT size, void** data, DWORD flags, HRESULT* result) {
  if (failed_ || vb == vb_ || !data || (flags & D3DLOCK_READONLY)) return false;
  auto known = sizes_.find(vb);
  if (desc) {
    const UINT current = (desc->Usage & D3DUSAGE_DYNAMIC) ? desc->Size : 0;
    if (known != sizes_.end() && known->second != current) {
      // Another size or usage at a known address: the game released that
      // buffer and a new one got its address.
      Forget(vb);
      known = sizes_.end();
    }
    if (known == sizes_.end()) known = sizes_.emplace(vb, current).first;
  } else if (known == sizes_.end()) {
    return false;
  }
  const UINT buffer_size = known->second;
  if (buffer_size == 0 || offset >= buffer_size) return false;

  Region& region = regions_[vb];
  if (flags & D3DLOCK_DISCARD) shadows_[vb].ranges.clear();  // old contents are gone
  if ((flags & D3DLOCK_DISCARD) || region.generation != generation_ || region.size != buffer_size) {
    // A fresh region holds only what is written from now on; bring back the
    // shadowed rest (NOOVERWRITE locks keep earlier data valid).
    if (!(flags & D3DLOCK_DISCARD) && region.generation != generation_) {
      IDirect3DVertexBuffer9* ignored = nullptr;
      UINT ignored_offset = 0;
      if (!Resolve(vb, 0, &ignored, &ignored_offset) && !Allocate(&region, buffer_size)) {
        return false;
      }
    } else if (!Allocate(&region, buffer_size)) {
      return false;
    }
  }
  const UINT bytes = (size == 0 || offset + size > buffer_size) ? buffer_size - offset : size;
  const DWORD arena_flags = discard_next_ ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
  discard_next_ = false;
  *result = vb_->Lock(region.base + offset, bytes, data, arena_flags);
  if (verify_) {
    auto& history = lock_history_[vb];
    history.push_back({offset, bytes, flags, generation_, region.base});
    if (history.size() > 6) history.erase(history.begin());
  }
  if (SUCCEEDED(*result) && *data) {
    // The game writes into the CPU shadow; Unlock copies it into the arena.
    // Reading back from the locked arena memory instead (write-combined GPU
    // memory, ~240 MB/s here) cost 11-19 ms per gameplay frame once every
    // write was shadowed (measured 2026-09-17). Every write is: when only
    // writes up to 4 KB were, a 6.5 KB buffer written once at level start
    // lost its data after enough wraps and drew garbage (verify mode). Carrying
    // regions over by GPU readback instead was tried and dropped: the arena
    // could then not be WRITEONLY, and gameplay frames took 15-20 ms.
    Shadow& shadow = shadows_[vb];
    if (shadow.bytes.size() != buffer_size) shadow.bytes.assign(buffer_size, 0);
    auto* arena_pointer = static_cast<uint8_t*>(*data);
    *data = shadow.bytes.data() + offset;
    vb_->AddRef();
    pending_.push_back({vb, vb_, arena_pointer, offset, bytes});
    ++stats_.locks;
    stats_.bytes += bytes;
  }
  return true;
}

bool VertexArena::Unlock(IDirect3DVertexBuffer9* vb, HRESULT* result) {
  if (vb == vb_) return false;
  for (size_t i = pending_.size(); i-- > 0;) {
    if (pending_[i].game != vb) continue;
    const Pending p = pending_[i];
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
    Shadow& shadow = shadows_[vb];
    const uint8_t* written = shadow.bytes.data() + p.offset;
    std::memcpy(p.pointer, written, p.locked);  // shadow -> arena (fast direction)
    if (verify_) {
      std::vector<uint8_t>& mirror = mirrors_[vb];
      if (mirror.size() != sizes_[vb]) mirror.assign(sizes_[vb], 0);
      std::memcpy(mirror.data() + p.offset, written, p.locked);
    }
    // Merge [begin, end) into the written ranges (touching ranges join).
    UINT begin = p.offset, end = p.offset + p.locked;
    auto& ranges = shadow.ranges;
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin()) {
      auto before = std::prev(it);
      if (before->second >= begin) {  // overlaps or touches the range before
        begin = before->first;
        end = (std::max)(end, before->second);
        it = ranges.erase(before);
      }
    }
    while (it != ranges.end() && it->first <= end) {  // swallow ranges it reaches
      end = (std::max)(end, it->second);
      it = ranges.erase(it);
    }
    ranges.emplace_hint(it, begin, end);
    *result = p.arena->Unlock();
    p.arena->Release();
    return true;
  }
  return false;
}

long VertexArena::VerifyRange(IDirect3DVertexBuffer9* game, UINT game_offset,
                              IDirect3DVertexBuffer9* arena, UINT arena_offset, UINT bytes) {
  auto mirror = mirrors_.find(game);
  if (!verify_ || mirror == mirrors_.end() || game_offset + bytes > mirror->second.size() ||
      !arena || bytes == 0) {
    return -1;
  }
  void* data = nullptr;
  if (FAILED(arena->Lock(arena_offset, bytes, &data, D3DLOCK_READONLY)) || !data) return -1;
  long differing = 0;
  const auto* a = static_cast<const uint8_t*>(data);
  const uint8_t* m = mirror->second.data() + game_offset;
  for (UINT i = 0; i < bytes; ++i) differing += a[i] != m[i];
  arena->Unlock();
  return differing;
}

std::string VertexArena::Describe(IDirect3DVertexBuffer9* vb) const {
  char line[160];
  auto region = regions_.find(vb);
  auto size = sizes_.find(vb);
  snprintf(line, sizeof(line), "size %u, region base %u gen %u (current gen %u); locks:",
           size != sizes_.end() ? size->second : 0,
           region != regions_.end() ? region->second.base : 0,
           region != regions_.end() ? region->second.generation : 0, generation_);
  std::string out = line;
  auto history = lock_history_.find(vb);
  if (history != lock_history_.end()) {
    for (const LockRecord& r : history->second) {
      snprintf(line, sizeof(line), " [%u+%u flags 0x%lx gen %u base %u]", r.offset, r.size, r.flags,
               r.generation, r.arena_base);
      out += line;
    }
  }
  return out;
}

// The wrap must happen here, before the frame's first lock. It once only
// set a flag and wrapped at the frame's first new region: NOOVERWRITE locks
// earlier in that frame had written into the old generation's regions, the
// wrap's DISCARD renamed the buffer and new regions from offset 0 overlapped
// them. Live draws had already read the old bytes (correct on screen, and
// verify mode saw nothing), but the eyes rendered later read the new ones:
// one-frame garbage geometry in one eye (menu ribbon, gameplay shapes; caught
// by glitch_catch, 2026-09-17).
void VertexArena::BeginFrame() {
  if (append_ <= capacity_ / 2) return;
  append_ = 0;
  ++generation_;  // every region is stale: the next lock or draw re-resolves it
  discard_next_ = true;
  ++stats_.wraps;
}

VertexArena::Stats VertexArena::TakeStats() {
  Stats s = stats_;
  s.capacity = capacity_;
  stats_ = Stats();
  return s;
}

}  // namespace kkvr
