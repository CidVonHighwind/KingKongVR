#include "device/lock_hooks.h"

#include "common/log.h"

#include <windows.h>

#include <map>
#include <utility>

namespace kkvr {
namespace {

using BufferLock = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, void**, DWORD);
using BufferUnlock = HRESULT(STDMETHODCALLTYPE*)(void*);
// Originals by vtable (+ slot): the runtime has several classes per resource
// type (managed, default, dynamic, system memory), each with its own vtable.
std::map<std::pair<void**, int>, void*> g_originals;
VertexBufferRedirect* g_redirect = nullptr;

void* Original(void* object, int slot) {
  void** vtable = *static_cast<void***>(object);
  auto it = g_originals.find({vtable, slot});
  return it == g_originals.end() ? nullptr : it->second;
}

HRESULT STDMETHODCALLTYPE HookVbLock(void* self, UINT offset, UINT size, void** data,
                                     DWORD flags) {
  if (g_redirect) {
    auto* vb = static_cast<IDirect3DVertexBuffer9*>(self);
    D3DVERTEXBUFFER_DESC d{};
    const bool have_desc = vb->GetDesc(&d) == D3D_OK;
    HRESULT redirected = D3D_OK;
    if (g_redirect->Lock(vb, have_desc ? &d : nullptr, offset, size, data, flags, &redirected)) {
      return redirected;
    }
  }
  return static_cast<BufferLock>(Original(self, 11))(self, offset, size, data, flags);
}

HRESULT STDMETHODCALLTYPE HookVbUnlock(void* self) {
  if (g_redirect) {
    HRESULT redirected = D3D_OK;
    if (g_redirect->Unlock(static_cast<IDirect3DVertexBuffer9*>(self), &redirected)) {
      return redirected;
    }
  }
  return static_cast<BufferUnlock>(Original(self, 12))(self);
}

// Point vtable slot `index` of `object` at `hook`, once per vtable.
bool PatchSlot(IUnknown* object, int index, void* hook) {
  void** vtable = *static_cast<void***>(static_cast<void*>(object));
  if (vtable[index] == hook || g_originals.count({vtable, index})) return false;
  g_originals[{vtable, index}] = vtable[index];
  DWORD protect = 0;
  VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &protect);
  vtable[index] = hook;
  VirtualProtect(&vtable[index], sizeof(void*), protect, &protect);
  return true;
}

}  // namespace

void InstallVertexBufferLockHooks(IDirect3DDevice9* dev) {
  static bool installed = false;
  if (installed || !dev) return;
  installed = true;
  // Vtable indices: IUnknown 0-2, IDirect3DResource9 3-10, Lock 11, Unlock 12.
  // Objects of one runtime class share a vtable; create a dummy of every
  // pool/usage combination so each class gets hooked.
  struct Variant { DWORD usage; D3DPOOL pool; };
  constexpr Variant kVariants[] = {{0, D3DPOOL_MANAGED}, {0, D3DPOOL_DEFAULT},
                                   {D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT},
                                   {D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DPOOL_DEFAULT},
                                   {D3DUSAGE_WRITEONLY, D3DPOOL_MANAGED},
                                   {0, D3DPOOL_SYSTEMMEM}};
  int patched = 0;
  for (const Variant& v : kVariants) {
    IDirect3DVertexBuffer9* vb = nullptr;
    if (SUCCEEDED(dev->CreateVertexBuffer(16, v.usage, 0, v.pool, &vb, nullptr))) {
      patched += PatchSlot(vb, 11, &HookVbLock);
      patched += PatchSlot(vb, 12, &HookVbUnlock);
      vb->Release();
    }
  }
  Logf("vertex buffer lock hooks on %d distinct vtable slots", patched);
}

void SetVertexBufferRedirect(VertexBufferRedirect* redirect) { g_redirect = redirect; }

}  // namespace kkvr
