// Shared helpers for the Phase 0 probes. Windows x64, built with llvm-mingw.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static FILE* g_probe_log;

static void probe_log(const char* fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (!g_probe_log) g_probe_log = fopen("probe.log", "w");
  if (g_probe_log) { fprintf(g_probe_log, "%s\n", line); fflush(g_probe_log); }
  printf("%s\n", line);
  fflush(stdout);
}

static double probe_now_ms(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER t;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static LRESULT WINAPI probe_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND probe_create_window(const wchar_t* title, int w, int h) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = probe_wndproc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"ss2rtx_probe";
  RegisterClassExW(&wc);
  RECT r = { 0, 0, w, h };
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, 60, 60,
                       r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
}

// The Remix d3d9.dll is a small shim that loads its real implementation and ~40
// dependency DLLs from its own folder with a plain LoadLibrary, so that folder has
// to be on the DLL search path before the SDK loader runs.
static void probe_use_dll_dir_of(const wchar_t* dll_path) {
  wchar_t dir[MAX_PATH * 2];
  lstrcpynW(dir, dll_path, (int)(sizeof dir / sizeof dir[0]));
  wchar_t* slash = wcsrchr(dir, L'\\');
  if (slash) *slash = 0;
  BOOL ok = SetDllDirectoryW(dir);
  probe_log("SetDllDirectory(%ls) -> %d", dir, ok);
}

// In-process stack sampler. gdb cannot attach to Wine processes on this machine
// (ptrace_scope=1), so a stuck probe walks its own threads and logs module+offset per
// frame; tools/symbolize.py turns those into function names using NVIDIA's PDBs.
static void probe_dump_thread_stack(DWORD tid) {
  enum { MAX_FRAMES = 48 };
  DWORD64 frames[MAX_FRAMES];
  int count = 0;
  HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!th) return;
  if (SuspendThread(th) != (DWORD)-1) {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx)) {
      // Only unwinding happens while the thread is suspended; no logging or allocation.
      while (count < MAX_FRAMES && ctx.Rip) {
        frames[count++] = ctx.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
        if (fn) {
          PVOID handler_data = NULL;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, NULL);
        } else {
          if (IsBadReadPtr((void*)ctx.Rsp, 8)) break;
          ctx.Rip = *(DWORD64*)ctx.Rsp;
          ctx.Rsp += 8;
        }
      }
    }
    ResumeThread(th);
  }
  CloseHandle(th);
  probe_log("thread %lu: %d frames", tid, count);
  for (int i = 0; i < count; ++i) {
    HMODULE mod = NULL;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)frames[i], &mod) && mod)
      GetModuleFileNameW(mod, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    probe_log("  #%02d %ls+0x%llx", i, base ? base + 1 : name, (unsigned long long)(frames[i] - (DWORD64)mod));
  }
}

static void probe_dump_all_stacks(void) {
  DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) { probe_log("stack dump: snapshot failed %lu", GetLastError()); return; }
  THREADENTRY32 te = {};
  te.dwSize = sizeof te;
  probe_log("=== stack dump of process %lu ===", pid);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) probe_dump_thread_stack(te.th32ThreadID);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  probe_log("=== end stack dump ===");
}

// Stall detector: each probe marks what it is about to do; a watchdog thread reports
// the stage if nothing has moved for a while, so a hang names the call it is in.
static volatile const char* g_probe_stage = "start";
static volatile LONG g_probe_ticks;
#define PROBE_STAGE(name) do { g_probe_stage = (name); InterlockedIncrement(&g_probe_ticks); } while (0)

static DWORD WINAPI probe_watchdog(LPVOID param) {
  DWORD limit_ms = (DWORD)(uintptr_t)param;
  LONG seen = g_probe_ticks;
  DWORD still = 0;
  int reported = 0;
  for (;;) {
    Sleep(500);
    if (g_probe_ticks != seen) { seen = g_probe_ticks; still = 0; reported = 0; continue; }
    still += 500;
    if (still >= limit_ms && !reported) {
      probe_log("STALL: no progress for %lu ms in stage '%s' (tick %ld)", still, (const char*)g_probe_stage, seen);
      probe_dump_all_stacks();
      reported = 1;
    }
  }
}

static void probe_start_watchdog(unsigned limit_ms) {
  CloseHandle(CreateThread(NULL, 0, probe_watchdog, (LPVOID)(uintptr_t)limit_ms, 0, NULL));
}

// Returns 0 when WM_QUIT was seen.
static int probe_pump(void) {
  MSG msg;
  while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) return 0;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 1;
}
