// Draw-stream census: one JSON line per event of a captured frame, written to
// census-<n>.jsonl next to the adapter log. tools/census.py turns it into a summary.
//
// State is read back from the context with the D3D11 getters at each draw, so only the
// draw calls themselves (plus shader/layout creation, for identities) need hooks.
// Constant-buffer contents are read back through a staging copy; that is slow, which is
// fine for a handful of census frames and is never done outside them.
#include "census.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "bufshadow.h"
#include "log.h"
#include "remix_link.h"
#include "scene.h"
#include "shaders.h"
#include "vtable.h"

namespace ss2rtx {

// ---- identities --------------------------------------------------------------------------

static CRITICAL_SECTION g_lock;
static bool g_lock_ready;
static std::map<void*, std::string> g_layouts;  // input layout -> JSON array of elements
static unsigned g_deferred_contexts;

struct Lock {
  Lock() { EnterCriticalSection(&g_lock); }
  ~Lock() { LeaveCriticalSection(&g_lock); }
};

// ---- capture state -----------------------------------------------------------------------

static FILE* g_out;
static int g_capture_index;
static unsigned g_seq;
static std::vector<double> g_capture_at;
static bool g_schedule_parsed;
static std::map<std::string, int> g_blob_ids;  // constant-buffer contents already written
static ID3D11Device* g_device;
static ID3D11DeviceContext* g_immediate;

static void emit(const std::string& line) {
  if (!g_out) return;
  fputs(line.c_str(), g_out);
  fputc('\n', g_out);
}

static std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

static std::string shader_json(void* shader) {
  if (!shader) return "null";
  const ShaderRecord* rec = shader_find(shader);
  if (!rec) return fmt("{\"ptr\":\"%p\",\"id\":\"unseen\"}", shader);
  return fmt("{\"ptr\":\"%p\",\"id\":\"%s\",\"size\":%zu}", shader, rec->id.c_str(), rec->size);
}

static std::string resource_json(ID3D11Resource* res) {
  if (!res) return "null";
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    return fmt("{\"ptr\":\"%p\",\"tex2d\":[%u,%u],\"fmt\":%d,\"mips\":%u,\"array\":%u,\"samples\":%u,\"bind\":%u,\"usage\":%d}", (void*)res,
               d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags, (int)d.Usage);
  }
  if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d;
    static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    return fmt("{\"ptr\":\"%p\",\"buffer\":%u,\"stride\":%u,\"bind\":%u,\"usage\":%d,\"misc\":%u}", (void*)res, d.ByteWidth,
               d.StructureByteStride, d.BindFlags, (int)d.Usage, d.MiscFlags);
  }
  return fmt("{\"ptr\":\"%p\",\"dim\":%d}", (void*)res, (int)dim);
}

static std::string buffer_json(ID3D11Buffer* buf) {
  if (!buf) return "null";
  D3D11_BUFFER_DESC d;
  buf->GetDesc(&d);
  return fmt("{\"ptr\":\"%p\",\"bytes\":%u,\"usage\":%d,\"bind\":%u}", (void*)buf, d.ByteWidth, (int)d.Usage, d.BindFlags);
}

template <typename View> static std::string view_json(View* view) {
  if (!view) return "null";
  ID3D11Resource* res = nullptr;
  view->GetResource(&res);
  std::string s = resource_json(res);
  if (res) res->Release();
  return s;
}

// Reads a constant buffer's contents through a staging copy and returns the id of the
// blob line holding them (written once per distinct content). Immediate context only.
static int constant_blob(ID3D11DeviceContext* ctx, ID3D11Buffer* buf) {
  if (!buf || !g_device || ctx != g_immediate) return -1;
  D3D11_BUFFER_DESC d;
  buf->GetDesc(&d);
  if (d.ByteWidth == 0 || d.ByteWidth > 4096) return -1;
  D3D11_BUFFER_DESC sd = {};
  sd.ByteWidth = d.ByteWidth;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* staging = nullptr;
  if (FAILED(g_device->CreateBuffer(&sd, nullptr, &staging))) return -1;
  ctx->CopyResource(staging, buf);
  D3D11_MAPPED_SUBRESOURCE map = {};
  int id = -1;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
    std::string hex;
    hex.reserve(d.ByteWidth * 2);
    static const char digits[] = "0123456789abcdef";
    const unsigned char* p = static_cast<const unsigned char*>(map.pData);
    for (UINT i = 0; i < d.ByteWidth; ++i) { hex.push_back(digits[p[i] >> 4]); hex.push_back(digits[p[i] & 15]); }
    ctx->Unmap(staging, 0);
    auto it = g_blob_ids.find(hex);
    if (it != g_blob_ids.end()) {
      id = it->second;
    } else {
      id = (int)g_blob_ids.size();
      g_blob_ids.emplace(hex, id);
      emit(fmt("{\"ev\":\"blob\",\"id\":%d,\"bytes\":%u,\"hex\":\"", id, d.ByteWidth) + hex + "\"}");
    }
  }
  staging->Release();
  return id;
}

static void record_draw(ID3D11DeviceContext* ctx, const char* kind, UINT count, UINT start, INT base_vertex, UINT instances) {
  std::string line = fmt("{\"ev\":\"draw\",\"seq\":%u,\"ctx\":\"%p\",\"deferred\":%d,\"kind\":\"%s\",\"count\":%u,\"start\":%u,\"base\":%d,\"instances\":%u",
                         g_seq++, (void*)ctx, (int)(ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED), kind, count, start, base_vertex, instances);

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ctx->PSGetShader(&ps, nullptr, nullptr);
  ctx->GSGetShader(&gs, nullptr, nullptr);
  line += ",\"vs\":" + shader_json(vs) + ",\"ps\":" + shader_json(ps) + fmt(",\"gs\":%d", (int)(gs != nullptr));
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (gs) gs->Release();

  ID3D11InputLayout* layout = nullptr;
  ctx->IAGetInputLayout(&layout);
  D3D11_PRIMITIVE_TOPOLOGY topo;
  ctx->IAGetPrimitiveTopology(&topo);
  line += fmt(",\"layout\":\"%p\",\"topology\":%d", (void*)layout, (int)topo);
  if (layout) layout->Release();

  ID3D11Buffer* vbs[4] = {};
  UINT strides[4] = {}, offsets[4] = {};
  ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
  line += ",\"vb\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) line += ",";
    line += vbs[i] ? fmt("{\"buf\":%s,\"stride\":%u,\"offset\":%u}", buffer_json(vbs[i]).c_str(), strides[i], offsets[i]) : "null";
    if (vbs[i]) vbs[i]->Release();
  }
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
  UINT ib_offset = 0;
  ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
  line += fmt("],\"ib\":{\"buf\":%s,\"fmt\":%d,\"offset\":%u}", buffer_json(ib).c_str(), (int)ib_format, ib_offset);
  if (ib) ib->Release();

  ID3D11RenderTargetView* rtvs[4] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(4, rtvs, &dsv);
  line += ",\"rt\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) line += ",";
    line += view_json(rtvs[i]);
    if (rtvs[i]) rtvs[i]->Release();
  }
  line += "],\"ds\":" + view_json(dsv);
  if (dsv) dsv->Release();

  ID3D11ShaderResourceView* srvs[8] = {};
  ctx->PSGetShaderResources(0, 8, srvs);
  line += ",\"ps_srv\":[";
  for (int i = 0; i < 8; ++i) {
    if (i) line += ",";
    line += view_json(srvs[i]);
    if (srvs[i]) srvs[i]->Release();
  }
  ID3D11ShaderResourceView* vsrvs[4] = {};
  ctx->VSGetShaderResources(0, 4, vsrvs);
  line += "],\"vs_srv\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) line += ",";
    line += view_json(vsrvs[i]);
    if (vsrvs[i]) vsrvs[i]->Release();
  }

  // Constant buffers: sizes always, contents (as blob ids) on the immediate context.
  for (int stage = 0; stage < 2; ++stage) {
    ID3D11Buffer* cbs[8] = {};
    if (stage == 0) ctx->VSGetConstantBuffers(0, 8, cbs); else ctx->PSGetConstantBuffers(0, 8, cbs);
    line += stage == 0 ? "],\"vs_cb\":[" : "],\"ps_cb\":[";
    for (int i = 0; i < 8; ++i) {
      if (i) line += ",";
      if (cbs[i]) {
        D3D11_BUFFER_DESC d;
        cbs[i]->GetDesc(&d);
        line += fmt("{\"ptr\":\"%p\",\"bytes\":%u,\"blob\":%d}", (void*)cbs[i], d.ByteWidth, constant_blob(ctx, cbs[i]));
        cbs[i]->Release();
      } else {
        line += "null";
      }
    }
  }
  line += "]";

  UINT vp_count = 1;
  D3D11_VIEWPORT vp = {};
  ctx->RSGetViewports(&vp_count, &vp);
  line += fmt(",\"viewport\":[%.1f,%.1f,%.1f,%.1f,%.3f,%.3f]", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth);

  ID3D11BlendState* blend = nullptr;
  FLOAT factor[4];
  UINT mask = 0;
  ctx->OMGetBlendState(&blend, factor, &mask);
  if (blend) {
    D3D11_BLEND_DESC bd;
    blend->GetDesc(&bd);
    const auto& t = bd.RenderTarget[0];
    line += fmt(",\"blend\":[%d,%d,%d,%d,%d]", (int)t.BlendEnable, (int)t.SrcBlend, (int)t.DestBlend, (int)t.BlendOp, (int)t.RenderTargetWriteMask);
    blend->Release();
  }
  ID3D11DepthStencilState* dss = nullptr;
  UINT stencil_ref = 0;
  ctx->OMGetDepthStencilState(&dss, &stencil_ref);
  if (dss) {
    D3D11_DEPTH_STENCIL_DESC dd;
    dss->GetDesc(&dd);
    line += fmt(",\"depth\":[%d,%d,%d,%d]", (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc, (int)dd.StencilEnable);
    dss->Release();
  }
  ID3D11RasterizerState* rs = nullptr;
  ctx->RSGetState(&rs);
  if (rs) {
    D3D11_RASTERIZER_DESC rd;
    rs->GetDesc(&rd);
    line += fmt(",\"raster\":[%d,%d,%d]", (int)rd.FillMode, (int)rd.CullMode, (int)rd.FrontCounterClockwise);
    rs->Release();
  }
  line += "}";
  emit(line);
}

// ---- context hooks -----------------------------------------------------------------------

using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_ClearRTV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

// One set of originals per context class (immediate, deferred): they have separate vtables.
struct ContextOriginals {
  PFN_DrawIndexed DrawIndexed;
  PFN_Draw Draw;
  PFN_DrawIndexedInstanced DrawIndexedInstanced;
  PFN_DrawInstanced DrawInstanced;
  PFN_ClearRTV ClearRTV;
  PFN_ClearDSV ClearDSV;
  PFN_ExecuteCommandList ExecuteCommandList;
  PFN_CopyResource CopyResource;
};
static ContextOriginals g_imm, g_def;
static volatile LONG64 g_draws_total, g_draws_deferred, g_command_lists;
static bool g_in_census_call;  // our own CopyResource for constant readback must not be logged

// Runs the adapter's per-draw work for an immediate-context draw. Returns true when the
// game's draw should be dropped (passes that make no sense on a path-traced image).
static bool before_draw(ID3D11DeviceContext* c, bool indexed, UINT count, UINT start, INT base) {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  c->VSGetShader(&vs, nullptr, nullptr);
  c->PSGetShader(&ps, nullptr, nullptr);
  const ShaderRecord* vs_rec = shader_find(vs);
  const ShaderRecord* ps_rec = shader_find(ps);
  if (vs) vs->Release();
  if (ps) ps->Release();
  bool skip = remix_link_before_draw(c, vs_rec, ps_rec);
  if (!skip && indexed) scene_on_draw_indexed(c, vs_rec, ps_rec, count, start, base);
  return skip;
}

template <int K> struct Hooks {
  static ContextOriginals& o() { return K ? g_def : g_imm; }
  static void note(ID3D11DeviceContext*) {
    InterlockedIncrement64(&g_draws_total);
    if (K) InterlockedIncrement64(&g_draws_deferred);
  }
  static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    note(c);
    bool skip = K == 0 && before_draw(c, true, n, start, base);
    if (g_out) { Lock l; g_in_census_call = true; record_draw(c, skip ? "DrawIndexed(skipped)" : "DrawIndexed", n, start, base, 1); g_in_census_call = false; }
    if (!skip) o().DrawIndexed(c, n, start, base);
  }
  static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT n, UINT start) {
    note(c);
    bool skip = K == 0 && before_draw(c, false, n, start, 0);
    if (g_out) { Lock l; g_in_census_call = true; record_draw(c, skip ? "Draw(skipped)" : "Draw", n, start, 0, 1); g_in_census_call = false; }
    if (!skip) o().Draw(c, n, start);
  }
  static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base, UINT start_inst) {
    note(c);
    if (g_out) { Lock l; g_in_census_call = true; record_draw(c, "DrawIndexedInstanced", n, start, base, inst); g_in_census_call = false; }
    o().DrawIndexedInstanced(c, n, inst, start, base, start_inst);
  }
  static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT start_inst) {
    note(c);
    if (g_out) { Lock l; g_in_census_call = true; record_draw(c, "DrawInstanced", n, start, 0, inst); g_in_census_call = false; }
    o().DrawInstanced(c, n, inst, start, start_inst);
  }
  static void STDMETHODCALLTYPE ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT color[4]) {
    if (g_out) {
      Lock l;
      emit(fmt("{\"ev\":\"clear_rt\",\"seq\":%u,\"deferred\":%d,\"target\":%s,\"color\":[%.3f,%.3f,%.3f,%.3f]}", g_seq++, K,
               view_json(v).c_str(), color[0], color[1], color[2], color[3]));
    }
    o().ClearRTV(c, v, color);
  }
  static void STDMETHODCALLTYPE ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil) {
    if (g_out) {
      Lock l;
      emit(fmt("{\"ev\":\"clear_ds\",\"seq\":%u,\"deferred\":%d,\"target\":%s,\"flags\":%u,\"depth\":%.3f}", g_seq++, K,
               view_json(v).c_str(), flags, depth));
    }
    o().ClearDSV(c, v, flags, depth, stencil);
  }
  static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
    InterlockedIncrement64(&g_command_lists);
    if (g_out) { Lock l; emit(fmt("{\"ev\":\"execute_command_list\",\"seq\":%u,\"list\":\"%p\",\"restore\":%d}", g_seq++, (void*)list, (int)restore)); }
    o().ExecuteCommandList(c, list, restore);
  }
  static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_out && !g_in_census_call) {
      Lock l;
      emit(fmt("{\"ev\":\"copy\",\"seq\":%u,\"deferred\":%d,\"dst\":%s,\"src\":%s}", g_seq++, K, resource_json(dst).c_str(), resource_json(src).c_str()));
    }
    o().CopyResource(c, dst, src);
  }
};

template <int K> static bool hook_context(ID3D11DeviceContext* ctx) {
  ContextOriginals& o = K ? g_def : g_imm;
  bool ok = true;
  ok &= patch_vtable(ctx, 12, &Hooks<K>::DrawIndexed, &o.DrawIndexed);
  ok &= patch_vtable(ctx, 13, &Hooks<K>::Draw, &o.Draw);
  ok &= patch_vtable(ctx, 20, &Hooks<K>::DrawIndexedInstanced, &o.DrawIndexedInstanced);
  ok &= patch_vtable(ctx, 21, &Hooks<K>::DrawInstanced, &o.DrawInstanced);
  ok &= patch_vtable(ctx, 47, &Hooks<K>::CopyResource, &o.CopyResource);
  ok &= patch_vtable(ctx, 50, &Hooks<K>::ClearRTV, &o.ClearRTV);
  ok &= patch_vtable(ctx, 53, &Hooks<K>::ClearDSV, &o.ClearDSV);
  ok &= patch_vtable(ctx, 58, &Hooks<K>::ExecuteCommandList, &o.ExecuteCommandList);
  return ok;
}

// ---- device hooks ------------------------------------------------------------------------

using PFN_CreateVertexShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_CreateInputLayout = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
using PFN_CreateDeferredContext = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

static PFN_CreateVertexShader g_orig_CreateVertexShader;
static PFN_CreatePixelShader g_orig_CreatePixelShader;
static PFN_CreateInputLayout g_orig_CreateInputLayout;
static PFN_CreateDeferredContext g_orig_CreateDeferredContext;

static HRESULT STDMETHODCALLTYPE hook_CreateVertexShader(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11VertexShader** out) {
  HRESULT hr = g_orig_CreateVertexShader(dev, code, size, link, out);
  if (SUCCEEDED(hr) && out && *out) shader_register(*out, 'v', code, size);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreatePixelShader(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* link, ID3D11PixelShader** out) {
  HRESULT hr = g_orig_CreatePixelShader(dev, code, size, link, out);
  if (SUCCEEDED(hr) && out && *out) shader_register(*out, 'p', code, size);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* elems, UINT count, const void* code,
                                                        SIZE_T size, ID3D11InputLayout** out) {
  HRESULT hr = g_orig_CreateInputLayout(dev, elems, count, code, size, out);
  if (SUCCEEDED(hr) && out && *out && elems) {
    std::string json = "[";
    for (UINT i = 0; i < count; ++i) {
      if (i) json += ",";
      json += fmt("[\"%s\",%u,%d,%u,%u,%d]", elems[i].SemanticName ? elems[i].SemanticName : "", elems[i].SemanticIndex, (int)elems[i].Format,
                  elems[i].InputSlot, elems[i].AlignedByteOffset, (int)elems[i].InputSlotClass);
    }
    json += "]";
    Lock l;
    g_layouts[*out] = json;
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateDeferredContext(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out) {
  HRESULT hr = g_orig_CreateDeferredContext(dev, flags, out);
  if (SUCCEEDED(hr) && out && *out) {
    bool ok = hook_context<1>(*out);
    Lock l;
    if (++g_deferred_contexts <= 4) logf("census: deferred context %p created, hooks %s", (void*)*out, ok ? "installed" : "FAILED");
  }
  return hr;
}

void census_on_device(ID3D11Device* device, const char* via) {
  if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }
  if (!device || g_device == device) return;
  g_device = device;
  shader_table_load();
  bool ok = true;
  ok &= patch_vtable(device, 11, hook_CreateInputLayout, &g_orig_CreateInputLayout);
  ok &= patch_vtable(device, 12, hook_CreateVertexShader, &g_orig_CreateVertexShader);
  ok &= patch_vtable(device, 15, hook_CreatePixelShader, &g_orig_CreatePixelShader);
  ok &= patch_vtable(device, 27, hook_CreateDeferredContext, &g_orig_CreateDeferredContext);
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  g_immediate = ctx;  // the game keeps it alive; we hold no extra reference
  scene_set_device(device, ctx);
  if (ctx) { ok &= hook_context<0>(ctx); ok &= shadow_install(ctx); ok &= shadow_install_device(device); ctx->Release(); }
  logf("census: device %p hooked via %s: %s", (void*)device, via, ok ? "ok" : "FAILED");
}

// ---- D3D11CreateDevice import hook --------------------------------------------------------

using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                               ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDevice g_orig_D3D11CreateDevice;

static HRESULT WINAPI hook_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels,
                                             UINT level_count, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
  HRESULT hr = g_orig_D3D11CreateDevice(adapter, type, sw, flags, levels, level_count, sdk, device, level, ctx);
  logf("D3D11CreateDevice(flags 0x%x) -> 0x%08lx device %p", flags, (unsigned long)hr, device ? (void*)*device : nullptr);
  if (SUCCEEDED(hr) && device && *device) census_on_device(*device, "D3D11CreateDevice");
  return hr;
}

void census_install_import_hook() {
  if (g_orig_D3D11CreateDevice) return;
  if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }
  BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) { logf("census: host has no import table"); return; }
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "d3d11.dll") != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++thunks) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(by_name->Name), "D3D11CreateDevice") != 0) continue;
      DWORD old = 0;
      if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) { logf("census: cannot unprotect import slot"); return; }
      g_orig_D3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(thunks->u1.Function);
      thunks->u1.Function = reinterpret_cast<ULONG_PTR>(&hook_D3D11CreateDevice);
      VirtualProtect(&thunks->u1.Function, sizeof(void*), old, &old);
      logf("census: D3D11CreateDevice import hooked");
      return;
    }
  }
  logf("census: D3D11CreateDevice import not found; will hook at swap-chain creation");
}

// ---- frame control -----------------------------------------------------------------------

static std::wstring output_dir() {
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"SS2RTX_LOG", path, (DWORD)(sizeof path / sizeof path[0]));
  if (n == 0 || n >= sizeof path / sizeof path[0])   // installed: next to the log, in the game's ss2rtx folder
    return GetFileAttributesW(L"ss2rtx") != INVALID_FILE_ATTRIBUTES ? L"ss2rtx\\" : L"";
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return L"";
  slash[1] = 0;
  return path;
}

void census_capture_next_frame() {
  if (!g_lock_ready) return;
  Lock l;
  g_capture_at.insert(g_capture_at.begin(), 0.0);
}

void census_on_present(double seconds) {
  if (!g_lock_ready) return;
  Lock l;
  if (!g_schedule_parsed) {
    g_schedule_parsed = true;
    char spec[256];
    DWORD n = GetEnvironmentVariableA("SS2RTX_CENSUS_AT", spec, sizeof spec);
    if (n > 0 && n < sizeof spec) {
      char* ctx = nullptr;
      for (char* tok = strtok_s(spec, ",;", &ctx); tok; tok = strtok_s(nullptr, ",;", &ctx)) g_capture_at.push_back(atof(tok));
      logf("census: %zu capture(s) scheduled", g_capture_at.size());
    }
  }
  if (g_out) {
    emit(fmt("{\"ev\":\"present\",\"seq\":%u,\"t\":%.3f,\"draws_total\":%lld,\"draws_deferred\":%lld,\"command_lists\":%lld,\"deferred_contexts\":%u}",
             g_seq, seconds, (long long)g_draws_total, (long long)g_draws_deferred, (long long)g_command_lists, g_deferred_contexts));
    // Identity tables, so each capture file is self-contained.
    for (const auto& kv : g_layouts) emit(fmt("{\"ev\":\"layout\",\"ptr\":\"%p\",\"elements\":", kv.first) + kv.second + "}");
    shader_for_each([](void* ptr, const ShaderRecord& rec, void*) {
      emit(fmt("{\"ev\":\"shader\",\"ptr\":\"%p\",\"kind\":\"%c\",\"id\":\"%s\",\"size\":%zu}", ptr, rec.kind, rec.id.c_str(), rec.size));
    }, nullptr);
    fclose(g_out);
    g_out = nullptr;
    logf("census: capture %d written, %u events", g_capture_index, g_seq);
    ++g_capture_index;
  }
  if (!g_capture_at.empty() && seconds >= g_capture_at.front()) {
    g_capture_at.erase(g_capture_at.begin());
    wchar_t name[64];
    swprintf(name, 64, L"census-%d.jsonl", g_capture_index);
    g_out = _wfopen((output_dir() + name).c_str(), L"w");
    g_seq = 0;
    g_blob_ids.clear();
    if (g_out) emit(fmt("{\"ev\":\"frame_start\",\"t\":%.3f,\"layouts_known\":%zu}", seconds, g_layouts.size()));
    else logf("census: could not open capture file");
  }
}

}  // namespace ss2rtx
