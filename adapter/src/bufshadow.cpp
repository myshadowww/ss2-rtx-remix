#include "bufshadow.h"

#include <string.h>

#include <unordered_map>
#include <vector>

#include "log.h"
#include "vtable.h"

namespace ss2rtx {

// Buffers above this size are vertex/index data or static tables: not shadowed.
static const UINT kMaxShadowBytes = 64 * 1024;

struct Shadow {
  std::vector<uint8_t> data;
  bool valid = false;
  void* real = nullptr;   // the driver's pointer while a substituted Map is open
  uint64_t serial = 0;    // g_serial at the last write
};

using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

static PFN_Map g_orig_Map;
static PFN_Unmap g_orig_Unmap;
static PFN_UpdateSubresource g_orig_UpdateSubresource;
static ID3D11DeviceContext* g_ctx;
static std::unordered_map<ID3D11Resource*, Shadow> g_shadows;   // render thread only
static unsigned g_open_substitutions;
static uint64_t g_serial;

// Returns the size if `res` is a buffer we shadow, else 0.
static UINT shadowable_size(ID3D11Resource* res) {
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  if (d.ByteWidth == 0 || d.ByteWidth > kMaxShadowBytes) return 0;
  if (!(d.BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE))) return 0;
  return d.ByteWidth;
}

// WRITE_DISCARD maps of shadowed buffers hand the game our own memory; Unmap copies it to
// the driver's pointer. That gives us the contents without reading uncached GPU memory.
static HRESULT STDMETHODCALLTYPE hook_Map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                          D3D11_MAPPED_SUBRESOURCE* out) {
  HRESULT hr = g_orig_Map(ctx, res, sub, type, flags, out);
  if (FAILED(hr) || ctx != g_ctx || !res || !out || sub != 0) return hr;
  if (type != D3D11_MAP_WRITE_DISCARD) {
    // A partial or in-place write leaves our copy out of date.
    if (type != D3D11_MAP_READ) {
      auto it = g_shadows.find(res);
      if (it != g_shadows.end()) it->second.valid = false;
    }
    return hr;
  }
  UINT size = shadowable_size(res);
  if (!size || !out->pData) return hr;
  Shadow& s = g_shadows[res];
  if (s.real) return hr;   // already mapped: leave it alone
  s.data.resize(size);
  s.real = out->pData;
  s.valid = false;
  out->pData = s.data.data();
  ++g_open_substitutions;
  return hr;
}

static void STDMETHODCALLTYPE hook_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub) {
  if (ctx == g_ctx && g_open_substitutions && sub == 0) {
    auto it = g_shadows.find(res);
    if (it != g_shadows.end() && it->second.real) {
      memcpy(it->second.real, it->second.data.data(), it->second.data.size());
      it->second.real = nullptr;
      it->second.valid = true;
      it->second.serial = ++g_serial;
      --g_open_substitutions;
    }
  }
  g_orig_Unmap(ctx, res, sub);
}

static void STDMETHODCALLTYPE hook_UpdateSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                                                     const void* src, UINT row_pitch, UINT depth_pitch) {
  if (ctx == g_ctx && res && src && sub == 0) {
    UINT size = shadowable_size(res);
    if (size) {
      Shadow& s = g_shadows[res];
      if (!box) {
        s.data.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
        s.valid = true;
        s.serial = ++g_serial;
      } else if (box->left < box->right && box->right <= size && s.data.size() == size) {
        memcpy(s.data.data() + box->left, src, box->right - box->left);
        s.serial = ++g_serial;
      } else {
        s.valid = false;
      }
    }
  }
  g_orig_UpdateSubresource(ctx, res, sub, box, src, row_pitch, depth_pitch);
}

bool shadow_install(ID3D11DeviceContext* immediate) {
  g_ctx = immediate;
  bool ok = true;
  ok &= patch_vtable(immediate, 14, hook_Map, &g_orig_Map);
  ok &= patch_vtable(immediate, 15, hook_Unmap, &g_orig_Unmap);
  ok &= patch_vtable(immediate, 48, hook_UpdateSubresource, &g_orig_UpdateSubresource);
  logf("bufshadow: Map/Unmap/UpdateSubresource hooks %s", ok ? "installed" : "FAILED");
  return ok;
}

using PFN_CreateBuffer = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
static PFN_CreateBuffer g_orig_CreateBuffer;
static DWORD g_render_thread;

static HRESULT STDMETHODCALLTYPE hook_CreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                                   ID3D11Buffer** out) {
  HRESULT hr = g_orig_CreateBuffer(dev, desc, init, out);
  // The shadow table belongs to the render thread; buffers created elsewhere are left to the readback path.
  if (SUCCEEDED(hr) && out && *out && desc && GetCurrentThreadId() == g_render_thread) {
    bool wanted = desc->ByteWidth > 0 && desc->ByteWidth <= kMaxShadowBytes &&
                  (desc->BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE));
    auto it = g_shadows.find(*out);
    if (wanted && init && init->pSysMem) {
      Shadow& s = g_shadows[*out];
      s.data.assign(static_cast<const uint8_t*>(init->pSysMem), static_cast<const uint8_t*>(init->pSysMem) + desc->ByteWidth);
      s.valid = true;
      s.real = nullptr;
      s.serial = ++g_serial;
    } else if (it != g_shadows.end()) {
      it->second.valid = false;   // a new buffer at a recycled address: the old copy is not its contents
    }
  }
  return hr;
}

bool shadow_install_device(ID3D11Device* device) {
  g_render_thread = GetCurrentThreadId();
  return patch_vtable(device, 3, hook_CreateBuffer, &g_orig_CreateBuffer);
}

bool shadow_get(ID3D11Buffer* buf, const uint8_t** data, size_t* size, uint64_t* serial) {
  auto it = g_shadows.find(buf);
  if (it == g_shadows.end() || !it->second.valid || it->second.real) return false;
  *data = it->second.data.data();
  *size = it->second.data.size();
  if (serial) *serial = it->second.serial;
  return true;
}

uint64_t shadow_serial() { return g_serial; }

}  // namespace ss2rtx
