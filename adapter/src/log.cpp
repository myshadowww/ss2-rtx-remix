#include "log.h"

#include <string>

#include <stdarg.h>
#include <stdio.h>

namespace ss2rtx {

static CRITICAL_SECTION g_lock;
static FILE* g_file;
static bool g_ready;
static double g_start_ms;

double now_ms() {
  static LARGE_INTEGER freq;
  LARGE_INTEGER t;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

void log_open() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_start_ms = now_ms();
  // SS2RTX_LOG overrides the default location (the process working directory).
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"SS2RTX_LOG", path, (DWORD)(sizeof path / sizeof path[0]));
  if (n == 0 || n >= sizeof path / sizeof path[0]) {
    CreateDirectoryW(L"ss2rtx", nullptr);
    lstrcpyW(path, GetFileAttributesW(L"ss2rtx") != INVALID_FILE_ATTRIBUTES ? L"ss2rtx\\ss2rtx.log" : L"ss2rtx.log");
  }
  // Keep the last session's log: it is the one somebody wants read after a glitch.
  std::wstring previous = path;
  size_t dot = previous.rfind(L'.');
  if (dot != std::wstring::npos) {
    previous.insert(dot, L"-prev");
    MoveFileExW(path, previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  g_file = _wfopen(path, L"w");
  g_ready = true;
}

void logf(const char* fmt, ...) {
  if (!g_ready) log_open();
  if (!g_file) return;
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  EnterCriticalSection(&g_lock);
  fprintf(g_file, "[%9.3f] [%5lu] %s\n", (now_ms() - g_start_ms) / 1000.0, GetCurrentThreadId(), line);
  fflush(g_file);
  LeaveCriticalSection(&g_lock);
}

}  // namespace ss2rtx
