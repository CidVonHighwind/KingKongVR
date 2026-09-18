#include "game/aspect.h"

#include "common/log.h"
#include "game/memory.h"

#include <windows.h>

#include <cstring>

namespace kkvr {
namespace {

constexpr DWORD kRatioTable = 0xaebf50;  // float[4], see aspect.h
constexpr BYTE kFallbackRead[] = {0xd9, 0x05, 0x54, 0xbf, 0xae, 0x00};
constexpr DWORD kFallbackReadAt = 0x96a30a;
constexpr BYTE kTableRead[] = {0xd9, 0x04, 0x85, 0x50, 0xbf, 0xae, 0x00};
constexpr DWORD kTableReadAt = 0x96a31e;
constexpr float kOriginal[4] = {1.0f, 1.0f, 0.5625f, 0.5627451f};

bool Matches(DWORD address, const BYTE* bytes, size_t size) {
  return IsReadable(address, size) &&
         std::memcmp(reinterpret_cast<void*>(address), bytes, size) == 0;
}

}  // namespace

bool PatchScreenAspect(unsigned width, unsigned height) {
  if (width == 0 || height == 0) return false;
  if (!Matches(kFallbackReadAt, kFallbackRead, sizeof(kFallbackRead)) ||
      !Matches(kTableReadAt, kTableRead, sizeof(kTableRead))) {
    Logf("aspect: unknown game build, view stays 16:9");
    return false;
  }
  auto* table = reinterpret_cast<float*>(kRatioTable);
  // Entries 0 and 1 must be untouched; 2 and 3 are either original or ours
  // from an earlier call (both equal then).
  if (table[0] != kOriginal[0] || table[1] != kOriginal[1] ||
      !((table[2] == kOriginal[2] && table[3] == kOriginal[3]) || table[2] == table[3])) {
    Logf("aspect: ratio table at 0x%08lX has unexpected values, left alone", kRatioTable);
    return false;
  }
  const float ratio = static_cast<float>(height) / static_cast<float>(width);
  DWORD protect = 0;
  VirtualProtect(table, sizeof(float) * 4, PAGE_READWRITE, &protect);
  table[2] = ratio;
  table[3] = ratio;
  VirtualProtect(table, sizeof(float) * 4, protect, &protect);
  Logf("aspect: engine view ratio set from 9/16 to %u/%u (%.4f)", height, width, ratio);
  return true;
}

}  // namespace kkvr
