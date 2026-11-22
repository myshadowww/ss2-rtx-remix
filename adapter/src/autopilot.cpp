// Test-only key script, so automated runs can get from the main menu into a level.
//
//   SS2RTX_KEYS="16:2;19:enter;30:left:1.5"   seconds after the first Present : key [: seconds held]
//   SS2RTX_KEYS="21:click=1036,523"           a left click at client pixel 1036,523 (menus only)
//
// Keys are posted as window messages to the game's own window from inside the game
// process. Nothing is injected at desktop level, so a script can never type into another
// application, whatever has focus. Unset, this file does nothing.
//
// While a script runs, the real keyboard and mouse are kept out of the game (unless
// SS2RTX_LIVEINPUT=1): somebody using the desktop, or a controller that acts as a mouse,
// would otherwise turn the camera of a test.
#include "autopilot.h"

#include <stdint.h>
#include <stdio.h>

#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "census.h"
#include "log.h"
#include "remix_link.h"

namespace ss2rtx {

struct Step {
  double at_s;
  UINT vk;
  bool extended;
  bool down_sent;
  bool up_sent;
  double hold_s;
  char name[24];
  bool click;        // a left click at (x, y) instead of a key
  bool move_sent;
  int x, y;
};

static Step g_steps[512];
static int g_step_count = -1;  // -1: not parsed yet

static HWND g_autopilot_hwnd;
static double g_autopilot_seconds;

// Pseudo-keys that act on the adapter or the window instead of the game: "rtx_on",
// "rtx_off"; "close", which asks the game window to close the way its close button would;
// "minimize" and "restore" (what switching away from the game and back does to it); and
// "size=1600x900", which gives the window a new client size.
static bool is_action(const char* name) {
  return !_stricmp(name, "rtx_on") || !_stricmp(name, "rtx_off") || !_stricmp(name, "close") || !_stricmp(name, "minimize") || !_stricmp(name, "mark") ||
         !_stricmp(name, "restore") || !_strnicmp(name, "size=", 5);
}

static bool adapter_action(const char* name) {
  if (!_stricmp(name, "close")) { PostMessageW(g_autopilot_hwnd, WM_CLOSE, 0, 0); return true; }
  if (!_stricmp(name, "mark")) { remix_link_mark(g_autopilot_seconds); census_capture_next_frame(); return true; }
  if (!_stricmp(name, "minimize")) { ShowWindow(g_autopilot_hwnd, SW_MINIMIZE); return true; }
  if (!_stricmp(name, "restore")) { ShowWindow(g_autopilot_hwnd, SW_RESTORE); return true; }
  if (!_strnicmp(name, "size=", 5)) {
    int w = 0, h = 0;
    if (sscanf(name + 5, "%dx%d", &w, &h) != 2 || w < 320 || h < 200) return false;
    RECT r = { 0, 0, w, h };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(g_autopilot_hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(g_autopilot_hwnd, GWL_EXSTYLE));
    SetWindowPos(g_autopilot_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
  }
  if (!_stricmp(name, "rtx_on")) { remix_link_set_enabled(true); return true; }
  if (!_stricmp(name, "rtx_off")) { remix_link_set_enabled(false); return true; }
  return false;
}

static bool parse_key(const char* name, UINT* vk, bool* extended) {
  static const struct { const char* name; UINT vk; bool ext; } table[] = {
    {"enter", VK_RETURN, false}, {"esc", VK_ESCAPE, false}, {"space", VK_SPACE, false}, {"tab", VK_TAB, false},
    {"up", VK_UP, true}, {"down", VK_DOWN, true}, {"left", VK_LEFT, true}, {"right", VK_RIGHT, true},
    {"f5", VK_F5, false}, {"f9", VK_F9, false}, {"f11", VK_F11, false}, {"pgup", VK_PRIOR, true}, {"pgdn", VK_NEXT, true},
    {"kp_left", VK_NUMPAD4, false}, {"kp_right", VK_NUMPAD6, false}, {"kp_up", VK_NUMPAD8, false}, {"kp_down", VK_NUMPAD2, false},
  };
  for (const auto& e : table) {
    if (!_stricmp(name, e.name)) { *vk = e.vk; *extended = e.ext; return true; }
  }
  if (is_action(name)) { *vk = 0; *extended = false; return true; }
  if (name[0] && !name[1]) {
    char c = name[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { *vk = (UINT)c; *extended = false; return true; }
  }
  return false;
}

static void parse() {
  g_step_count = 0;
  static char script[16384];
  DWORD n = GetEnvironmentVariableA("SS2RTX_KEYS", script, sizeof script);
  if (n == 0 || n >= sizeof script) return;
  char* ctx = nullptr;
  for (char* tok = strtok_s(script, ";", &ctx); tok && g_step_count < 512; tok = strtok_s(nullptr, ";", &ctx)) {
    char* colon = strchr(tok, ':');
    if (!colon) continue;
    *colon = 0;
    Step s = {};
    s.at_s = atof(tok);
    s.hold_s = 0.08;
    char* hold = strchr(colon + 1, ':');
    if (hold) { *hold = 0; s.hold_s = atof(hold + 1); }
    if (!_strnicmp(colon + 1, "click=", 6) && sscanf(colon + 7, "%d,%d", &s.x, &s.y) == 2) {
      s.click = true;
      s.vk = 1;   // not an adapter action
    } else if (!parse_key(colon + 1, &s.vk, &s.extended)) {
      logf("autopilot: unknown key '%s'", colon + 1);
      continue;
    }
    strncpy(s.name, colon + 1, sizeof s.name - 1);
    g_steps[g_step_count++] = s;
  }
  logf("autopilot: %d key steps from SS2RTX_KEYS", g_step_count);
}

// A reserved bit of a key message's lParam marks the keys this file posts, so the filter
// below can tell them from real ones. It is cleared again before the game sees the message.
static const LPARAM kPostedMark = (LPARAM)1 << 25;

// Mouse messages posted by the script and not yet seen by the filter.
static struct { UINT message; LPARAM where; } g_expected[16];
static int g_expected_count;

static void post_mouse(HWND hwnd, UINT message, WPARAM buttons, LPARAM where) {
  if (g_expected_count < 16) g_expected[g_expected_count++] = { message, where };
  PostMessageW(hwnd, message, buttons, where);
}

static LRESULT CALLBACK filter_messages(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && wparam == PM_REMOVE) {
    MSG* m = reinterpret_cast<MSG*>(lparam);
    UINT msg = m->message;
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT) {
      // The script's own clicks are let through: they are recognised by message and position.
      bool ours = false;
      for (int i = 0; i < g_expected_count && !ours; ++i) {
        if (g_expected[i].message == msg && g_expected[i].where == m->lParam) {
          ours = true;
          g_expected[i] = g_expected[--g_expected_count];
        }
      }
      if (!ours) m->message = WM_NULL;
    } else if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
      if (m->lParam & kPostedMark) m->lParam &= ~kPostedMark;
      else m->message = WM_NULL;
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

using PFN_GetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PFN_GetRawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
static PFN_GetRawInputData g_real_GetRawInputData;
static PFN_GetRawInputBuffer g_real_GetRawInputBuffer;

static UINT WINAPI deaf_GetRawInputData(HRAWINPUT, UINT, LPVOID, PUINT, UINT) { return (UINT)-1; }

// The queue is still emptied, so the reader does not spin on input it can never fetch.
static UINT WINAPI deaf_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header) {
  UINT n = g_real_GetRawInputBuffer(data, size, header);
  return (n == (UINT)-1 || !data) ? n : 0;
}

// Points one of `module`'s imports from user32 at `replacement`. Returns the old target.
static void* patch_user32_import(HMODULE module, const char* name, void* replacement) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++thunks) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(by_name->Name, name) != 0) continue;
      DWORD old_protect = 0;
      if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protect)) return nullptr;
      void* old = reinterpret_cast<void*>(thunks->u1.Function);
      thunks->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
      VirtualProtect(&thunks->u1.Function, sizeof(void*), old_protect, &old_protect);
      return old;
    }
  }
  return nullptr;
}

static void keep_real_input_out(HWND hwnd) {
  char live[8];
  DWORD n = GetEnvironmentVariableA("SS2RTX_LIVEINPUT", live, sizeof live);
  if (n > 0 && n < sizeof live && live[0] == '1') { logf("autopilot: real keyboard and mouse left connected (SS2RTX_LIVEINPUT=1)"); return; }
  HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, filter_messages, nullptr, GetWindowThreadProcessId(hwnd, nullptr));
  int patched = 0;
  // The game reads the mouse through SDL, which fetches raw input on a thread of its own.
  for (const wchar_t* name : { L"SDL3.dll", (const wchar_t*)nullptr }) {
    HMODULE module = GetModuleHandleW(name);
    if (!module) continue;
    if (void* old = patch_user32_import(module, "GetRawInputBuffer", reinterpret_cast<void*>(deaf_GetRawInputBuffer))) {
      if (!g_real_GetRawInputBuffer) g_real_GetRawInputBuffer = reinterpret_cast<PFN_GetRawInputBuffer>(old);
      ++patched;
    }
    if (void* old = patch_user32_import(module, "GetRawInputData", reinterpret_cast<void*>(deaf_GetRawInputData))) {
      if (!g_real_GetRawInputData) g_real_GetRawInputData = reinterpret_cast<PFN_GetRawInputData>(old);
      ++patched;
    }
  }
  if (!g_real_GetRawInputBuffer) g_real_GetRawInputBuffer = GetRawInputBuffer;
  logf("autopilot: real keyboard and mouse kept out for this scripted run (message filter %s, %d raw-input import(s) redirected)",
       hook ? "installed" : "FAILED", patched);
}

void autopilot_tick(HWND hwnd, double seconds) {
  if (g_step_count < 0) parse();
  if (g_step_count == 0 || !hwnd) return;
  if (!g_autopilot_hwnd) keep_real_input_out(hwnd);
  g_autopilot_hwnd = hwnd;
  g_autopilot_seconds = seconds;
  for (int i = 0; i < g_step_count; ++i) {
    Step& s = g_steps[i];
    LPARAM scan = (LPARAM)MapVirtualKeyW(s.vk, MAPVK_VK_TO_VSC) << 16;
    if (s.extended) scan |= (LPARAM)1 << 24;
    if (s.click) {
      // Pointer first, so the menu sees it hover; then press and release on later frames.
      LPARAM where = MAKELPARAM(s.x, s.y);
      if (!s.move_sent && seconds >= s.at_s) {
        post_mouse(hwnd, WM_MOUSEMOVE, 0, where);
        s.move_sent = true;
      } else if (s.move_sent && !s.down_sent && seconds >= s.at_s + 0.15) {
        post_mouse(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, where);
        s.down_sent = true;
        logf("autopilot: %.1fs click at %d,%d", seconds, s.x, s.y);
      } else if (s.down_sent && !s.up_sent && seconds >= s.at_s + 0.30) {
        post_mouse(hwnd, WM_LBUTTONUP, 0, where);
        s.up_sent = true;
      }
      continue;
    }
    if (!s.down_sent && seconds >= s.at_s && s.vk == 0) {
      adapter_action(s.name);
      s.down_sent = s.up_sent = true;
      logf("autopilot: %.1fs %s", seconds, s.name);
    } else if (!s.down_sent && seconds >= s.at_s) {
      PostMessageW(hwnd, WM_KEYDOWN, s.vk, scan | 1 | kPostedMark);
      s.down_sent = true;
      logf("autopilot: %.1fs key %s down", seconds, s.name);
    } else if (s.down_sent && !s.up_sent && seconds >= s.at_s + s.hold_s) {
      PostMessageW(hwnd, WM_KEYUP, s.vk, scan | 1 | kPostedMark | ((LPARAM)1 << 30) | ((LPARAM)1 << 31));
      s.up_sent = true;
    }
  }
}

}  // namespace ss2rtx
