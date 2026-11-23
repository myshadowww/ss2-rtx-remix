// Mission file layout (Dark Engine): a 4-byte offset to a chunk inventory at the end of
// the file; each inventory entry is a 12-byte name, offset and length. The world chunk
// (WREXT, WRRGB or WR) holds the level's cells followed by the static light table: two
// counts, then 768 records of 48 bytes (position, direction, RGB brightness, inner, outer,
// radius).
//
// Rather than walking every cell to reach the table, we find it from a light position we
// already know from runtime, then confirm by matching the other known lights.
#include "mission.h"

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "kpf.h"
#include "log.h"

namespace ss2rtx {

static const size_t kRecord = 48;
static const unsigned kMaxLights = 768;

static bool ends_with_mis(const std::string& name) {
  size_t n = name.size();
  return n > 4 && (name.compare(n - 4, 4, ".mis") == 0 || name.compare(n - 4, 4, ".MIS") == 0);
}

// Returns the number of anchors found in the table starting at `table` with `count` records.
static unsigned match_count(const unsigned char* table, unsigned count, const std::vector<float>& anchors) {
  unsigned found = 0;
  for (size_t a = 0; a + 2 < anchors.size(); a += 3) {
    for (unsigned i = 0; i < count; ++i) {
      float p[3];
      memcpy(p, table + (size_t)i * kRecord, 12);
      if (fabsf(p[0] - anchors[a]) < 0.02f && fabsf(p[1] - anchors[a + 1]) < 0.02f && fabsf(p[2] - anchors[a + 2]) < 0.02f) { ++found; break; }
    }
  }
  return found;
}

static bool find_table(const std::vector<unsigned char>& chunk, const std::vector<float>& anchors, std::vector<MissionLight>* out) {
  if (anchors.size() < 3 || chunk.size() < kRecord + 8) return false;
  unsigned char key[12];
  memcpy(key, anchors.data(), 12);
  unsigned needed = (unsigned)(anchors.size() / 3);
  needed = needed > 3 ? needed * 4 / 5 : needed;   // tolerate a few dynamic lights among the anchors
  for (size_t hit = 8; hit + kRecord <= chunk.size(); ++hit) {
    if (chunk[hit] != key[0] || memcmp(&chunk[hit], key, 12) != 0) continue;
    for (unsigned k = 0; k < kMaxLights && hit >= (size_t)k * kRecord + 8; ++k) {
      size_t start = hit - (size_t)k * kRecord;
      unsigned num_static, num_dynamic;
      memcpy(&num_static, &chunk[start - 8], 4);
      memcpy(&num_dynamic, &chunk[start - 4], 4);
      if (num_static == 0 || num_static > kMaxLights || num_dynamic > 64 || k >= num_static) continue;
      if (start + (size_t)num_static * kRecord > chunk.size()) continue;
      if (match_count(&chunk[start], num_static, anchors) < needed) continue;
      out->resize(num_static);
      memcpy(out->data(), &chunk[start], (size_t)num_static * kRecord);
      return true;
    }
  }
  return false;
}

static bool contains(const std::vector<unsigned char>& hay, const void* needle, size_t n, size_t* where = nullptr, size_t from = 0) {
  if (hay.size() < n) return false;
  const unsigned char* key = static_cast<const unsigned char*>(needle);
  for (size_t i = from; i + n <= hay.size(); ++i) {
    const unsigned char* at = static_cast<const unsigned char*>(memchr(&hay[i], key[0], hay.size() - n - i + 1));
    if (!at) return false;
    i = (size_t)(at - hay.data());
    if (memcmp(at, key, n) == 0) { if (where) *where = i; return true; }
  }
  return false;
}

// One chunk of a mission file, without its 24-byte header.
static bool read_chunk(const std::wstring& archive, const KpfEntry& e, const std::vector<unsigned char>& inventory, const char* name,
                       std::vector<unsigned char>* out) {
  for (size_t i = 0; i + 20 <= inventory.size(); i += 20) {
    if (strncmp((const char*)&inventory[i], name, 12) != 0) continue;
    unsigned offset, length;
    memcpy(&offset, &inventory[i + 12], 4);
    memcpy(&length, &inventory[i + 16], 4);
    if ((uint64_t)offset + 24 + length > e.size) return false;
    out->resize(length);
    return length == 0 || kpf_read(archive, e, (uint64_t)offset + 24, length, out->data());
  }
  return false;
}

// Property chunks are runs of { object id, byte count, data }.
template <typename F> static void each_record(const std::vector<unsigned char>& chunk, F&& f) {
  for (size_t p = 0; p + 8 <= chunk.size();) {
    int object;
    unsigned size;
    memcpy(&object, &chunk[p], 4);
    memcpy(&size, &chunk[p + 4], 4);
    if (p + 8 + size > chunk.size()) break;
    f(object, &chunk[p + 8], size);
    p += 8 + (size_t)size;
  }
}

static std::vector<MissionLightAnim> g_anims;
static MissionPolyLights g_poly_lights;

const MissionPolyLights& mission_poly_lights() { return g_poly_lights; }

// The world chunk's cells, walked only for what says which animated lights have a lightmap
// layer on which polygon. `kind`: 0 WR, 1 WRRGB, 2 WREXT; they differ in header, in the size
// of a polygon's texturing record and in the bytes per lightmap texel.
// Returns where the cells end in the chunk, or 0.
static size_t read_cells(const std::vector<unsigned char>& w, int kind) {
  g_poly_lights.first.clear();
  g_poly_lights.lights.clear();
  g_poly_lights.water.clear();
  g_poly_lights.baked.clear();
  g_poly_lights.layer.clear();
  g_poly_lights.sample_first.clear();
  g_poly_lights.samples.clear();
  g_poly_lights.layer_sample_first.clear();
  g_poly_lights.layer_samples.clear();
  auto u32 = [&](size_t at) { unsigned v = 0; if (at + 4 <= w.size()) memcpy(&v, &w[at], 4); return v; };
  size_t p = kind == 2 ? 28 : 8;
  unsigned cells = u32(kind == 2 ? 24 : 4);
  size_t texel = kind == 2 ? (u32(12) == 0 ? 2 : 4) : kind == 1 ? 2 : 1;
  size_t render_poly = kind == 2 ? 52 : 48;
  bool ok = w.size() > p && cells > 0 && cells < 100000;
  struct Opening { unsigned polygon, target; unsigned char medium; };
  std::vector<Opening> openings;
  std::vector<unsigned char> media;
  for (unsigned c = 0; c < cells && ok; ++c) {
    if (p + 31 > w.size()) { ok = false; break; }
    unsigned vertices = w[p], polys = w[p + 1], render_polys = w[p + 2], portals = w[p + 3], planes = w[p + 4], medium = w[p + 5], anim_lights = w[p + 13];
    unsigned solid = polys >= portals ? polys - portals : polys;   // the polygons after these are openings to other cells
    media.push_back((unsigned char)medium);
    size_t vertices_at = p + 31, polys_at = vertices_at + 12 * (size_t)vertices, render_at = polys_at + 8 * (size_t)polys;
    p += 31 + 12 * (size_t)vertices + 8 * (size_t)polys + render_poly * render_polys;
    if (p + 4 > w.size()) { ok = false; break; }
    size_t index_at = p + 4, index_bytes = u32(p), index_next = 0;
    p += 4 + (size_t)u32(p) + 16 * (size_t)planes;
    size_t anim_at = p;
    p += 2 * (size_t)anim_lights;
    size_t info_at = p;
    p += 20 * (size_t)render_polys;
    if (p > w.size()) { ok = false; break; }
    for (unsigned i = 0; i < render_polys; ++i) {
      const unsigned char* info = &w[info_at + 20 * (size_t)i];
      unsigned height = info[6], width = info[7], flags;
      memcpy(&flags, info + 16, 4);
      g_poly_lights.first.push_back((unsigned)g_poly_lights.lights.size());
      if (i >= solid && i < polys) {
        unsigned short target = 0;
        memcpy(&target, &w[polys_at + 8 * (size_t)i + 4], 2);
        openings.push_back({ (unsigned)g_poly_lights.water.size(), target, (unsigned char)medium });
      }
      g_poly_lights.water.push_back(i >= solid ? (medium == 2 ? 2 : 1) : 0);
      unsigned layers = 0;
      for (unsigned b = 0; b < 32; ++b) {
        if (!(flags >> b & 1)) continue;
        ++layers;
        if (b >= anim_lights) { ok = false; break; }
        unsigned short light;
        memcpy(&light, &w[anim_at + 2 * (size_t)b], 2);
        g_poly_lights.lights.push_back(light);
      }
      // The lightmap itself: the steady layer first, then one layer per animated light.
      size_t texels = (size_t)width * height;
      if (p + (size_t)(1 + layers) * texels * texel > w.size()) { ok = false; break; }
      auto texel_rgb = [&](unsigned layer, size_t t, float* rgb) {
        const unsigned char* data = &w[p + (size_t)layer * texels * texel];
        if (texel == 4) {   // stored blue, green, red, alpha
          rgb[0] = data[t * 4 + 2] / 255.0f; rgb[1] = data[t * 4 + 1] / 255.0f; rgb[2] = data[t * 4] / 255.0f;
        } else if (texel == 2) {
          unsigned v = data[t * 2] | data[t * 2 + 1] << 8;
          rgb[0] = (v >> 10 & 31) / 31.0f; rgb[1] = (v >> 5 & 31) / 31.0f; rgb[2] = (v & 31) / 31.0f;
        } else {
          rgb[0] = rgb[1] = rgb[2] = data[t] / 255.0f;
        }
      };
      // Where each texel is in the world. The polygon's texture axes U and V (in its plane)
      // and its anchor vertex O give a point's texture coordinates a, b by P = O + aU + bV;
      // the lightmap has four texels to one unit of those, texel (i, j)'s middle being at
      // a = (i + info.u) / 4 - u_base, likewise b. (This game's format only: floats.)
      g_poly_lights.sample_first.push_back((unsigned)(g_poly_lights.samples.size() / 7));
      for (unsigned layer = 0; layer < layers; ++layer) g_poly_lights.layer_sample_first.push_back((unsigned)(g_poly_lights.layer_samples.size() / 3));
      unsigned corners = i < polys ? w[polys_at + 8 * (size_t)i + 1] : 0;
      size_t corner_at = index_at + index_next;
      index_next += corners;
      if (kind == 2 && texels > 1 && corners >= 3 && corners <= 32 && index_next <= index_bytes) {
        const unsigned char* rp = &w[render_at + render_poly * (size_t)i];
        float U[3], V[3], base[2];
        unsigned short anchor;
        short lm_u, lm_v;
        memcpy(U, rp, 12); memcpy(V, rp + 12, 12); memcpy(base, rp + 24, 8); memcpy(&anchor, rp + 34, 2);
        memcpy(&lm_u, info, 2); memcpy(&lm_v, info + 2, 2);
        float corner_uv[32][2], O[3] = { 0, 0, 0 };
        bool usable = anchor < corners;
        for (unsigned k = 0; k < corners && usable; ++k) if (w[corner_at + k] >= vertices) usable = false;
        if (usable) memcpy(O, &w[vertices_at + 12 * (size_t)w[corner_at + anchor]], 12);
        double uu = 0, vv = 0, uv = 0;
        for (int k = 0; k < 3; ++k) { uu += (double)U[k] * U[k]; vv += (double)V[k] * V[k]; uv += (double)U[k] * V[k]; }
        double det = uu * vv - uv * uv;
        if (!(det > 1e-12)) usable = false;
        for (unsigned k = 0; k < corners && usable; ++k) {   // the polygon's outline in texel coordinates
          float P[3];
          memcpy(P, &w[vertices_at + 12 * (size_t)w[corner_at + k]], 12);
          double pu = 0, pv = 0;
          for (int a = 0; a < 3; ++a) { pu += (double)U[a] * (P[a] - O[a]); pv += (double)V[a] * (P[a] - O[a]); }
          corner_uv[k][0] = (float)(4.0 * ((pu * vv - pv * uv) / det + base[0]) + 0.5 - lm_u);
          corner_uv[k][1] = (float)(4.0 * ((pv * uu - pu * uv) / det + base[1]) + 0.5 - lm_v);
        }
        // Texels whose middle is on the polygon (or within a third of a texel of it), in
        // blocks, at most four blocks each way.
        double turn = 0.0;
        for (unsigned k = 0; k < corners && usable; ++k) {
          const float *a = corner_uv[k], *b = corner_uv[(k + 1) % corners], *c2 = corner_uv[(k + 2) % corners];
          turn += (double)(b[0] - a[0]) * (c2[1] - b[1]) - (double)(b[1] - a[1]) * (c2[0] - b[0]);
        }
        auto inside = [&](float x, float y) {
          for (unsigned k = 0; k < corners; ++k) {
            const float *a = corner_uv[k], *b = corner_uv[(k + 1) % corners];
            float ex = b[0] - a[0], ey = b[1] - a[1], len = sqrtf(ex * ex + ey * ey);
            if (len < 1e-6f) continue;
            float side = (ex * (y - a[1]) - ey * (x - a[0])) / len;
            if ((turn >= 0.0 ? side : -side) < -0.34f) return false;
          }
          return true;
        };
        unsigned step_x = (width + 3) / 4, step_y = (height + 3) / 4;
        size_t first_sample = g_poly_lights.samples.size();
        std::vector<float> layer_values;   // per block: 3 floats a layer
        unsigned inside_total = 0;
        for (unsigned by = 0; by < height && usable; by += step_y)
          for (unsigned bx = 0; bx < width; bx += step_x) {
            double sum[3] = { 0, 0, 0 }, at_x = 0, at_y = 0;
            std::vector<double> layer_sum(layers * 3, 0.0);
            unsigned n = 0;
            for (unsigned y = by; y < by + step_y && y < height; ++y)
              for (unsigned x = bx; x < bx + step_x && x < width; ++x) {
                if (!inside(x + 0.5f, y + 0.5f)) continue;
                float rgb[3];
                size_t t = (size_t)y * width + x;
                texel_rgb(0, t, rgb);
                for (int c2 = 0; c2 < 3; ++c2) sum[c2] += rgb[c2];
                for (unsigned layer = 0; layer < layers; ++layer) {
                  texel_rgb(1 + layer, t, rgb);
                  for (int c2 = 0; c2 < 3; ++c2) layer_sum[layer * 3 + c2] += rgb[c2];
                }
                at_x += x + 0.5; at_y += y + 0.5;
                ++n;
              }
            if (!n) continue;
            double a = (at_x / n - 0.5 + lm_u) / 4.0 - base[0], b = (at_y / n - 0.5 + lm_v) / 4.0 - base[1];
            for (int k = 0; k < 3; ++k) g_poly_lights.samples.push_back((float)(O[k] + a * U[k] + b * V[k]));
            g_poly_lights.samples.push_back((float)n);
            for (int c2 = 0; c2 < 3; ++c2) g_poly_lights.samples.push_back((float)(sum[c2] / n));
            for (double v : layer_sum) layer_values.push_back((float)(v / n));
            inside_total += n;
          }
        size_t made = (g_poly_lights.samples.size() - first_sample) / 7;
        for (size_t k = 0; k < made; ++k) g_poly_lights.samples[first_sample + k * 7 + 3] /= (float)inside_total;
        for (unsigned layer = 0; layer < layers; ++layer)   // from block-major to layer-major
          for (size_t k = 0; k < made; ++k)
            for (int c2 = 0; c2 < 3; ++c2) g_poly_lights.layer_samples.push_back(layer_values[k * layers * 3 + layer * 3 + c2]);
      }
      for (unsigned layer = 0; layer <= layers; ++layer) {
        const unsigned char* data = &w[p + (size_t)layer * texels * texel];
        double sum[3] = { 0, 0, 0 };
        for (size_t t = 0; t < texels; ++t) {
          if (texel == 4) {   // stored blue, green, red, alpha
            sum[0] += data[t * 4 + 2] / 255.0; sum[1] += data[t * 4 + 1] / 255.0; sum[2] += data[t * 4] / 255.0;
          } else if (texel == 2) {
            unsigned v = data[t * 2] | data[t * 2 + 1] << 8;
            sum[0] += (v >> 10 & 31) / 31.0; sum[1] += (v >> 5 & 31) / 31.0; sum[2] += (v & 31) / 31.0;
          } else {
            for (int c = 0; c < 3; ++c) sum[c] += data[t] / 255.0;
          }
        }
        for (int c = 0; c < 3; ++c) {
          float mean = texels ? (float)(sum[c] / (double)texels) : -1.0f;
          if (layer == 0) g_poly_lights.baked.push_back(mean);
          else g_poly_lights.layer.push_back(mean < 0.0f ? 0.0f : mean);
        }
      }
      p += (size_t)(1 + layers) * texels * texel;
    }
    if (p + 4 > w.size()) { ok = false; break; }
    p += 4 + 2 * (size_t)u32(p);
  }
  // Water is where the medium changes across the opening (2 is water). The same on both
  // sides: nothing to see.
  for (const Opening& o : openings) {
    if (!ok || o.target >= media.size() || o.polygon >= g_poly_lights.water.size()) continue;
    bool here = o.medium == 2, there = media[o.target] == 2;
    g_poly_lights.water[o.polygon] = here == there ? 3 : here ? 2 : 1;
  }
  if (ok && g_poly_lights.layer.size() != g_poly_lights.lights.size() * 3) ok = false;
  if (!ok) {
    g_poly_lights.first.clear();
    g_poly_lights.lights.clear();
    g_poly_lights.sample_first.clear();
    g_poly_lights.samples.clear();
    g_poly_lights.layer_sample_first.clear();
    g_poly_lights.layer_samples.clear();
    logf("mission: the level's cells could not be read; flashing lights will be replayed from their settings");
    return 0;
  }
  g_poly_lights.first.push_back((unsigned)g_poly_lights.lights.size());
  g_poly_lights.sample_first.push_back((unsigned)(g_poly_lights.samples.size() / 7));
  logf("mission: %u cells, %zu polygons, %zu lightmap layers of animated lights, %zu lightmap sample points", cells, g_poly_lights.first.size() - 1,
       g_poly_lights.lights.size(), g_poly_lights.samples.size() / 7);
  return p;
}

const std::vector<MissionLightAnim>& mission_light_anims() { return g_anims; }

static void read_anims(const std::wstring& archive, const KpfEntry& e, const std::vector<unsigned char>& inventory) {
  g_anims.clear();
  std::vector<unsigned char> chunk;
  if (!read_chunk(archive, e, inventory, "P$AnimLight", &chunk)) return;
  std::vector<unsigned char> colours;
  read_chunk(archive, e, inventory, "P$LightColo", &colours);   // chunk names are cut to 11 characters
  each_record(chunk, [&](int object, const unsigned char* data, unsigned size) {
    if (size < 60) return;
    MissionLightAnim a = {};
    each_record(colours, [&](int other, const unsigned char* c, unsigned c_size) {
      if (other != object || c_size < 8) return;
      memcpy(&a.hue, c, 4);
      memcpy(&a.saturation, c + 4, 4);
    });
    short index, mode;
    int inactive;
    memcpy(&index, data + 24, 2);
    memcpy(&mode, data + 26, 2);
    memcpy(&a.rise_ms, data + 28, 4);
    memcpy(&a.fall_ms, data + 32, 4);
    memcpy(&a.min, data + 36, 4);
    memcpy(&a.max, data + 40, 4);
    memcpy(&a.current, data + 44, 4);
    memcpy(&a.countdown_ms, data + 52, 4);
    memcpy(&inactive, data + 56, 4);
    if (index < 0) return;
    a.index = index;
    a.mode = mode;
    a.inactive = inactive != 0;
    g_anims.push_back(a);
  });
}

bool mission_find_lights(const std::vector<float>& anchors, std::vector<MissionLight>* out, std::string* source) {
  g_poly_lights.first.clear();
  g_poly_lights.lights.clear();
  g_poly_lights.water.clear();
  g_poly_lights.baked.clear();
  g_poly_lights.layer.clear();
  static_assert(sizeof(MissionLight) == kRecord, "light record layout");
  double t0 = now_ms();
  unsigned missions = 0;
  for (const std::wstring& archive : kpf_archives()) {
    std::vector<KpfEntry> entries;
    if (!kpf_list(archive, &entries)) continue;
    for (const KpfEntry& e : entries) {
      if (!e.stored || !ends_with_mis(e.name) || e.size < 1024) continue;
      ++missions;
      unsigned inventory_offset = 0, count = 0;
      if (!kpf_read(archive, e, 0, 4, &inventory_offset) || inventory_offset + 4 > e.size) continue;
      if (!kpf_read(archive, e, inventory_offset, 4, &count) || count > 100000) continue;
      std::vector<unsigned char> inventory((size_t)count * 20);
      if (!kpf_read(archive, e, inventory_offset + 4, inventory.size(), inventory.data())) continue;
      for (unsigned i = 0; i < count; ++i) {
        const unsigned char* item = &inventory[(size_t)i * 20];
        if (memcmp(item, "WR", 2) != 0) continue;   // WR, WRRGB, WREXT
        if (strncmp((const char*)item, "WREXT", 12) != 0 && strncmp((const char*)item, "WRRGB", 12) != 0 && strncmp((const char*)item, "WR", 12) != 0) continue;
        unsigned offset, length;
        memcpy(&offset, item + 12, 4);
        memcpy(&length, item + 16, 4);
        if ((uint64_t)offset + 24 + length > e.size) continue;
        std::vector<unsigned char> chunk((size_t)length + 24);
        if (!kpf_read(archive, e, offset, chunk.size(), chunk.data())) continue;
        if (find_table(chunk, anchors, out)) {
          read_anims(archive, e, inventory);
          char archive_utf8[260];
          WideCharToMultiByte(CP_UTF8, 0, archive.c_str(), -1, archive_utf8, sizeof archive_utf8, nullptr, nullptr);
          *source = std::string(archive_utf8) + ":" + e.name;
          logf("mission: %s matched, %zu static lights (searched %u missions in %.0f ms)", source->c_str(), out->size(), missions, now_ms() - t0);
          return true;
        }
      }
    }
  }
  logf("mission: no mission matches the %zu lights seen at runtime (searched %u missions in %.0f ms)", anchors.size() / 3, missions, now_ms() - t0);
  return false;
}

bool mission_find_by_geometry(const std::vector<float>& vertices, std::vector<MissionLight>* out, std::string* source) {
  g_poly_lights.first.clear();
  g_poly_lights.lights.clear();
  g_poly_lights.water.clear();
  g_poly_lights.baked.clear();
  g_poly_lights.layer.clear();
  if (vertices.size() < 9) return false;
  double t0 = now_ms();
  unsigned missions = 0;
  for (const std::wstring& archive : kpf_archives()) {
    std::vector<KpfEntry> entries;
    if (!kpf_list(archive, &entries)) continue;
    for (const KpfEntry& e : entries) {
      if (!e.stored || !ends_with_mis(e.name) || e.size < 1024) continue;
      unsigned inventory_offset = 0, count = 0;
      if (!kpf_read(archive, e, 0, 4, &inventory_offset) || inventory_offset + 4 > e.size) continue;
      if (!kpf_read(archive, e, inventory_offset, 4, &count) || count > 100000) continue;
      std::vector<unsigned char> inventory((size_t)count * 20);
      if (!kpf_read(archive, e, inventory_offset + 4, inventory.size(), inventory.data())) continue;
      std::vector<unsigned char> world;
      int kind = 2;
      if (!read_chunk(archive, e, inventory, "WREXT", &world)) {
        kind = 1;
        if (!read_chunk(archive, e, inventory, "WRRGB", &world)) {
          kind = 0;
          if (!read_chunk(archive, e, inventory, "WR", &world)) continue;
        }
      }
      ++missions;
      bool same_level = true;
      for (size_t v = 0; v + 2 < vertices.size() && same_level; v += 3) same_level = contains(world, &vertices[v], 12);
      if (!same_level) continue;

      char archive_utf8[260];
      WideCharToMultiByte(CP_UTF8, 0, archive.c_str(), -1, archive_utf8, sizeof archive_utf8, nullptr, nullptr);
      *source = std::string(archive_utf8) + ":" + e.name;
      // Where each animated light sits and which table entry it is.
      // `slack`: the light sits up to this far from its object (the offset is given in the
      // object's own frame, which may be turned).
      struct Known { float position[3]; int index; bool exact; float slack; };
      std::vector<Known> known;
      std::vector<unsigned char> anim, positions;
      if (read_chunk(archive, e, inventory, "P$AnimLight", &anim) && read_chunk(archive, e, inventory, "P$Position", &positions)) {
        each_record(anim, [&](int object, const unsigned char* data, unsigned size) {
          if (size < 28) return;
          float offset[3];
          short index;
          memcpy(offset, data + 4, 12);
          memcpy(&index, data + 24, 2);
          if (index < 0 || index >= (int)kMaxLights) return;
          each_record(positions, [&](int other, const unsigned char* pos, unsigned pos_size) {
            if (other != object || pos_size < 12) return;
            Known k;
            memcpy(k.position, pos, 12);
            k.exact = offset[0] == 0.0f && offset[1] == 0.0f && offset[2] == 0.0f;
            k.slack = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]) + 0.02f;
            k.index = index;
            known.push_back(k);
          });
        });
      }
      for (const Known& anchor : known) {
        if (!anchor.exact) continue;
        for (size_t hit = 0, from = 0; contains(world, anchor.position, 12, &hit, from); from = hit + 1) {
          if (hit < (size_t)anchor.index * kRecord + 8) continue;
          size_t start = hit - (size_t)anchor.index * kRecord;
          unsigned num_static, num_dynamic;
          memcpy(&num_static, &world[start - 8], 4);
          memcpy(&num_dynamic, &world[start - 4], 4);
          if (num_static == 0 || num_static > kMaxLights || num_dynamic > 64 || (unsigned)anchor.index >= num_static) continue;
          if (start + (size_t)num_static * kRecord > world.size()) continue;
          unsigned agree = 0, asked = 0;
          for (const Known& k : known) {
            if ((unsigned)k.index >= num_static) continue;
            ++asked;
            float p[3];
            memcpy(p, &world[start + (size_t)k.index * kRecord], 12);
            float dx = p[0] - k.position[0], dy = p[1] - k.position[1], dz = p[2] - k.position[2];
            agree += dx * dx + dy * dy + dz * dz <= k.slack * k.slack;
          }
          if (agree < 2 && asked > 1) continue;
          if (agree * 5 < asked * 3) continue;   // most of the animated lights must be where the table says
          out->resize(num_static);
          memcpy(out->data(), &world[start], (size_t)num_static * kRecord);
          read_anims(archive, e, inventory);
          read_cells(world, kind);
          logf("mission: %s recognised by its geometry, %zu static lights (%u of %u animated lights confirm the table; %u missions in %.0f ms)",
               source->c_str(), out->size(), agree, asked, missions, now_ms() - t0);
          return true;
        }
      }
      // No animated light sits exactly where its object does (each hangs off its object by
      // some offset), so there was no position to search for. The table lies somewhere
      // after the cells: try every place for two plausible counts followed by records that
      // put every animated light where its object says, give or take its offset.
      size_t cells_end = read_cells(world, kind);
      if (cells_end && !known.empty()) {
        for (size_t h = cells_end; h + 8 + kRecord <= world.size(); ++h) {
          unsigned num_static, num_dynamic;
          memcpy(&num_static, &world[h], 4);
          if (num_static == 0 || num_static > kMaxLights) continue;
          memcpy(&num_dynamic, &world[h + 4], 4);
          size_t start = h + 8;
          if (num_dynamic > 64 || start + (size_t)num_static * kRecord > world.size()) continue;
          unsigned agree = 0, asked = 0;
          for (const Known& k : known) {
            if ((unsigned)k.index >= num_static) { asked = ~0u; break; }
            ++asked;
            float q[3];
            memcpy(q, &world[start + (size_t)k.index * kRecord], 12);
            float dx = q[0] - k.position[0], dy = q[1] - k.position[1], dz = q[2] - k.position[2];
            if (!(dx * dx + dy * dy + dz * dz <= k.slack * k.slack)) break;
            ++agree;
          }
          if (asked == ~0u || agree != asked) continue;
          out->resize(num_static);
          memcpy(out->data(), &world[start], (size_t)num_static * kRecord);
          read_anims(archive, e, inventory);
          logf("mission: %s recognised by its geometry, %zu static lights (table found after the cells by all %u animated lights; %u missions in %.0f ms)",
               source->c_str(), out->size(), agree, missions, now_ms() - t0);
          return true;
        }
      }
      g_poly_lights.first.clear();
      g_poly_lights.lights.clear();
      g_poly_lights.water.clear();
      g_poly_lights.baked.clear();
      g_poly_lights.layer.clear();
      logf("mission: %s recognised by its geometry, but its light table was not found from %zu animated lights", source->c_str(), known.size());
      return false;
    }
  }
  logf("mission: no mission contains this level's geometry (%u missions in %.0f ms)", missions, now_ms() - t0);
  return false;
}

}  // namespace ss2rtx
