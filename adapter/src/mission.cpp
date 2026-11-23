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
static void read_cells(const std::vector<unsigned char>& w, int kind) {
  g_poly_lights.first.clear();
  g_poly_lights.lights.clear();
  g_poly_lights.water.clear();
  g_poly_lights.baked.clear();
  g_poly_lights.layer.clear();
  auto u32 = [&](size_t at) { unsigned v = 0; if (at + 4 <= w.size()) memcpy(&v, &w[at], 4); return v; };
  size_t p = kind == 2 ? 28 : 8;
  unsigned cells = u32(kind == 2 ? 24 : 4);
  size_t texel = kind == 2 ? (u32(12) == 0 ? 2 : 4) : kind == 1 ? 2 : 1;
  size_t render_poly = kind == 2 ? 52 : 48;
  bool ok = w.size() > p && cells > 0 && cells < 100000;
  for (unsigned c = 0; c < cells && ok; ++c) {
    if (p + 31 > w.size()) { ok = false; break; }
    unsigned vertices = w[p], polys = w[p + 1], render_polys = w[p + 2], portals = w[p + 3], planes = w[p + 4], medium = w[p + 5], anim_lights = w[p + 13];
    unsigned solid = polys >= portals ? polys - portals : polys;   // the polygons after these are openings to other cells
    p += 31 + 12 * (size_t)vertices + 8 * (size_t)polys + render_poly * render_polys;
    if (p + 4 > w.size()) { ok = false; break; }
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
      for (unsigned layer = 0; layer <= layers; ++layer) {
        const unsigned char* data = &w[p + (size_t)layer * texels * texel];
        double sum = 0.0;
        for (size_t t = 0; t < texels; ++t) {
          if (texel == 4) sum += (data[t * 4] + data[t * 4 + 1] + data[t * 4 + 2]) / (3.0 * 255.0);
          else if (texel == 2) {
            unsigned v = data[t * 2] | data[t * 2 + 1] << 8;
            sum += ((v & 31) + (v >> 5 & 31) + (v >> 10 & 31)) / (3.0 * 31.0);
          } else sum += data[t] / 255.0;
        }
        float mean = texels ? (float)(sum / (double)texels) : -1.0f;
        if (layer == 0) g_poly_lights.baked.push_back(mean);
        else g_poly_lights.layer.push_back(mean < 0.0f ? 0.0f : mean);
      }
      p += (size_t)(1 + layers) * texels * texel;
    }
    if (p + 4 > w.size()) { ok = false; break; }
    p += 4 + 2 * (size_t)u32(p);
  }
  if (ok && g_poly_lights.layer.size() != g_poly_lights.lights.size()) ok = false;
  if (!ok) {
    g_poly_lights.first.clear();
    g_poly_lights.lights.clear();
    logf("mission: the level's cells could not be read; flashing lights will be replayed from their settings");
    return;
  }
  g_poly_lights.first.push_back((unsigned)g_poly_lights.lights.size());
  logf("mission: %u cells, %zu polygons, %zu lightmap layers of animated lights", cells, g_poly_lights.first.size() - 1, g_poly_lights.lights.size());
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
      logf("mission: %s recognised by its geometry, but its light table was not found from %zu animated lights", source->c_str(), known.size());
      return false;
    }
  }
  logf("mission: no mission contains this level's geometry (%u missions in %.0f ms)", missions, now_ms() - t0);
  return false;
}

}  // namespace ss2rtx
