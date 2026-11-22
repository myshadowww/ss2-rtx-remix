#include "kpf.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace ss2rtx {

static unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

bool kpf_list(const std::wstring& archive, std::vector<KpfEntry>* out) {
  FILE* f = _wfopen(archive.c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long size = _ftelli64(f);
  long long tail = size < 70000 ? size : 70000;
  std::vector<unsigned char> buf((size_t)tail);
  _fseeki64(f, size - tail, SEEK_SET);
  bool ok = fread(buf.data(), 1, buf.size(), f) == buf.size();
  long long eocd = -1;
  for (long long i = tail - 22; ok && i >= 0; --i) {
    if (rd32(&buf[(size_t)i]) == 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) { fclose(f); return false; }
  unsigned count = rd16(&buf[(size_t)eocd + 10]);
  unsigned cd_size = rd32(&buf[(size_t)eocd + 12]);
  unsigned cd_offset = rd32(&buf[(size_t)eocd + 16]);
  std::vector<unsigned char> cd(cd_size);
  _fseeki64(f, cd_offset, SEEK_SET);
  ok = fread(cd.data(), 1, cd.size(), f) == cd.size();
  fclose(f);
  if (!ok) return false;
  size_t pos = 0;
  for (unsigned i = 0; i < count && pos + 46 <= cd.size() && rd32(&cd[pos]) == 0x02014b50; ++i) {
    KpfEntry e;
    e.stored = rd16(&cd[pos + 10]) == 0;
    e.size = rd32(&cd[pos + 24]);
    unsigned nlen = rd16(&cd[pos + 28]), xlen = rd16(&cd[pos + 30]), clen = rd16(&cd[pos + 32]);
    e.local_offset = rd32(&cd[pos + 42]);
    if (pos + 46 + nlen > cd.size()) break;
    e.name.assign(reinterpret_cast<const char*>(&cd[pos + 46]), nlen);
    pos += 46 + nlen + xlen + clen;
    out->push_back(e);
  }
  return true;
}

bool kpf_read(const std::wstring& archive, const KpfEntry& entry, uint64_t offset, size_t size, void* dst) {
  if (!entry.stored || offset + size > entry.size) return false;
  FILE* f = _wfopen(archive.c_str(), L"rb");
  if (!f) return false;
  unsigned char local[30];
  _fseeki64(f, (long long)entry.local_offset, SEEK_SET);
  bool ok = fread(local, 1, 30, f) == 30 && rd32(local) == 0x04034b50;
  if (ok) {
    uint64_t data = entry.local_offset + 30 + rd16(local + 26) + rd16(local + 28);
    _fseeki64(f, (long long)(data + offset), SEEK_SET);
    ok = fread(dst, 1, size, f) == size;
  }
  fclose(f);
  return ok;
}

std::vector<std::wstring> kpf_archives() {
  std::vector<std::wstring> result;
  for (const wchar_t* pattern : { L"mods\\*.kpf", L"*.kpf" }) {   // mods first: they override the base game
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      std::wstring name = fd.cFileName;
      result.push_back(wcschr(pattern, L'\\') ? L"mods\\" + name : name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return result;
}

}  // namespace ss2rtx
