#include "game/joystick.h"

#include "debug/autotest.h"
#include "common/config.h"
#include "common/log.h"
#include "game/memory.h"

#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#include <cstring>

namespace kkvr {
namespace {

using JoyGetPosExFn = MMRESULT(WINAPI*)(UINT, LPJOYINFOEX);
using JoyGetDevCapsFn = MMRESULT(WINAPI*)(UINT_PTR, LPJOYCAPSA, UINT);
using DriverUpdateFn = void(__cdecl*)(BYTE* buttons);

JoyGetPosExFn g_real_get_pos = nullptr;
JoyGetDevCapsFn g_real_get_caps = nullptr;
DriverUpdateFn g_real_update = nullptr;

// kingkong9d.exe (fixed image base 0x400000, no ASLR).
constexpr DWORD kUpdateSlot = 0x3de9368;   // driver table entry for update
constexpr DWORD kUpdateFunction = 0xa00750;
// "mov dword ptr [0x3de9368], 0xa00750" in the driver init; checked before
// patching so another build of the game is left alone.
constexpr DWORD kInitStoreAddress = 0xa00ef0;
constexpr BYTE kInitStoreBytes[] = {0xc7, 0x05, 0x68, 0x93, 0xde, 0x03,
                                    0x50, 0x07, 0xa0, 0x00};
// Highest button index we set (16 is the keyboard's walk/run state).
constexpr int kMaxButtonIndex = 15;

int g_pad = -1;           // XInput user index in use, -1 = none
DWORD g_last_scan = 0;    // GetTickCount of the last search for a pad
DWORD g_last_pressed = 0; // for change logging
int g_logged_changes = 0;
constexpr int kMaxLoggedChanges = 100;

// XInput stick value (-32768..32767, up positive) to winmm (0..65535, up 0).
DWORD AxisX(SHORT v) { return static_cast<DWORD>(static_cast<LONG>(v) + 32768); }
DWORD AxisY(SHORT v) { return 65535 - AxisX(v); }

// The first connected XInput pad, or false if there is none.
bool ReadPad(XINPUT_STATE* state) {
  if (g_pad >= 0 && XInputGetState(g_pad, state) != ERROR_SUCCESS) {
    Logf("joystick: XInput pad %d lost", g_pad);
    g_pad = -1;
  }
  if (g_pad < 0) {
    // Scanning empty slots is slow-ish; at most once a second.
    const DWORD now = GetTickCount();
    if (now - g_last_scan < 1000) return false;
    g_last_scan = now;
    for (DWORD i = 0; i < XUSER_MAX_COUNT && g_pad < 0; ++i) {
      if (XInputGetState(i, state) == ERROR_SUCCESS) {
        g_pad = static_cast<int>(i);
        Logf("joystick: using XInput pad %d", g_pad);
      }
    }
  }
  return g_pad >= 0;
}

using XInputGetStateExFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
constexpr WORD kGuideButton = 0x0400;

XInputGetStateExFn GetStateEx() {
  static XInputGetStateExFn fn = [] {
    HMODULE xinput = GetModuleHandleA("xinput1_4.dll");
    const auto f = xinput ? reinterpret_cast<XInputGetStateExFn>(
                                GetProcAddress(xinput, MAKEINTRESOURCEA(100)))
                          : nullptr;
    Logf("joystick: Xbox button (XInputGetStateEx, ordinal 100) %s",
         f ? "available: recentres the window" : "not available");
    return f;
  }();
  return fn;
}

// Xbox controls in the order of kPadControlNames (config.cpp); bit i set
// when control i is pressed.
DWORD PressedControls(const XINPUT_STATE& state) {
  const WORD b = state.Gamepad.wButtons;
  const BYTE t = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  const bool pressed[kPadControlCount] = {
      (b & XINPUT_GAMEPAD_A) != 0,
      (b & XINPUT_GAMEPAD_B) != 0,
      (b & XINPUT_GAMEPAD_X) != 0,
      (b & XINPUT_GAMEPAD_Y) != 0,
      (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
      (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0,
      (b & XINPUT_GAMEPAD_BACK) != 0,
      (b & XINPUT_GAMEPAD_START) != 0,
      (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0,
      (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0,
      state.Gamepad.bLeftTrigger > t,
      state.Gamepad.bRightTrigger > t,
      (b & XINPUT_GAMEPAD_DPAD_UP) != 0,
      (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0,
      (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0,
      (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0,
  };
  DWORD mask = 0;
  for (int i = 0; i < kPadControlCount; ++i) {
    if (pressed[i]) mask |= 1ul << i;
  }
  return mask;
}

// Wraps the driver update: the game fills the buffer (keyboard, joystick
// axes), then the Xbox buttons are added through the [controller] mapping.
void __cdecl HookedDriverUpdate(BYTE* buttons) {
  g_real_update(buttons);
  if (!buttons) return;
  XINPUT_STATE state{};
  const bool pad = ReadPad(&state);
  const AutotestInput script = AutotestCurrentInput();
  if (!pad && !script.controls) return;

  const Config& config = GetConfig();
  const DWORD pressed = (pad ? PressedControls(state) : 0) | script.controls;
  for (int i = 0; i < kPadControlCount; ++i) {
    const int index = config.pad_buttons[i];
    if ((pressed & (1ul << i)) && index >= 0 && index <= kMaxButtonIndex) {
      buttons[index] = 0xff;
    }
  }
  if (pressed != g_last_pressed && g_logged_changes < kMaxLoggedChanges) {
    Logf("joystick: xbox controls 0x%04lX", pressed);
    ++g_logged_changes;
  }
  g_last_pressed = pressed;
}

}  // namespace

// Replace the driver update in the game's table, once the game has set it.
void HookDriverUpdate() {
  static bool not_this_build = false;
  if (g_real_update || not_this_build) return;
  // Only kingkong9d.exe has the code we patch; in another build these
  // addresses may not even be mapped.
  if (!IsReadable(kInitStoreAddress, sizeof(kInitStoreBytes)) ||
      !IsReadable(kUpdateSlot, sizeof(DWORD))) {
    not_this_build = true;
    Logf("joystick: unknown game build, buttons keep the registry bindings");
    return;
  }
  if (std::memcmp(reinterpret_cast<void*>(kInitStoreAddress), kInitStoreBytes,
                  sizeof(kInitStoreBytes)) != 0) {
    return;
  }
  auto* slot = reinterpret_cast<DWORD*>(kUpdateSlot);
  if (*slot != kUpdateFunction) return;
  g_real_update = reinterpret_cast<DriverUpdateFn>(*slot);
  *slot = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(&HookedDriverUpdate));
  Logf("joystick: driver update wrapped, buttons set directly");
}

namespace {

// Scripted stick value (-1..1) as an XInput value.
SHORT ScriptAxis(float v) {
  if (v < -1.0f) v = -1.0f;
  if (v > 1.0f) v = 1.0f;
  return static_cast<SHORT>(v * 32767.0f);
}

MMRESULT WINAPI HookedJoyGetPosEx(UINT id, LPJOYINFOEX info) {
  MMRESULT result = g_real_get_pos(id, info);
  // Called from inside the driver update, so the table is set up by now.
  HookDriverUpdate();
  if (!info) return result;
  // A test script needs a joystick even when no controller is connected.
  if (result != JOYERR_NOERROR && AutotestActive() && id == 0) {
    const DWORD flags = info->dwFlags;
    std::memset(info, 0, sizeof(*info));
    info->dwSize = sizeof(*info);
    info->dwFlags = flags;
    info->dwXpos = info->dwYpos = info->dwZpos = info->dwRpos = 32767;
    info->dwPOV = JOY_POVCENTERED;
    result = JOYERR_NOERROR;
  }
  if (result != JOYERR_NOERROR) return result;

  XINPUT_STATE state{};
  const bool pad = ReadPad(&state);
  const AutotestInput script = AutotestCurrentInput();
  if (!pad && !script.left_stick && !script.right_stick) return result;
  SHORT lx = pad ? state.Gamepad.sThumbLX : 0, ly = pad ? state.Gamepad.sThumbLY : 0;
  SHORT rx = pad ? state.Gamepad.sThumbRX : 0, ry = pad ? state.Gamepad.sThumbRY : 0;
  if (script.left_stick) {
    lx = ScriptAxis(script.lx);
    ly = ScriptAxis(script.ly);
  }
  if (script.right_stick) {
    rx = ScriptAxis(script.rx);
    ry = ScriptAxis(script.ry);
  }
  if (info->dwFlags & JOY_RETURNX) info->dwXpos = AxisX(lx);
  if (info->dwFlags & JOY_RETURNY) info->dwYpos = AxisY(ly);
  if (info->dwFlags & JOY_RETURNZ) info->dwZpos = AxisX(rx);
  if (info->dwFlags & JOY_RETURNR) info->dwRpos = AxisY(ry);
  // Buttons are set directly (HookedDriverUpdate). Only if
  // that wrapper is unavailable do the registry bindings keep working.
  if (g_real_update && (info->dwFlags & JOY_RETURNBUTTONS)) {
    info->dwButtons = 0;
    info->dwButtonNumber = 0;
  }
  return result;
}

MMRESULT WINAPI HookedJoyGetDevCapsA(UINT_PTR id, LPJOYCAPSA caps, UINT size) {
  MMRESULT result = g_real_get_caps(id, caps, size);
  AutotestLoad();
  if (result != JOYERR_NOERROR && AutotestActive() && id == 0 && caps &&
      size >= sizeof(JOYCAPSA)) {
    // A test script needs a joystick even when no controller is connected.
    std::memset(caps, 0, sizeof(*caps));
    strcpy(caps->szPname, "kkvr autotest pad");
    caps->wXmax = caps->wYmax = caps->wZmax = caps->wRmax = 65535;
    caps->wNumButtons = caps->wMaxButtons = 16;
    caps->wNumAxes = caps->wMaxAxes = 4;
    caps->wCaps = JOYCAPS_HASZ | JOYCAPS_HASR;
    result = JOYERR_NOERROR;
  }
  if (result == JOYERR_NOERROR && caps && size >= sizeof(JOYCAPSA)) {
    Logf("joystick %u caps: \"%s\" buttons=%u axes=%u", static_cast<UINT>(id),
         caps->szPname, caps->wNumButtons, caps->wNumAxes);
  }
  return result;
}

// Point the executable's import of `dll!function` at `hook`. Returns false if
// the import was not found. The game's import descriptors have no separate
// name table (OriginalFirstThunk is 0), so once the loader has bound the
// imports the names are gone; slots are matched by the address they hold.
bool PatchImport(const char* dll, void* real, void* hook) {
  const auto base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
  const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;

  for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       desc->Name; ++desc) {
    if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
    for (auto slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
         slot->u1.Function; ++slot) {
      if (slot->u1.Function != reinterpret_cast<ULONG_PTR>(real)) continue;
      DWORD protect = 0;
      VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function),
                     PAGE_READWRITE, &protect);
      slot->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
      VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), protect,
                     &protect);
      return true;
    }
  }
  return false;
}

}  // namespace

void InstallJoystickHooks() {
  if (g_real_get_pos) return;
  // winmm is a static import of the game, so it is already loaded (no
  // LoadLibrary, which DllMain must avoid). Take the real functions from its
  // exports rather than from the import slots, which may not be bound yet.
  const HMODULE winmm = GetModuleHandleA("winmm.dll");
  if (!winmm) {
    Logf("joystick: winmm.dll not loaded, no controller support");
    return;
  }
  g_real_get_pos = reinterpret_cast<JoyGetPosExFn>(GetProcAddress(winmm, "joyGetPosEx"));
  g_real_get_caps =
      reinterpret_cast<JoyGetDevCapsFn>(GetProcAddress(winmm, "joyGetDevCapsA"));
  if (!g_real_get_pos || !g_real_get_caps) {
    Logf("joystick: winmm exports missing, no controller support");
    g_real_get_pos = nullptr;
    return;
  }
  const bool pos = PatchImport("winmm.dll", g_real_get_pos, &HookedJoyGetPosEx);
  const bool caps = PatchImport("winmm.dll", g_real_get_caps, &HookedJoyGetDevCapsA);
  Logf("joystick: imports patched (joyGetPosEx %s, joyGetDevCapsA %s)",
       pos ? "ok" : "NOT FOUND", caps ? "ok" : "NOT FOUND");
}

bool JoystickGuideButton() {
  XINPUT_STATE state{};
  const XInputGetStateExFn get_state = GetStateEx();
  if (!get_state || g_pad < 0) {
    if (!ReadPad(&state)) return false;  // finds the pad (rate-limited scan)
  }
  if (!get_state || g_pad < 0) return false;
  return get_state(static_cast<DWORD>(g_pad), &state) == ERROR_SUCCESS &&
         (state.Gamepad.wButtons & kGuideButton) != 0;
}

}  // namespace kkvr
