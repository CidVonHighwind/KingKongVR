// Checks before the game patches read the game's memory: an address of
// kingkong9d.exe may not even be mapped in another build.
#pragma once

#include <windows.h>

#include <cstddef>

namespace kkvr {

// True if `size` bytes at `address` are committed, accessible and inside one
// memory region.
inline bool IsReadable(DWORD address, size_t size) {
  MEMORY_BASIC_INFORMATION mbi{};
  return VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) &&
         mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
         address + size <= reinterpret_cast<DWORD>(mbi.BaseAddress) + mbi.RegionSize;
}

}  // namespace kkvr
