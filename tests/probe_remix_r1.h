// Start the Remix 1.6.x runtime while keeping ownership of its D3D9 device, so a probe can
// create surfaces, shared textures and queries on it. Include after <d3d9.h>,
// <remix/remix_c.h> and "probe_common.h".
#pragma once

struct RemixR1 {
  remixapi_Interface api = {};
  HMODULE dll = nullptr;
  IDirect3D9Ex* d3d9 = nullptr;
  IDirect3DDevice9Ex* dev = nullptr;
  D3DPRESENT_PARAMETERS pp = {};
};

static bool remix_r1_start(const wchar_t* d3d9_path, HWND hwnd, int w, int h, RemixR1* out) {
  probe_use_dll_dir_of(d3d9_path);
  remixapi_ErrorCode r = remixapi_lib_loadRemixDllAndInitialize(d3d9_path, &out->api, &out->dll);
  probe_log("loadRemixDllAndInitialize -> %d", r);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return false;
  r = out->api.dxvk_CreateD3D9(FALSE, &out->d3d9);
  probe_log("dxvk_CreateD3D9 -> %d", r);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS || !out->d3d9) return false;
  out->pp.BackBufferWidth = w;
  out->pp.BackBufferHeight = h;
  out->pp.BackBufferFormat = D3DFMT_UNKNOWN;
  out->pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  out->pp.hDeviceWindow = hwnd;
  out->pp.Windowed = TRUE;
  HRESULT hr = out->d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &out->pp, nullptr, &out->dev);
  probe_log("CreateDeviceEx %dx%d -> 0x%08lx", w, h, (unsigned long)hr);
  if (FAILED(hr) || !out->dev) return false;
  r = out->api.dxvk_RegisterD3D9Device(out->dev);
  probe_log("dxvk_RegisterD3D9Device -> %d", r);
  return r == REMIXAPI_ERROR_CODE_SUCCESS;
}

// Blocks until the Remix device's GPU has finished everything queued so far.
static bool remix_r1_wait_gpu(IDirect3DQuery9* event_query, double timeout_ms) {
  event_query->Issue(D3DISSUE_END);
  BOOL done = FALSE;
  double t0 = probe_now_ms();
  for (;;) {
    HRESULT hr = event_query->GetData(&done, sizeof done, D3DGETDATA_FLUSH);
    if (hr == S_OK) return true;
    if (hr != S_FALSE || probe_now_ms() - t0 > timeout_ms) return false;
    YieldProcessor();
  }
}
