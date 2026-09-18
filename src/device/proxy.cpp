// d3d9.dll proxy entry point.
//
// King Kong statically imports exactly one symbol from d3d9.dll --
// Direct3DCreate9 -- so this is the entire surface we need to stand in for.
// We load the real system DLL, forward the call, and hand the game a wrapped
// IDirect3D9 instead of the genuine article.

#include "common/config.h"
#include "common/crash.h"
#include "game/joystick.h"
#include "common/log.h"
#include "device/wrapper.h"

#include <windows.h>
#include <d3d9.h>

namespace {

HMODULE g_real_d3d9 = nullptr;

using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
PFN_Direct3DCreate9 g_real_create = nullptr;

// Load the genuine d3d9.dll out of the system directory. A 32-bit process on
// 64-bit Windows gets SysWOW64 here automatically via WOW64 redirection, which
// is what we want -- and crucially it is not this directory, so we cannot
// accidentally load ourselves.
bool LoadRealD3D9() {
  if (g_real_create) return true;

  char path[MAX_PATH] = {};
  const UINT len = GetSystemDirectoryA(path, MAX_PATH);
  if (len == 0 || len > MAX_PATH - 16) {
    kkvr::Logf("GetSystemDirectory failed (%u)", GetLastError());
    return false;
  }
  lstrcatA(path, "\\d3d9.dll");

  g_real_d3d9 = LoadLibraryA(path);
  if (!g_real_d3d9) {
    kkvr::Logf("failed to load %s (%u)", path, GetLastError());
    return false;
  }

  g_real_create = reinterpret_cast<PFN_Direct3DCreate9>(
      GetProcAddress(g_real_d3d9, "Direct3DCreate9"));
  if (!g_real_create) {
    kkvr::Logf("Direct3DCreate9 not found in %s", path);
    return false;
  }

  kkvr::Logf("loaded real d3d9 from %s", path);
  return true;
}

}  // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
  kkvr::Logf("Direct3DCreate9(SDKVersion=%u)", SDKVersion);

  if (!LoadRealD3D9()) return nullptr;

  IDirect3D9* real = nullptr;
  {
    // An IDirect3D9Ex, whose devices can share textures with D3D11 (the
    // headset output) and are never lost.
    using PFN_Direct3DCreate9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    const auto create_ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(
        GetProcAddress(g_real_d3d9, "Direct3DCreate9Ex"));
    IDirect3D9Ex* ex = nullptr;
    const HRESULT hr = create_ex ? create_ex(SDKVersion, &ex) : E_FAIL;
    kkvr::Logf("Direct3DCreate9Ex hr=0x%08X", static_cast<unsigned>(hr));
    real = ex;
  }
  if (!real) real = g_real_create(SDKVersion);
  if (!real) {
    kkvr::Logf("real Direct3DCreate9 returned null");
    return nullptr;
  }

  // From here on the game talks to the wrapper (wrapper.h); its devices
  // record the game's frames and render them per eye.
  return new kkvr::D3D9(real);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      kkvr::LogInit("kkvr.log", "d3d9 proxy");
      kkvr::Logf("attached to pid %u, d3d9 proxy base %p", GetCurrentProcessId(),
                 static_cast<void*>(module));
      kkvr::InstallCrashHandler();
      kkvr::InstallJoystickHooks();
      break;
    case DLL_PROCESS_DETACH:
      kkvr::LogShutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
