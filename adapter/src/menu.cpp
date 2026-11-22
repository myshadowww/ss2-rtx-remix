#include "menu.h"

#include <d3d11.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "remix_link.h"

namespace ss2rtx {

// ---- keys --------------------------------------------------------------------------------
//
// The game reads its keyboard from window messages, so a message filter on its thread can
// keep a key to ourselves: the menu key always, and the navigation keys while the panel is
// open (otherwise the arrow keys would walk the player about behind it).

static UINT g_menu_key = VK_F11;
static volatile LONG g_open;
static LONG g_keys[32];
static volatile LONG g_keys_written, g_keys_read;

static bool navigation_key(UINT vk) {
  return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_PRIOR || vk == VK_NEXT;
}

static LRESULT CALLBACK key_filter(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && wparam == PM_REMOVE) {
    MSG* m = reinterpret_cast<MSG*>(lparam);
    if (m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP) {
      UINT vk = (UINT)m->wParam;
      if (vk == g_menu_key || (g_open && navigation_key(vk))) {
        if (m->message == WM_KEYDOWN) {
          bool repeat = (m->lParam >> 30) & 1;
          if (vk == g_menu_key) {
            if (!repeat) InterlockedExchange(&g_open, g_open ? 0 : 1);
          } else if (vk == VK_ESCAPE) {
            InterlockedExchange(&g_open, 0);
          } else {
            LONG at = g_keys_written;
            g_keys[at & 31] = (LONG)vk;
            InterlockedExchange(&g_keys_written, at + 1);
          }
        }
        m->message = WM_NULL;
      }
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

void menu_install(HWND game_window) {
  static bool installed;
  if (installed || !game_window) return;
  installed = true;
  g_menu_key = (UINT)config_number(L"MenuKey", VK_F11);
  if (!g_menu_key) return;
  HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, key_filter, nullptr, GetWindowThreadProcessId(game_window, nullptr));
  logf("menu: settings panel on key %u (%s)", g_menu_key, hook ? "key filter installed" : "key filter FAILED");
}

// ---- the panel ---------------------------------------------------------------------------
//
// Drawn with GDI into a bitmap and copied onto the back buffer: no shaders needed, and an
// opaque rectangle needs no blending.

struct Canvas {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  uint32_t* bits = nullptr;
  HFONT font = nullptr, bold = nullptr;
  int w = 0, h = 0, px = 0;
};
static Canvas g_canvas;
static ID3D11Texture2D* g_texture;
static DXGI_FORMAT g_texture_format;
static int g_selected = -1;
static bool g_dirty = true;
static std::wstring g_status_drawn;

static void canvas_release() {
  if (g_canvas.font) DeleteObject(g_canvas.font);
  if (g_canvas.bold) DeleteObject(g_canvas.bold);
  if (g_canvas.bitmap) DeleteObject(g_canvas.bitmap);
  if (g_canvas.dc) DeleteDC(g_canvas.dc);
  g_canvas = Canvas();
  if (g_texture) { g_texture->Release(); g_texture = nullptr; }
}

static bool canvas_ensure(ID3D11Device* dev, int w, int h, int px, DXGI_FORMAT format) {
  if (g_canvas.dc && g_canvas.w == w && g_canvas.h == h && g_canvas.px == px && g_texture && g_texture_format == format) return true;
  canvas_release();
  g_canvas.dc = CreateCompatibleDC(nullptr);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  g_canvas.bitmap = g_canvas.dc ? CreateDIBSection(g_canvas.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!g_canvas.bitmap || !bits) { canvas_release(); return false; }
  SelectObject(g_canvas.dc, g_canvas.bitmap);
  g_canvas.bits = static_cast<uint32_t*>(bits);
  g_canvas.font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");
  g_canvas.bold = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Tahoma");
  g_canvas.w = w; g_canvas.h = h; g_canvas.px = px;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_texture))) { canvas_release(); return false; }
  g_texture_format = format;
  g_dirty = true;
  return true;
}

static void fill(int x0, int y0, int x1, int y1, COLORREF color) {
  RECT r = { x0, y0, x1, y1 };
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(g_canvas.dc, &r, brush);
  DeleteObject(brush);
}

static void text(const std::wstring& s, int x0, int y0, int x1, int y1, COLORREF color, UINT align, HFONT font) {
  RECT r = { x0, y0, x1, y1 };
  SelectObject(g_canvas.dc, font);
  SetBkMode(g_canvas.dc, TRANSPARENT);
  SetTextColor(g_canvas.dc, color);
  DrawTextW(g_canvas.dc, s.c_str(), (int)s.size(), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static void paint(const std::wstring& status) {
  const int px = g_canvas.px, row = px * 3 / 2, pad = px;
  const COLORREF back = RGB(8, 14, 20), edge = RGB(0, 200, 170), plain = RGB(214, 222, 226), value = RGB(150, 240, 200), dim = RGB(110, 130, 138);
  fill(0, 0, g_canvas.w, g_canvas.h, edge);
  fill(2, 2, g_canvas.w - 2, g_canvas.h - 2, back);
  int y = pad / 2;
  text(L"System Shock 2: RTX Remix", pad, y, g_canvas.w - pad, y + row + px / 2, edge, DT_LEFT, g_canvas.bold);
  y += row + px / 2;
  int count = remix_settings_count();
  for (int i = 0; i < count; ++i) {
    std::wstring label, val;
    bool heading = remix_settings_row(i, &label, &val);
    if (heading) {
      y += px / 3;
      text(label, pad, y, g_canvas.w - pad, y + row, dim, DT_LEFT, g_canvas.bold);
    } else {
      bool selected = i == g_selected;
      if (selected) fill(pad / 2, y, g_canvas.w - pad / 2, y + row, RGB(0, 84, 74));
      text(label, pad * 3 / 2, y, g_canvas.w * 3 / 5, y + row, selected ? RGB(255, 255, 255) : plain, DT_LEFT, g_canvas.font);
      text(selected ? L"<  " + val + L"  >" : val, g_canvas.w * 2 / 5, y, g_canvas.w - pad * 3 / 2, y + row, selected ? RGB(255, 255, 255) : value, DT_RIGHT,
           g_canvas.font);
    }
    y += row;
  }
  y += px / 2;
  text(status, pad, y, g_canvas.w - pad, y + row, value, DT_LEFT, g_canvas.font);
  y += row;
  text(L"Up/Down: choose    Left/Right: change    F11 or Esc: close", pad, y, g_canvas.w - pad, y + row, dim, DT_LEFT, g_canvas.font);
  GdiFlush();
  // GDI leaves the fourth byte alone; the back buffer wants it opaque, and maybe red and blue the other way round.
  bool rgba = g_texture_format == DXGI_FORMAT_R8G8B8A8_UNORM || g_texture_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  size_t n = (size_t)g_canvas.w * g_canvas.h;
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = g_canvas.bits[i];
    if (rgba) c = (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
    g_canvas.bits[i] = c | 0xFF000000u;
  }
}

static int panel_height(int px) {
  int count = remix_settings_count(), headings = 0;
  for (int i = 0; i < count; ++i) {
    std::wstring a, b;
    headings += remix_settings_row(i, &a, &b);
  }
  int row = px * 3 / 2;
  return px / 2 + row + px / 2 + count * row + headings * (px / 3) + px / 2 + row * 2 + px / 2;
}

static void move_selection(int direction) {
  int count = remix_settings_count();
  for (int step = 0; step < count; ++step) {
    g_selected = ((g_selected + direction) % count + count) % count;
    std::wstring a, b;
    if (!remix_settings_row(g_selected, &a, &b)) return;
  }
}

void menu_present(IDXGISwapChain* swapchain) {
  // Keys first: they change what is drawn.
  while (g_keys_read != g_keys_written) {
    UINT vk = (UINT)g_keys[g_keys_read & 31];
    InterlockedIncrement(&g_keys_read);
    if (g_selected < 0) move_selection(+1);
    if (vk == VK_UP) move_selection(-1);
    else if (vk == VK_DOWN) move_selection(+1);
    else if (vk == VK_PRIOR) { for (int i = 0; i < 6; ++i) move_selection(-1); }
    else if (vk == VK_NEXT) { for (int i = 0; i < 6; ++i) move_selection(+1); }
    else if (vk == VK_LEFT) remix_settings_step(g_selected, -1);
    else if (vk == VK_RIGHT || vk == VK_RETURN) remix_settings_step(g_selected, +1);
    g_dirty = true;
  }
  static LONG was_open;
  if (g_open != was_open) {
    was_open = g_open;
    g_dirty = true;
    logf("menu: settings panel %s", was_open ? "opened" : "closed");
  }
  if (!g_open || !swapchain) return;
  if (g_selected < 0) move_selection(+1);

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
  D3D11_TEXTURE2D_DESC bd;
  back->GetDesc(&bd);
  ID3D11Device* dev = nullptr;
  back->GetDevice(&dev);
  ID3D11DeviceContext* ctx = nullptr;
  if (dev) dev->GetImmediateContext(&ctx);
  bool supported = bd.SampleDesc.Count == 1 &&
                   (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                    bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  static bool complained;
  if (!supported && !complained) {
    complained = true;
    logf("menu: cannot draw on this back buffer (format %d, %u samples)", (int)bd.Format, bd.SampleDesc.Count);
  }
  if (dev && ctx && supported) {
    int px = (int)bd.Height / 60;
    if (px < 11) px = 11;
    int w = px * 40, h = panel_height(px);
    if (w > (int)bd.Width - 8) w = (int)bd.Width - 8;
    if (h > (int)bd.Height - 8) h = (int)bd.Height - 8;
    if (canvas_ensure(dev, w, h, px, bd.Format)) {
      static DWORD status_at;
      std::wstring status = g_status_drawn;
      if (GetTickCount() - status_at > 500 || status.empty()) {   // the timing line ticks over twice a second
        status_at = GetTickCount();
        status = remix_settings_status();
      }
      if (g_dirty || status != g_status_drawn) {
        g_status_drawn = status;
        paint(status);
        ctx->UpdateSubresource(g_texture, 0, nullptr, g_canvas.bits, (UINT)w * 4, 0);
        g_dirty = false;
      }
      UINT x = bd.Width / 40, y = (bd.Height - (UINT)h) / 2;
      ctx->CopySubresourceRegion(back, 0, x, y, 0, g_texture, 0, nullptr);
    }
  }
  if (ctx) ctx->Release();
  if (dev) dev->Release();
  back->Release();
}

}  // namespace ss2rtx
