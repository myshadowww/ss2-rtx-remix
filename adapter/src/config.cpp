#include "config.h"

#include <windows.h>
#include <stdlib.h>

namespace ss2rtx {

std::wstring config_text(const wchar_t* name) {
  wchar_t buf[1024];
  std::wstring env = L"SS2RTX_";
  for (const wchar_t* p = name; *p; ++p) env.push_back((wchar_t)towupper(*p));
  DWORD n = GetEnvironmentVariableW(env.c_str(), buf, 1024);
  if (n > 0 && n < 1024) return buf;
  wchar_t ini[MAX_PATH + 32];
  DWORD len = GetCurrentDirectoryW(MAX_PATH, ini);
  if (len == 0 || len >= MAX_PATH) return L"";
  lstrcatW(ini, L"\\ss2rtx\\ss2rtx.ini");
  n = GetPrivateProfileStringW(L"ss2rtx", name, L"", buf, 1024, ini);
  return n > 0 ? std::wstring(buf) : std::wstring();
}

void config_set(const wchar_t* name, const std::wstring& value) {
  wchar_t nosave[8];
  DWORD n = GetEnvironmentVariableW(L"SS2RTX_NOSAVE", nosave, 8);
  if (n > 0 && n < 8 && nosave[0] == L'1') return;
  wchar_t ini[MAX_PATH + 32];
  DWORD len = GetCurrentDirectoryW(MAX_PATH, ini);
  if (len == 0 || len >= MAX_PATH) return;
  lstrcatW(ini, L"\\ss2rtx");
  CreateDirectoryW(ini, nullptr);
  lstrcatW(ini, L"\\ss2rtx.ini");
  WritePrivateProfileStringW(L"ss2rtx", name, value.c_str(), ini);
}

bool config_flag(const wchar_t* name, bool fallback) {
  std::wstring v = config_text(name);
  if (v.empty()) return fallback;
  return v[0] == L'1' || v[0] == L'y' || v[0] == L'Y' || v[0] == L't' || v[0] == L'T';
}

double config_number(const wchar_t* name, double fallback) {
  std::wstring v = config_text(name);
  return v.empty() ? fallback : _wtof(v.c_str());
}

}  // namespace ss2rtx
