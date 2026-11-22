#include "shaders.h"

#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>

#include "log.h"

namespace ss2rtx {

static CRITICAL_SECTION g_lock;
static bool g_ready;
static std::map<std::string, std::string> g_names;   // DXBC checksum -> archive name
static std::map<void*, ShaderRecord> g_shaders;

static void ensure() {
  if (!g_ready) { InitializeCriticalSection(&g_lock); g_ready = true; }
}

static std::string dxbc_id(const unsigned char* b, size_t size) {
  if (!b || size < 20 || memcmp(b, "DXBC", 4) != 0) return "not-dxbc";
  char hex[33];
  for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", b[4 + i]);
  return hex;
}

static unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// Minimal zip reader: hlsl.kpf stores its entries uncompressed.
void shader_table_load() {
  ensure();
  FILE* f = fopen("hlsl.kpf", "rb");
  if (!f) { logf("shaders: hlsl.kpf not found in the working directory; draws cannot be classified"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> zip((size_t)size);
  size_t got = fread(zip.data(), 1, zip.size(), f);
  fclose(f);
  if (got != zip.size() || zip.size() < 22) { logf("shaders: could not read hlsl.kpf"); return; }

  long eocd = -1;
  for (long i = (long)zip.size() - 22; i >= 0 && i > (long)zip.size() - 70000; --i) {
    if (rd32(&zip[i]) == 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) { logf("shaders: hlsl.kpf has no zip directory"); return; }
  unsigned count = rd16(&zip[eocd + 10]);
  size_t pos = rd32(&zip[eocd + 16]);
  unsigned compressed = 0;
  for (unsigned i = 0; i < count && pos + 46 <= zip.size() && rd32(&zip[pos]) == 0x02014b50; ++i) {
    unsigned method = rd16(&zip[pos + 10]);
    unsigned csize = rd32(&zip[pos + 20]);
    unsigned nlen = rd16(&zip[pos + 28]), xlen = rd16(&zip[pos + 30]), clen = rd16(&zip[pos + 32]);
    size_t local = rd32(&zip[pos + 42]);
    std::string name(reinterpret_cast<const char*>(&zip[pos + 46]), nlen);
    pos += 46 + nlen + xlen + clen;
    if (method != 0) { ++compressed; continue; }
    if (local + 30 > zip.size()) continue;
    size_t data = local + 30 + rd16(&zip[local + 26]) + rd16(&zip[local + 28]);
    if (data + csize > zip.size()) continue;
    size_t slash = name.find_last_of('/');
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".bin") == 0) base.resize(base.size() - 4);
    // The blob may sit behind a small engine header; find every DXBC container in it.
    for (size_t off = 0; off + 20 <= csize; ++off) {
      if (memcmp(&zip[data + off], "DXBC", 4) == 0) g_names.emplace(dxbc_id(&zip[data + off], csize - off), base);
    }
  }
  logf("shaders: %zu named shaders from hlsl.kpf (%u entries, %u compressed and skipped)", g_names.size(), count, compressed);
}

static ShaderClass classify(const std::string& name) {
  auto starts = [&](const char* p) { return name.compare(0, strlen(p), p) == 0; };
  if (starts("DarkWorldRep")) return ShaderClass::WorldRep;
  if (starts("DarkMDModel")) return ShaderClass::MDModel;
  if (starts("DarkMMModel")) return ShaderClass::MMModel;
  if (starts("DarkEMMModel")) return ShaderClass::EMMModel;
  if (starts("DarkWS")) return ShaderClass::WorldSpace;
  if (starts("DarkSky")) return ShaderClass::Sky;
  if (name.empty()) return ShaderClass::Unknown;
  return ShaderClass::Post;
}

void shader_register(void* shader, char kind, const void* bytecode, SIZE_T size) {
  ensure();
  ShaderRecord rec;
  rec.kind = kind;
  rec.size = size;
  rec.id = dxbc_id(static_cast<const unsigned char*>(bytecode), size);
  EnterCriticalSection(&g_lock);
  auto it = g_names.find(rec.id);
  if (it != g_names.end()) rec.name = it->second;
  rec.cls = classify(rec.name);
  rec.alpha_test = rec.name.find("_at1") != std::string::npos;
  g_shaders[shader] = rec;
  LeaveCriticalSection(&g_lock);
}

const ShaderRecord* shader_find(void* shader) {
  if (!shader || !g_ready) return nullptr;
  EnterCriticalSection(&g_lock);
  auto it = g_shaders.find(shader);
  const ShaderRecord* rec = it == g_shaders.end() ? nullptr : &it->second;  // map nodes are stable
  LeaveCriticalSection(&g_lock);
  return rec;
}

void shader_for_each(void (*fn)(void*, const ShaderRecord&, void*), void* user) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  for (const auto& kv : g_shaders) fn(kv.first, kv.second, user);
  LeaveCriticalSection(&g_lock);
}

}  // namespace ss2rtx
