// ss2rtx: dxgi.dll proxy for System Shock 2: 25th Anniversary Remaster (KEX, D3D11 mode).
//
// Stage 0 (this file): load the real DXGI, forward every export, and observe swap-chain
// creation and Present so we know the adapter sits in the game's frame loop. No rendering
// is changed. Set SS2RTX_DISABLE=1 for pure pass-through with no hooks at all.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "autopilot.h"
#include "census.h"
#include "config.h"
#include "log.h"
#include "menu.h"
#include "remix_link.h"
#include "scene.h"
#include "vtable.h"

namespace ss2rtx {

using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIDeclareAdapterRemovalSupport = HRESULT(WINAPI*)();
using PFN_DXGIGetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_real_dxgi;
static bool g_disabled;
static PFN_CreateDXGIFactory g_real_CreateDXGIFactory;
static PFN_CreateDXGIFactory g_real_CreateDXGIFactory1;
static PFN_CreateDXGIFactory2 g_real_CreateDXGIFactory2;
static PFN_DXGIDeclareAdapterRemovalSupport g_real_DeclareAdapterRemovalSupport;
static PFN_DXGIGetDebugInterface1 g_real_GetDebugInterface1;

// ---- observed game objects -------------------------------------------------------------

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PFN_CreateSwapChain g_orig_CreateSwapChain;
static PFN_CreateSwapChainForHwnd g_orig_CreateSwapChainForHwnd;
static PFN_Present g_orig_Present;
static PFN_ResizeBuffers g_orig_ResizeBuffers;

static volatile LONG64 g_frames;
static double g_first_present_ms;
static double g_last_report_ms;
static LONG64 g_frames_at_report;
static HWND g_game_hwnd;

static HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
  LONG64 frame = InterlockedIncrement64(&g_frames);
  double now = now_ms();
  if (frame == 1) {
    logf("first Present (sync %u flags 0x%x)", sync_interval, flags);
    g_first_present_ms = now;
    g_last_report_ms = now;
  } else if (now - g_last_report_ms >= 10000.0) {
    logf("frame %lld, %.1f fps over the last %.0f s", (long long)frame,
         (frame - g_frames_at_report) * 1000.0 / (now - g_last_report_ms), (now - g_last_report_ms) / 1000.0);
    g_last_report_ms = now;
    g_frames_at_report = frame;
  }
  // Scroll Lock switches between the path-traced image and the engine's own rendering,
  // Pause/Break switches the glow layers. Both keys are free in the game's bindings (every
  // function key is taken by its quick-use slots); ToggleKey/GlowKey change them.
  static const int toggle_key = (int)config_number(L"ToggleKey", VK_SCROLL);
  static const int glow_key = (int)config_number(L"GlowKey", VK_PAUSE);
  bool focused = GetForegroundWindow() == g_game_hwnd;
  static bool toggle_was_down;
  bool toggle_down = focused && toggle_key > 0 && (GetAsyncKeyState(toggle_key) & 0x8000) != 0;
  if (toggle_down && !toggle_was_down) remix_link_set_enabled(!remix_link_enabled());
  toggle_was_down = toggle_down;
  static bool glow_was_down;
  bool glow_down = focused && glow_key > 0 && (GetAsyncKeyState(glow_key) & 0x8000) != 0;
  if (glow_down && !glow_was_down) remix_link_toggle_glow();
  glow_was_down = glow_down;
  // Insert marks a glitch: the scene's state goes to the log, and that frame's draw stream
  // and path-traced image are saved (MarkKey changes the key).
  static const int mark_key = (int)config_number(L"MarkKey", VK_INSERT);
  static bool mark_was_down;
  bool mark_down = focused && mark_key > 0 && (GetAsyncKeyState(mark_key) & 0x8000) != 0;
  if (mark_down && !mark_was_down) {
    remix_link_mark((now - g_first_present_ms) / 1000.0);
    census_capture_next_frame();
  }
  mark_was_down = mark_down;
  menu_install(g_game_hwnd);
  menu_present(self);   // last thing drawn: the settings panel, on top of the finished frame
  census_on_present((now - g_first_present_ms) / 1000.0);
  remix_link_frame((now - g_first_present_ms) / 1000.0);
  scene_frame_end();
  autopilot_tick(g_game_hwnd, (now - g_first_present_ms) / 1000.0);
  return g_orig_Present(self, sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
  HRESULT hr = g_orig_ResizeBuffers(self, count, w, h, fmt, flags);
  logf("ResizeBuffers(%u buffers, %ux%u, format %d, flags 0x%x) -> 0x%08lx", count, w, h, (int)fmt, flags, (unsigned long)hr);
  return hr;
}

static void on_swapchain_created(IUnknown* device, IDXGISwapChain* swapchain) {
  DXGI_SWAP_CHAIN_DESC desc = {};
  swapchain->GetDesc(&desc);
  g_game_hwnd = desc.OutputWindow;
  remix_link_set_output_size(desc.BufferDesc.Width, desc.BufferDesc.Height);
  logf("swap chain %p: %ux%u format %d, %u buffers, swap effect %d, flags 0x%x, windowed %d, hwnd %p", (void*)swapchain,
       desc.BufferDesc.Width, desc.BufferDesc.Height, (int)desc.BufferDesc.Format, desc.BufferCount, (int)desc.SwapEffect,
       desc.Flags, (int)desc.Windowed, (void*)desc.OutputWindow);

  ID3D11Device* d3d11 = nullptr;
  if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device), (void**)&d3d11))) {
    ID3D11DeviceContext* ctx = nullptr;
    d3d11->GetImmediateContext(&ctx);
    logf("device %p is ID3D11Device: feature level 0x%x, creation flags 0x%x, immediate context %p (type %d)", (void*)d3d11,
         (int)d3d11->GetFeatureLevel(), d3d11->GetCreationFlags(), (void*)ctx, ctx ? (int)ctx->GetType() : -1);
    census_on_device(d3d11, "swap chain creation");
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(d3d11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev)) && SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
      DXGI_ADAPTER_DESC ad = {};
      adapter->GetDesc(&ad);
      logf("adapter: %ls, %u MiB", ad.Description, (unsigned)(ad.DedicatedVideoMemory >> 20));
    }
    if (adapter) adapter->Release();
    if (dxgi_dev) dxgi_dev->Release();
    if (ctx) ctx->Release();
    d3d11->Release();
  } else {
    logf("device %p is NOT an ID3D11Device; the game is probably not in D3D11 mode", (void*)device);
  }

  bool ok = patch_vtable(swapchain, 8, hook_Present, &g_orig_Present);            // IDXGISwapChain::Present
  ok &= patch_vtable(swapchain, 13, hook_ResizeBuffers, &g_orig_ResizeBuffers);   // IDXGISwapChain::ResizeBuffers
  logf("swap chain hooks %s", ok ? "installed" : "FAILED");
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  HRESULT hr = g_orig_CreateSwapChain(self, device, desc, out);
  logf("IDXGIFactory::CreateSwapChain -> 0x%08lx", (unsigned long)hr);
  if (SUCCEEDED(hr) && out && *out) on_swapchain_created(device, *out);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output, IDXGISwapChain1** out) {
  HRESULT hr = g_orig_CreateSwapChainForHwnd(self, device, hwnd, desc, fs, output, out);
  logf("IDXGIFactory2::CreateSwapChainForHwnd -> 0x%08lx", (unsigned long)hr);
  if (SUCCEEDED(hr) && out && *out) on_swapchain_created(device, *out);
  return hr;
}

static void on_factory_created(const char* via, void* factory_unknown) {
  if (g_disabled || !factory_unknown) return;
  IUnknown* unk = static_cast<IUnknown*>(factory_unknown);
  IDXGIFactory* f1 = nullptr;
  if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGIFactory), (void**)&f1))) {
    bool ok = patch_vtable(f1, 10, hook_CreateSwapChain, &g_orig_CreateSwapChain);  // IDXGIFactory::CreateSwapChain
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
      ok &= patch_vtable(f2, 15, hook_CreateSwapChainForHwnd, &g_orig_CreateSwapChainForHwnd);  // IDXGIFactory2::CreateSwapChainForHwnd
      f2->Release();
    }
    logf("%s: factory %p hooks %s", via, (void*)f1, ok ? "installed" : "FAILED");
    f1->Release();
  }
}

// ---- real DXGI ---------------------------------------------------------------------------

static BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID*) {
  log_open();
  wchar_t exe[MAX_PATH] = L"?";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  logf("ss2rtx adapter stage 2 (scene + remix link), built " __DATE__ " " __TIME__);
  logf("host process: %ls", exe);
  wchar_t env[8];
  g_disabled = GetEnvironmentVariableW(L"SS2RTX_DISABLE", env, 8) > 0 && env[0] == L'1';
  if (g_disabled) logf("SS2RTX_DISABLE=1: pass-through only, no hooks");
  else census_install_import_hook();

  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (n == 0 || n > MAX_PATH - 12) { logf("GetSystemDirectory failed"); return TRUE; }
  lstrcatW(path, L"\\dxgi.dll");
  g_real_dxgi = LoadLibraryW(path);
  logf("real DXGI %ls -> %p (error %lu)", path, (void*)g_real_dxgi, g_real_dxgi ? 0UL : GetLastError());
  if (!g_real_dxgi) return TRUE;
  g_real_CreateDXGIFactory = (PFN_CreateDXGIFactory)(void*)GetProcAddress(g_real_dxgi, "CreateDXGIFactory");
  g_real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory)(void*)GetProcAddress(g_real_dxgi, "CreateDXGIFactory1");
  g_real_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)(void*)GetProcAddress(g_real_dxgi, "CreateDXGIFactory2");
  g_real_DeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)(void*)GetProcAddress(g_real_dxgi, "DXGIDeclareAdapterRemovalSupport");
  g_real_GetDebugInterface1 = (PFN_DXGIGetDebugInterface1)(void*)GetProcAddress(g_real_dxgi, "DXGIGetDebugInterface1");
  return TRUE;
}

static void ensure_init() { InitOnceExecuteOnce(&g_init_once, init_once, nullptr, nullptr); }

}  // namespace ss2rtx

using namespace ss2rtx;

extern "C" {

HRESULT WINAPI ss2rtx_CreateDXGIFactory(REFIID riid, void** factory) {
  ensure_init();
  if (!g_real_CreateDXGIFactory) return E_FAIL;
  HRESULT hr = g_real_CreateDXGIFactory(riid, factory);
  if (SUCCEEDED(hr) && factory) on_factory_created("CreateDXGIFactory", *factory);
  return hr;
}

HRESULT WINAPI ss2rtx_CreateDXGIFactory1(REFIID riid, void** factory) {
  ensure_init();
  if (!g_real_CreateDXGIFactory1) return E_FAIL;
  HRESULT hr = g_real_CreateDXGIFactory1(riid, factory);
  if (SUCCEEDED(hr) && factory) on_factory_created("CreateDXGIFactory1", *factory);
  return hr;
}

HRESULT WINAPI ss2rtx_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
  ensure_init();
  if (!g_real_CreateDXGIFactory2) return E_FAIL;
  HRESULT hr = g_real_CreateDXGIFactory2(flags, riid, factory);
  if (SUCCEEDED(hr) && factory) on_factory_created("CreateDXGIFactory2", *factory);
  return hr;
}

HRESULT WINAPI ss2rtx_DXGIDeclareAdapterRemovalSupport(void) {
  ensure_init();
  return g_real_DeclareAdapterRemovalSupport ? g_real_DeclareAdapterRemovalSupport() : S_OK;
}

HRESULT WINAPI ss2rtx_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out) {
  ensure_init();
  return g_real_GetDebugInterface1 ? g_real_GetDebugInterface1(flags, riid, out) : E_NOINTERFACE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  // Nothing heavy under the loader lock: the real DXGI is loaded on the first export call.
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
  // The Remix runtime leaves worker threads behind that keep a process from exiting
  // (README.md). When the game is shutting down with Remix loaded, end the process here.
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr && remix_link_started()) TerminateProcess(GetCurrentProcess(), 0);
  return TRUE;
}

}  // extern "C"
