// Vertex buffer Lock/Unlock hooks: the game's vertex writes go into the
// vertex arena (vertex_arena.h).
//
// The game locks its buffers on the real resources, which the proxy does not
// wrap, so Lock and Unlock are patched in the runtime's shared vertex buffer
// vtables (one per runtime class: managed, default, dynamic, system memory).
#pragma once

#include <d3d9.h>

namespace kkvr {

// Patch Lock/Unlock of the runtime's vertex buffer classes (once per process).
void InstallVertexBufferLockHooks(IDirect3DDevice9* real_device);

// Redirect for vertex buffer locks (vertex_arena.h). Returning true means the
// call was handled and `result` holds its HRESULT. `desc` is the buffer's
// description at this lock (null if GetDesc failed).
class VertexBufferRedirect {
 public:
  virtual ~VertexBufferRedirect() = default;
  virtual bool Lock(IDirect3DVertexBuffer9* vb, const D3DVERTEXBUFFER_DESC* desc, UINT offset,
                    UINT size, void** data, DWORD flags, HRESULT* result) = 0;
  virtual bool Unlock(IDirect3DVertexBuffer9* vb, HRESULT* result) = 0;
};
void SetVertexBufferRedirect(VertexBufferRedirect* redirect);

}  // namespace kkvr
