// Phase 0 probe 3 (gate G0): Remix and a D3D11 device in one process, with Remix's image
// handed to D3D11 on the GPU through a shared texture (hand-off H1).
//
//   D3D11 device:      CreateTexture2D(MISC_SHARED) -> GetSharedHandle
//   Remix D3D9 device: CreateTexture(RENDERTARGET, &handle) imports it <- dxvk_CopyRenderingOutput
//   D3D11 device:      CopyResource from the shared texture into a "world" render target
//
// The opposite direction (Remix exports, D3D11 opens) does not work on Proton 11: Remix's
// DXVK fork attaches the texture description through the old \\.\SharedGpuResource ioctl,
// which Proton replaced with D3DKMT, so D3D11 finds no description on the handle. Pass
// "d3d9" as the 6th argument to reproduce that failure.
//
// Every frame is handed over. Every Nth frame the D3D11 copy is read back and compared
// byte-for-byte with a CPU readback taken on the Remix side, which catches stale or torn
// frames. Half way through, both devices are resized and the hand-off is rebuilt.
//
// Usage: t03_handoff.exe <Remix d3d9.dll> [seconds] [WxH] [verify_every] [show|hide] [d3d11|d3d9]
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <remix/remix_c.h>
#include "probe_common.h"
#include "probe_scene.h"
#include "probe_remix_r1.h"

struct Handoff {
  int w = 0, h = 0;
  IDirect3DTexture9* shared_tex = nullptr;   // on the Remix device
  IDirect3DSurface9* shared_surf = nullptr;
  IDirect3DSurface9* sys_surf = nullptr;     // CPU reference readback
  IDirect3DQuery9* event_query = nullptr;
  HANDLE handle = nullptr;
  ID3D11Texture2D* d11_shared = nullptr;     // the same memory, opened on D3D11
  ID3D11Texture2D* d11_world = nullptr;      // stands in for the game's world render target
  ID3D11Texture2D* d11_staging = nullptr;
};

template <typename T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void handoff_destroy(Handoff* h) {
  release(h->d11_staging); release(h->d11_world); release(h->d11_shared);
  release(h->event_query); release(h->sys_surf); release(h->shared_surf); release(h->shared_tex);
  h->handle = nullptr;
}

static bool g_export_from_d3d9 = false;

static bool handoff_create(RemixR1& remix, ID3D11Device* d11, int w, int h, Handoff* out) {
  out->w = w; out->h = h;
  HRESULT hr;
  D3D11_TEXTURE2D_DESC desc = {};
  if (g_export_from_d3d9) {
    hr = remix.dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                  &out->shared_tex, &out->handle);
    probe_log("D3D9 CreateTexture(export, %dx%d) -> 0x%08lx handle %p", w, h, (unsigned long)hr, (void*)out->handle);
    if (FAILED(hr) || !out->handle) return false;
    hr = d11->OpenSharedResource(out->handle, __uuidof(ID3D11Texture2D), (void**)&out->d11_shared);
    probe_log("D3D11 OpenSharedResource -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr) || !out->d11_shared) return false;
    out->d11_shared->GetDesc(&desc);
  } else {
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = d11->CreateTexture2D(&desc, nullptr, &out->d11_shared);
    probe_log("D3D11 CreateTexture2D(shared, %dx%d) -> 0x%08lx", w, h, (unsigned long)hr);
    if (FAILED(hr)) return false;
    IDXGIResource* res = nullptr;
    hr = out->d11_shared->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    if (SUCCEEDED(hr)) { hr = res->GetSharedHandle(&out->handle); res->Release(); }
    probe_log("D3D11 GetSharedHandle -> 0x%08lx handle %p", (unsigned long)hr, (void*)out->handle);
    if (FAILED(hr) || !out->handle) return false;
    HANDLE import = out->handle;
    hr = remix.dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                  &out->shared_tex, &import);
    probe_log("D3D9 CreateTexture(import) -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr) || !out->shared_tex) return false;
  }
  out->shared_tex->GetSurfaceLevel(0, &out->shared_surf);
  hr = remix.dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &out->sys_surf, nullptr);
  if (FAILED(hr)) { probe_log("CreateOffscreenPlainSurface -> 0x%08lx", (unsigned long)hr); return false; }
  hr = remix.dev->CreateQuery(D3DQUERYTYPE_EVENT, &out->event_query);
  if (FAILED(hr)) { probe_log("CreateQuery(EVENT) -> 0x%08lx", (unsigned long)hr); return false; }

  D3D11_TEXTURE2D_DESC world = desc;
  world.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  world.MiscFlags = 0;
  hr = d11->CreateTexture2D(&world, nullptr, &out->d11_world);
  D3D11_TEXTURE2D_DESC staging = world;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  HRESULT hr2 = d11->CreateTexture2D(&staging, nullptr, &out->d11_staging);
  if (FAILED(hr) || FAILED(hr2)) { probe_log("D3D11 CreateTexture2D -> 0x%08lx 0x%08lx", (unsigned long)hr, (unsigned long)hr2); return false; }
  return true;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { probe_log("usage: t03_handoff <d3d9.dll> [seconds] [WxH] [verify_every] [show]"); return 2; }
  double seconds = argc >= 3 ? _wtof(argv[2]) : 20.0;
  int W = 1280, H = 720;
  if (argc >= 4) swscanf(argv[3], L"%dx%d", &W, &H);
  int verify_every = argc >= 5 ? _wtoi(argv[4]) : 50;
  bool show = argc >= 6 && !lstrcmpiW(argv[5], L"show");
  g_export_from_d3d9 = argc >= 7 && !lstrcmpiW(argv[6], L"d3d9");
  probe_log("t03_handoff: %dx%d for %.0fs, verifying every %d frames", W, H, seconds, verify_every);

  HWND hwnd = probe_create_window(L"ss2rtx t03 handoff", 1280, 720);
  RemixR1 remix;
  if (!remix_r1_start(argv[1], hwnd, W, H, &remix)) { probe_log("RESULT FAIL (remix start)"); return 3; }

  ID3D11Device* d11 = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, &level, &ctx);
  probe_log("D3D11CreateDevice -> 0x%08lx feature level 0x%x", (unsigned long)hr, (int)level);
  if (FAILED(hr)) { probe_log("RESULT FAIL (d3d11)"); return 4; }
  {
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad = {};
    if (SUCCEEDED(d11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev)) && SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
      adapter->GetDesc(&ad);
      probe_log("D3D11 adapter: %ls (%u MiB)", ad.Description, (unsigned)(ad.DedicatedVideoMemory >> 20));
    }
    release(adapter); release(dxgi_dev);
    D3DADAPTER_IDENTIFIER9 id9 = {};
    remix.d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id9);
    probe_log("Remix D3D9 adapter: %s", id9.Description);
  }

  ProbeScene scene;
  int failures = probe_scene_create(remix.api, &scene);
  Handoff ho;
  if (!handoff_create(remix, d11, W, H, &ho)) { probe_log("RESULT FAIL (handoff create)"); fflush(NULL); TerminateProcess(GetCurrentProcess(), 5); }

  if (show) { ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd); }
  probe_start_watchdog(8000);

  const double start_ms = probe_now_ms();
  double last_report = start_ms, handoff_ms_total = 0, handoff_ms_max = 0;
  int frames = 0, frames_at_report = 0, verified = 0, mismatched = 0, dark = 0, sync_failures = 0;
  bool resized = false, resized_back = false;
  int cur_w = W, cur_h = H;

  for (;;) {
    PROBE_STAGE("pump");
    double now = probe_now_ms();
    double t = (now - start_ms) / 1000.0;
    if (!probe_pump() || t >= seconds) break;

    // Resize both sides once at 50% (to 3/4 size) and back at 75%, rebuilding the hand-off.
    bool want_small = t >= seconds * 0.5 && t < seconds * 0.75;
    if ((want_small && !resized) || (!want_small && resized && !resized_back)) {
      PROBE_STAGE("resize");
      if (want_small) resized = true; else resized_back = true;
      cur_w = want_small ? W * 3 / 4 : W;
      cur_h = want_small ? H * 3 / 4 : H;
      ctx->ClearState(); ctx->Flush();
      handoff_destroy(&ho);
      remix.pp.BackBufferWidth = cur_w; remix.pp.BackBufferHeight = cur_h;
      hr = remix.dev->ResetEx(&remix.pp, nullptr);
      probe_log("frame %d: ResetEx to %dx%d -> 0x%08lx", frames, cur_w, cur_h, (unsigned long)hr);
      if (FAILED(hr) || !handoff_create(remix, d11, cur_w, cur_h, &ho)) { ++failures; break; }
    }

    PROBE_STAGE("draw");
    failures += probe_scene_draw(remix.api, scene, (float)cur_w / cur_h, 2.5f * sinf(frames * 0.05f));
    PROBE_STAGE("Present");
    // Device Present, not remixapi_Present: the latter resets the swapchain to the window size.
    hr = remix.dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    failures += FAILED(hr);

    // --- hand-off, as the adapter would do it at the world/weapon boundary ---
    double h0 = probe_now_ms();
    PROBE_STAGE("CopyRenderingOutput");
    failures += remix.api.dxvk_CopyRenderingOutput(ho.shared_surf, REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR) != REMIXAPI_ERROR_CODE_SUCCESS;
    PROBE_STAGE("wait Remix GPU");
    if (!remix_r1_wait_gpu(ho.event_query, 5000)) ++sync_failures;
    PROBE_STAGE("D3D11 CopyResource");
    ctx->CopyResource(ho.d11_world, ho.d11_shared);
    ctx->Flush();
    double h_ms = probe_now_ms() - h0;
    handoff_ms_total += h_ms;
    if (h_ms > handoff_ms_max) handoff_ms_max = h_ms;
    ++frames;

    if (frames % verify_every == 0) {
      PROBE_STAGE("verify");
      ctx->CopyResource(ho.d11_staging, ho.d11_world);
      D3D11_MAPPED_SUBRESOURCE map = {};
      hr = ctx->Map(ho.d11_staging, 0, D3D11_MAP_READ, 0, &map);
      HRESULT hr9 = remix.dev->GetRenderTargetData(ho.shared_surf, ho.sys_surf);
      D3DLOCKED_RECT lr = {};
      if (SUCCEEDED(hr9)) hr9 = ho.sys_surf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
      if (SUCCEEDED(hr) && SUCCEEDED(hr9)) {
        size_t diff = 0;
        for (int y = 0; y < cur_h; ++y) {
          const uint8_t* a = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
          const uint8_t* b = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
          for (int x = 0; x < cur_w * 4; ++x) diff += a[x] != b[x];
        }
        ProbeImageStats st = probe_image_stats((const uint8_t*)map.pData, cur_w, cur_h, map.RowPitch);
        ++verified;
        mismatched += diff != 0;
        dark += st.lit_percent < 5.0;
        if (verified <= 3 || diff != 0 || (resized && verified % 20 == 0))
          probe_log("verify frame %d (%dx%d): differing bytes %zu, lit %.1f%% mean rgb %.1f %.1f %.1f", frames, cur_w, cur_h,
                    diff, st.lit_percent, st.mean_r, st.mean_g, st.mean_b);
        if (verified == 3) probe_save_bmp("d3d11_view.bmp", (const uint8_t*)map.pData, cur_w, cur_h, map.RowPitch);
      } else {
        probe_log("verify frame %d FAILED: Map 0x%08lx readback 0x%08lx", frames, (unsigned long)hr, (unsigned long)hr9);
        ++failures;
      }
      if (SUCCEEDED(hr9)) ho.sys_surf->UnlockRect();
      if (SUCCEEDED(hr)) ctx->Unmap(ho.d11_staging, 0);
    }

    if (now - last_report >= 5000.0) {
      probe_log("t=%.0fs frames=%d fps=%.1f verified=%d mismatched=%d", t, frames,
                (frames - frames_at_report) * 1000.0 / (now - last_report), verified, mismatched);
      last_report = now; frames_at_report = frames;
    }
  }

  double elapsed = (probe_now_ms() - start_ms) / 1000.0;
  probe_log("loop done: frames=%d (%.1f fps avg), hand-off mean %.2f ms max %.1f ms", frames, frames / elapsed,
            frames ? handoff_ms_total / frames : 0.0, handoff_ms_max);
  probe_log("verified=%d mismatched=%d dark=%d sync_failures=%d api_failures=%d resized=%d/%d", verified, mismatched, dark,
            sync_failures, failures, (int)resized, (int)resized_back);

  PROBE_STAGE("teardown");
  ctx->ClearState(); ctx->Flush();
  handoff_destroy(&ho);
  release(ctx);
  ULONG d11_left = d11->Release();
  remixapi_ErrorCode r = remix.api.Shutdown();
  probe_log("teardown: D3D11 device refs left %lu, Remix Shutdown -> %d", d11_left, r);

  bool pass = failures == 0 && sync_failures == 0 && verified > 0 && mismatched == 0 && dark == 0 && resized && resized_back;
  probe_log(pass ? "RESULT PASS" : "RESULT FAIL");
  fflush(NULL);
  TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
  return 0;
}
