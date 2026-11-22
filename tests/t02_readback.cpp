// Phase 0 probe 2: read Remix's rendered image back inside the process (Remix 1.6.1, API 0.6.x).
// Proves what the picture actually contains without relying on a desktop screenshot, and
// measures the slow CPU hand-off path (H4) as a correctness baseline.
// Usage: t02_readback.exe <path to Remix d3d9.dll> [seconds] [show]
// The window stays hidden unless "show" is given.
#include <d3d9.h>
#include <remix/remix_c.h>
#include "probe_common.h"
#include "probe_scene.h"

static remixapi_Interface g_remix;
static HMODULE g_remix_dll;

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { probe_log("usage: t02_readback <d3d9.dll path> [seconds] [show]"); return 2; }
  double seconds = argc >= 3 ? _wtof(argv[2]) : 20.0;
  bool show = argc >= 4 && !lstrcmpiW(argv[3], L"show");
  const int W = 1280, H = 720;
  probe_log("t02_readback: header API %d.%d.%d", REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

  HWND hwnd = probe_create_window(L"ss2rtx t02 readback", W, H);
  probe_use_dll_dir_of(argv[1]);
  remixapi_ErrorCode r = remixapi_lib_loadRemixDllAndInitialize(argv[1], &g_remix, &g_remix_dll);
  probe_log("loadRemixDllAndInitialize -> %d", r);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return 4;

  // Own the D3D9 device ourselves (instead of Startup) so we can create surfaces on it.
  IDirect3D9Ex* d3d9 = nullptr;
  r = g_remix.dxvk_CreateD3D9(FALSE, &d3d9);
  probe_log("dxvk_CreateD3D9 -> %d (%p)", r, (void*)d3d9);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS || !d3d9) return 5;

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  IDirect3DDevice9Ex* dev = nullptr;
  HRESULT hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &dev);
  probe_log("CreateDeviceEx -> 0x%08lx (%p)", (unsigned long)hr, (void*)dev);
  if (FAILED(hr) || !dev) return 6;
  r = g_remix.dxvk_RegisterD3D9Device(dev);
  probe_log("dxvk_RegisterD3D9Device -> %d", r);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return 7;

  ProbeScene scene;
  int failures = probe_scene_create(g_remix, &scene);

  IDirect3DTexture9* rt_tex = nullptr;
  IDirect3DSurface9* rt_surf = nullptr;
  IDirect3DSurface9* sys_surf = nullptr;
  hr = dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt_tex, nullptr);
  probe_log("CreateTexture(render target) -> 0x%08lx", (unsigned long)hr);
  if (SUCCEEDED(hr)) hr = rt_tex->GetSurfaceLevel(0, &rt_surf);
  HRESULT hr2 = dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_surf, nullptr);
  probe_log("CreateOffscreenPlainSurface(sysmem) -> 0x%08lx", (unsigned long)hr2);
  if (FAILED(hr) || FAILED(hr2)) return 8;

  if (show) { ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd); }
  probe_start_watchdog(8000);

  double start_ms = probe_now_ms(), next_sample = start_ms + 2000.0;
  int frames = 0, samples = 0, lit_samples = 0;
  double readback_ms_total = 0;
  for (;;) {
    PROBE_STAGE("pump");
    double now = probe_now_ms();
    if (!probe_pump() || now - start_ms >= seconds * 1000.0) break;
    PROBE_STAGE("draw");
    failures += probe_scene_draw(g_remix, scene, (float)W / H, 2.5f * sinf((float)((now - start_ms) / 700.0)));
    PROBE_STAGE("Present");
    failures += g_remix.Present(NULL) != REMIXAPI_ERROR_CODE_SUCCESS;
    ++frames;

    if (now >= next_sample) {
      next_sample = now + 2000.0;
      PROBE_STAGE("CopyRenderingOutput");
      double t0 = probe_now_ms();
      r = g_remix.dxvk_CopyRenderingOutput(rt_surf, REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR);
      PROBE_STAGE("GetRenderTargetData");
      hr = dev->GetRenderTargetData(rt_surf, sys_surf);
      D3DLOCKED_RECT lr = {};
      PROBE_STAGE("LockRect");
      hr2 = sys_surf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
      double ms = probe_now_ms() - t0;
      if (r == REMIXAPI_ERROR_CODE_SUCCESS && SUCCEEDED(hr) && SUCCEEDED(hr2)) {
        ProbeImageStats st = probe_image_stats((const uint8_t*)lr.pBits, W, H, lr.Pitch);
        char name[64];
        snprintf(name, sizeof name, "frame_%02d.bmp", samples);
        probe_save_bmp(name, (const uint8_t*)lr.pBits, W, H, lr.Pitch);
        probe_log("sample %d at %.1fs frame %d: lit %.1f%% mean rgb %.1f %.1f %.1f, readback %.1f ms -> %s",
                  samples, (now - start_ms) / 1000.0, frames, st.lit_percent, st.mean_r, st.mean_g, st.mean_b, ms, name);
        lit_samples += st.lit_percent > 5.0;
        readback_ms_total += ms;
        sys_surf->UnlockRect();
      } else {
        probe_log("sample %d FAILED: copy %d GetRenderTargetData 0x%08lx LockRect 0x%08lx", samples, r,
                  (unsigned long)hr, (unsigned long)hr2);
        ++failures;
      }
      ++samples;
    }
  }
  double elapsed = (probe_now_ms() - start_ms) / 1000.0;
  probe_log("loop done: frames=%d (%.1f fps avg) samples=%d lit=%d failures=%d mean readback %.1f ms",
            frames, frames / elapsed, samples, lit_samples, failures, samples ? readback_ms_total / samples : 0.0);
  bool pass = failures == 0 && samples > 0 && lit_samples > 0;
  probe_log(pass ? "RESULT PASS" : "RESULT FAIL");
  fflush(NULL);
  TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
  return 0;
}
