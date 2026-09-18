#include "game/timing.h"

#include "common/log.h"
#include "game/memory.h"

#include <windows.h>

#include <cstring>

namespace kkvr {
namespace {

// Instructions in the frame step function that read the 0.01 s floor from
// the shared constant 0xa83c94 (other code may use that constant, so the
// instructions are redirected instead of changing it):
//   0xa051bd  fcom dword ptr [0xa83c94]   D8 15 94 3C A8 00
//   0xa051cc  fld  dword ptr [0xa83c94]   D9 05 94 3C A8 00
struct Site {
  DWORD address;
  BYTE opcode[2];
};
constexpr Site kFloorSites[] = {{0xa051bd, {0xd8, 0x15}}, {0xa051cc, {0xd9, 0x05}}};
constexpr DWORD kFloorConstant = 0xa83c94;
constexpr DWORD kGameClock = 0xf357a0;

float g_min_step = 0.01f;  // read by the patched instructions

bool IsExpectedBuild() {
  for (const Site& site : kFloorSites) {
    if (!IsReadable(site.address, 6)) return false;
    const auto* code = reinterpret_cast<const BYTE*>(site.address);
    DWORD operand = 0;
    std::memcpy(&operand, code + 2, sizeof(operand));
    const DWORD ours = reinterpret_cast<DWORD>(&g_min_step);
    if (code[0] != site.opcode[0] || code[1] != site.opcode[1] ||
        (operand != kFloorConstant && operand != ours)) {
      return false;
    }
  }
  return IsReadable(kGameClock, sizeof(float));
}

}  // namespace

bool PatchMinimumFrameStep(float seconds) {
  if (!IsExpectedBuild()) {
    Logf("timing: unknown game build, frame step floor left at 0.01 s");
    return false;
  }
  g_min_step = seconds;
  const DWORD ours = reinterpret_cast<DWORD>(&g_min_step);
  for (const Site& site : kFloorSites) {
    const auto operand = reinterpret_cast<BYTE*>(site.address + 2);
    DWORD protect = 0;
    VirtualProtect(operand, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &protect);
    std::memcpy(operand, &ours, sizeof(DWORD));
    VirtualProtect(operand, sizeof(DWORD), protect, &protect);
  }
  FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
  Logf("timing: frame step floor lowered from 0.01 s to %.4f s", seconds);
  return true;
}

float GameClockSeconds() {
  static const bool available = IsExpectedBuild();
  if (!available) return -1.0f;
  return *reinterpret_cast<const float*>(kGameClock);
}

}  // namespace kkvr
