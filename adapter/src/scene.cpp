#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <set>

#include "bufshadow.h"
#include "config.h"
#include "log.h"
#include "mission.h"
#include "readback.h"
#include "shaders.h"

namespace ss2rtx {

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static World g_world;
static std::vector<SceneTexture> g_textures;
static std::map<ID3D11Texture2D*, int> g_texture_index;
static Camera g_camera;
static uint64_t g_frame;
static uint64_t g_camera_frame = ~0ull;
static std::vector<uint8_t> g_basis;   // sWRTexCoordBasis[], 64 bytes per polygon
static std::wstring g_texture_dir;
static unsigned g_failures_logged;
static std::vector<ModelMesh> g_models;
static std::map<std::pair<ID3D11Buffer*, ID3D11Buffer*>, int> g_model_index;
static std::vector<ObjectDraw> g_frame_objects;
static std::vector<float> g_frame_nodes;
static std::vector<SkinnedMesh> g_skinned;
static std::map<std::pair<ID3D11Buffer*, ID3D11Buffer*>, int> g_skinned_index;
static std::vector<SkinnedDraw> g_frame_skinned;
static std::vector<float> g_frame_bones;
static std::vector<SceneLight> g_lights;
static std::vector<SceneLight> g_far_lights;   // the engine's directional lights (position holds the direction)
static float g_ambient[3];                     // the engine's ambient colour for objects, last frame's average
static double g_ambient_sum[3];
static unsigned g_ambient_count;
static std::map<uint64_t, int> g_light_index;
static uint64_t g_readback_fallbacks;
// Lights seen in the engine's per-object lists are only a sample of the level's lights.
// Once a few are known they identify the mission file, whose table has all of them.
static bool g_mission_lights;          // g_lights now holds the mission's full table
// The latest copy of the engine's animated-light brightness values; see capture_anim_brightness().
static std::vector<float> g_anim_values;
static int g_anim_slot = -1;
static size_t g_anim_entries;     // how many entries the level's polygons use
static bool g_mission_geometry_tried;   // the by-geometry lookup has run for this level
static unsigned g_lights_generation;
static unsigned g_mission_attempts;
static size_t g_mission_tried_with;
static ID3D11Texture2D* g_world_target;        // reference held
static uint64_t g_world_target_frame = ~0ull;

// Contents of a small dynamic buffer: from the shadow copy when we have one, otherwise
// through a (slow) staging readback into `scratch`.
static bool buffer_bytes(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const uint8_t** data, size_t* size, std::vector<uint8_t>* scratch) {
  if (!buf) return false;
  if (shadow_get(buf, data, size)) return true;
  if (!read_buffer(g_dev, ctx, buf, scratch)) return false;
  ++g_readback_fallbacks;
  *data = scratch->data();
  *size = scratch->size();
  return true;
}

static ID3D11Buffer* vs_resource_buffer(ID3D11DeviceContext* ctx, UINT slot) {
  ID3D11ShaderResourceView* srv = nullptr;
  ctx->VSGetShaderResources(slot, 1, &srv);
  if (!srv) return nullptr;
  ID3D11Resource* res = nullptr;
  srv->GetResource(&res);
  srv->Release();
  if (!res) return nullptr;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) { res->Release(); return nullptr; }
  return static_cast<ID3D11Buffer*>(res);   // caller releases
}

// Per-polygon texture basis, as declared in DarkWorldRep.shader.
struct TexCoordBasis {
  float texU[3]; float u_base;
  float texV[3]; float v_base;
  float anchor[3]; float denom_uv;
  float u2p; float v2p; float pad0; float pad1;
};

void scene_set_device(ID3D11Device* dev, ID3D11DeviceContext* immediate) {
  g_dev = dev;
  g_ctx = immediate;
  wchar_t cwd[MAX_PATH];
  GetCurrentDirectoryW(MAX_PATH, cwd);
  std::wstring base = std::wstring(cwd) + L"\\ss2rtx_cache";
  CreateDirectoryW(base.c_str(), nullptr);
  g_texture_dir = base + L"\\textures\\";
  CreateDirectoryW(g_texture_dir.c_str(), nullptr);
}

static void fail_once(const char* what) {
  if (g_failures_logged++ < 12) logf("scene: %s", what);
}

static std::map<uint64_t, int> g_cached_texture_index;   // textures named in the level memory: hash -> record
static std::set<uint64_t> g_uniform_hashes;              // textures known to be blank

// Identity of the texture view bound for the surface texture. Compared, never used.
static const void* bound_texture_view(ID3D11DeviceContext* ctx) {
  ID3D11ShaderResourceView* srv = nullptr;
  ctx->PSGetShaderResources(0, 1, &srv);
  if (srv) srv->Release();
  return srv;
}

static int texture_for(ID3D11DeviceContext* ctx) {
  ID3D11ShaderResourceView* srv = nullptr;
  ctx->PSGetShaderResources(0, 1, &srv);
  if (!srv) return -1;
  ID3D11Resource* res = nullptr;
  srv->GetResource(&res);
  D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
  srv->GetDesc(&vd);
  srv->Release();
  if (!res) return -1;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { res->Release(); return -1; }
  ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(res);
  auto it = g_texture_index.find(tex);
  if (it != g_texture_index.end()) { res->Release(); return it->second; }

  SceneTexture rec;
  rec.tex = tex;  // keeps the reference taken by GetResource
  bool first_time_ever = false;
  TextureDump info;
  if (dump_texture_dds(g_dev, ctx, tex, vd.Format, g_texture_dir, &rec.path, &info)) {
    rec.hash = info.hash;
    rec.width = info.width;
    rec.height = info.height;
    rec.solid = info.solid;
    rec.uniform = info.uniform;
    memcpy(rec.solid_rgba, info.solid_rgba, sizeof rec.solid_rgba);
    first_time_ever = !info.already_on_disk;
  } else {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    rec.path.clear();
    rec.hash = fnv1a64(&tex, sizeof tex);
    if (g_failures_logged++ < 12) logf("scene: could not export texture %ux%u format %d", d.Width, d.Height, (int)d.Format);
  }
  if (!rec.path.empty()) {
    // A texture remembered from an earlier session has a record already: it becomes this one.
    auto cached = g_cached_texture_index.find(rec.hash);
    if (cached != g_cached_texture_index.end() && cached->second >= 0 && !g_textures[cached->second].tex) {
      g_textures[cached->second] = rec;
      g_texture_index[tex] = cached->second;
      return cached->second;
    }
  }
  int index = (int)g_textures.size();
  if (first_time_ever && !rec.path.empty() && !rec.solid) {
    // Running list of textures, for writing material overrides against (ss2rtx\\materials.txt).
    FILE* list = _wfopen((g_texture_dir + L"..\\textures_seen.txt").c_str(), L"a");
    if (list) { fprintf(list, "%016llX %ux%u\n", (unsigned long long)rec.hash, rec.width, rec.height); fclose(list); }
  }
  g_textures.push_back(rec);
  g_texture_index[tex] = index;
  return index;
}

static void load_remembered_polygons();

static bool load_level(ID3D11DeviceContext* ctx, ID3D11Buffer* vb, ID3D11Buffer* ib, DXGI_FORMAT ib_format) {
  if (g_world.vb) g_world.vb->Release();
  if (g_world.ib) g_world.ib->Release();
  uint32_t next_revision = g_world.revision + 1;
  g_world = World();
  g_world.revision = next_revision;
  g_basis.clear();
  g_lights.clear();
  g_far_lights.clear();
  g_light_index.clear();
  g_mission_lights = false;
  g_mission_geometry_tried = false;
  g_anim_entries = 0;
  g_anim_slot = -1;
  g_anim_values.clear();
  ++g_lights_generation;
  g_mission_attempts = 0;
  g_mission_tried_with = 0;

  std::vector<uint8_t> vdata, idata;
  if (!read_buffer(g_dev, ctx, vb, &vdata) || !read_buffer(g_dev, ctx, ib, &idata)) { fail_once("level buffer readback failed"); return false; }
  g_world.vertices.resize(vdata.size() / sizeof(WorldVertex));
  memcpy(g_world.vertices.data(), vdata.data(), g_world.vertices.size() * sizeof(WorldVertex));
  if (ib_format == DXGI_FORMAT_R32_UINT) {
    g_world.indices.resize(idata.size() / 4);
    memcpy(g_world.indices.data(), idata.data(), g_world.indices.size() * 4);
  } else {
    g_world.indices.resize(idata.size() / 2);
    for (size_t i = 0; i < g_world.indices.size(); ++i) g_world.indices[i] = idata[i * 2] | (idata[i * 2 + 1] << 8);
  }
  g_world.uv.assign(g_world.vertices.size() * 2, 0.0f);
  g_world.vb_hash = fnv1a64(vdata.data(), vdata.size());

  // Vertex-shader resource slot 1 is texCoordBasisArray (slot 0 is the dynamic light bits).
  ID3D11ShaderResourceView* srv = nullptr;
  ctx->VSGetShaderResources(1, 1, &srv);
  if (srv) {
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (res) res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && !read_buffer(g_dev, ctx, static_cast<ID3D11Buffer*>(res), &g_basis)) g_basis.clear();
    if (res) res->Release();
  }
  vb->AddRef();
  ib->AddRef();
  g_world.vb = vb;
  g_world.ib = ib;
  g_world.loaded = true;
  logf("scene: level loaded: %zu vertices, %zu indices (%zu triangles), %zu polygon bases, vb hash %016llx", g_world.vertices.size(),
       g_world.indices.size(), g_world.indices.size() / 3, g_basis.size() / sizeof(TexCoordBasis), (unsigned long long)g_world.vb_hash);
  load_remembered_polygons();
  return true;
}

static void capture_camera(ID3D11DeviceContext* ctx) {
  ID3D11Buffer* cb = nullptr;
  ctx->VSGetConstantBuffers(0, 1, &cb);
  if (!cb) return;
  std::vector<uint8_t> scratch;
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (buffer_bytes(ctx, cb, &data, &size, &scratch) && size >= 128) {
    memcpy(g_camera.proj, data, 64);        // RenderView.uProjectionMatrix
    memcpy(g_camera.view, data + 64, 64);   // RenderView.uModelViewMatrix (model is identity for the level)
    g_camera.valid = true;
    g_camera.frame = g_frame;
  }
  cb->Release();
}

// Reads the constants that decide a polygon's texture mapping and tint at draw time.
static void read_polygon_constants(ID3D11DeviceContext* ctx, WorldRange& range) {
  ID3D11Buffer* cbs[3] = {};
  ctx->VSGetConstantBuffers(0, 3, cbs);
  std::vector<uint8_t> scratch;
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (buffer_bytes(ctx, cbs[1], &data, &size, &scratch) && size >= 8) memcpy(range.scale, data, 8);   // DarkNewParams.uTerrainScale
  if (buffer_bytes(ctx, cbs[2], &data, &size, &scratch) && size >= 64) {
    memcpy(range.uv_matrix, data, 48);
    memcpy(range.color, data + 48, 16);
    if (size >= 84) {
      int shade = 1;
      memcpy(&range.illumination, data + 64, 4);
      memcpy(&shade, data + 80, 4);
      range.unshaded = shade == 0;
    }
  }
  for (auto* cb : cbs) if (cb) cb->Release();
}

void scene_level_uv(uint32_t vi, const float scale[2], const float m[12], float out[2]) {
  out[0] = out[1] = 0.0f;
  if (vi >= g_world.vertices.size()) return;
  const size_t basis_count = g_basis.size() / sizeof(TexCoordBasis);
  const TexCoordBasis* bases = reinterpret_cast<const TexCoordBasis*>(g_basis.data());
  const WorldVertex& v = g_world.vertices[vi];
  uint32_t poly = v.flags & 0x0007FFFF;
  float u = v.uv_unused[0], t = v.uv_unused[1];
  if (poly < basis_count) {
    const TexCoordBasis& b = bases[poly];
    float d[3] = { v.position[0] - b.anchor[0], v.position[1] - b.anchor[1], v.position[2] - b.anchor[2] };
    float du = d[0] * b.texU[0] + d[1] * b.texU[1] + d[2] * b.texU[2];
    float dv = d[0] * b.texV[0] + d[1] * b.texV[1] + d[2] * b.texV[2];
    u = scale[0] * (b.u_base + b.v2p * du - b.denom_uv * dv);
    t = scale[1] * (b.v_base + b.u2p * dv - b.denom_uv * du);
  }
  out[0] = m[0] * u + m[4] * t + m[8];    // float3x3 as three padded columns
  out[1] = m[1] * u + m[5] * t + m[9];
}

// Computes the texture coordinates the vertex shader would produce for one polygon.
static void compute_polygon_uv(const WorldRange& range) {
  for (uint32_t i = range.start; i < range.start + range.count && i < g_world.indices.size(); ++i) {
    uint32_t vi = g_world.indices[i];
    if (vi >= g_world.vertices.size()) continue;
    scene_level_uv(vi, range.scale, range.uv_matrix, &g_world.uv[vi * 2]);
  }
}

// ---- remembered polygons ------------------------------------------------------------------
// Which texture a level polygon uses is only known once the engine has drawn it. What has
// been seen is kept per level in ss2rtx_cache\\levels\\<level hash>.txt, so on the next
// visit everything explored before is textured from the first frame and the level mesh
// settles on one stable form.
static std::wstring g_level_file;
static std::vector<std::string> g_pending_lines;

// The sky box sits in the last 36 indices: six quads whose corners all lie the same
// distance out on every axis.
static void find_sky_box() {
  g_world.sky.clear();
  size_t n = g_world.indices.size();
  if (n < 36) return;
  float size = 0.0f;
  for (size_t i = n - 36; i < n; ++i) {
    uint32_t vi = g_world.indices[i];
    if (vi >= g_world.vertices.size()) return;
    const float* p = g_world.vertices[vi].position;
    float a = fabsf(p[0]);
    if (a < 100.0f || fabsf(fabsf(p[1]) - a) > 0.01f || fabsf(fabsf(p[2]) - a) > 0.01f || (size > 0.0f && fabsf(a - size) > 0.01f)) return;
    size = a;
  }
  for (size_t i = n - 36; i < n; i += 6) g_world.sky.push_back({ (uint32_t)i, 6, -1 });
  logf("scene: the level's sky box found (%.0f units out)", size);
}

static void on_sky_draw(ID3D11DeviceContext* ctx, UINT count, UINT start) {
  for (auto& face : g_world.sky) {
    if (start < face.start || start >= face.start + face.count) continue;
    int t = texture_for(ctx);
    if (t >= 0 && t != face.texture) { face.texture = t; ++g_world.sky_revision; }
    return;
  }
}

static void load_remembered_polygons() {
  find_sky_box();
  wchar_t name[64];
  swprintf(name, 64, L"%016llx.txt", (unsigned long long)g_world.vb_hash);
  std::wstring dir = g_texture_dir + L"..\\levels\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  g_level_file = dir + name;
  g_pending_lines.clear();
  g_cached_texture_index.clear();
  FILE* f = _wfopen(g_level_file.c_str(), L"r");
  if (!f) return;
  char line[512];
  unsigned loaded = 0, skipped = 0;
  auto cached_texture = [&](unsigned long long hash) -> int {
    auto it = g_cached_texture_index.find(hash);
    if (it != g_cached_texture_index.end()) return it->second;
    SceneTexture rec;
    wchar_t dds[64];
    swprintf(dds, 64, L"%016llx.dds", hash);
    rec.path = g_texture_dir + dds;
    rec.hash = hash;
    int index = -1;
    if (GetFileAttributesW(rec.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      index = (int)g_textures.size();
      g_textures.push_back(rec);
    }
    g_cached_texture_index[hash] = index;
    return index;
  };
  while (fgets(line, sizeof line, f)) {
    if (line[0] == 'G') {   // "G": full-bright texture added on top of a polygon
      unsigned start = 0;
      unsigned long long hash = 0;
      float strength = 0, sc[2], m[12];
      int n = sscanf(line + 1, "%u %llx %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &start, &hash, &strength, &sc[0], &sc[1], &m[0], &m[1], &m[2],
                     &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]);
      auto it = g_world.ranges.find(start);
      int index = n == 17 ? cached_texture(hash) : -1;
      if (it == g_world.ranges.end() || index < 0) { ++skipped; continue; }
      if (it->second.glow_texture < 0) g_world.glowing.push_back(start);
      it->second.glow_texture = index;
      it->second.glow_strength = strength;
      memcpy(it->second.glow_scale, sc, sizeof sc);
      memcpy(it->second.glow_uv_matrix, m, sizeof m);
      continue;
    }
    if (line[0] == 'F') {   // "F": lighting flags of a polygon
      unsigned start = 0;
      int unshaded = 0, over_unshaded = 0;
      float illum = 0;
      if (sscanf(line + 1, "%u %d %f %d", &start, &unshaded, &illum, &over_unshaded) == 4) {
        auto it = g_world.ranges.find(start);
        if (it != g_world.ranges.end()) {
          it->second.unshaded = unshaded != 0;
          it->second.illumination = illum;
          it->second.overlay_unshaded = over_unshaded != 0;
        }
      }
      continue;
    }
    if (line[0] == 'O' || line[0] == 'U') {   // "O": second layer of a polygon; "U": its base texture is blank
      unsigned start = 0;
      unsigned long long hash = 0;
      float sc[2], m[12];
      if (line[0] == 'U') {
        if (sscanf(line + 1, "%llx", &hash) == 1) g_uniform_hashes.insert(hash);
        continue;
      }
      int n = sscanf(line + 1, "%u %llx %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &start, &hash, &sc[0], &sc[1], &m[0], &m[1], &m[2], &m[3], &m[4],
                     &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]);
      auto it = g_world.ranges.find(start);
      int index = n == 16 ? cached_texture(hash) : -1;
      if (it == g_world.ranges.end() || index < 0) { ++skipped; continue; }
      it->second.overlay_texture = index;
      memcpy(it->second.overlay_scale, sc, sizeof sc);
      memcpy(it->second.overlay_uv_matrix, m, sizeof m);
      continue;
    }
    WorldRange r;
    unsigned long long tex_hash = 0;
    int alpha = 0;
    float* c = r.color; float* sc = r.scale; float* m = r.uv_matrix;
    int n = sscanf(line, "%u %u %llx %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &r.start, &r.count, &tex_hash, &alpha, &c[0], &c[1],
                   &c[2], &c[3], &sc[0], &sc[1], &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]);
    if (n != 22 || (size_t)r.start + r.count > g_world.indices.size()) { ++skipped; continue; }
    r.alpha_test = alpha != 0;
    int index = cached_texture(tex_hash);
    if (index < 0) { ++skipped; continue; }   // its texture file is gone: let the engine show it to us again
    r.texture = index;
    compute_polygon_uv(r);
    auto earlier = g_world.ranges.find(r.start);
    if (earlier != g_world.ranges.end()) {   // a correction written later in the file: it wins, the second layer stays
      r.overlay_texture = earlier->second.overlay_texture;
      r.glow_texture = earlier->second.glow_texture;
      r.glow_strength = earlier->second.glow_strength;
      memcpy(r.glow_scale, earlier->second.glow_scale, sizeof r.glow_scale);
      memcpy(r.glow_uv_matrix, earlier->second.glow_uv_matrix, sizeof r.glow_uv_matrix);
      r.later_passes_checked = earlier->second.later_passes_checked;
      memcpy(r.overlay_scale, earlier->second.overlay_scale, sizeof r.overlay_scale);
      memcpy(r.overlay_uv_matrix, earlier->second.overlay_uv_matrix, sizeof r.overlay_uv_matrix);
    } else {
      ++loaded;
    }
    g_world.ranges[r.start] = r;
  }
  fclose(f);
  for (auto& t : g_textures) if (!t.tex && g_uniform_hashes.count(t.hash)) t.uniform = true;
  if (loaded) ++g_world.revision;
  logf("scene: %u polygons remembered from earlier sessions for this level (%u lines skipped)", loaded, skipped);
}

static void remember_polygon(const WorldRange& r) {
  if (r.texture < 0 || g_textures[r.texture].path.empty() || g_textures[r.texture].solid) return;
  char line[512];
  const float* c = r.color; const float* sc = r.scale; const float* m = r.uv_matrix;
  snprintf(line, sizeof line, "%u %u %016llx %d %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n", r.start, r.count,
           (unsigned long long)g_textures[r.texture].hash, r.alpha_test ? 1 : 0, c[0], c[1], c[2], c[3], sc[0], sc[1], m[0], m[1], m[2], m[3], m[4],
           m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  g_pending_lines.push_back(line);
  if (g_textures[r.texture].uniform && g_uniform_hashes.insert(g_textures[r.texture].hash).second) {
    snprintf(line, sizeof line, "U %016llx\n", (unsigned long long)g_textures[r.texture].hash);
    g_pending_lines.push_back(line);
  }
}

static void remember_glow(const WorldRange& r) {
  if (r.glow_texture < 0 || g_textures[r.glow_texture].path.empty()) return;
  char line[512];
  const float* sc = r.glow_scale; const float* m = r.glow_uv_matrix;
  snprintf(line, sizeof line, "G %u %016llx %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n", r.start, (unsigned long long)g_textures[r.glow_texture].hash,
           r.glow_strength, sc[0], sc[1], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  g_pending_lines.push_back(line);
}

static void remember_flags(const WorldRange& r) {
  char line[96];
  snprintf(line, sizeof line, "F %u %d %g %d\n", r.start, r.unshaded ? 1 : 0, r.illumination, r.overlay_unshaded ? 1 : 0);
  g_pending_lines.push_back(line);
}

static void remember_overlay(const WorldRange& r) {
  if (r.overlay_texture < 0 || g_textures[r.overlay_texture].path.empty()) return;
  char line[512];
  const float* sc = r.overlay_scale; const float* m = r.overlay_uv_matrix;
  snprintf(line, sizeof line, "O %u %016llx %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n", r.start, (unsigned long long)g_textures[r.overlay_texture].hash,
           sc[0], sc[1], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  g_pending_lines.push_back(line);
}

static void flush_remembered_polygons() {
  if (g_pending_lines.empty() || g_level_file.empty()) return;
  FILE* f = _wfopen(g_level_file.c_str(), L"a");
  if (f) {
    for (const auto& l : g_pending_lines) fputs(l.c_str(), f);
    fclose(f);
  }
  g_pending_lines.clear();
}

// The level shader also takes up to 32 dynamic lights (pixel-shader slot 2, kDynLightInfoSlot).
// They are not submitted to Remix yet; this only records how many are in use, so the
// log shows whether a scene has any (muzzle flashes, explosions, moving lamps).
static std::vector<SceneLight> g_dynamic_lights;
static uint64_t g_serial_last_frame, g_serial_two_frames_ago;   // shadow_serial() at the last two frame ends

// The level shader scales each animated light's lightmap layer by a brightness it reads
// from a buffer of floats (pixel-shader slot kDynLightBrightnessSlot): one entry per layer
// of each polygon. This keeps the latest copy; see connect_light_feeds() for which entry
// belongs to which light.

static void capture_anim_brightness(ID3D11DeviceContext* ctx) {
  if (!g_anim_entries) return;
  for (UINT slot = g_anim_slot >= 0 ? (UINT)g_anim_slot : 3; slot < (g_anim_slot >= 0 ? (UINT)g_anim_slot + 1 : 8u); ++slot) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(slot, 1, &srv);
    if (!srv) continue;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) continue;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool found = dim == D3D11_RESOURCE_DIMENSION_BUFFER && shadow_get(static_cast<ID3D11Buffer*>(res), &data, &size) && size >= g_anim_entries * 4;
    if (found) {
      if (g_anim_slot < 0) {
        g_anim_slot = (int)slot;
        logf("scene: the engine's animated-light brightness buffer is in pixel-shader slot %u (%zu values, %zu in use)", slot, size / 4, g_anim_entries);
      }
      g_anim_values.resize(g_anim_entries);
      memcpy(g_anim_values.data(), data, g_anim_entries * 4);
    }
    res->Release();
    if (found) return;
  }
}

// The level shader's moving lights: position, squared reach, colour, brightness.
static void capture_dynamic_lights(ID3D11DeviceContext* ctx) {
  static int last_count = -1;
  g_dynamic_lights.clear();
  ID3D11ShaderResourceView* srv = nullptr;
  ctx->PSGetShaderResources(2, 1, &srv);
  if (!srv) return;
  ID3D11Resource* res = nullptr;
  srv->GetResource(&res);
  srv->Release();
  if (!res) return;
  D3D11_RESOURCE_DIMENSION dim;
  res->GetType(&dim);
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint64_t written = 0;
  if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && shadow_get(static_cast<ID3D11Buffer*>(res), &data, &size, &written)) {
    // The engine leaves a light's record in this buffer after the light has gone (a muzzle
    // flash stayed lit for ever). What says a light is in use is the per-polygon bit mask
    // the level shader loops over: light i counts while some polygon has bit i set, and
    // only while the engine is still writing these buffers.
    uint32_t in_use = 0;
    uint64_t bits_written = 0;
    const uint8_t* bits = nullptr;
    size_t bits_size = 0;
    ID3D11Buffer* bits_buf = vs_resource_buffer(ctx, 0);   // kDynLightBitsSlot: one mask per polygon
    bool have_bits = bits_buf && shadow_get(bits_buf, &bits, &bits_size, &bits_written);
    if (bits_buf) bits_buf->Release();
    if (have_bits) {
      for (size_t off = 0; off + 4 <= bits_size; off += 4) {
        uint32_t m;
        memcpy(&m, bits + off, 4);
        in_use |= m;
      }
      if (bits_written <= g_serial_two_frames_ago) in_use = 0;   // not rewritten lately: nothing is lit
    } else {
      in_use = written > g_serial_two_frames_ago ? ~0u : 0;       // no mask to read: go by the light buffer's age
    }
    for (size_t off = 0; off + 32 <= size && g_dynamic_lights.size() < 32; off += 32) {
      if (!(in_use >> (off / 32) & 1)) continue;
      float rec[8];
      memcpy(rec, data + off, 32);
      if (!(rec[4] > 0.0f || rec[5] > 0.0f || rec[6] > 0.0f) || !(rec[7] > 0.0f) || !(rec[3] > 0.0f)) continue;
      SceneLight l;
      memcpy(l.position, rec, 12);
      for (int k = 0; k < 3; ++k) l.color[k] = rec[4 + k] * rec[7];
      l.radius = sqrtf(rec[3]);
      g_dynamic_lights.push_back(l);
    }
    int lit = (int)g_dynamic_lights.size();
    if (lit != last_count) {
      last_count = lit;
      if (lit)
        logf("scene: %d moving level light(s), first at %.1f %.1f %.1f colour %.2f %.2f %.2f reach %.1f", lit, g_dynamic_lights[0].position[0],
             g_dynamic_lights[0].position[1], g_dynamic_lights[0].position[2], g_dynamic_lights[0].color[0], g_dynamic_lights[0].color[1],
             g_dynamic_lights[0].color[2], g_dynamic_lights[0].radius);
      else
        logf("scene: no moving level lights");
    }
  }
  res->Release();
}

static void on_world_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base_vertex, const ShaderRecord* ps) {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT stride = 0, offset = 0, ib_offset = 0;
  DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
  ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
  bool usable = vb && ib && stride == sizeof(WorldVertex) && offset == 0 && ib_offset == 0 && base_vertex == 0;
  if (usable && (vb != g_world.vb || ib != g_world.ib)) usable = load_level(ctx, vb, ib, ib_format);
  if (vb) vb->Release();
  if (ib) ib->Release();
  if (!usable) { fail_once("world draw with an unexpected buffer layout; ignored"); return; }

  if (g_camera_frame != g_frame) {
    g_camera_frame = g_frame;
    capture_camera(ctx);
    capture_dynamic_lights(ctx);
    capture_anim_brightness(ctx);
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
      ID3D11Resource* res = nullptr;
      rtv->GetResource(&res);
      rtv->Release();
      D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      if (res) res->GetType(&dim);
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        if (g_world_target) g_world_target->Release();
        g_world_target = static_cast<ID3D11Texture2D*>(res);   // keeps the reference from GetResource
        g_world_target_frame = g_frame;
      } else if (res) {
        res->Release();
      }
    }
  }
  auto known = g_world.ranges.find(start);
  if (known != g_world.ranges.end()) {
    // A later pass over a polygon we already have. Looked at once: if it alpha-blends a
    // different texture over the first one, that texture is the surface's second layer.
    WorldRange& r = known->second;
    if (r.seen_frame != g_frame) {   // its first pass in this frame
      r.seen_frame = g_frame;
      const void* view = bound_texture_view(ctx);
      if (view == r.last_view) return;
      // The texture bound is not the one we last saw here. Either this polygon came from
      // an earlier session and is being confirmed, or the engine has swapped its texture.
      bool confirming = r.last_view == nullptr;
      r.last_view = view;
      int t = texture_for(ctx);
      if (t < 0) return;
      bool rebuild = false;
      if (confirming) {
        WorldRange now;
        read_polygon_constants(ctx, now);
        if (now.unshaded != r.unshaded || (now.illumination >= 0.5f) != (r.illumination >= 0.5f)) {
          r.unshaded = now.unshaded;
          r.illumination = now.illumination;
          remember_flags(r);
          rebuild = true;
        }
      }
      bool new_content = r.texture < 0 || g_textures[t].hash != g_textures[r.texture].hash;
      r.texture = t;   // same content or not, follow the live texture
      if (new_content) {
        WorldRange now;
        read_polygon_constants(ctx, now);
        if (memcmp(now.scale, r.scale, sizeof r.scale) != 0 || memcmp(now.uv_matrix, r.uv_matrix, sizeof r.uv_matrix) != 0) {
          memcpy(r.scale, now.scale, sizeof r.scale);
          memcpy(r.uv_matrix, now.uv_matrix, sizeof r.uv_matrix);
          compute_polygon_uv(r);
        }
        if (confirming) {
          remember_polygon(r);   // what we remembered was out of date
          rebuild = true;
        } else if (!r.animated) {
          r.animated = true;
          g_world.animated.push_back(r.start);
          rebuild = true;        // once, to take it out of the level mesh
          if (g_world.animated.size() <= 16) logf("scene: level polygon at index %u changes texture while shown; drawn separately", r.start);
        }
      }
      if (rebuild) ++g_world.revision;
      return;
    }
    if (r.later_passes_checked >= 4 && r.glow_texture < 0) return;
    if (r.later_passes_checked < 255) ++r.later_passes_checked;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    bool layered = false, additive = false;
    if (blend) {
      D3D11_BLEND_DESC bd;
      blend->GetDesc(&bd);
      const auto& rt = bd.RenderTarget[0];
      layered = rt.BlendEnable && rt.SrcBlend == D3D11_BLEND_SRC_ALPHA && rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
      additive = rt.BlendEnable && rt.SrcBlend == D3D11_BLEND_SRC_ALPHA && rt.DestBlend == D3D11_BLEND_ONE;
      blend->Release();
    }
    if (!layered && !additive) return;
    if (additive && r.glow_texture >= 0) {
      // Known glow layer: follow its brightness, which the engine pulses on some screens.
      // A polygon may have two additive passes; the stronger one of a frame counts.
      WorldRange now;
      read_polygon_constants(ctx, now);
      const float* c = now.color;
      float strength = c[3] * (c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]));
      if (r.glow_frame != g_frame || strength > r.glow_strength) r.glow_strength = strength;
      r.glow_frame = g_frame;
      return;
    }
    if (layered && r.overlay_texture >= 0) return;
    int t = texture_for(ctx);
    if (t < 0 || g_textures[t].uniform || g_textures[t].solid) return;   // a blank layer adds nothing
    WorldRange constants;
    read_polygon_constants(ctx, constants);
    if (layered) {
      if (t == r.texture) return;
      r.overlay_texture = t;
      r.overlay_unshaded = constants.unshaded;
      memcpy(r.overlay_scale, constants.scale, sizeof r.overlay_scale);
      memcpy(r.overlay_uv_matrix, constants.uv_matrix, sizeof r.overlay_uv_matrix);
      remember_overlay(r);
      if (r.unshaded || r.overlay_unshaded) remember_flags(r);
    } else {
      const float* c = constants.color;
      float strength = c[3] * (c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]));
      if (!(constants.unshaded || constants.illumination >= 0.5f) || strength < 0.05f) return;
      r.glow_texture = t;
      r.glow_strength = strength;
      r.glow_frame = g_frame;
      memcpy(r.glow_scale, constants.scale, sizeof r.glow_scale);
      memcpy(r.glow_uv_matrix, constants.uv_matrix, sizeof r.glow_uv_matrix);
      g_world.glowing.push_back(r.start);
      remember_glow(r);
      return;   // glow layers are drawn on their own; the level mesh does not change
    }
    ++g_world.revision;
    return;
  }
  WorldRange range;
  range.start = start;
  range.seen_frame = g_frame;
  range.last_view = bound_texture_view(ctx);
  range.count = count;
  range.texture = texture_for(ctx);
  range.alpha_test = ps && ps->alpha_test;
  read_polygon_constants(ctx, range);
  compute_polygon_uv(range);
  g_world.ranges[start] = range;
  ++g_world.revision;
  remember_polygon(range);
  if (range.unshaded || range.illumination >= 0.5f) remember_flags(range);
}

static int model_for(ID3D11DeviceContext* ctx, ID3D11Buffer* vb, ID3D11Buffer* ib, DXGI_FORMAT ib_format) {
  auto key = std::make_pair(vb, ib);
  auto it = g_model_index.find(key);
  if (it != g_model_index.end()) return it->second;
  ModelMesh mesh;
  std::vector<uint8_t> vdata, idata;
  int index = -1;
  if (read_buffer(g_dev, ctx, vb, &vdata) && read_buffer(g_dev, ctx, ib, &idata)) {
    mesh.vertices.resize(vdata.size() / sizeof(ModelVertex));
    memcpy(mesh.vertices.data(), vdata.data(), mesh.vertices.size() * sizeof(ModelVertex));
    if (ib_format == DXGI_FORMAT_R32_UINT) {
      mesh.indices.resize(idata.size() / 4);
      memcpy(mesh.indices.data(), idata.data(), mesh.indices.size() * 4);
    } else {
      mesh.indices.resize(idata.size() / 2);
      for (size_t i = 0; i < mesh.indices.size(); ++i) mesh.indices[i] = idata[i * 2] | (idata[i * 2 + 1] << 8);
    }
    for (const auto& v : mesh.vertices) if (v.transform_id > mesh.max_transform_id && v.transform_id < 256) mesh.max_transform_id = v.transform_id;
    mesh.hash = fnv1a64(idata.data(), idata.size(), fnv1a64(vdata.data(), vdata.size()));
    vb->AddRef();
    ib->AddRef();
    mesh.vb = vb;
    mesh.ib = ib;
    index = (int)g_models.size();
    g_models.push_back(std::move(mesh));
  } else {
    fail_once("object buffer readback failed");
  }
  g_model_index[key] = index;
  return index;
}

// darkMDDrawCmd_s with shape keys, 80 bytes (DarkModel.inc).
struct DrawCommand {
  int sk_param[4]; float sk_weight[4]; int sk_tex_u; int sk_pad[3];
  float ambient[4];
  int light_start; int light_bits; int transform_start; float alpha_ref;
};

static uint64_t light_key(const float* position) {
  int q[3] = { (int)lrintf(position[0] * 8.0f), (int)lrintf(position[1] * 8.0f), (int)lrintf(position[2] * 8.0f) };   // 1/8 unit grid
  return fnv1a64(q, sizeof q);
}

static void harvest_lights(const uint8_t* lights, size_t lights_size, const DrawCommand& cmd) {
  int near_count = cmd.light_bits & 0xFFFF;
  for (int k = 0; k < 3; ++k) g_ambient_sum[k] += cmd.ambient[k];
  ++g_ambient_count;
  int far_count = (cmd.light_bits >> 16) & 0xFFFF;
  // "Far" lights are given as a vector to dot with the normal. For an ordinary light a long
  // way off the vector is direction / distance, much shorter than 1, and the light is in the
  // mission's table anyway. Only a unit vector is a true directional light (sunlight).
  for (int i = 0; i < far_count && i < 8; ++i) {
    size_t off = (size_t)(cmd.light_start + near_count + i) * 32;
    if (cmd.light_start < 0 || off + 32 > lights_size) break;
    float rec[8];
    memcpy(rec, lights + off, 32);
    float len = sqrtf(rec[0] * rec[0] + rec[1] * rec[1] + rec[2] * rec[2]);
    if (!(len > 0.9f) || !(rec[4] > 0.0f || rec[5] > 0.0f || rec[6] > 0.0f)) continue;
    bool known = false;
    for (SceneLight& l : g_far_lights) {
      float dot = (l.position[0] * rec[0] + l.position[1] * rec[1] + l.position[2] * rec[2]) / len;
      if (dot > 0.999f) { memcpy(l.color, rec + 4, 12); known = true; break; }
    }
    if (known || g_far_lights.size() >= 8) continue;
    SceneLight l;
    for (int k = 0; k < 3; ++k) l.position[k] = rec[k] / len;
    memcpy(l.color, rec + 4, 12);
    g_far_lights.push_back(l);
    logf("scene: directional light %zu towards (%.2f %.2f %.2f) colour %.2f %.2f %.2f, vector length %.2f", g_far_lights.size(), l.position[0],
         l.position[1], l.position[2], l.color[0], l.color[1], l.color[2], len);
  }
  for (int i = 0; i < near_count; ++i) {
    size_t off = (size_t)(cmd.light_start + i) * 32;
    if (cmd.light_start < 0 || off + 32 > lights_size) break;
    float rec[8];
    memcpy(rec, lights + off, 32);
    uint64_t key = light_key(rec);
    if (g_mission_lights) {
      // The mission's table says where the lights are. The engine's list for this object
      // has a brightness for each, but it is what reaches this object: zero when the
      // object is shadowed from the light, so taking it for the light's own brightness
      // switched lights off and on with whatever was on screen. FollowEngineLights=1
      // turns that following back on.
      static const bool follow = config_flag(L"FollowEngineLights", false);
      if (!follow) continue;
      auto found = g_light_index.find(key);
      if (found == g_light_index.end()) continue;
      SceneLight& l = g_lights[found->second];
      if (l.anim_mode >= 0 || l.feed >= 0) continue;   // replayed from the mission file or fed by the engine, see scene_animate_lights()
      if (l.spot) continue;             // for a spotlight the list holds what reaches this object, not the light's own brightness
      l.has_live = true;
      memcpy(l.live, rec + 4, 12);
      continue;
    }
    if (!(rec[4] > 0.0f || rec[5] > 0.0f || rec[6] > 0.0f)) continue;
    auto it = g_light_index.find(key);
    if (it != g_light_index.end()) {
      memcpy(g_lights[it->second].color, rec + 4, 12);   // follow brightness changes
      continue;
    }
    if (g_lights.size() >= 512) continue;
    SceneLight l;
    memcpy(l.position, rec, 12);
    memcpy(l.color, rec + 4, 12);
    g_light_index[key] = (int)g_lights.size();
    g_lights.push_back(l);
  }
}

static void on_object_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base_vertex, const ShaderRecord* ps) {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT stride = 0, offset = 0, ib_offset = 0;
  DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
  ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
  int model = -1;
  if (vb && ib && stride == sizeof(ModelVertex) && offset == 0 && ib_offset == 0 && base_vertex == 0) model = model_for(ctx, vb, ib, ib_format);
  if (vb) vb->Release();
  if (ib) ib->Release();
  if (model < 0) { fail_once("object draw with an unexpected buffer layout; ignored"); return; }

  ID3D11Buffer* cbs[3] = {};
  ctx->VSGetConstantBuffers(0, 3, cbs);
  ID3D11Buffer* light_buf = vs_resource_buffer(ctx, 0);   // kModelLightBufferSlot
  ID3D11Buffer* cmd_buf = vs_resource_buffer(ctx, 1);     // kModelDrawCommandBufferSlot
  ID3D11Buffer* node_buf = vs_resource_buffer(ctx, 2);    // kModelNodeTransformBufferSlot
  std::vector<uint8_t> s1, s2, s3, s4, s5;
  const uint8_t *params = nullptr, *material = nullptr, *cmds = nullptr, *nodes = nullptr, *lights = nullptr;
  size_t params_size = 0, material_size = 0, cmds_size = 0, nodes_size = 0, lights_size = 0;
  bool ok = buffer_bytes(ctx, cbs[1], &params, &params_size, &s1) && params_size >= 32 &&
            buffer_bytes(ctx, cmd_buf, &cmds, &cmds_size, &s2) && buffer_bytes(ctx, node_buf, &nodes, &nodes_size, &s3);
  if (ok) {
    int draw_index = 0;
    memcpy(&draw_index, params + 28, 4);   // DarkNewParams.nDrawIndex
    DrawCommand cmd;
    if (draw_index >= 0 && (size_t)(draw_index + 1) * sizeof cmd <= cmds_size) {
      memcpy(&cmd, cmds + (size_t)draw_index * sizeof cmd, sizeof cmd);
      const ModelMesh& mesh = g_models[model];
      ObjectDraw d;
      d.model = model;
      d.start = start;
      d.count = count;
      d.texture = texture_for(ctx);
      d.alpha_test = ps && ps->alpha_test;
      static const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
      memcpy(d.uv_matrix, identity, sizeof identity);
      // DarkMaterialConstants: UVMatrix (48 bytes), Color (+48), Illumination (+64), Shade (+80).
      if (buffer_bytes(ctx, cbs[2], &material, &material_size, &s4) && material_size >= 84) {
        memcpy(d.uv_matrix, material, 48);
        memcpy(d.color, material + 48, 16);
        int shade = 1;
        memcpy(&shade, material + 80, 4);
        d.unshaded = shade == 0;
        memcpy(&d.illumination, material + 64, 4);
      }
      ID3D11BlendState* blend = nullptr;
      FLOAT blend_factor[4];
      UINT sample_mask = 0;
      ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
      if (blend) {
        D3D11_BLEND_DESC bd;
        blend->GetDesc(&bd);
        d.blended = bd.RenderTarget[0].BlendEnable != 0;
        d.additive = d.blended && bd.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA && bd.RenderTarget[0].DestBlend == D3D11_BLEND_ONE;
        d.alpha_blend = d.blended && bd.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA && bd.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
        blend->Release();
      }
      ID3D11RasterizerState* raster = nullptr;
      ctx->RSGetState(&raster);
      if (raster) {
        D3D11_RASTERIZER_DESC rd;
        raster->GetDesc(&rd);
        d.two_sided = rd.CullMode == D3D11_CULL_NONE;
        raster->Release();
      } else {
        d.two_sided = false;   // the default state culls back faces
      }
      d.node_count = mesh.max_transform_id + 1;
      size_t first = (size_t)cmd.transform_start * 64, bytes = (size_t)d.node_count * 64;
      if (cmd.transform_start >= 0 && first + bytes <= nodes_size) {
        // The engine often draws the same geometry again right away (a blended second
        // pass, or a flat-colour pass followed by the textured one). Keep one instance.
        ObjectDraw* prev = g_frame_objects.empty() ? nullptr : &g_frame_objects.back();
        bool repeat = prev && prev->model == d.model && prev->start == d.start && prev->count == d.count && prev->node_count == d.node_count &&
                      memcmp(&g_frame_nodes[(size_t)prev->node_offset * 16], nodes + first, bytes) == 0;
        if (repeat) {
          bool prev_flat = prev->texture < 0 || g_textures[prev->texture].solid;
          bool this_flat = d.texture < 0 || g_textures[d.texture].solid;
          // A blended, self-lit repeat is the surface's glow: bright where the texture's
          // alpha says so, on top of the normally lit surface.
          bool lit_layer = (d.additive || d.alpha_blend) && (d.unshaded || d.illumination > 0.5f) && d.texture >= 0 && !g_textures[d.texture].solid &&
                           !g_textures[d.texture].uniform && !prev->blended;
          if (lit_layer) {
            const float* c = d.color;
            float strength = c[3] * (c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]));
            bool base_self_lit = prev->unshaded || prev->illumination >= 0.5f;
            if (strength >= 0.05f && !(base_self_lit && strength < 0.5f) && strength > prev->glow_strength) {
              prev->glow = true;
              prev->glow_texture = d.texture;
              prev->glow_strength = strength;
            }
          } else {
            prev->unshaded = prev->unshaded || d.unshaded;
          }
          if (prev_flat && !this_flat) {   // the textured pass describes the surface better
            prev->texture = d.texture;
            prev->alpha_test = d.alpha_test;
            memcpy(prev->color, d.color, sizeof d.color);
            memcpy(prev->uv_matrix, d.uv_matrix, sizeof d.uv_matrix);
          }
        } else {
          d.node_offset = (uint32_t)(g_frame_nodes.size() / 16);
          g_frame_nodes.resize(g_frame_nodes.size() + (size_t)d.node_count * 16);
          memcpy(&g_frame_nodes[(size_t)d.node_offset * 16], nodes + first, bytes);
          g_frame_objects.push_back(d);
        }
      }
      if (buffer_bytes(ctx, light_buf, &lights, &lights_size, &s5)) harvest_lights(lights, lights_size, cmd);
    }
  } else {
    fail_once("object draw: constants not readable");
  }
  for (auto* b : cbs) if (b) b->Release();
  if (light_buf) light_buf->Release();
  if (cmd_buf) cmd_buf->Release();
  if (node_buf) node_buf->Release();
}

static int skinned_for(ID3D11DeviceContext* ctx, ID3D11Buffer* vb, ID3D11Buffer* ib, DXGI_FORMAT ib_format) {
  auto key = std::make_pair(vb, ib);
  auto it = g_skinned_index.find(key);
  if (it != g_skinned_index.end()) return it->second;
  SkinnedMesh mesh;
  std::vector<uint8_t> vdata, idata;
  int index = -1;
  if (read_buffer(g_dev, ctx, vb, &vdata) && read_buffer(g_dev, ctx, ib, &idata)) {
    mesh.vertices.resize(vdata.size() / sizeof(SkinnedVertex));
    memcpy(mesh.vertices.data(), vdata.data(), mesh.vertices.size() * sizeof(SkinnedVertex));
    if (ib_format == DXGI_FORMAT_R32_UINT) {
      mesh.indices.resize(idata.size() / 4);
      memcpy(mesh.indices.data(), idata.data(), mesh.indices.size() * 4);
    } else {
      mesh.indices.resize(idata.size() / 2);
      for (size_t i = 0; i < mesh.indices.size(); ++i) mesh.indices[i] = idata[i * 2] | (idata[i * 2 + 1] << 8);
    }
    for (const auto& v : mesh.vertices)
      for (int k = 0; k < 4; ++k) if (v.weights[k] && v.joints[k] > mesh.max_joint) mesh.max_joint = v.joints[k];
    mesh.hash = fnv1a64(idata.data(), idata.size(), fnv1a64(vdata.data(), vdata.size()));
    vb->AddRef();
    ib->AddRef();
    mesh.vb = vb;
    mesh.ib = ib;
    index = (int)g_skinned.size();
    logf("scene: skinned model %d: %zu vertices, %zu indices, %u joints", index, mesh.vertices.size(), mesh.indices.size(), mesh.max_joint + 1);
    g_skinned.push_back(std::move(mesh));
  } else {
    fail_once("character buffer readback failed");
  }
  g_skinned_index[key] = index;
  return index;
}

// DarkEMMModel.shader: world = sum over joints of weight * node * (vertex - offset).
static void on_skinned_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base_vertex, const ShaderRecord* ps) {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT stride = 0, offset = 0, ib_offset = 0;
  DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
  ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
  int model = -1;
  if (vb && ib && stride == sizeof(SkinnedVertex) && offset == 0 && ib_offset == 0 && base_vertex == 0) model = skinned_for(ctx, vb, ib, ib_format);
  if (vb) vb->Release();
  if (ib) ib->Release();
  if (model < 0) { fail_once("character draw with an unexpected buffer layout; ignored"); return; }

  ID3D11Buffer* cbs[3] = {};
  ctx->VSGetConstantBuffers(0, 3, cbs);
  ID3D11Buffer* light_buf = vs_resource_buffer(ctx, 0);
  ID3D11Buffer* cmd_buf = vs_resource_buffer(ctx, 1);
  ID3D11Buffer* node_buf = vs_resource_buffer(ctx, 2);
  ID3D11Buffer* offset_buf = vs_resource_buffer(ctx, 3);   // quatArray: per-joint bind offsets
  std::vector<uint8_t> s1, s2, s3, s4, s5, s6;
  const uint8_t *params = nullptr, *material = nullptr, *cmds = nullptr, *nodes = nullptr, *offsets = nullptr, *lights = nullptr;
  size_t params_size = 0, material_size = 0, cmds_size = 0, nodes_size = 0, offsets_size = 0, lights_size = 0;
  bool ok = buffer_bytes(ctx, cbs[1], &params, &params_size, &s1) && params_size >= 32 && buffer_bytes(ctx, cmd_buf, &cmds, &cmds_size, &s2) &&
            buffer_bytes(ctx, node_buf, &nodes, &nodes_size, &s3) && buffer_bytes(ctx, offset_buf, &offsets, &offsets_size, &s6);
  if (ok) {
    int draw_index = 0;
    memcpy(&draw_index, params + 28, 4);
    DrawCommand cmd;
    if (draw_index >= 0 && (size_t)(draw_index + 1) * sizeof cmd <= cmds_size) {
      memcpy(&cmd, cmds + (size_t)draw_index * sizeof cmd, sizeof cmd);
      const SkinnedMesh& mesh = g_skinned[model];
      SkinnedDraw d;
      d.model = model;
      d.start = start;
      d.count = count;
      d.texture = texture_for(ctx);
      d.alpha_test = ps && ps->alpha_test;
      static const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
      memcpy(d.uv_matrix, identity, sizeof identity);
      if (buffer_bytes(ctx, cbs[2], &material, &material_size, &s4) && material_size >= 64) {
        memcpy(d.uv_matrix, material, 48);
        memcpy(d.color, material + 48, 16);
      }
      ID3D11BlendState* blend = nullptr;
      FLOAT blend_factor[4];
      UINT sample_mask = 0;
      ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
      if (blend) {
        D3D11_BLEND_DESC bd;
        blend->GetDesc(&bd);
        d.blended = bd.RenderTarget[0].BlendEnable != 0;
        blend->Release();
      }
      d.bone_count = mesh.max_joint + 1;
      size_t first = (size_t)cmd.transform_start;
      if (cmd.transform_start >= 0 && (first + d.bone_count) * 64 <= nodes_size && (first + d.bone_count) * 16 <= offsets_size) {
        std::vector<float> bones((size_t)d.bone_count * 12);
        for (uint32_t j = 0; j < d.bone_count; ++j) {
          float n[16], q[4];
          memcpy(n, nodes + (first + j) * 64, 64);      // column-major
          memcpy(q, offsets + (first + j) * 16, 16);
          float* b = &bones[(size_t)j * 12];
          for (int r = 0; r < 3; ++r) {
            b[r * 4 + 0] = n[0 * 4 + r];
            b[r * 4 + 1] = n[1 * 4 + r];
            b[r * 4 + 2] = n[2 * 4 + r];
            // translation of node * translate(-offset); the offset's w takes the node's own translation with it
            b[r * 4 + 3] = n[3 * 4 + r] * (1.0f - q[3]) - (n[0 * 4 + r] * q[0] + n[1 * 4 + r] * q[1] + n[2 * 4 + r] * q[2]);
          }
        }
        // Same repeated-pass rule as for rigid objects.
        SkinnedDraw* prev = g_frame_skinned.empty() ? nullptr : &g_frame_skinned.back();
        bool repeat = prev && prev->model == d.model && prev->start == d.start && prev->count == d.count && prev->bone_count == d.bone_count &&
                      memcmp(&g_frame_bones[(size_t)prev->bone_offset * 12], bones.data(), bones.size() * sizeof(float)) == 0;
        if (repeat) {
          bool prev_flat = prev->texture < 0 || g_textures[prev->texture].solid;
          bool this_flat = d.texture < 0 || g_textures[d.texture].solid;
          if (prev_flat && !this_flat) { prev->texture = d.texture; prev->alpha_test = d.alpha_test; memcpy(prev->color, d.color, sizeof d.color); }
        } else {
          d.bone_offset = (uint32_t)(g_frame_bones.size() / 12);
          g_frame_bones.insert(g_frame_bones.end(), bones.begin(), bones.end());
          g_frame_skinned.push_back(d);
        }
      }
      if (buffer_bytes(ctx, light_buf, &lights, &lights_size, &s5)) harvest_lights(lights, lights_size, cmd);
    }
  } else {
    fail_once("character draw: constants not readable");
  }
  for (auto* b : cbs) if (b) b->Release();
  for (auto* b : { light_buf, cmd_buf, node_buf, offset_buf }) if (b) b->Release();
}

void scene_on_draw_indexed(ID3D11DeviceContext* ctx, const ShaderRecord* vs, const ShaderRecord* ps, UINT count, UINT start, INT base_vertex) {
  if (!g_dev || ctx != g_ctx || !vs) return;
  if (vs->cls == ShaderClass::Sky) { if (g_world.loaded) on_sky_draw(ctx, count, start); return; }
  if (vs->cls == ShaderClass::WorldRep) on_world_draw(ctx, count, start, base_vertex, ps);
  else if (vs->cls == ShaderClass::MDModel && g_world.loaded) on_object_draw(ctx, count, start, base_vertex, ps);
  else if (vs->cls == ShaderClass::EMMModel && g_world.loaded) on_skinned_draw(ctx, count, start, base_vertex, ps);
  else if (vs->cls == ShaderClass::MMModel && g_world.loaded) fail_once("DarkMMModel character draw seen: that skinning path is not handled yet");
}

ID3D11Texture2D* scene_world_target() { return g_world_target_frame == g_frame ? g_world_target : nullptr; }

// Ties each animated light to an entry of the engine's brightness buffer. A level vertex
// carries its polygon's number, how many animated layers the polygon has and where its run
// of entries starts; the mission file says which light each layer of each polygon is. The
// two are only trusted if they agree on the layer count of practically every polygon.
static void connect_light_feeds(const std::vector<int>& scene_index) {
  const MissionPolyLights& file = mission_poly_lights();
  g_anim_entries = 0;
  if (file.first.size() < 2) return;
  size_t polys = file.first.size() - 1, checked = 0, agree = 0, layered = 0, entries = 0;
  std::vector<uint8_t> seen(polys, 0);
  unsigned connected = 0;
  std::vector<std::pair<SceneLight*, int>> picks;
  for (const WorldVertex& v : g_world.vertices) {
    uint32_t poly = v.flags & 0x7FFFF, layers = v.flags >> 19 & 31, start = v.flags2 & 0x7FFFF;
    if (poly >= polys || seen[poly]) continue;
    if (fabsf(v.position[0]) >= 1199.0f && fabsf(v.position[1]) >= 1199.0f && fabsf(v.position[2]) >= 1199.0f) continue;   // the sky box
    seen[poly] = 1;
    ++checked;
    uint32_t in_file = file.first[poly + 1] - file.first[poly];
    if (in_file != layers) continue;
    ++agree;
    if (!layers) continue;
    ++layered;
    if (start + layers > entries) entries = start + layers;
    for (uint32_t i = 0; i < layers; ++i) {
      unsigned light = file.lights[file.first[poly] + i];
      if (light >= scene_index.size() || scene_index[light] < 0) continue;
      picks.emplace_back(&g_lights[scene_index[light]], (int)(start + i));
    }
  }
  bool trusted = checked > 0 && agree * 1000 >= checked * 995 && layered > 0;
  if (trusted) {
    for (auto& pick : picks)
      if (pick.first->feed < 0) { pick.first->feed = pick.second; ++connected; }
    g_anim_entries = entries;
  }
  logf("scene: animated lightmap layers: %zu of %zu polygons agree with the mission file, %zu have layers; %s, %u lights connected to the engine's brightness values",
       agree, checked, layered, trusted ? "trusted" : "NOT trusted", connected);
}

static void try_mission_lights() {
  if (g_mission_lights || !g_world.loaded) return;
  std::vector<MissionLight> table;
  std::string source;
  bool found = false;
  if (!g_mission_geometry_tried) {
    // First choice, once per level: recognise the mission by a few of the level's own
    // vertices. Needs nothing from the frame, so the lights are there from the first one.
    g_mission_geometry_tried = true;
    std::vector<float> vertices;
    size_t n = g_world.vertices.size();
    for (size_t pick : { n / 7, n / 3, n / 2, n * 4 / 5 })
      if (pick < n) vertices.insert(vertices.end(), g_world.vertices[pick].position, g_world.vertices[pick].position + 3);
    found = mission_find_by_geometry(vertices, &table, &source);
  }
  if (!found) {
    // Otherwise by the lights the engine has shown in its per-object lists.
    if (g_lights.size() < 4 || g_mission_attempts >= 3 || g_lights.size() < g_mission_tried_with + 4) return;
    ++g_mission_attempts;
    g_mission_tried_with = g_lights.size();
    std::vector<float> anchors;
    for (const SceneLight& l : g_lights) anchors.insert(anchors.end(), l.position, l.position + 3);
    if (!mission_find_lights(anchors, &table, &source)) return;
  }
  g_lights.clear();
  g_light_index.clear();
  std::vector<int> scene_index(table.size(), -1);   // table entry -> g_lights entry
  std::vector<uint8_t> can_come_on(table.size(), 0);   // dark in the file, but an animated light: the game may light it
  for (const MissionLightAnim& a : mission_light_anims())
    if (a.index >= 0 && (size_t)a.index < table.size() && a.max > 0.0f) can_come_on[a.index] = 1;
  for (size_t entry = 0; entry < table.size(); ++entry) {
    const MissionLight& m = table[entry];
    if (!(m.color[0] > 0.0f || m.color[1] > 0.0f || m.color[2] > 0.0f) && !can_come_on[entry]) continue;
    scene_index[entry] = (int)g_lights.size();
    SceneLight l;
    memcpy(l.position, m.position, 12);
    memcpy(l.color, m.color, 12);
    memcpy(l.direction, m.direction, 12);
    l.spot = m.inner != -1.0f;
    l.inner = m.inner;
    l.outer = m.outer;
    l.radius = m.radius;
    g_light_index[light_key(l.position)] = (int)g_lights.size();
    g_lights.push_back(l);
  }
  // Lights that change on their own. Steady ones (held at a level, or asleep) are left to
  // the table and to what the engine shows.
  unsigned self_changing = 0;
  for (const MissionLightAnim& a : mission_light_anims()) {
    if (a.index < 0 || (size_t)a.index >= scene_index.size() || scene_index[a.index] < 0) continue;
    bool changes = a.mode == 0 || a.mode == 1 || a.mode == 2 || a.mode == 8 || a.mode == 9;
    if (!changes || a.inactive || !(a.current > 0.0f) || a.rise_ms + a.fall_ms <= 0) continue;
    SceneLight& l = g_lights[scene_index[a.index]];
    l.anim_mode = a.mode;
    l.rise_ms = a.rise_ms > 0 ? a.rise_ms : 1;
    l.fall_ms = a.fall_ms > 0 ? a.fall_ms : 1;
    l.phase_ms = a.countdown_ms;
    l.anim_low = a.min / a.current;
    l.anim_high = a.max / a.current;
    // The file was saved with the light at `current`: start it there, and stagger lights
    // of one kind so that they do not all tick together.
    l.anim_up = a.current >= (a.min + a.max) * 0.5f;
    l.anim_level = l.anim_from = l.anim_to = 1.0f;
    l.anim_span_ms = (float)(l.anim_up ? l.fall_ms : l.rise_ms);
    l.anim_left_ms = a.countdown_ms > 0 ? (float)a.countdown_ms : l.anim_span_ms * (float)((self_changing * 37) % 97) / 97.0f;
    if (l.anim_span_ms < 1.0f) l.anim_span_ms = 1.0f;
    ++self_changing;
  }
  logf("scene: %u of the mission's lights flash, slide or flicker on their own; their rhythm is replayed", self_changing);
  connect_light_feeds(scene_index);
  // What each animated light is at the engine's full value: the table has it at the
  // brightness the file was saved with; one saved dark has only its colour setting left.
  unsigned fed = 0;
  for (const MissionLightAnim& a : mission_light_anims()) {
    if (a.index < 0 || (size_t)a.index >= table.size() || scene_index[a.index] < 0) continue;
    SceneLight& l = g_lights[scene_index[a.index]];
    if (l.feed < 0 || !(a.max > 0.0f)) { l.feed = -1; continue; }
    const MissionLight& m = table[a.index];
    if (a.current > 0.0f && (m.color[0] > 0.0f || m.color[1] > 0.0f || m.color[2] > 0.0f)) {
      for (int k = 0; k < 3; ++k) l.feed_full[k] = m.color[k] * a.max / a.current;
    } else {
      float h = a.hue - floorf(a.hue), sat = a.saturation < 0.0f ? 0.0f : a.saturation > 1.0f ? 1.0f : a.saturation;
      float rgb[3];
      float t[3] = { fabsf(6.0f * h - 3.0f) - 1.0f, 2.0f - fabsf(6.0f * h - 2.0f), 2.0f - fabsf(6.0f * h - 4.0f) };   // red, green, blue of the hue
      for (int k = 0; k < 3; ++k) rgb[k] = 1.0f - sat + sat * (t[k] < 0.0f ? 0.0f : t[k] > 1.0f ? 1.0f : t[k]);
      for (int k = 0; k < 3; ++k) l.feed_full[k] = rgb[k] * a.max / 32.0f;
    }
    ++fed;
  }
  if (fed) logf("scene: %u animated lights take their brightness from the engine each frame", fed);
  g_mission_lights = true;
  // The table, for looking at a level's lighting offline.
  wchar_t name[80];
  swprintf(name, 80, L"..\\levels\\%016llx-lights.txt", (unsigned long long)g_world.vb_hash);
  FILE* dump = _wfopen((g_texture_dir + name).c_str(), L"w");
  if (dump) {
    fprintf(dump, "# %s\n# x y z  r g b  dirx diry dirz  inner outer radius\n", source.c_str());
    for (const SceneLight& l : g_lights)
      fprintf(dump, "%g %g %g  %g %g %g  %g %g %g  %g %g %g\n", l.position[0], l.position[1], l.position[2], l.color[0], l.color[1], l.color[2],
              l.direction[0], l.direction[1], l.direction[2], l.inner, l.outer, l.radius);
    fclose(dump);
  }
  ++g_lights_generation;
}

// The engine's rules for a light that changes on its own, replayed. A flipping light sits
// at its dimmest for its "rising" time and at its brightest for its "falling" time (an
// alarm strobe is 1000 and 100 ms: a tenth of a second of light every 1.1 s). A flickering
// one does the same with each wait cut to a random length. A sliding one ramps up over the
// rising time and down over the falling time. The random kinds pick a new level each time.
void scene_animate_lights(double seconds) {
  if (!g_mission_lights) return;
  static double last = -1.0;
  double dt_ms = last < 0.0 ? 0.0 : (seconds - last) * 1000.0;
  last = seconds;
  if (dt_ms < 0.0 || dt_ms > 500.0) dt_ms = 16.0;   // a pause or a hitch is not half a second of flicker
  static uint64_t rng = 0x9e3779b97f4a7c15ull;
  auto random = [&]() {   // 0..1
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (float)(rng >> 40) / 16777216.0f;
  };
  for (SceneLight& l : g_lights) {
    if (l.feed >= 0 && (size_t)l.feed < g_anim_values.size()) {   // the engine's own value for it
      float v = g_anim_values[l.feed];
      if (!(v >= 0.0f)) v = 0.0f;
      if (v > 4.0f) v = 4.0f;
      l.has_live = true;
      for (int k = 0; k < 3; ++k) l.live[k] = l.feed_full[k] * v;
      continue;
    }
    if (l.anim_mode < 0) continue;
    float span = l.anim_high - l.anim_low;
    l.anim_left_ms -= (float)dt_ms;
    for (int guard = 0; l.anim_left_ms <= 0.0f && guard < 8; ++guard) {   // the state has run out: next one
      l.anim_up = !l.anim_up;
      float wait = (float)(l.anim_up ? l.fall_ms : l.rise_ms);   // up: bright until it falls; down: dim until it rises
      switch (l.anim_mode) {
        case 9: wait *= 0.05f + 0.95f * random(); break;
        case 1: wait = (float)(l.anim_up ? l.rise_ms : l.fall_ms); break;   // the ramp itself takes that long
        case 2: l.anim_to = l.anim_low + span * random(); break;
        case 8:
          l.anim_from = l.anim_level;
          l.anim_to = l.anim_low + span * random();
          wait = (float)(l.anim_to > l.anim_from ? l.rise_ms : l.fall_ms);
          break;
      }
      if (wait < 1.0f) wait = 1.0f;
      l.anim_span_ms = wait;
      l.anim_left_ms += wait;
    }
    float done = 1.0f - l.anim_left_ms / l.anim_span_ms;   // 0..1 through the current state
    if (done < 0.0f) done = 0.0f;
    if (done > 1.0f) done = 1.0f;
    switch (l.anim_mode) {
      case 0: case 9: l.anim_level = l.anim_up ? l.anim_high : l.anim_low; break;
      case 1: l.anim_level = l.anim_up ? l.anim_low + span * done : l.anim_high - span * done; break;
      case 2: l.anim_level = l.anim_to; break;
      case 8: l.anim_level = l.anim_from + (l.anim_to - l.anim_from) * done; break;
    }
    float level = l.anim_level < 0.0f ? 0.0f : l.anim_level;
    l.has_live = true;
    for (int k = 0; k < 3; ++k) l.live[k] = l.color[k] * level;
  }
}

void scene_frame_end() {
  g_serial_two_frames_ago = g_serial_last_frame;
  g_serial_last_frame = shadow_serial();
  flush_remembered_polygons();
  try_mission_lights();
  if (g_ambient_count) {
    for (int k = 0; k < 3; ++k) g_ambient[k] = (float)(g_ambient_sum[k] / g_ambient_count);
    g_ambient_sum[0] = g_ambient_sum[1] = g_ambient_sum[2] = 0.0;
    g_ambient_count = 0;
  }
  ++g_frame;
  g_frame_objects.clear();
  g_frame_nodes.clear();
  g_frame_skinned.clear();
  g_frame_bones.clear();
  if (g_frame % 1200 == 600 && g_mission_lights) {
    unsigned followed = 0, replayed = 0, dark = 0;
    for (const SceneLight& l : g_lights) {
      if (!l.has_live) continue;
      if (l.anim_mode >= 0) ++replayed; else ++followed;
      dark += !(l.live[0] > 0.0f || l.live[1] > 0.0f || l.live[2] > 0.0f);
      static int shown;
      float live = l.live[0] + l.live[1] + l.live[2], table = l.color[0] + l.color[1] + l.color[2];
      if (l.anim_mode < 0 && table > 0.0f && (live > table * 1.15f || live < table * 0.85f) && shown < 6) {
        ++shown;
        logf("scene: light at (%.1f %.1f %.1f): table %.2f %.2f %.2f, engine now %.2f %.2f %.2f", l.position[0], l.position[1], l.position[2],
             l.color[0], l.color[1], l.color[2], l.live[0], l.live[1], l.live[2]);
      }
    }
    logf("scene: of %zu mission lights, %u follow the engine's current value and %u replay their own rhythm; %u are dark right now", g_lights.size(),
         followed, replayed, dark);
  }
  if (g_frame % 1200 == 0 && g_readback_fallbacks)
    logf("scene: %llu slow buffer readbacks so far (buffers not covered by shadow copies)", (unsigned long long)g_readback_fallbacks);
}

const World& scene_world() { return g_world; }
const std::vector<SceneTexture>& scene_textures() { return g_textures; }
const Camera& scene_camera() { return g_camera; }
const std::vector<ModelMesh>& scene_models() { return g_models; }
const std::vector<ObjectDraw>& scene_frame_objects() { return g_frame_objects; }
const std::vector<float>& scene_frame_nodes() { return g_frame_nodes; }
const std::vector<SkinnedMesh>& scene_skinned_models() { return g_skinned; }
const std::vector<SkinnedDraw>& scene_frame_skinned() { return g_frame_skinned; }
const std::vector<float>& scene_frame_bones() { return g_frame_bones; }
const std::vector<SceneLight>& scene_lights() { return g_lights; }
const std::vector<SceneLight>& scene_dynamic_lights() { return g_dynamic_lights; }
const float* scene_ambient() { return g_ambient; }
const std::vector<SceneLight>& scene_far_lights() { return g_far_lights; }
unsigned scene_lights_generation() { return g_lights_generation; }

}  // namespace ss2rtx
