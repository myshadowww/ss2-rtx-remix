// Rules for running Remix here are in README.md: runtime folder on the DLL search path,
// own the D3D9 device, present with PresentEx, frame generation off in rtx.conf.
#include "remix_link.h"

#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#include <remix/remix_c.h>

#include "config.h"
#include "log.h"
#include "readback.h"
#include "mission.h"
#include "scene.h"
#include "shaders.h"

namespace ss2rtx {

static unsigned g_width = 1280, g_height = 720;
static bool g_tried_start, g_running;
static remixapi_Interface g_api;
static HMODULE g_dll;
static IDirect3D9Ex* g_d3d9;
static IDirect3DDevice9Ex* g_dev;
static HWND g_hwnd;
static IDirect3DTexture9* g_shot_tex;
static IDirect3DSurface9* g_shot_surf;
static IDirect3DSurface9* g_shot_sys;

static remixapi_MeshHandle g_level_mesh;
static float g_gloss_roughness = 0.3f;   // GlossRoughness: surfaces the engine fakes reflections on
static remixapi_MeshHandle g_level_overlay_mesh;
static remixapi_MeshHandle g_level_lit_mesh;
static uint64_t g_level_built_hash;
static uint32_t g_level_built_revision = ~0u;
static double g_level_built_at = -1e9;
static std::map<uint64_t, remixapi_MaterialHandle> g_materials;   // by texture hash (+ alpha-test bit)
static remixapi_LightHandle g_headlight;

struct Part {
  uint32_t transform_id;
  remixapi_MeshHandle mesh;
  bool planar = false;              // every face lies in one plane (a pane, a sheet)
  float axis[3] = { 0, 0, 0 };      // that plane's normal, in the part's own space
};
static std::map<uint64_t, std::vector<Part>> g_parts;       // object draw identity -> rigid parts
static std::vector<remixapi_LightHandle> g_light_handles;    // parallel to scene_lights()
static float g_light_scale = 40.0f;                          // engine light colour -> Remix radiance
static float g_light_radius = 0.2f;                          // game units
static bool g_force_two_sided;                               // SS2RTX_TWO_SIDED=1: ignore the engine's culling (debug)
// How the light scale ties the two renderers together: an engine light value of x leaves a
// surface with radiance albedo * x / pi here. A full-bright engine surface (x = 1) is
// therefore about 0.32 of its colour, which the fixed exposure in rtx.conf shows at full
// brightness. The defaults sit just above that: higher values look the same on screen
// (they clip) but throw more light into the room than the engine's lamps and screens do.
static float g_emissive_scale = 0.4f;                        // SS2RTX_EMISSIVE: full-bright object parts (lamp faces)
// SS2RTX_SCREENEMISSIVE: the same for full-bright level surfaces and glow layers (screens,
// lit signs). Lower than lamps, or their pictures and writing wash out.
static float g_screen_emissive = 0.4f;
// SS2RTX_EMISSIVELIGHT: how much of a self-lit surface's brightness it also casts as light.
// The engine's screens and lamp faces are bright to look at but light nothing; as plain
// emissive surfaces they flood dark rooms. So such a surface is shown in two parts: the
// surface itself, emitting this fraction, and a layer over it that Remix adds for the eye
// only (emissive-blend layers stay out of bounced light) carrying the rest.
static float g_emissive_light = 0.15f;
static std::vector<float> g_light_sweep;                      // test aid: one scale per readback shot
static double g_last_stats = -1e9;
static size_t g_stat_instances;
// Glass=1 sends the engine's upright see-through surfaces to Remix as thin glass. Off by
// default: no window has been looked at with it yet, and anything else the engine draws
// see-through and upright would turn into a glass pane as well.
// The engine adds an ambient colour to everything it lights (about 0.07); a path tracer
// has no such term. AmbientScale > 0 makes ordinary surfaces give off their own colour
// times that level instead. Off by default: emitted light bounces, and in a closed pale
// room it multiplies several times over; the Hydroponics bulkhead rooms, dark in the
// engine, came out fully lit with it at 1. Bounced light from the real lights already
// fills rooms more than the engine's ambient does. g_ambient_emissive is the level the
// materials were made with.
static float g_ambient_scale = 1.0f;
static float g_ambient_emissive;
static bool g_see_through = true;       // SeeThrough=0 leaves the engine's see-through surfaces out, as before
static bool g_use_live_lights = true;   // LiveLights=0: mission lights always at their table brightness
static bool g_glass = false;
static bool g_sky = true;             // the engine's sky box is sent to Remix
static bool g_stand_off = true;       // lights close to a wall are moved out from it; see place_lights()
static bool g_fit_lights = true;      // each light's strength is fitted to the room it lights; see place_lights()
static bool g_linear_light_colour = true;   // light colours are taken from display space to linear light; see make_light()
static bool g_skip_unseen = false;    // debug (Skip=u): polygons the engine has not drawn yet are left out, not grey
static bool g_dark_emitters = true;   // full-bright surfaces reflect no light, as in the engine (DarkEmitters)
static bool g_fit_texels = true;      // ...and those lightmaps point by point, not as one mean per polygon (FitTexels)
static bool g_fit_baked = true;       // ...against the engine's own lightmaps where the mission file gives them; see fit_to_baked()
static int g_water_mode = 1;          // water surfaces: 0 as the engine textures them, 1 the same but mirror-smooth, 2 a see-through skin, 3 a boundary that bends light
static bool g_sprite_vertex_colour = true;   // particles keep the colour and fade of each vertex, through the engine's own blend
static bool g_sprites = true;         // the engine's world-space quads (particles, sparks) go into the path-traced scene
static size_t g_stat_translucent;
static size_t g_stat_see_through;
static size_t g_stat_glow;

// Objects the engine has stopped drawing because they left the view. The engine only draws
// what the camera can see; a path tracer needs what is beside and behind the camera too,
// or shadows, bounced light and lamp faces come and go as the view turns. An object that
// has stood still for a while is therefore kept, where it was, until it is fully back in
// view without the engine drawing it (it has gone, or is hidden) or shows up moved.
struct KeptObject {
  ObjectDraw draw;
  std::vector<float> nodes;
  uint64_t seen = 0;        // frame it was last drawn by the engine
  uint32_t steady = 0;      // frames in a row it was drawn exactly like this
  uint64_t shape = 0;       // which geometry, wherever it is
  float centre[3] = { 0, 0, 0 };
  float radius = 0;
};
static std::unordered_map<uint64_t, KeptObject> g_kept;
static bool g_keep_objects = true;   // KeepObjects=0: only what the engine draws this frame
static size_t g_stat_kept;

// GlowLayers=0 leaves out the light the engine adds on top of surfaces in blended passes
// (lamp glows, the writing on screens).
static bool g_glow_layers = true;
static std::vector<double> g_shot_at;
static int g_shot_index;
static uint64_t g_frames;

// Hand-off to the game's D3D11 device (README.md): D3D11 creates the shared texture,
// Remix's D3D9 device imports it.
static bool g_enabled = true;
static bool g_composite_wanted;          // SS2RTX_COMPOSITE=1
static bool g_split_screen;              // SS2RTX_SPLIT=1: engine image on the left half, Remix on the right
static bool g_composited_this_frame;
// Sprite draws (the engine's "world space" shader: particles, coronas, and also the HUD)
// seen while the world pass is still open, that is before the path-traced image goes in.
// Those are lost with the engine's world image; counted to know how much that matters.
static unsigned g_sprites_this_frame, g_sprites_most;
static bool g_rendered_this_frame;
static ID3D11Texture2D* g_d11_shared;
static IDirect3DTexture9* g_d9_shared;
static IDirect3DSurface9* g_d9_shared_surf;
static IDirect3DQuery9* g_event_query;
static unsigned g_shared_w, g_shared_h;
static uint64_t g_composites, g_composite_failures;
static double g_composite_ms_total, g_composite_ms_max;
static double g_recent_ms;   // running average of render + hand-off, for the settings panel
static void settings_start();   // defined at the end: applies what the settings panel has stored

void remix_link_set_output_size(unsigned width, unsigned height) {
  if (width && height) { g_width = width; g_height = height; }
}

static std::wstring env_w(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof buf / sizeof buf[0]));
  return n > 0 && n < sizeof buf / sizeof buf[0] ? std::wstring(buf) : std::wstring();
}

static std::wstring output_dir() {
  std::wstring log = env_w(L"SS2RTX_LOG");
  if (log.empty())   // installed: next to the log, in the game's ss2rtx folder
    return GetFileAttributesW(L"ss2rtx") != INVALID_FILE_ATTRIBUTES ? L"ss2rtx\\" : L"";
  size_t slash = log.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : log.substr(0, slash + 1);
}

static void set_remix_option(const char* key, const char* value) {
  remixapi_ErrorCode r = g_api.SetConfigVariable ? g_api.SetConfigVariable(key, value) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  logf("remix: option %s = %s%s", key, value, r == REMIXAPI_ERROR_CODE_SUCCESS ? "" : " was NOT accepted");
}

static bool start() {
  std::wstring dir = config_text(L"RemixDir");
  if (dir.empty()) { logf("remix: no RemixDir setting; Remix stays off"); return false; }
  if (dir.size() < 2 || dir[1] != L':') {   // relative to the game folder
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    dir = std::wstring(cwd) + L"\\" + dir;
  }
  std::wstring dll = dir + L"\\d3d9.dll";
  SetDllDirectoryW(dir.c_str());
  double t0 = now_ms();
  remixapi_ErrorCode r = remixapi_lib_loadRemixDllAndInitialize(dll.c_str(), &g_api, &g_dll);
  logf("remix: load %ls -> %d", dll.c_str(), (int)r);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return false;
  r = g_api.dxvk_CreateD3D9(FALSE, &g_d3d9);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS || !g_d3d9) { logf("remix: dxvk_CreateD3D9 -> %d", (int)r); return false; }

  // Remix needs a window for its own swap chain. It is never shown; the game presents.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ss2rtx_remix_host";
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"ss2rtx remix host", WS_POPUP, 0, 0, (int)g_width, (int)g_height, nullptr, nullptr,
                           wc.hInstance, nullptr);
  if (!g_hwnd) { logf("remix: CreateWindow failed %lu", GetLastError()); return false; }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = g_width;
  pp.BackBufferHeight = g_height;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_hwnd;
  pp.Windowed = TRUE;
  // FPU_PRESERVE and MULTITHREADED: we live inside someone else's process.
  HRESULT hr = g_d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED, &pp,
                                      nullptr, &g_dev);
  if (FAILED(hr) || !g_dev) { logf("remix: CreateDeviceEx -> 0x%08lx", (unsigned long)hr); return false; }
  r = g_api.dxvk_RegisterD3D9Device(g_dev);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) { logf("remix: dxvk_RegisterD3D9Device -> %d", (int)r); return false; }

  g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_shot_tex, nullptr);
  if (g_shot_tex) g_shot_tex->GetSurfaceLevel(0, &g_shot_surf);
  g_dev->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_shot_sys, nullptr);

  // Remix's texture streaming never promotes our textures past a small mip here, which
  // leaves every surface blurred. All mips, always: the game's textures are small.
  set_remix_option("rtx.texturemanager.neverDowngradeTextures", "True");
  g_light_scale = (float)config_number(L"LightScale", g_light_scale);
  g_light_radius = (float)config_number(L"LightRadius", g_light_radius);
  g_emissive_scale = (float)config_number(L"Emissive", g_emissive_scale);
  g_screen_emissive = (float)config_number(L"ScreenEmissive", g_screen_emissive);
  g_emissive_light = (float)config_number(L"EmissiveLight", g_emissive_light);
  if (g_emissive_light < 0.0f) g_emissive_light = 0.0f;
  if (g_emissive_light > 1.0f) g_emissive_light = 1.0f;
  g_force_two_sided = config_flag(L"TwoSided");
  g_glow_layers = config_flag(L"GlowLayers", true);
  g_glass = config_flag(L"Glass", false);
  g_sky = config_flag(L"Sky", true);
  g_stand_off = config_flag(L"StandOff", true);
  g_fit_lights = config_flag(L"FitLights", true);
  g_fit_baked = config_flag(L"FitLightmaps", true);
  g_fit_texels = config_flag(L"FitTexels", true);
  g_dark_emitters = config_flag(L"DarkEmitters", true);
  g_skip_unseen = config_text(L"Skip").find(L'u') != std::wstring::npos;
  g_linear_light_colour = config_flag(L"LinearLightColour", true);
  g_sprites = config_flag(L"Sprites", true);
  g_sprite_vertex_colour = config_flag(L"SpriteVertexColour", true);
  g_water_mode = (int)config_number(L"Water", g_water_mode);
  g_see_through = config_flag(L"SeeThrough", true);
  g_keep_objects = config_flag(L"KeepObjects", true);
  settings_start();
  g_ambient_scale = (float)config_number(L"AmbientFloor", g_ambient_scale);
  g_use_live_lights = config_flag(L"LiveLights", true);
  g_gloss_roughness = (float)config_number(L"GlossRoughness", 0.3);
  char sweep[256];
  DWORD sn = GetEnvironmentVariableA("SS2RTX_LIGHT_SWEEP", sweep, sizeof sweep);
  if (sn > 0 && sn < sizeof sweep) {
    char* sctx = nullptr;
    for (char* tok = strtok_s(sweep, ",;", &sctx); tok; tok = strtok_s(nullptr, ",;", &sctx)) g_light_sweep.push_back((float)atof(tok));
    if (!g_light_sweep.empty()) { g_light_scale = g_light_sweep.front(); g_light_sweep.erase(g_light_sweep.begin()); }
  }
  char spec[256];
  DWORD n = GetEnvironmentVariableA("SS2RTX_REMIX_SHOT_AT", spec, sizeof spec);
  if (n > 0 && n < sizeof spec) {
    char* ctx = nullptr;
    for (char* tok = strtok_s(spec, ",;", &ctx); tok; tok = strtok_s(nullptr, ",;", &ctx)) g_shot_at.push_back(atof(tok));
  }
  logf("remix: started in %.0f ms, %ux%u, %zu readback shot(s) scheduled", now_ms() - t0, g_width, g_height, g_shot_at.size());
  return true;
}

// ---- material overrides -------------------------------------------------------------------
// Remix's own asset replacement (USD mods) does not apply to API-submitted materials, so
// the adapter has its own table: ss2rtx\materials.txt in the game folder, one line per
// texture, keyed by the texture's content hash (the name of its .dds in ss2rtx_cache):
//
//   63BB90387AE74720 roughness=0.35 metallic=1 normal=pbr\floor_n.dds
//   default roughness=0.8
//
// Keys: roughness, metallic, emissive_intensity, displace, albedo, normal, roughness_tex,
// metallic_tex, height, emissive (a .dds path, or "self" to glow with the albedo).
// Paths are relative to the file. The file is re-read when it changes.
struct MaterialOverride {
  float roughness = -1, metallic = -1, emissive_intensity = -1, displace = 0;
  std::wstring albedo, normal, roughness_tex, metallic_tex, height, emissive;
  bool emissive_self = false;
  uint64_t signature = 0;   // changes when the line changes, so the Remix material is rebuilt
};
static std::map<uint64_t, MaterialOverride> g_overrides;
static MaterialOverride g_override_default;
static std::wstring g_override_file, g_override_dir;
static FILETIME g_override_time;
static double g_override_checked = -1e9;

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static bool load_overrides() {
  FILE* f = _wfopen(g_override_file.c_str(), L"r");
  g_overrides.clear();
  g_override_default = MaterialOverride();
  if (!f) return false;
  char line[2048];
  unsigned count = 0;
  while (fgets(line, sizeof line, f)) {
    char* hash_end = strchr(line, '#');
    if (hash_end) *hash_end = 0;
    char* ctx = nullptr;
    char* id = strtok_s(line, " \t\r\n", &ctx);
    if (!id) continue;
    MaterialOverride o;
    o.signature = fnv1a64(id, strlen(id));
    for (char* tok = strtok_s(nullptr, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx)) {
      o.signature = fnv1a64(tok, strlen(tok), o.signature);
      char* eq = strchr(tok, '=');
      if (!eq) continue;
      *eq = 0;
      std::string key = tok, value = eq + 1;
      auto path = [&](const std::string& v) { return g_override_dir + widen(v); };
      if (key == "roughness") o.roughness = (float)atof(value.c_str());
      else if (key == "metallic") o.metallic = (float)atof(value.c_str());
      else if (key == "emissive_intensity") o.emissive_intensity = (float)atof(value.c_str());
      else if (key == "displace") o.displace = (float)atof(value.c_str());
      else if (key == "albedo") o.albedo = path(value);
      else if (key == "normal") o.normal = path(value);
      else if (key == "roughness_tex") o.roughness_tex = path(value);
      else if (key == "metallic_tex") o.metallic_tex = path(value);
      else if (key == "height") o.height = path(value);
      else if (key == "emissive") { if (value == "self") o.emissive_self = true; else o.emissive = path(value); }
    }
    if (!_stricmp(id, "default")) g_override_default = o;
    else g_overrides[strtoull(id, nullptr, 16)] = o;
    ++count;
  }
  fclose(f);
  logf("remix: %u material override line(s) loaded from %ls", count, g_override_file.c_str());
  return true;
}

static void forget_scene_objects();   // defined below: drops meshes and materials so they are rebuilt

// Loads the table on first use and again whenever the file's timestamp changes.
static void refresh_overrides(double seconds) {
  if (seconds - g_override_checked < 1.0) return;
  g_override_checked = seconds;
  if (g_override_file.empty()) {
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    g_override_dir = std::wstring(cwd) + L"\\ss2rtx\\";
    g_override_file = g_override_dir + L"materials.txt";
  }
  WIN32_FILE_ATTRIBUTE_DATA info = {};
  FILETIME t = {};
  if (GetFileAttributesExW(g_override_file.c_str(), GetFileExInfoStandard, &info)) t = info.ftLastWriteTime;
  if (CompareFileTime(&t, &g_override_time) == 0) return;
  bool first = g_override_time.dwLowDateTime == 0 && g_override_time.dwHighDateTime == 0 && g_materials.empty();
  g_override_time = t;
  load_overrides();
  if (!first) forget_scene_objects();
}

// A material is either a texture or, for the engine's flat-coloured materials, a constant
// colour (the flat texel times DarkMaterialConstants.Color). `color` may be null.
// What a material is for. Remix only treats a surface as a decal, or as light added on top
// of what is behind it, when its material blends; an alpha-tested copy of a surface laid
// over it is plain geometry and fights with the surface underneath.
// kSeeThrough: blended the way the engine blends it, with the blend state and the constant
// alpha given per instance (see_through_blend()).
// kLit: the for-the-eye part of a self-lit surface; its light is added whatever the
// texture's alpha says.
// kLines: a decal that is mostly see-through (grid lines, lettering). The engine lays it
// on in display values, where a tenth of opacity is a tenth of brightness; laid on in
// linear light the same tenth shows as a third, and a whole wall of faint texels becomes
// a wash of the lines' colour. So its faintest texels (under a tenth) are cut away.
enum MaterialLayer { kSurface = 0, kDecal = 1, kGlow = 2, kSeeThrough = 3, kLit = 4, kLines = 5 };

// `glossy`: 0 no; 1 a surface the engine fakes a reflection on (polished panels); 2 water.
static remixapi_MaterialHandle material_for(const SceneTexture* tex, bool alpha_test, const float* color, bool emissive = false,
                                            MaterialLayer layer = kSurface, float strength = 1.0f, int glossy = 0, bool unlit = false) {
  if (layer == kGlow || layer == kLit) emissive = true;
  if (layer != kSurface) alpha_test = false;
  bool textured = tex && !tex->solid && !tex->path.empty();
  float albedo[3] = { 0.35f, 0.35f, 0.35f };   // neutral grey for surfaces we know nothing about
  if (tex && tex->solid) {
    for (int i = 0; i < 3; ++i) albedo[i] = tex->solid_rgba[i] * (color ? color[i] : 1.0f);
  } else if (textured) {
    albedo[0] = albedo[1] = albedo[2] = 1.0f;
  }
  uint8_t q[3] = { (uint8_t)(albedo[0] * 255.0f + 0.5f), (uint8_t)(albedo[1] * 255.0f + 0.5f), (uint8_t)(albedo[2] * 255.0f + 0.5f) };
  uint64_t key = textured ? tex->hash : fnv1a64(q, sizeof q, 0x5eed5eed5eed5eedull);
  key ^= alpha_test ? 0x9e3779b97f4a7c15ull : 0;
  key ^= emissive ? 0x7f4a7c159e3779b9ull : 0;
  if (layer != kSurface || (emissive && strength != 1.0f)) {
    uint32_t tag[2] = { (uint32_t)layer, emissive ? (uint32_t)(strength * 64.0f + 0.5f) : 0u };
    key = fnv1a64(tag, sizeof tag, key);
  }
  key ^= glossy == 1 ? 0x474c4f5353595f31ull : glossy == 2 ? 0x57415445525f5f31ull : 0;
  // `unlit`: the engine does not shade this surface at all (and it may be dark: a lamp
  // that is off, the black between a grid's lines).
  unlit = g_dark_emitters && (unlit || emissive) && layer == kSurface && !alpha_test;
  key ^= unlit ? 0x554e4c49545f5f31ull : 0;
  const MaterialOverride* ov = &g_override_default;
  if (textured) {
    auto o = g_overrides.find(tex->hash);
    if (o != g_overrides.end()) ov = &o->second;
  }
  uint64_t identity = key;                 // what the override table and the log refer to
  key ^= ov->signature * 0x100000001b3ull; // the Remix material itself changes with its override
  auto it = g_materials.find(key);
  if (it != g_materials.end()) return it->second;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = { albedo[0], albedo[1], albedo[2] };
  opaque.opacityConstant = 1.0f;
  // DLSS neural rendering is masked per material, and a material that leaves these at
  // zero tells it to change nothing there (it still runs, and costs its time). Full
  // strength everywhere; the settings panel's sliders scale the whole picture.
  opaque.enableDlssControlMask = 1;
  opaque.dlssControlMaskIntensity = 1.0f;
  opaque.dlssControlMaskToneStrength = 1.0f;
  opaque.dlssControlMaskStructuralStrength = 1.0f;
  // `glossy`: a surface the engine fakes a reflection on. Its own entry in the override table still wins.
  opaque.roughnessConstant = ov->roughness >= 0 ? ov->roughness : glossy == 2 ? 0.04f : glossy ? g_gloss_roughness :
                             g_override_default.roughness >= 0 ? g_override_default.roughness : 0.75f;
  opaque.metallicConstant = ov->metallic >= 0 ? ov->metallic : g_override_default.metallic >= 0 ? g_override_default.metallic : 0.0f;
  opaque.roughnessTexture = ov->roughness_tex.c_str();
  opaque.metallicTexture = ov->metallic_tex.c_str();
  opaque.heightTexture = ov->height.c_str();
  opaque.displaceIn = ov->displace;
  if (layer == kLines && textured) {
    opaque.alphaTestType = 4;
    opaque.alphaReferenceValue = 26;
  } else if (alpha_test && textured) {
    opaque.alphaTestType = 4;            // AlphaTestType::kGreater: keep texels above the reference
    opaque.alphaReferenceValue = 127;
  } else {
    opaque.alphaTestType = 7;            // kAlways
  }
  if (layer == kSeeThrough) {
    opaque.useDrawCallAlphaState = 1;
    if (tex && tex->solid) opaque.opacityConstant = tex->solid_rgba[3];
  } else if (layer != kSurface) {
    opaque.blendType_hasvalue = 1;
    opaque.blendType_value = layer == kGlow ? 1 : layer == kLit ? 6 : 0;   // BlendType::kAlphaEmissive, kEmissive, kAlpha
    if (tex && tex->solid) opaque.opacityConstant = tex->solid_rgba[3] * (color ? color[3] : 1.0f);
  }
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = key;
  info.albedoTexture = !ov->albedo.empty() ? ov->albedo.c_str() : textured ? tex->path.c_str() : L"";
  // A full-bright surface shows its texture whatever light falls on it: in the engine it
  // takes none. So here it only gives light off and throws none back; else a lit sign
  // under a lamp is brighter than the engine's, and a room walled in glowing lines is
  // washed in their colour. (A cut-out keeps its texture: its shape is in the alpha.)
  if (unlit && ov->albedo.empty()) {
    // (Black, and a metal: a black non-metal still gives back a twenty-fifth of the
    // light as sheen, which over a whole black room is the wash again.)
    info.albedoTexture = L"";
    opaque.albedoConstant = { 0.0f, 0.0f, 0.0f };
    opaque.metallicConstant = 1.0f;
    opaque.roughnessConstant = 1.0f;
    opaque.roughnessTexture = L"";
    opaque.metallicTexture = L"";
  }
  info.normalTexture = ov->normal.c_str();
  info.tangentTexture = L"";
  // Full-bright engine materials (lamps, screens) glow with their own colour.
  bool glow = emissive || ov->emissive_self || !ov->emissive.empty();
  info.emissiveTexture = !ov->emissive.empty() ? ov->emissive.c_str() : glow && textured ? tex->path.c_str() : L"";
  info.emissiveIntensity = !glow ? 0.0f : ov->emissive_intensity >= 0 ? ov->emissive_intensity : g_emissive_scale;
  if (ov->emissive_intensity < 0) info.emissiveIntensity *= strength;   // `strength` scales the default, not an override
  if (!glow && layer == kSurface && g_ambient_emissive > 0.0f) {         // the stand-in for the engine's ambient light
    info.emissiveTexture = !ov->albedo.empty() ? ov->albedo.c_str() : textured ? tex->path.c_str() : L"";
    info.emissiveIntensity = g_ambient_emissive;
    info.emissiveColorConstant = { albedo[0], albedo[1], albedo[2] };
  }
  info.emissiveColorConstant = glow ? remixapi_Float3D{ albedo[0], albedo[1], albedo[2] } : remixapi_Float3D{ 0.0f, 0.0f, 0.0f };
  info.filterMode = 1;                   // linear
  info.wrapModeU = 1;                    // repeat
  info.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  remixapi_ErrorCode r = g_api.CreateMaterial(&info, &handle);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) { logf("remix: CreateMaterial -> %d", (int)r); handle = nullptr; }
  (void)identity;
  if (textured && ov != &g_override_default && g_materials.size() < 2000)
    logf("remix: material %016llX uses an override (roughness %.2f metallic %.2f%s%s)", (unsigned long long)tex->hash, opaque.roughnessConstant,
         opaque.metallicConstant, ov->normal.empty() ? "" : ", normal map", glow ? ", emissive" : "");
  g_materials[key] = handle;
  return handle;
}

struct SurfaceBuild {
  remixapi_MaterialHandle material = nullptr;
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  std::map<uint32_t, uint32_t> remap;   // level vertex index -> index in `vertices`
};

// Glass: what the engine draws see-through (alpha-blended with a material alpha below 1).
// A thin sheet that reflects and lets light through; the more opaque the engine draws it,
// the more it tints.
// Water as a thin tinted sheet that reflects and lets the pool's floor show through: the
// surface polygons have no volume behind them here, so it is not bent light, only a skin.
static remixapi_MaterialHandle water_material(bool bends) {
  const uint64_t key = bends ? 0x57415445525f4244ull : 0x57415445525f5348ull;
  auto it = g_materials.find(key);
  if (it != g_materials.end()) return it->second;
  remixapi_MaterialInfoTranslucentEXT water = {};
  water.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  water.transmittanceTexture = L"";
  water.refractiveIndex = 1.33f;
  water.transmittanceColor = { 0.55f, 0.80f, 0.82f };
  // Bending light: a real boundary between air and water, with the tint deepening over
  // a few units of depth. The engine's pools have floors and walls, so a ray that goes in
  // ends on one of them.
  water.transmittanceMeasurementDistance = bends ? 6.0f : 1.0f;
  water.thinWallThickness_hasvalue = bends ? 0 : 1;
  water.thinWallThickness_value = 1.0f;
  water.useDiffuseLayer = 0;
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &water;
  info.hash = key;
  info.albedoTexture = L"";
  info.normalTexture = L"";
  info.tangentTexture = L"";
  info.emissiveTexture = L"";
  info.filterMode = 1;
  info.wrapModeU = 1;
  info.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  if (g_api.CreateMaterial(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) handle = nullptr;
  g_materials[key] = handle;
  return handle;
}

// A part the engine shows as a reflection (its cube map) times a colour: polished metal of
// that colour. `blur` is the engine's mip bias into the cube map, 0 sharp.
static remixapi_MaterialHandle mirror_material(const float* color, float blur) {
  uint8_t q[4] = { (uint8_t)(std::clamp(color[0], 0.0f, 1.0f) * 32.0f + 0.5f), (uint8_t)(std::clamp(color[1], 0.0f, 1.0f) * 32.0f + 0.5f),
                   (uint8_t)(std::clamp(color[2], 0.0f, 1.0f) * 32.0f + 0.5f), (uint8_t)(std::clamp(blur, 0.0f, 6.0f) * 2.0f + 0.5f) };
  uint64_t key = fnv1a64(q, sizeof q, 0x4d4952524f525f31ull);
  auto it = g_materials.find(key);
  if (it != g_materials.end()) return it->second;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  // The engine's colours are for the eye; a metal's reflectance is in linear light.
  opaque.albedoConstant = { powf(q[0] / 32.0f, 2.2f), powf(q[1] / 32.0f, 2.2f), powf(q[2] / 32.0f, 2.2f) };
  opaque.opacityConstant = 1.0f;
  opaque.enableDlssControlMask = 1;
  opaque.dlssControlMaskIntensity = 1.0f;
  opaque.dlssControlMaskToneStrength = 1.0f;
  opaque.dlssControlMaskStructuralStrength = 1.0f;
  opaque.roughnessConstant = std::clamp(g_gloss_roughness * 0.5f + 0.08f * (q[3] / 2.0f), 0.04f, 0.6f);
  opaque.metallicConstant = 1.0f;
  opaque.roughnessTexture = L"";
  opaque.metallicTexture = L"";
  opaque.heightTexture = L"";
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = key;
  info.albedoTexture = L"";
  info.normalTexture = L"";
  info.tangentTexture = L"";
  info.emissiveTexture = L"";
  info.filterMode = 1;
  info.wrapModeU = 1;
  info.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  remixapi_ErrorCode r = g_api.CreateMaterial(&info, &handle);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) { logf("remix: CreateMaterial(mirror) -> %d", (int)r); handle = nullptr; }
  g_materials[key] = handle;
  return handle;
}

static remixapi_MaterialHandle glass_material(float alpha) {
  uint32_t q = (uint32_t)(alpha * 16.0f + 0.5f);
  uint64_t key = fnv1a64(&q, sizeof q, 0x474c4153535f5f31ull);
  auto it = g_materials.find(key);
  if (it != g_materials.end()) return it->second;
  float through = 1.0f - 0.5f * (q / 16.0f);
  remixapi_MaterialInfoTranslucentEXT glass = {};
  glass.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  glass.transmittanceTexture = L"";
  glass.refractiveIndex = 1.5f;
  glass.transmittanceColor = { through * 0.96f, through * 0.98f, through };
  glass.transmittanceMeasurementDistance = 1.0f;
  glass.thinWallThickness_hasvalue = 1;
  glass.thinWallThickness_value = 0.5f;
  glass.useDiffuseLayer = 0;
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &glass;
  info.hash = key;
  info.albedoTexture = L"";
  info.normalTexture = L"";
  info.tangentTexture = L"";
  info.emissiveTexture = L"";
  info.filterMode = 1;
  info.wrapModeU = 1;
  info.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  remixapi_ErrorCode r = g_api.CreateMaterial(&info, &handle);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) { logf("remix: CreateMaterial(glass) -> %d", (int)r); handle = nullptr; }
  g_materials[key] = handle;
  return handle;
}

// Emissive strength of a screen-like surface, as material_for() wants it: a multiple of the lamp setting.
static float screen_strength(float layer_strength) {
  return g_emissive_scale > 0.0f ? layer_strength * g_screen_emissive / g_emissive_scale : 0.0f;
}

// How far a glow layer is lifted off its surface, in game units (about 3 mm), so the two
// never share a depth.
static const float kGlowLift = 0.01f;

// `scale`/`uv_matrix` are null for the polygon's own texture mapping, or a layer's mapping.
static void add_index(SurfaceBuild& s, const World& w, uint32_t vi, const float* scale = nullptr, const float* uv_matrix = nullptr,
                      float lift = 0.0f) {
  auto it = s.remap.find(vi);
  if (it == s.remap.end()) {
    const WorldVertex& src = w.vertices[vi];
    remixapi_HardcodedVertex v = {};
    memcpy(v.position, src.position, sizeof v.position);
    memcpy(v.normal, src.normal, sizeof v.normal);
    for (int k = 0; k < 3; ++k) v.position[k] += src.normal[k] * lift;
    if (scale && uv_matrix) {
      scene_level_uv(vi, scale, uv_matrix, v.texcoord);
    } else {
      v.texcoord[0] = w.uv[vi * 2];
      v.texcoord[1] = w.uv[vi * 2 + 1];
    }
    v.color = 0xFFFFFFFF;
    it = s.remap.emplace(vi, (uint32_t)s.vertices.size()).first;
    s.vertices.push_back(v);
  }
  s.indices.push_back(it->second);
}

// Level polygons whose texture the engine swaps (screens): one small mesh per polygon and
// texture, made the first time that pairing is shown and kept.
static std::map<std::pair<uint32_t, uint64_t>, remixapi_MeshHandle> g_animated_meshes;

static void drop_scroll_meshes();
static void drop_animated_meshes() {
  for (auto& kv : g_animated_meshes) if (kv.second) g_api.DestroyMesh(kv.second);
  g_animated_meshes.clear();
  drop_scroll_meshes();
}

// A polygon whose texture the engine slides across it: a black floor under it that stays,
// and the texture as a layer of light rebuilt whenever its matrix has moved (and the
// second layer the engine adds, likewise). One small mesh a layer, replaced as it changes.
struct ScrollMesh { remixapi_MeshHandle mesh = nullptr; uint32_t serial = ~0u; uint64_t texture = 0; };
static std::map<uint64_t, ScrollMesh> g_scroll_meshes;

static void drop_scroll_meshes() {
  for (auto& kv : g_scroll_meshes) if (kv.second.mesh) g_api.DestroyMesh(kv.second.mesh);
  g_scroll_meshes.clear();
}

static void submit_scroll_layer(const World& w, const WorldRange& r, int layer, const SceneTexture& tex, const float* scale, const float* uv_matrix,
                                float strength) {
  static const int order[3] = { 0, 2, 1 };
  ScrollMesh& m = g_scroll_meshes[(uint64_t)r.start << 2 | (uint64_t)layer];
  if (m.serial != r.uv_serial || m.texture != tex.hash || !m.mesh) {
    SurfaceBuild s;
    s.material = material_for(&tex, false, r.color, true, kLit, screen_strength(strength));
    for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
      for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(s, w, w.indices[i + k], scale, uv_matrix, kGlowLift * (0.5f + 0.25f * layer));
    remixapi_MeshHandle mesh = nullptr;
    if (s.indices.size() >= 3) {
      remixapi_MeshInfoSurfaceTriangles t = {};
      t.vertices_values = s.vertices.data();
      t.vertices_count = s.vertices.size();
      t.indices_values = s.indices.data();
      t.indices_count = s.indices.size() - s.indices.size() % 3;
      t.material = s.material;
      remixapi_MeshInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      uint64_t h = fnv1a64(&r.start, sizeof r.start, w.vb_hash);
      h = fnv1a64(&r.uv_serial, sizeof r.uv_serial, h);
      h = fnv1a64(&tex.hash, sizeof tex.hash, h);
      info.hash = fnv1a64(&layer, sizeof layer, h) ^ 0x5343524f4c4c5f31ull;   // (a mesh is known by its hash: a new one each time)
      info.surfaces_values = &t;
      info.surfaces_count = 1;
      if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
    }
    if (m.mesh) g_api.DestroyMesh(m.mesh);
    m.mesh = mesh;
    m.serial = r.uv_serial;
    m.texture = tex.hash;
  }
  if (!m.mesh) return;
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.mesh = m.mesh;
  inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
  inst.doubleSided = 1;
  g_api.DrawInstance(&inst);
}

static void submit_animated_polygons() {
  const World& w = scene_world();
  const auto& textures = scene_textures();
  static const int order[3] = { 0, 2, 1 };
  for (uint32_t start : w.animated) {
    auto it = w.ranges.find(start);
    if (it == w.ranges.end()) continue;
    const WorldRange& r = it->second;
    if (r.texture < 0 || (size_t)r.start + r.count > w.indices.size()) continue;
    const SceneTexture& tex = textures[r.texture];
    bool self_lit = r.unshaded;
    if (r.scrolling && self_lit) {
      // The floor itself: black, and there. Then the light on it.
      auto base_key = std::make_pair(start ^ 0x20000000u, (uint64_t)0);
      auto base = g_animated_meshes.find(base_key);
      if (base == g_animated_meshes.end()) {
        SurfaceBuild s;
        s.material = material_for(&tex, false, r.color, false, kSurface, 1.0f, 0, true);
        for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
          for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(s, w, w.indices[i + k]);
        remixapi_MeshHandle mesh = nullptr;
        if (s.indices.size() >= 3) {
          remixapi_MeshInfoSurfaceTriangles t = {};
          t.vertices_values = s.vertices.data();
          t.vertices_count = s.vertices.size();
          t.indices_values = s.indices.data();
          t.indices_count = s.indices.size() - s.indices.size() % 3;
          t.material = s.material;
          remixapi_MeshInfo info = {};
          info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
          info.hash = fnv1a64(&start, sizeof start, w.vb_hash) ^ 0x5343524f4c4c4253ull;
          info.surfaces_values = &t;
          info.surfaces_count = 1;
          if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
        }
        base = g_animated_meshes.emplace(base_key, mesh).first;
      }
      if (base->second) {
        remixapi_InstanceInfo inst = {};
        inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        inst.mesh = base->second;
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
        inst.doubleSided = 1;
        g_api.DrawInstance(&inst);
      }
      float tint = std::clamp(std::max(r.color[0], std::max(r.color[1], r.color[2])), 0.0f, 1.0f);
      bool second = r.add_texture >= 0 && (size_t)r.add_texture < textures.size() && r.seen_frame != ~0ull && r.add_frame != ~0ull && r.seen_frame <= r.add_frame + 1;
      // The engine adds the two layers as displayed; added as light, two equal ones come
      // to 1.4 times one on screen, not twice. Each is given the difference.
      float both = second ? 2.3f : 1.0f;
      if (tint > 0.02f) submit_scroll_layer(w, r, 0, tex, r.scale, r.uv_matrix, both * powf(tint, 2.2f));
      if (second) submit_scroll_layer(w, r, 1, textures[(size_t)r.add_texture], r.add_scale, r.add_uv_matrix, both * powf(std::clamp(r.add_strength, 0.0f, 1.0f), 2.2f));
      continue;
    }
    auto key = std::make_pair(start, tex.hash);
    auto found = g_animated_meshes.find(key);
    if (found == g_animated_meshes.end()) {
      if (g_animated_meshes.size() > 4096) drop_animated_meshes();   // a runaway would otherwise grow without limit
      SurfaceBuild s;
      s.material = material_for(&tex, r.alpha_test, r.color, self_lit, kSurface, screen_strength(g_emissive_light));
      for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
        for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(s, w, w.indices[i + k]);
      remixapi_MeshHandle mesh = nullptr;
      if (s.indices.size() >= 3) {
        remixapi_MeshInfoSurfaceTriangles t = {};
        t.vertices_values = s.vertices.data();
        t.vertices_count = s.vertices.size();
        t.indices_values = s.indices.data();
        t.indices_count = s.indices.size() - s.indices.size() % 3;
        t.material = s.material;
        remixapi_MeshInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        uint64_t h = fnv1a64(&start, sizeof start, w.vb_hash);
        info.hash = fnv1a64(&tex.hash, sizeof tex.hash, h) ^ 0x414e494d50424c59ull;
        info.surfaces_values = &t;
        info.surfaces_count = 1;
        if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
      }
      found = g_animated_meshes.emplace(key, mesh).first;
    }
    if (!found->second) continue;
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = found->second;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.doubleSided = 1;
    g_api.DrawInstance(&inst);
    if (!self_lit || g_emissive_light >= 1.0f) continue;
    // A screen: the rest of its brightness as a layer for the eye only.
    auto lit_key = std::make_pair(start ^ 0x40000000u, tex.hash);
    auto lit = g_animated_meshes.find(lit_key);
    if (lit == g_animated_meshes.end()) {
      SurfaceBuild s;
      s.material = material_for(&tex, false, r.color, true, kLit, screen_strength(1.0f - g_emissive_light));
      for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
        for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(s, w, w.indices[i + k], nullptr, nullptr, kGlowLift);
      remixapi_MeshHandle mesh = nullptr;
      if (s.indices.size() >= 3) {
        remixapi_MeshInfoSurfaceTriangles t = {};
        t.vertices_values = s.vertices.data();
        t.vertices_count = s.vertices.size();
        t.indices_values = s.indices.data();
        t.indices_count = s.indices.size() - s.indices.size() % 3;
        t.material = s.material;
        remixapi_MeshInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        uint64_t h = fnv1a64(&start, sizeof start, w.vb_hash);
        info.hash = fnv1a64(&tex.hash, sizeof tex.hash, h) ^ 0x414e494d4c495431ull;
        info.surfaces_values = &t;
        info.surfaces_count = 1;
        if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
      }
      lit = g_animated_meshes.emplace(lit_key, mesh).first;
    }
    if (!lit->second) continue;
    inst.mesh = lit->second;
    g_api.DrawInstance(&inst);
  }
}

// The sky: the engine's own box and textures, kept round the camera as the engine keeps it,
// so that what a window shows (stars, the Earth) is there for the path tracer's rays too.
// It gives off its picture at the brightness the engine shows it.
static remixapi_MeshHandle g_sky_mesh;
static uint32_t g_sky_built = ~0u;
static uint64_t g_sky_level;
static remixapi_Float3D g_camera_pos;

static void submit_sky() {
  const World& w = scene_world();
  if (w.sky.empty() || !g_sky) return;
  if (g_sky_built != w.sky_revision || g_sky_level != w.vb_hash) {
    g_sky_built = w.sky_revision;
    g_sky_level = w.vb_hash;
    if (g_sky_mesh) { g_api.DestroyMesh(g_sky_mesh); g_sky_mesh = nullptr; }
    const auto& textures = scene_textures();
    std::vector<SurfaceBuild> faces;
    faces.reserve(w.sky.size());
    for (const auto& face : w.sky) {
      if (face.texture < 0) continue;
      faces.emplace_back();
      SurfaceBuild& s = faces.back();
      // 0.32 of its colour is what the fixed exposure shows at face value.
      s.material = material_for(&textures[face.texture], false, nullptr, true, kSurface, g_emissive_scale > 0.0f ? 0.32f / g_emissive_scale : 0.0f);
      for (uint32_t i = face.start; i < face.start + face.count && i < w.indices.size(); ++i) {
        uint32_t vi = w.indices[i];
        if (vi >= w.vertices.size()) continue;
        const WorldVertex& src = w.vertices[vi];
        remixapi_HardcodedVertex v = {};
        memcpy(v.position, src.position, sizeof v.position);
        for (int k = 0; k < 3; ++k) v.normal[k] = -src.position[k] / 2078.0f;   // towards the middle
        v.texcoord[0] = src.uv_unused[0];
        v.texcoord[1] = src.uv_unused[1];
        v.color = 0xFFFFFFFF;
        s.indices.push_back((uint32_t)s.vertices.size());
        s.vertices.push_back(v);
      }
    }
    std::vector<remixapi_MeshInfoSurfaceTriangles> tris;
    for (SurfaceBuild& s : faces) {
      if (s.indices.size() < 3) continue;
      remixapi_MeshInfoSurfaceTriangles t = {};
      t.vertices_values = s.vertices.data();
      t.vertices_count = s.vertices.size();
      t.indices_values = s.indices.data();
      t.indices_count = s.indices.size() - s.indices.size() % 3;
      t.material = s.material;
      tris.push_back(t);
    }
    if (!tris.empty()) {
      remixapi_MeshInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      uint64_t h = w.vb_hash ^ 0x534b59424f585f31ull;
      info.hash = fnv1a64(&w.sky_revision, sizeof w.sky_revision, h);
      info.surfaces_values = tris.data();
      info.surfaces_count = (uint32_t)tris.size();
      if (g_api.CreateMesh(&info, &g_sky_mesh) != REMIXAPI_ERROR_CODE_SUCCESS) g_sky_mesh = nullptr;
      logf("remix: sky box built from %zu of the engine's faces", tris.size());
    }
  }
  if (!g_sky_mesh) return;
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.mesh = g_sky_mesh;
  inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
  inst.transform.matrix[0][3] = g_camera_pos.x;   // it goes where the camera goes
  inst.transform.matrix[1][3] = g_camera_pos.y;
  inst.transform.matrix[2][3] = g_camera_pos.z;
  inst.doubleSided = 1;
  g_api.DrawInstance(&inst);
}

// Glow layers of level polygons (the writing on a screen, a lamp's halo): one small mesh
// per polygon, texture and brightness step, so a layer the engine pulses can follow it
// without the level being rebuilt.
static const float kGlowSteps = 16.0f;

static void submit_glow_polygons() {
  const World& w = scene_world();
  const auto& textures = scene_textures();
  static const int order[3] = { 0, 2, 1 };
  for (uint32_t start : w.glowing) {
    auto it = w.ranges.find(start);
    if (it == w.ranges.end()) continue;
    const WorldRange& r = it->second;
    if (r.glow_texture < 0 || (r.animated && !r.scrolling) || (size_t)r.start + r.count > w.indices.size()) continue;
    // Drawn this frame without its glow pass: the engine has switched the glow off.
    if (r.seen_frame != ~0ull && r.glow_frame != ~0ull && r.seen_frame > r.glow_frame + 1) continue;
    uint32_t step = (uint32_t)(r.glow_strength * kGlowSteps + 0.5f);
    // The engine adds a glow layer scaled by its texture's alpha. One whose texture is
    // see-through nearly everywhere adds next to nothing there, whatever colour it holds.
    if (r.glow_texture >= 0 && (size_t)r.glow_texture < textures.size() && textures[(size_t)r.glow_texture].faint(r.scrolling)) step = 0;
    if (step == 0) continue;
    const SceneTexture& tex = textures[r.glow_texture];
    auto key = std::make_pair(start ^ 0x80000000u, tex.hash * 31 + step);
    auto found = g_animated_meshes.find(key);
    if (found == g_animated_meshes.end()) {
      if (g_animated_meshes.size() > 4096) drop_animated_meshes();
      SurfaceBuild s;
      // A layer whose texture has no see-through parts is pure added light (the engine adds
      // it scaled by the material's alpha); as a covering it would hide the wall under it.
      s.material = material_for(&tex, false, r.color, true, tex.has_mean && tex.mean_alpha > 0.9f ? kLit : kGlow, screen_strength(step / kGlowSteps));
      for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
        for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(s, w, w.indices[i + k], r.glow_scale, r.glow_uv_matrix, kGlowLift);
      remixapi_MeshHandle mesh = nullptr;
      if (s.indices.size() >= 3) {
        remixapi_MeshInfoSurfaceTriangles t = {};
        t.vertices_values = s.vertices.data();
        t.vertices_count = s.vertices.size();
        t.indices_values = s.indices.data();
        t.indices_count = s.indices.size() - s.indices.size() % 3;
        t.material = s.material;
        remixapi_MeshInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        uint64_t h = fnv1a64(&start, sizeof start, w.vb_hash);
        h = fnv1a64(&tex.hash, sizeof tex.hash, h);
        info.hash = fnv1a64(&step, sizeof step, h) ^ 0x474c4f57504f4c59ull;
        info.surfaces_values = &t;
        info.surfaces_count = 1;
        if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
      }
      found = g_animated_meshes.emplace(key, mesh).first;
    }
    if (!found->second) continue;
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = found->second;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.doubleSided = 1;
    g_api.DrawInstance(&inst);
  }
}

// Builds one Remix mesh for the whole level: a surface per texture, plus one untextured
// surface for polygons the engine has not drawn yet (they still block and bounce light).
static void build_level(double seconds) {
  const World& w = scene_world();
  const auto& textures = scene_textures();
  std::map<uint64_t, SurfaceBuild> surfaces;   // key: texture index and alpha flag; ~0 = unknown
  std::map<uint64_t, SurfaceBuild> overlays;   // second layers that sit on a real first layer
  std::map<uint64_t, SurfaceBuild> lit_layers; // the for-the-eye part of self-lit surfaces
  std::vector<bool> covered(w.indices.size(), false);
  static const int order[3] = { 0, 2, 1 };     // reversed winding, see parts_for()
  size_t layered = 0, water_polygons = 0;
  for (const auto& kv : w.ranges) {
    const WorldRange& r = kv.second;
    if ((size_t)r.start + r.count > w.indices.size()) continue;
    if (r.animated) {   // drawn by submit_animated_polygons()
      for (uint32_t i = r.start; i < r.start + r.count; ++i) covered[i] = true;
      continue;
    }
    // (A first pass that is the engine's reflection has no texture of its own, whatever
    // was still bound.)
    const SceneTexture* base = r.texture >= 0 && !r.env_mapped ? &textures[r.texture] : nullptr;
    const SceneTexture* over = r.overlay_texture >= 0 ? &textures[r.overlay_texture] : nullptr;
    // The engine's two-layer surfaces are polished panels: a full-bright first pass with a
    // fake reflection (or nothing) in it, then the panel itself, slightly see-through. Here
    // the second layer is the surface, made glossy so it reflects for real. A second layer
    // over a normally lit first one is a decal instead.
    // (A second layer that is mostly see-through is lines or lettering on the first, never
    // the surface itself: the training rooms' grid.)
    bool sparse = over && over->has_mean && over->mean_alpha < 0.5f;
    bool second_is_surface = over && !sparse && (!base || base->uniform || base->solid || r.unshaded);
    const SceneTexture* main = second_is_surface ? over : base;
    // Full-bright polygons (screens, lit signs) glow. A blank first layer says nothing
    // about the surface, so there the second layer's own flag counts.
    bool self_lit = second_is_surface ? r.overlay_unshaded : r.unshaded;
    bool engine_unlit = self_lit;   // the engine does not shade it, however dark its colour makes it
    // Only a reflection, with nothing or just lines over it: a dark mirror, not a lamp.
    bool mirror_base = r.env_mapped && !second_is_surface;
    if (mirror_base) self_lit = false;
    // Water, by the mission file. Mirror-smooth with the engine's texture, or a see-through
    // skin: then only the side seen from the air is kept, the other would lie on top of it.
    int water = g_water_mode ? scene_water_at(r.start) : 0;
    if (water) ++water_polygons;
    if (water == 2 && g_water_mode >= 2) {
      for (uint32_t i = r.start; i < r.start + r.count; ++i) covered[i] = true;
      continue;
    }
    bool skin = water && g_water_mode >= 2;
    if (skin) self_lit = false;
    if (water || mirror_base) engine_unlit = false;
    // A full-bright surface is as bright as its material's colour says: the engine shows
    // texture times colour, and a dark colour is how it keeps a switched-off panel dim.
    float tint = r.color[0] > r.color[1] ? (r.color[0] > r.color[2] ? r.color[0] : r.color[2]) : (r.color[1] > r.color[2] ? r.color[1] : r.color[2]);
    tint = tint < 0.0f ? 0.0f : tint > 1.0f ? 1.0f : tint;
    uint32_t tint_step = (uint32_t)(tint * 16.0f + 0.5f);
    if (self_lit && tint_step == 0) self_lit = false;
    float lit_level = self_lit ? powf((float)tint_step / 16.0f, 2.2f) : 1.0f;   // to linear light, like everything the engine multiplies on screen
    uint64_t key = ((uint64_t)(uint32_t)(second_is_surface ? r.overlay_texture : r.texture) << 5) | (r.alpha_test ? 1 : 0) | (second_is_surface ? 2 : 0) |
                   (self_lit ? 4 : 0) | (water ? 8 : 0) | (skin ? 16 : 0) | ((uint64_t)(self_lit ? tint_step : 0) << 40);
    key |= (uint64_t)(r.env_layer ? 1 : 0) << 46 | (uint64_t)(engine_unlit ? 1 : 0) << 47;
    if (skin) key = 0x57415445525f5348ull + (uint64_t)g_water_mode;   // one surface for all of it
    if (mirror_base && !skin) {
      uint32_t q = (uint32_t)(std::clamp(r.color[0], 0.0f, 1.0f) * 32.0f + 0.5f) << 16 | (uint32_t)(std::clamp(r.color[1], 0.0f, 1.0f) * 32.0f + 0.5f) << 8 |
                   (uint32_t)(std::clamp(r.color[2], 0.0f, 1.0f) * 32.0f + 0.5f);
      key = 0x4d49525200000000ull | q;
    }
    SurfaceBuild& s = surfaces[key];
    if (!s.material)
      s.material = skin ? water_material(g_water_mode == 3) : mirror_base ? mirror_material(r.color, 0.0f) :
                   material_for(main, r.alpha_test && !second_is_surface, r.color, self_lit, kSurface, screen_strength(g_emissive_light) * lit_level,
                                water ? 2 : second_is_surface || r.env_layer ? 1 : 0, engine_unlit);
    if (self_lit && main && g_emissive_light < 1.0f) {   // the rest of its brightness, for the eye only
      SurfaceBuild& lit = lit_layers[key];
      if (!lit.material) lit.material = material_for(main, false, r.color, true, kLit, screen_strength(1.0f - g_emissive_light) * lit_level);
      for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
        for (int k : order)
          if (w.indices[i + k] < w.vertices.size())
            add_index(lit, w, w.indices[i + k], second_is_surface ? r.overlay_scale : nullptr, second_is_surface ? r.overlay_uv_matrix : nullptr, kGlowLift);
    }
    for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3) {
      for (int k : order)
        if (w.indices[i + k] < w.vertices.size())
          add_index(s, w, w.indices[i + k], second_is_surface ? r.overlay_scale : nullptr, second_is_surface ? r.overlay_uv_matrix : nullptr);
      covered[i] = covered[i + 1] = covered[i + 2] = true;
    }
    if (over) ++layered;
    if (over && !second_is_surface) {   // lettering and the like: a decal on top of the wall
      SurfaceBuild& o = overlays[(uint64_t)(uint32_t)r.overlay_texture];
      if (!o.material) o.material = material_for(over, false, r.color, r.overlay_unshaded, sparse ? kLines : kDecal);
      for (uint32_t i = r.start; i + 2 < r.start + r.count; i += 3)
        for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(o, w, w.indices[i + k], r.overlay_scale, r.overlay_uv_matrix);
    }
  }
  for (const auto& face : w.sky)   // the sky box is not part of the level: see submit_sky()
    for (uint32_t i = face.start; i < face.start + face.count && i < covered.size(); ++i) covered[i] = true;
  SurfaceBuild& unknown = surfaces[~0ull];
  unknown.material = material_for(nullptr, false, nullptr);
  for (size_t i = 0; i + 2 < w.indices.size(); i += 3) {
    if (covered[i]) continue;
    // (An opening between two cells is no wall: until the engine draws it, as it does
    // water, nothing stands there.)
    if (scene_opening_at((uint32_t)i)) continue;
    if (g_skip_unseen) continue;
    for (int k : order) if (w.indices[i + k] < w.vertices.size()) add_index(unknown, w, w.indices[i + k]);
  }

  std::vector<remixapi_MeshInfoSurfaceTriangles> tris;
  size_t total_tris = 0;
  for (auto& kv : surfaces) {
    SurfaceBuild& s = kv.second;
    if (s.indices.size() < 3) continue;
    remixapi_MeshInfoSurfaceTriangles t = {};
    t.vertices_values = s.vertices.data();
    t.vertices_count = s.vertices.size();
    t.indices_values = s.indices.data();
    t.indices_count = s.indices.size() - s.indices.size() % 3;
    t.material = s.material;
    tris.push_back(t);
    total_tris += t.indices_count / 3;
  }
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  // Identity follows content: the level plus the set of polygons whose texture is known.
  uint64_t known = w.vb_hash;
  for (const auto& kv : w.ranges) known = fnv1a64(&kv.first, sizeof kv.first, known);
  // ...and never the same as the mesh it replaces: Remix knows a mesh by this number, so
  // destroying the old one under the same number would take the new one with it.
  static uint32_t build_serial = 0;
  ++build_serial;
  info.hash = fnv1a64(&build_serial, sizeof build_serial, known);
  info.surfaces_values = tris.data();
  info.surfaces_count = (uint32_t)tris.size();
  remixapi_MeshHandle mesh = nullptr;
  remixapi_ErrorCode r = g_api.CreateMesh(&info, &mesh);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) { logf("remix: CreateMesh(level) -> %d", (int)r); return; }
  if (g_level_mesh) g_api.DestroyMesh(g_level_mesh);
  g_level_mesh = mesh;
  if (w.vb_hash != g_level_built_hash) { drop_animated_meshes(); g_kept.clear(); }   // another level
  // Second layers go in their own mesh, drawn as decals over the level; glow layers in another.
  auto layer_mesh = [&](std::map<uint64_t, SurfaceBuild>& groups, uint64_t tag, remixapi_MeshHandle* out) -> size_t {
    if (*out) { g_api.DestroyMesh(*out); *out = nullptr; }
    std::vector<remixapi_MeshInfoSurfaceTriangles> list;
    for (auto& kv : groups) {
      SurfaceBuild& o = kv.second;
      if (o.indices.size() < 3) continue;
      remixapi_MeshInfoSurfaceTriangles t = {};
      t.vertices_values = o.vertices.data();
      t.vertices_count = o.vertices.size();
      t.indices_values = o.indices.data();
      t.indices_count = o.indices.size() - o.indices.size() % 3;
      t.material = o.material;
      list.push_back(t);
    }
    if (list.empty()) return 0;
    remixapi_MeshInfo linfo = {};
    linfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    uint64_t h = info.hash ^ tag;
    for (auto& kv : groups) h = fnv1a64(&kv.first, sizeof kv.first, h);
    linfo.hash = h;
    linfo.surfaces_values = list.data();
    linfo.surfaces_count = (uint32_t)list.size();
    if (g_api.CreateMesh(&linfo, out) != REMIXAPI_ERROR_CODE_SUCCESS) *out = nullptr;
    return list.size();
  };
  size_t overlay_groups = layer_mesh(overlays, 0x4f5645524c415953ull, &g_level_overlay_mesh);
  layer_mesh(lit_layers, 0x4c49544c41594552ull, &g_level_lit_mesh);
  if (layered || !w.glowing.empty())
    logf("remix: %zu level polygons are two-layer surfaces (%zu decal groups), %zu glow groups, %zu water", layered, overlay_groups, w.glowing.size(), water_polygons);
  g_level_built_hash = w.vb_hash;
  g_level_built_revision = w.revision;
  g_level_built_at = seconds;
  logf("remix: level mesh built at %.1fs: %zu surfaces, %zu triangles, %zu polygons seen so far, %zu textures", seconds, tris.size(),
       total_tris, w.ranges.size(), textures.size());
}

static remixapi_Float3D g_camera_forward;

static void setup_camera() {
  const Camera& c = scene_camera();
  const float* m = c.view;   // column-major: m[col * 4 + row]
  float right[3] = { m[0], m[4], m[8] };
  float up[3] = { m[1], m[5], m[9] };
  float back[3] = { m[2], m[6], m[10] };
  float t[3] = { m[12], m[13], m[14] };
  remixapi_CameraInfoParameterizedEXT p = {};
  p.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  p.position = { -(right[0] * t[0] + up[0] * t[1] + back[0] * t[2]),
                 -(right[1] * t[0] + up[1] * t[1] + back[1] * t[2]),
                 -(right[2] * t[0] + up[2] * t[1] + back[2] * t[2]) };
  p.forward = { -back[0], -back[1], -back[2] };
  p.up = { up[0], up[1], up[2] };
  p.right = { right[0], right[1], right[2] };
  const float* pr = c.proj;
  p.fovYInDegrees = 2.0f * atanf(1.0f / pr[5]) * 57.29578f;
  p.aspect = pr[5] / pr[0];
  p.nearPlane = pr[14] / (pr[10] - 1.0f);
  p.farPlane = pr[14] / (pr[10] + 1.0f);
  if (!(p.nearPlane > 0.0f) || !(p.farPlane > p.nearPlane)) { p.nearPlane = 0.1f; p.farPlane = 2000.0f; }
  remixapi_CameraInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  info.pNext = &p;
  info.type = REMIXAPI_CAMERA_TYPE_WORLD;
  g_api.SetupCamera(&info);
  g_camera_pos = p.position;
  g_camera_forward = p.forward;

}

// The engine's object lights become small sphere lights. Its falloff is linear (1/d) and
// Remix's is physical (1/d^2), so brightness is matched at a typical room distance by one
// scale factor (SS2RTX_LIGHT_SCALE) until lights are calibrated properly.
static std::vector<remixapi_Float3D> g_light_built_color;   // parallel to g_light_handles: the colour each was made with
static std::vector<remixapi_LightHandle> g_moving_handles;

// How strong each light is made for Remix, and where it is put.
//
// The engine dims a light with the distance (value x at distance d lights a surface x/d,
// and its lightmaps hold no more than 1); Remix, as light really does, with the square.
// One number cannot turn one into the other: matched at five units, a lamp over a
// corridor is right and the lamp 120 units up in a hangar leaves its floor thirty times
// too dark. So each light is fitted to the room it is in. Rays go out from it in every
// direction it shines in; where one lands, d away, the engine would have put min(x/d, 1)
// on d-squared of surface. The Remix light is given the strength that delivers the same
// total. Near surfaces then get more than the engine's and far ones less, but the room
// as a whole gets what the engine gave it. `kFitShare` is the part of that total the
// light itself supplies; the rest comes back as bounced light, which the engine has none
// of (set so that corridors measured equal to the engine's stay so).
//
// Then the hot spot. A light hung half a unit from a wall gives the engine a wide soft
// glow there and a path tracer a small white disc. A light that is not a spotlight is
// moved straight out from the surface it is nearest to, to the distance at which its pool
// on that surface peaks at the engine's brightness, and never more than half-way to
// whatever lies beyond it.
static std::vector<remixapi_Float3D> g_light_shown;
static std::vector<float> g_light_gain;      // strength relative to the plain five-unit match
static const float kFitShare = 0.6f;
static float g_baked_floor = -1.0f;   // the engine's ambient level as its lightmaps show it (their darkest twentieth), or -1
static const float kDarkSide = 0.8f;   // the lightmap fit aims a little under the engine: darker is the better side to miss on
static void lights_restated();

static void closest_on_triangle(const float* p, const float* a, const float* b, const float* c, float* out) {
  float ab[3], ac[3], ap[3];
  for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
  auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  float d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
  float bp[3] = { p[0] - b[0], p[1] - b[1], p[2] - b[2] };
  float d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) out[k] = a[k] + v * ab[k]; return; }
  float cp[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
  float d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) out[k] = a[k] + w * ac[k]; return; }
  float va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    for (int k = 0; k < 3; ++k) out[k] = b[k] + w * (c[k] - b[k]);
    return;
  }
  float denom = 1.0f / (va + vb + vc), v = vb * denom, w = vc * denom;
  for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * v + ac[k] * w;
}

struct LevelTri { float a[3], b[3], c[3], n[3], mid[3], radius; };

// The level's triangles in a tree of boxes, for the many rays the light fitting casts.
struct LevelBvh {
  struct Node { float lo[3], hi[3]; int left, right, first, count; };
  std::vector<LevelTri> tris;
  std::vector<Node> nodes;

  int build_node(int first, int count) {
    Node node;
    for (int k = 0; k < 3; ++k) { node.lo[k] = 1e30f; node.hi[k] = -1e30f; }
    float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = first; i < first + count; ++i) {
      const LevelTri& t = tris[(size_t)i];
      for (const float* corner : { t.a, t.b, t.c })
        for (int k = 0; k < 3; ++k) { if (corner[k] < node.lo[k]) node.lo[k] = corner[k]; if (corner[k] > node.hi[k]) node.hi[k] = corner[k]; }
      for (int k = 0; k < 3; ++k) { if (t.mid[k] < clo[k]) clo[k] = t.mid[k]; if (t.mid[k] > chi[k]) chi[k] = t.mid[k]; }
    }
    node.left = node.right = -1;
    node.first = first;
    node.count = count;
    int index = (int)nodes.size();
    nodes.push_back(node);
    if (count > 8) {
      int axis = 0;
      for (int k = 1; k < 3; ++k) if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
      int middle = first + count / 2;
      std::nth_element(tris.begin() + first, tris.begin() + middle, tris.begin() + first + count,
                       [axis](const LevelTri& x, const LevelTri& y) { return x.mid[axis] < y.mid[axis]; });
      int left = build_node(first, middle - first);
      int right = build_node(middle, first + count - middle);
      nodes[(size_t)index].left = left;
      nodes[(size_t)index].right = right;
      nodes[(size_t)index].count = 0;
    }
    return index;
  }
  void build() {
    nodes.clear();
    nodes.reserve(tris.size() / 2 + 16);
    if (!tris.empty()) build_node(0, (int)tris.size());
  }
  // Distance to the nearest triangle along a ray, or a negative number if none within `limit`.
  float cast(const float* p, const float* dir, float limit) const {
    if (nodes.empty()) return -1.0f;
    float inv[3];
    for (int k = 0; k < 3; ++k) inv[k] = fabsf(dir[k]) > 1e-9f ? 1.0f / dir[k] : (dir[k] < 0.0f ? -1e9f : 1e9f);
    float nearest = limit;
    int stack[64], top = 0;
    stack[top++] = 0;
    while (top) {
      const Node& node = nodes[(size_t)stack[--top]];
      float t0 = 0.0f, t1 = nearest;
      for (int k = 0; k < 3 && t0 <= t1; ++k) {
        float x = (node.lo[k] - p[k]) * inv[k], y = (node.hi[k] - p[k]) * inv[k];
        if (x > y) { float swap = x; x = y; y = swap; }
        if (x > t0) t0 = x;
        if (y < t1) t1 = y;
      }
      if (t0 > t1) continue;
      if (node.left >= 0) {
        if (top < 62) { stack[top++] = node.left; stack[top++] = node.right; }
        continue;
      }
      for (int i = node.first; i < node.first + node.count; ++i) {
        const LevelTri& tri = tris[(size_t)i];
        float e1[3], e2[3], h[3], sv[3], qv[3];
        for (int k = 0; k < 3; ++k) { e1[k] = tri.b[k] - tri.a[k]; e2[k] = tri.c[k] - tri.a[k]; }
        h[0] = dir[1] * e2[2] - dir[2] * e2[1]; h[1] = dir[2] * e2[0] - dir[0] * e2[2]; h[2] = dir[0] * e2[1] - dir[1] * e2[0];
        float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
        if (fabsf(det) < 1e-7f) continue;
        float idet = 1.0f / det;
        for (int k = 0; k < 3; ++k) sv[k] = p[k] - tri.a[k];
        float u = idet * (sv[0] * h[0] + sv[1] * h[1] + sv[2] * h[2]);
        if (u < 0.0f || u > 1.0f) continue;
        qv[0] = sv[1] * e1[2] - sv[2] * e1[1]; qv[1] = sv[2] * e1[0] - sv[0] * e1[2]; qv[2] = sv[0] * e1[1] - sv[1] * e1[0];
        float v = idet * (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]);
        if (v < 0.0f || u + v > 1.0f) continue;
        float t = idet * (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]);
        if (t > 0.001f && t < nearest) nearest = t;
      }
    }
    return nearest < limit ? nearest : -1.0f;
  }
};

// A light's colour as Remix is given it. The engine multiplies a texture, as displayed, by
// the light's colour; Remix multiplies in linear light and the display curve comes after,
// which would leave a light twice as blue as red only a third bluer on screen. So each
// channel is taken to the power 2.2, at the same overall strength.
static void light_shade(const float* color, float* shade) {
  for (int k = 0; k < 3; ++k) shade[k] = color[k];
  if (!g_linear_light_colour) return;
  float mean = (color[0] + color[1] + color[2]) / 3.0f;
  float lin[3] = { powf(color[0] < 0.0f ? 0.0f : color[0], 2.2f), powf(color[1] < 0.0f ? 0.0f : color[1], 2.2f), powf(color[2] < 0.0f ? 0.0f : color[2], 2.2f) };
  float lin_mean = (lin[0] + lin[1] + lin[2]) / 3.0f;
  if (lin_mean > 0.0f) for (int k = 0; k < 3; ++k) shade[k] = lin[k] * mean / lin_mean;
}

// The second, better fit, when the mission file's lightmaps are to hand: what the engine
// actually baked on every polygon. A light's strength is set so that the light it sends
// to the polygons it reaches (straight, unshadowed, by the square of the distance) adds
// up, over their area, to the share of the baked light that is this light's. Which share
// that is comes out of the fit itself: every polygon's baked value is divided among the
// lights that reach it in proportion to what each would send, and the strengths are
// corrected, round and round, until they stop moving. A light that changes has a layer of
// its own in the lightmaps, so its share needs no dividing. `kFitShare` as before leaves
// room for bounced light. A polygon baked to full white only says "at least this much".
static bool fit_to_baked(const LevelBvh& bvh, const std::vector<SceneLight>& lights, const World& w, float reach) {
  const BakedLight& baked = scene_baked_light();
  size_t count = baked.steady.size() / 3;
  if (!count) return false;
  // A polygon is sampled at the middle of each of its triangles: one point at its centre
  // overstates what a light close to it delivers to the whole of it.
  struct Sample { float at[3]; float area; float value[3]; };   // value: the steady lightmap there
  struct Poly { double c[3] = { 0, 0, 0 }; float n[3] = { 0, 0, 0 }; double area = 0.0; std::vector<Sample> samples; bool fine = false; };
  std::vector<Poly> polys(count);
  for (size_t t = 0; t + 2 < w.indices.size(); t += 3) {
    uint32_t ia = w.indices[t], ib = w.indices[t + 1], ic = w.indices[t + 2];
    if (ia >= w.vertices.size() || ib >= w.vertices.size() || ic >= w.vertices.size()) continue;
    const WorldVertex &va = w.vertices[ia], &vb = w.vertices[ib], &vc = w.vertices[ic];
    uint32_t poly = va.flags & 0x7FFFF;
    if (poly >= count || fabsf(va.position[0]) >= 1199.0f) continue;
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) { e1[k] = vb.position[k] - va.position[k]; e2[k] = vc.position[k] - va.position[k]; }
    double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
    double area = 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
    Poly& q = polys[poly];
    for (int k = 0; k < 3; ++k) q.c[k] += area * (va.position[k] + vb.position[k] + vc.position[k]) / 3.0;
    memcpy(q.n, va.normal, 12);
    q.area += area;
    if (area > 0.0 && q.samples.size() < 12) {
      Sample sample;
      for (int k = 0; k < 3; ++k) sample.at[k] = (va.position[k] + vb.position[k] + vc.position[k]) / 3.0f;
      sample.area = (float)area;
      q.samples.push_back(sample);
    }
  }
  for (Poly& q : polys) if (q.area > 0.0) for (int k = 0; k < 3; ++k) q.c[k] /= q.area;
  // Better than triangle middles with the polygon's mean value: the lightmap's own texels,
  // each where it lies on the polygon, with what the engine baked right there. A lamp
  // close to a wall, a shadow's edge across a floor and a long corridor's far end then
  // each count for what they are.
  const MissionPolyLights& file = mission_poly_lights();
  bool have_fine = g_fit_texels && file.sample_first.size() == count + 1;
  size_t fine_polys = 0, fine_points = 0;
  for (size_t j = 0; j < count; ++j) {
    Poly& q = polys[j];
    if (!(q.area > 0.0)) continue;
    unsigned a = have_fine ? file.sample_first[j] : 0, b = have_fine ? file.sample_first[j + 1] : 0;
    if (b > a && (size_t)b * 7 <= file.samples.size()) {
      q.samples.clear();
      for (unsigned sample_index = a; sample_index < b; ++sample_index) {
        const float* f = &file.samples[(size_t)sample_index * 7];
        Sample sample;
        memcpy(sample.at, f, 12);
        sample.area = (float)(q.area * f[3]);
        memcpy(sample.value, f + 4, 12);
        q.samples.push_back(sample);
      }
      q.fine = true;
      ++fine_polys;
      fine_points += b - a;
    } else {
      for (Sample& sample : q.samples) for (int c = 0; c < 3; ++c) sample.value[c] = baked.steady[j * 3 + c];
    }
  }

  // How much of the light that falls on a polygon's surroundings comes back off them: the
  // textures' average colour, in linear light, over the polygons of the same 12-unit block
  // and its neighbours. Light bounces in a path tracer and did not in the engine, so where
  // walls are pale the lights themselves must supply a smaller part of what was baked.
  const auto& textures = scene_textures();
  std::vector<float> reflect(count, -1.0f);
  for (const auto& kv : w.ranges) {
    const WorldRange& r = kv.second;
    if (r.start >= w.indices.size() || r.texture < 0 || (size_t)r.texture >= textures.size()) continue;
    uint32_t vi = w.indices[r.start];
    if (vi >= w.vertices.size()) continue;
    uint32_t poly = w.vertices[vi].flags & 0x7FFFF;
    const SceneTexture& tex = textures[(size_t)r.texture];
    if (poly >= count || tex.uniform) continue;
    const float* rgb = tex.solid ? tex.solid_rgba : tex.has_mean ? tex.mean_rgb : nullptr;
    if (!rgb) continue;
    reflect[poly] = (powf(rgb[0], 2.2f) + powf(rgb[1], 2.2f) + powf(rgb[2], 2.2f)) / 3.0f;
  }
  struct Block { double weighted = 0.0, area = 0.0; };
  std::map<int64_t, Block> blocks;
  auto block_of = [](const double* c, int dx, int dy, int dz) {
    int64_t x = (int64_t)floor(c[0] / 12.0) + dx, y = (int64_t)floor(c[1] / 12.0) + dy, z = (int64_t)floor(c[2] / 12.0) + dz;
    return (x & 0x1FFFFF) << 42 | (y & 0x1FFFFF) << 21 | (z & 0x1FFFFF);
  };
  double level_weighted = 0.0, level_area = 0.0;
  for (size_t j = 0; j < count; ++j) {
    if (!(polys[j].area > 0.0) || reflect[j] < 0.0f) continue;
    Block& b = blocks[block_of(polys[j].c, 0, 0, 0)];
    b.weighted += polys[j].area * reflect[j];
    b.area += polys[j].area;
    level_weighted += polys[j].area * reflect[j];
    level_area += polys[j].area;
  }
  // On a first visit few polygons' textures are known yet, and the few are whatever the
  // player happened to face. So the level's average leans on a typical value (a tenth:
  // these decks are dark metal) until a quarter of its area is known.
  double all_area = 0.0;
  for (size_t j = 0; j < count; ++j) all_area += polys[j].area;
  double prior_area = 0.25 * all_area - level_area;
  if (prior_area < 0.0) prior_area = 0.0;
  float level_reflect = level_area + prior_area > 0.0 ? (float)((level_weighted + 0.10 * prior_area) / (level_area + prior_area)) : 0.10f;
  std::vector<float> share(count, 1.0f);
  double share_sum = 0.0, share_area = 0.0;
  for (size_t j = 0; j < count; ++j) {
    if (!(polys[j].area > 0.0)) continue;
    double weighted = 0.0, area = 0.0;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          auto found = blocks.find(block_of(polys[j].c, dx, dy, dz));
          if (found != blocks.end()) { weighted += found->second.weighted; area += found->second.area; }
        }
    float around = area > 50.0 ? (float)(weighted / area) : level_reflect;
    around = around < 0.05f ? 0.05f : around > 0.75f ? 0.75f : around;
    share[j] = kDarkSide * (1.0f - around);
    share_sum += polys[j].area * share[j];
    share_area += polys[j].area;
  }
  // What the lights must send a polygon: the baked value is a multiplier on the texture as
  // displayed, so in linear light it is that value to the power 2.2.
  auto wanted = [&](size_t j, float baked_value) { return share[j] * powf(baked_value < 0.0f ? 0.0f : baked_value, 2.2f); };
  const float* steady = baked.steady.data();

  // Each light's colour as Remix gets it, and its mean for "is this worth a ray". The fit
  // is channel by channel: a wall baked violet can only be the violet lights' doing.
  std::vector<std::array<float, 3>> shade(lights.size());
  std::vector<float> x(lights.size(), 0.0f);
  std::vector<uint8_t> changing(lights.size(), 0);
  for (const BakedLayer& layer : baked.layers) if (layer.light >= 0 && (size_t)layer.light < lights.size()) changing[(size_t)layer.light] = 1;
  for (size_t i = 0; i < lights.size(); ++i) {
    const SceneLight& l = lights[i];
    float steady = (l.color[0] + l.color[1] + l.color[2]) / 3.0f, full = (l.feed_full[0] + l.feed_full[1] + l.feed_full[2]) / 3.0f;
    x[i] = changing[i] && full > 0.0f ? full : steady;
    light_shade(changing[i] && full > 0.0f ? l.feed_full : l.color, shade[i].data());
  }
  // What light i, at strength 1, sends straight to polygon j (no shadow test here).
  auto sends_at = [&](size_t i, const float* point, const float* normal) -> float {
    const remixapi_Float3D& at = g_light_shown[i];
    float to[3] = { at.x - point[0], at.y - point[1], at.z - point[2] };
    float d2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
    if (d2 < 0.01f) d2 = 0.01f;
    float d = sqrtf(d2);
    float cosine = (to[0] * normal[0] + to[1] * normal[1] + to[2] * normal[2]) / d;
    if (cosine <= 0.0f) return 0.0f;
    float shape = 1.0f;
    const SceneLight& l = lights[i];
    float axis = sqrtf(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
    if (l.spot && axis > 0.5f && l.outer > -1.0f && l.outer < 1.0f) {
      float c = -(to[0] * l.direction[0] + to[1] * l.direction[1] + to[2] * l.direction[2]) / (d * axis);
      if (c <= l.outer) return 0.0f;
      shape = l.inner > l.outer ? (c - l.outer) / (l.inner - l.outer) : 1.0f;
      if (shape > 1.0f) shape = 1.0f;
      shape = shape * shape * (3.0f - 2.0f * shape);   // the smooth step Remix applies
    }
    return reach * cosine * shape / d2;   // times the light's colour, by channel
  };
  auto sends = [&](size_t i, const Poly& q, float* out_dir, float* out_dist) -> float {
    double total = 0.0, area = 0.0;
    for (const Sample& sample : q.samples) { total += (double)sample.area * sends_at(i, sample.at, q.n); area += sample.area; }
    const remixapi_Float3D& at = g_light_shown[i];
    float to[3] = { at.x - (float)q.c[0], at.y - (float)q.c[1], at.z - (float)q.c[2] };
    float d = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (out_dir) for (int k = 0; k < 3; ++k) out_dir[k] = d > 1e-4f ? to[k] / d : 0.0f;
    if (out_dist) *out_dist = d;
    return area > 0.0 ? (float)(total / area) : 0.0f;
  };

  // FitRule: 1 fits point by point (see below); 0 the light that shows over a lamp's whole
  // area. Walked through all 23 decks both ways (PHASE1.md, Update 12): by area matched
  // single rooms well but left more places too bright; point by point is the default.
  static const int kFitRule = (int)config_number(L"FitRule", 1.0);
  // Lights that change: each against its own layers. The same measure as below: what
  // shows of the lamp over the points the engine lit with it adds up to what it baked.
  struct LayerPoint { uint32_t light; float area, geo, cap, want[3]; };
  std::vector<LayerPoint> layer_points;
  for (const BakedLayer& layer : baked.layers) {
    if (layer.light < 0 || (size_t)layer.light >= lights.size() || layer.polygon >= count) continue;
    const Poly& q = polys[layer.polygon];
    if (!(q.area > 0.0)) continue;
    size_t li = (size_t)layer.light;
    const float* values = nullptr;
    if (q.fine && layer.entry < file.layer_sample_first.size()) {
      size_t at = (size_t)file.layer_sample_first[layer.entry] * 3;
      if (at + q.samples.size() * 3 <= file.layer_samples.size()) values = &file.layer_samples[at];
    }
    float cap = wanted(layer.polygon, 1.0f);
    if (values) {
      for (size_t k = 0; k < q.samples.size(); ++k) {
        const float* v = values + k * 3;
        if (v[0] + v[1] + v[2] < 0.006f) continue;   // the engine baked none of this lamp's light here: its shadow
        LayerPoint point = { (uint32_t)li, q.samples[k].area, sends_at(li, q.samples[k].at, q.n), cap,
                             { wanted(layer.polygon, v[0]), wanted(layer.polygon, v[1]), wanted(layer.polygon, v[2]) } };
        layer_points.push_back(point);
      }
      continue;
    }
    LayerPoint point = { (uint32_t)li, (float)q.area, sends(li, q, nullptr, nullptr), cap,
                         { wanted(layer.polygon, layer.mean[0]), wanted(layer.polygon, layer.mean[1]), wanted(layer.polygon, layer.mean[2]) } };
    layer_points.push_back(point);
  }
  std::vector<double> num(lights.size(), 0.0), den(lights.size(), 0.0);
  std::vector<float> layer_gain(lights.size(), 1.0f);
  for (int round = 0; round < (kFitRule == 0 ? 24 : 1); ++round) {
    std::fill(num.begin(), num.end(), 0.0);
    std::fill(den.begin(), den.end(), 0.0);
    for (const LayerPoint& point : layer_points)
      for (int c = 0; c < 3; ++c) {
        float have = layer_gain[point.light] * point.geo * shade[point.light][c];
        if (kFitRule == 0 && have > point.cap) have = point.cap;
        num[point.light] += (double)point.area * point.want[c];
        den[point.light] += (double)point.area * have;
      }
    for (size_t i = 0; i < lights.size(); ++i) {
      if (!changing[i] || !(den[i] > 0.0) || !(num[i] > 0.0)) continue;
      float g = layer_gain[i] * (float)(num[i] / den[i]);
      layer_gain[i] = g < 0.1f ? 0.1f : g > 12.0f ? 12.0f : g;
    }
  }
  unsigned fitted_changing = 0, fitted_steady = 0;
  for (size_t i = 0; i < lights.size(); ++i) {
    if (!changing[i] || !(den[i] > 0.0) || !(num[i] > 0.0)) continue;
    g_light_gain[i] = layer_gain[i];
    ++fitted_changing;
  }

  // Lights that never change: all against the steady layer, point by point, each point
  // with a ray to each light that could matter to it.
  // The engine's values stop at white, and so does the screen: a point the lights send
  // more than white is counted as white. A texel baked to full white says "at least this
  // much"; but light far beyond white there is light the engine did not have bouncing
  // round the room, and a glaring patch by every lamp, so past FitHeadroom times white
  // (2) such a point does count against its lights. 0: never.
  static const float kWhiteHeadroom = (float)config_number(L"FitHeadroom", 2.0);
  struct Point { float area; float want[3]; bool white[3]; float cap; };
  struct Entry { uint32_t point, light; float sends; };
  std::vector<Point> points;
  std::vector<Entry> entries;
  size_t rays = 0;
  for (size_t j = 0; j < count; ++j) {
    const Poly& q = polys[j];
    if (!(q.area > 0.0) || steady[j * 3] < 0.0f) continue;
    size_t first_point = points.size();
    for (const Sample& sample : q.samples) {
      Point point;
      point.area = sample.area;
      point.cap = wanted(j, 1.0f);   // white, at this point
      for (int c = 0; c < 3; ++c) { point.want[c] = wanted(j, sample.value[c]); point.white[c] = sample.value[c] >= 0.97f; }
      points.push_back(point);
    }
    for (size_t i = 0; i < lights.size(); ++i) {
      if (changing[i] || !(x[i] > 0.0f)) continue;
      if (sends(i, q, nullptr, nullptr) * x[i] < 0.003f) continue;
      const remixapi_Float3D& at = g_light_shown[i];
      for (size_t k = 0; k < q.samples.size(); ++k) {
        const Sample& sample = q.samples[k];
        float a = sends_at(i, sample.at, q.n);
        if (!(a > 0.0f)) continue;
        float origin[3] = { sample.at[0] + q.n[0] * 0.05f, sample.at[1] + q.n[1] * 0.05f, sample.at[2] + q.n[2] * 0.05f };
        float dir[3] = { at.x - origin[0], at.y - origin[1], at.z - origin[2] };
        float dist = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dist > 1e-4f) for (int c = 0; c < 3; ++c) dir[c] /= dist;
        ++rays;
        if (dist > 0.2f && bvh.cast(origin, dir, dist - 0.1f) > 0.0f) continue;   // in its shadow
        entries.push_back({ (uint32_t)(first_point + k), (uint32_t)i, a });
      }
    }
  }
  std::vector<float> gain(lights.size(), 1.0f), predicted(points.size() * 3, 0.0f);
  for (size_t i = 0; i < lights.size(); ++i) gain[i] = g_light_gain[i];
  double baked_total = 0.0, before = 0.0, after = 0.0;
  for (int round = 0; round <= 40; ++round) {
    std::fill(predicted.begin(), predicted.end(), 0.0f);
    for (const Entry& e : entries)
      for (int c = 0; c < 3; ++c) predicted[(size_t)e.point * 3 + c] += gain[e.light] * e.sends * shade[e.light][c];
    if (round == 0 || round == 40) {
      double total = 0.0;
      baked_total = 0.0;
      for (size_t k = 0; k < points.size(); ++k)
        for (int c = 0; c < 3; ++c) {   // (what shows: light beyond white is not counted)
          total += points[k].area * std::min(predicted[k * 3 + c], points[k].cap);
          baked_total += points[k].area * points[k].want[c];
        }
      (round == 0 ? before : after) = total;
      if (round == 40) break;
    }
    std::fill(num.begin(), num.end(), 0.0);
    std::fill(den.begin(), den.end(), 0.0);
    for (const Entry& e : entries) {
      const Point& point = points[e.point];
      for (int c = 0; c < 3; ++c) {
        float target = point.want[c], have = predicted[(size_t)e.point * 3 + c];
        float shown = have < point.cap ? have : point.cap;
        if (kFitRule == 0) {
          // By light on show: over the area a lamp lights, what shows of it on screen adds
          // up to what the engine baked there. Each point's light is shared among its
          // lamps by what each sends. This is the measure a screenshot's mean takes.
          if (!(have > 1e-9f)) continue;
          double part = (double)point.area * (gain[e.light] * e.sends * shade[e.light][c] / have);
          if (point.white[c] && shown > target) shown = kWhiteHeadroom > 0.0f && have > kWhiteHeadroom * point.cap ? have / kWhiteHeadroom : target;
          num[e.light] += part * target;
          den[e.light] += part * shown;
        } else {
          // By point: every point's own ratio, weighted by what the lamp sends it. The
          // points nearest a lamp weigh most, so far walls err on the dark side.
          float seen = kFitRule == 2 ? shown : have;   // 2: light beyond white is not held against a lamp
          float ratio = seen > 1e-6f ? target / seen : 1.0f;
          if (point.white[c] && ratio < 1.0f) ratio = kWhiteHeadroom > 0.0f && have > kWhiteHeadroom * point.cap ? kWhiteHeadroom * point.cap / have : 1.0f;
          ratio = ratio < 0.2f ? 0.2f : ratio > 5.0f ? 5.0f : ratio;
          double weight = (double)point.area * e.sends * shade[e.light][c];
          num[e.light] += weight * ratio;
          den[e.light] += weight;
        }
      }
    }
    for (size_t i = 0; i < lights.size(); ++i) {
      if (changing[i] || !(den[i] > 0.0)) continue;
      float step = (float)(num[i] / den[i]);
      step = step < 0.5f ? 0.5f : step > 2.0f ? 2.0f : step;   // no round moves a light far
      float g = gain[i] * step;
      gain[i] = g < 0.05f ? 0.05f : g > 12.0f ? 12.0f : g;
    }
  }
  std::vector<uint8_t> reached_light(lights.size(), 0);
  for (const Entry& e : entries) reached_light[e.light] = 1;
  std::vector<float> all;
  for (size_t i = 0; i < lights.size(); ++i) {
    if (changing[i]) { if (den[i] > 0.0 || g_light_gain[i] != 1.0f) all.push_back(g_light_gain[i]); continue; }
    if (!reached_light[i]) continue;
    g_light_gain[i] = gain[i];
    all.push_back(gain[i]);
    ++fitted_steady;
  }
  // The darkest the engine leaves any surface: its ambient light, which no lamp supplies.
  // Surfaces give that much off themselves (see follow_ambient()).
  {
    std::vector<float> levels;
    for (size_t j = 0; j < count; ++j)
      if (polys[j].area > 0.0 && steady[j * 3] >= 0.0f) levels.push_back((steady[j * 3] + steady[j * 3 + 1] + steady[j * 3 + 2]) / 3.0f);
    if (levels.size() > 20) {
      std::nth_element(levels.begin(), levels.begin() + levels.size() / 20, levels.end());
      g_baked_floor = levels[levels.size() / 20];
      if (g_baked_floor > 0.2f) g_baked_floor = 0.2f;
    }
  }
  std::sort(all.begin(), all.end());
  logf("remix: lights fitted to the engine's lightmaps: %u steady and %u changing ones (strength against the five-unit match: median %.2f, least %.2f, most %.1f); "
       "%zu point-light pairs from %zu rays, %zu of the polygons sampled at their lightmap's texels (%zu points); light sent to the level was %.0f%% of the baked target before, %.0f%% after; "
       "walls give back %.0f%% on average (known for %.0f%% of the area), so the lights supply %.0f%% of what was baked",
       fitted_steady, fitted_changing, all.empty() ? 1.0f : all[all.size() / 2], all.empty() ? 1.0f : all.front(), all.empty() ? 1.0f : all.back(),
       entries.size(), rays, fine_polys, fine_points, baked_total > 0.0 ? 100.0 * before / baked_total : 0.0, baked_total > 0.0 ? 100.0 * after / baked_total : 0.0,
       100.0 * level_reflect, share_area > 0.0 ? 100.0 * level_area / share_area : 0.0, share_area > 0.0 ? 100.0 * share_sum / share_area : 0.0);
  return true;
}

static void place_lights() {
  const auto& lights = scene_lights();
  const World& w = scene_world();
  g_light_shown.resize(lights.size());
  g_light_gain.assign(lights.size(), 1.0f);
  for (size_t i = 0; i < lights.size(); ++i) g_light_shown[i] = { lights[i].position[0], lights[i].position[1], lights[i].position[2] };
  if ((!g_stand_off && !g_fit_lights) || !w.loaded || lights.empty()) return;
  double t0 = now_ms();
  const float reach = g_light_scale / 8.0f;   // the plain match: five units at the default scale
  size_t tris = w.indices.size() / 3;
  static LevelBvh bvh;
  std::vector<LevelTri>& list = bvh.tris;
  list.clear();
  list.reserve(tris);
  for (size_t t = 0; t < tris; ++t) {
    uint32_t ia = w.indices[t * 3], ib = w.indices[t * 3 + 1], ic = w.indices[t * 3 + 2];
    if (ia >= w.vertices.size() || ib >= w.vertices.size() || ic >= w.vertices.size()) continue;
    LevelTri tri;
    memcpy(tri.a, w.vertices[ia].position, 12);
    memcpy(tri.b, w.vertices[ib].position, 12);
    memcpy(tri.c, w.vertices[ic].position, 12);
    if (fabsf(tri.a[0]) >= 1199.0f && fabsf(tri.a[1]) >= 1199.0f) continue;   // the sky box
    memcpy(tri.n, w.vertices[ia].normal, 12);
    float r2 = 0.0f;
    for (int k = 0; k < 3; ++k) tri.mid[k] = (tri.a[k] + tri.b[k] + tri.c[k]) / 3.0f;
    for (const float* corner : { tri.a, tri.b, tri.c }) {
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) d2 += (corner[k] - tri.mid[k]) * (corner[k] - tri.mid[k]);
      if (d2 > r2) r2 = d2;
    }
    tri.radius = sqrtf(r2);
    list.push_back(tri);
  }
  bvh.build();
  // Directions spread evenly over the sphere.
  static float dirs[96][3];
  static bool have_dirs = false;
  if (!have_dirs) {
    have_dirs = true;
    for (int i = 0; i < 96; ++i) {
      float z = 1.0f - 2.0f * ((float)i + 0.5f) / 96.0f, r = sqrtf(1.0f - z * z), phi = (float)i * 2.399963f;
      dirs[i][0] = r * cosf(phi); dirs[i][1] = r * sinf(phi); dirs[i][2] = z;
    }
  }
  unsigned moved = 0, fitted = 0;
  float furthest = 0.0f;
  // Lights only met in the engine's per-object lists carry what reaches an object, not
  // their own strength: nothing to fit.
  bool from_mission = scene_lights_from_mission();
  std::vector<float> gains;
  for (size_t i = 0; i < lights.size(); ++i) {
    const SceneLight& l = lights[i];
    const float* p = l.position;
    // Its brightest channel at full strength, in the engine's units.
    float x = 0.0f;
    for (int k = 0; k < 3; ++k) { if (l.color[k] > x) x = l.color[k]; if (l.feed_full[k] > x) x = l.feed_full[k]; }
    float fit = x * reach;   // what the plain match gives it
    if (g_fit_lights && from_mission && x > 0.0f) {
      float axis[3] = { l.direction[0], l.direction[1], l.direction[2] };
      float axis_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
      bool cone = l.spot && axis_len > 0.5f && l.outer > -1.0f && l.outer < 1.0f;
      double total = 0.0, weight = 0.0;
      for (int d = 0; d < 96; ++d) {
        float share = 1.0f;
        if (cone) {
          float c = (dirs[d][0] * axis[0] + dirs[d][1] * axis[1] + dirs[d][2] * axis[2]) / axis_len;
          if (c <= l.outer) continue;
          share = l.inner > l.outer ? (c - l.outer) / (l.inner - l.outer) : 1.0f;
          if (share > 1.0f) share = 1.0f;
          share = share * share * (3.0f - 2.0f * share);
        }
        weight += share;
        float hit = bvh.cast(p, dirs[d], 600.0f);
        if (hit <= 0.0f) continue;                    // out through a gap: lights nothing
        float engine = x / hit;
        if (l.radius > 0.0f) engine *= hit < l.radius ? 1.0f - hit / l.radius : 0.0f;   // the engine's cut-off distance
        if (engine > 1.0f) engine = 1.0f;             // a lightmap holds no more
        total += (double)share * engine * hit * hit;
      }
      if (weight > 0.0 && total > 0.0) {
        fit = kFitShare * (float)(total / weight);
        float gain = fit / (x * reach);
        gain = gain < 0.2f ? 0.2f : gain > 60.0f ? 60.0f : gain;
        fit = gain * x * reach;
        g_light_gain[i] = gain;
        gains.push_back(gain);
        ++fitted;
      }
    }
    if (!g_stand_off || !(x > 0.0f)) continue;   // spotlights too: one hung against a wall lights it at a grazing angle
    float best = 40.0f, at[3] = {};
    for (const LevelTri& tri : list) {
      float to_mid[3] = { p[0] - tri.mid[0], p[1] - tri.mid[1], p[2] - tri.mid[2] };
      float reachable = best + tri.radius;
      if (to_mid[0] * to_mid[0] + to_mid[1] * to_mid[1] + to_mid[2] * to_mid[2] >= reachable * reachable) continue;
      float side = (p[0] - tri.a[0]) * tri.n[0] + (p[1] - tri.a[1]) * tri.n[1] + (p[2] - tri.a[2]) * tri.n[2];
      if (side <= 0.0f || side >= best) continue;   // behind it, or its plane alone is already too far
      float q[3];
      closest_on_triangle(p, tri.a, tri.b, tri.c, q);
      float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
      if (d < best) { best = d; memcpy(at, q, 12); }
    }
    if (!(best < 40.0f) || best < 0.02f) continue;
    // Where the pool on that surface peaks at the engine's own value there (x over the
    // distance it was at). Two units at most: further and the light is somewhere else.
    float move = sqrtf(fit / x * best) - best;
    float most = l.spot ? 1.0f : 2.0f;   // a spotlight less: its aim is part of the level's look
    if (move > most) move = most;
    if (move < 0.05f) continue;
    float dir[3] = { (p[0] - at[0]) / best, (p[1] - at[1]) / best, (p[2] - at[2]) / best };
    float beyond = bvh.cast(p, dir, 600.0f);
    if (beyond > 0.0f && move > beyond * 0.5f) move = beyond * 0.5f;
    if (move < 0.05f) continue;
    g_light_shown[i] = { p[0] + dir[0] * move, p[1] + dir[1] * move, p[2] + dir[2] * move };
    ++moved;
    if (move > furthest) furthest = move;
  }
  double t1 = now_ms();
  bool by_lightmaps = g_fit_lights && g_fit_baked && fit_to_baked(bvh, lights, w, reach);
  float median = 1.0f, most = 1.0f;
  if (!gains.empty()) {
    std::sort(gains.begin(), gains.end());
    median = gains[gains.size() / 2];
    most = gains.back();
  }
  logf("remix: %u of %zu lights fitted to their rooms (strength against the five-unit match: median %.2f, least %.2f, most %.1f); %u stood off from a wall (%.1f units at most); %.0f ms",
       fitted, lights.size(), median, gains.empty() ? 1.0f : gains.front(), most, moved, furthest, t1 - t0);
  if (by_lightmaps) logf("remix: the lightmap fit took %.0f ms", now_ms() - t1);
}

static remixapi_LightHandle make_light(const SceneLight& l, const remixapi_Float3D& at, const float* color, uint64_t hash, float gain = 1.0f) {
  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = at;
  sphere.radius = g_light_radius;
  sphere.volumetricRadianceScale = 1.0f;
  float len = sqrtf(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
  if (l.spot && len > 0.5f && l.outer > -1.0f && l.outer < 1.0f) {
    sphere.shaping_hasvalue = 1;
    sphere.shaping_value.direction = { l.direction[0] / len, l.direction[1] / len, l.direction[2] / len };
    // Remix: nothing outside the cone angle, then a smooth step up over `coneSoftness`,
    // which is a span of the cosine, not of the angle: full strength from the engine's
    // inner cone inwards.
    float outer_deg = acosf(l.outer) * 57.29578f;
    float inner_cos = l.inner > l.outer && l.inner <= 1.0f ? l.inner : l.outer;
    sphere.shaping_value.coneAngleDegrees = outer_deg;
    sphere.shaping_value.coneSoftness = inner_cos - l.outer > 0.001f ? inner_cos - l.outer : 0.001f;
    sphere.shaping_value.focusExponent = 0.0f;
  }
  remixapi_LightInfo li = {};
  li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  li.pNext = &sphere;
  li.hash = hash;
  // The engine multiplies a texture, as displayed, by the light's colour; Remix multiplies
  // in linear light and the display curve comes after, which would leave a light twice as
  // blue as red only a third bluer on screen. So the colour is taken to linear light
  // (each channel to the power 2.2), at the same overall strength.
  float shade[3];
  light_shade(color, shade);
  li.radiance = { shade[0] * g_light_scale * gain, shade[1] * g_light_scale * gain, shade[2] * g_light_scale * gain };
  remixapi_LightHandle h = nullptr;
  if (g_api.CreateLight(&li, &h) != REMIXAPI_ERROR_CODE_SUCCESS) h = nullptr;
  return h;
}

static void submit_lights(const remixapi_Float3D& camera_pos) {
  const auto& lights = scene_lights();
  static unsigned built_generation = ~0u;
  if (scene_lights_generation() != built_generation) {   // level changed, or its mission file was found
    built_generation = scene_lights_generation();
    for (auto h : g_light_handles) if (h) g_api.DestroyLight(h);
    g_light_handles.clear();
    g_light_built_color.clear();
    g_light_shown.clear();
    g_light_gain.clear();
    g_baked_floor = -1.0f;
  }
  static float placed_for_scale = 0.0f;
  static bool placed_stand_off = true, placed_fit = true;
  static unsigned placed_generation = ~0u;
  if (placed_generation != built_generation || placed_for_scale != g_light_scale || placed_stand_off != g_stand_off || placed_fit != g_fit_lights) {
    placed_generation = built_generation;
    placed_for_scale = g_light_scale;
    placed_stand_off = g_stand_off;
    placed_fit = g_fit_lights;
    place_lights();
    lights_restated();
  }
  for (size_t i = g_light_shown.size(); i < lights.size(); ++i)   // lights met since: where the engine has them
    g_light_shown.push_back({ lights[i].position[0], lights[i].position[1], lights[i].position[2] });
  if (g_light_gain.size() < lights.size()) g_light_gain.resize(lights.size(), 1.0f);
  int remade = 0;
  for (size_t i = 0; i < lights.size(); ++i) {
    const SceneLight& l = lights[i];
    // The engine's current brightness for this light when it has shown one, else the table's.
    const float* c = g_use_live_lights && l.has_live ? l.live : l.color;
    bool is_new = i >= g_light_handles.size();
    if (is_new) {
      g_light_handles.push_back(nullptr);
      g_light_built_color.push_back({ -1.0f, -1.0f, -1.0f });
    }
    remixapi_Float3D& built = g_light_built_color[i];
    float biggest = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    float change = fabsf(c[0] - built.x) + fabsf(c[1] - built.y) + fabsf(c[2] - built.z);
    if (!is_new && change <= 0.04f * (biggest > 0.05f ? biggest : 0.05f)) continue;
    if (!is_new && remade >= 48) continue;   // a level-wide change is spread over a few frames
    ++remade;
    if (g_light_handles[i]) { g_api.DestroyLight(g_light_handles[i]); g_light_handles[i] = nullptr; }
    built = { c[0], c[1], c[2] };
    if (!(biggest > 0.0f)) continue;         // switched off
    uint64_t hash = fnv1a64(&g_light_scale, sizeof g_light_scale, fnv1a64(l.position, sizeof l.position, 0x55325254584c4954ull));
    hash = fnv1a64(&g_light_shown[i], sizeof g_light_shown[i], hash);
    hash = fnv1a64(&g_light_gain[i], sizeof g_light_gain[i], hash);
    g_light_handles[i] = make_light(l, g_light_shown[i], c, hash, g_light_gain[i]);
    if (is_new && i < 6) logf("remix: light %zu at (%.1f, %.1f, %.1f) colour (%.2f, %.2f, %.2f)", i, l.position[0], l.position[1], l.position[2],
                              c[0], c[1], c[2]);
  }
  for (auto h : g_light_handles) if (h) g_api.DrawLightInstance(h);

  // The engine's directional lights (sunlight through a window). It gives a light vector
  // towards the light and a colour with no falloff; here that is a distant light half a
  // degree across whose irradiance equals the engine's value.
  static std::vector<remixapi_LightHandle> distant;
  for (auto h : distant) if (h) g_api.DestroyLight(h);
  distant.clear();
  const auto& far_lights = scene_far_lights();
  for (size_t i = 0; i < far_lights.size(); ++i) {
    const SceneLight& l = far_lights[i];
    remixapi_LightInfoDistantEXT sun = {};
    sun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    sun.direction = { -l.position[0], -l.position[1], -l.position[2] };
    sun.angularDiameterDegrees = 0.5f;
    sun.volumetricRadianceScale = 1.0f;
    const float per_irradiance = 1.0f / 5.98e-5f;   // one over the solid angle of a 0.5 degree disc
    remixapi_LightInfo li = {};
    li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    li.pNext = &sun;
    uint64_t slot = i;
    li.hash = fnv1a64(&slot, sizeof slot, 0x55325254584c5355ull);
    li.radiance = { l.color[0] * per_irradiance, l.color[1] * per_irradiance, l.color[2] * per_irradiance };
    remixapi_LightHandle h = nullptr;
    if (g_api.CreateLight(&li, &h) == REMIXAPI_ERROR_CODE_SUCCESS && h) { g_api.DrawLightInstance(h); distant.push_back(h); }
  }

  // The engine's moving lights: made afresh every frame, keyed by their slot.
  for (auto h : g_moving_handles) if (h) g_api.DestroyLight(h);
  g_moving_handles.clear();
  const auto& moving = scene_dynamic_lights();
  for (size_t i = 0; i < moving.size(); ++i) {
    uint64_t slot = i;
    remixapi_LightHandle h = make_light(moving[i], { moving[i].position[0], moving[i].position[1], moving[i].position[2] }, moving[i].color, fnv1a64(&slot, sizeof slot, 0x55325254584d4f56ull));
    if (h) { g_api.DrawLightInstance(h); g_moving_handles.push_back(h); }
  }

  // Fallback so a level with no object lights yet is not black: a small light at the camera.
  if (g_headlight) { g_api.DestroyLight(g_headlight); g_headlight = nullptr; }
  if (!lights.empty()) return;
  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = camera_pos;
  sphere.radius = 0.15f;
  remixapi_LightInfo li = {};
  li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  li.pNext = &sphere;
  li.hash = 0x55325254584c0001ull;
  li.radiance = { 60.0f, 58.0f, 54.0f };
  if (g_api.CreateLight(&li, &g_headlight) == REMIXAPI_ERROR_CODE_SUCCESS) g_api.DrawLightInstance(g_headlight);
}

// The engine's alpha blend for a see-through surface, as Remix takes it per instance:
// source alpha over the background, opacity = texture alpha x the material's alpha, colour
// tinted by the material's colour.
static void see_through_blend(const float* color, remixapi_InstanceInfoBlendEXT* blend) {
  auto byte = [](float v) -> uint32_t { return (uint32_t)((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f); };
  blend->sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend->alphaTestEnabled = 0;
  blend->alphaTestCompareOp = 7;        // VK_COMPARE_OP_ALWAYS
  blend->alphaBlendEnabled = 1;
  blend->srcColorBlendFactor = 6;       // VK_BLEND_FACTOR_SRC_ALPHA
  blend->dstColorBlendFactor = 7;       // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
  blend->colorBlendOp = 0;              // VK_BLEND_OP_ADD
  blend->textureColorArg1Source = 1;    // texture
  blend->textureColorArg2Source = 0;
  blend->textureColorOperation = 3;     // modulate
  blend->textureAlphaArg1Source = 1;    // texture
  blend->textureAlphaArg2Source = 3;    // texture factor
  blend->textureAlphaOperation = 3;     // modulate
  blend->tFactor = byte(color[3]) << 24 | byte(color[0]) << 16 | byte(color[1]) << 8 | byte(color[2]);
  blend->isTextureFactorBlend = 1;
  blend->srcAlphaBlendFactor = 1;       // VK_BLEND_FACTOR_ONE
  blend->dstAlphaBlendFactor = 7;
  blend->alphaBlendOp = 0;
  blend->writeMask = 0xF;
  blend->isVertexColorBakedLighting = 0;
}

// The engine's particles, sparks and coronas: quads it draws in the world after the objects.
// Each batch becomes a mesh for this frame only, facing the camera as the engine made it.
// One that the engine adds to the picture gives off its texture's light; one it blends by
// alpha is blended the same way, with the batch's average colour as the tint; an opaque
// one is plain cut-out geometry.
static std::vector<remixapi_MeshHandle> g_sprite_meshes;
static size_t g_stat_sprites, g_stat_sprites_most;
static remixapi_Float3D g_sprite_camera;

static void submit_sprites() {
  for (auto m : g_sprite_meshes) g_api.DestroyMesh(m);
  g_sprite_meshes.clear();
  g_stat_sprites = 0;
  if (!g_sprites) return;
  const auto& textures = scene_textures();
  static uint64_t serial = 0;
  for (const SpriteDraw& d : scene_frame_sprites()) {
    if (d.texture < 0 || (size_t)d.texture >= textures.size()) continue;
    double sum[4] = { 0, 0, 0, 0 };
    for (const SpriteVertex& v : d.vertices)
      for (int k = 0; k < 4; ++k) sum[k] += v.rgba[k] / 255.0;
    float mean[4];
    for (int k = 0; k < 4; ++k) mean[k] = (float)(sum[k] / (double)d.vertices.size());
    remixapi_MaterialHandle material = nullptr;
    remixapi_InstanceInfoBlendEXT blend = {};
    bool blended = false;
    if (g_sprite_vertex_colour && (d.additive || !d.alpha_test)) {
      // The engine's own blend, with each vertex's colour and alpha: texture times vertex
      // colour, added to or laid over what is behind.
      if (mean[3] < 0.004f && (!d.additive || d.by_alpha)) continue;
      material = material_for(&textures[d.texture], false, nullptr, false, kSeeThrough);
      const float white[4] = { 1, 1, 1, 1 };
      see_through_blend(white, &blend);
      blend.textureColorArg2Source = 2;      // the vertex colour
      blend.textureAlphaArg2Source = 2;
      blend.isTextureFactorBlend = 0;
      if (d.additive) {
        blend.srcColorBlendFactor = d.by_alpha ? 6 : 1;   // source alpha, or one
        blend.dstColorBlendFactor = 1;                    // one: added
        blend.dstAlphaBlendFactor = 1;
      }
      blended = true;
    } else if (d.additive) {
      float level = (0.2126f * mean[0] + 0.7152f * mean[1] + 0.0722f * mean[2]) * (d.by_alpha ? mean[3] : 1.0f);
      int step = (int)(level * 8.0f + 0.5f);
      if (step < 1) continue;                 // faded out
      float strength = g_emissive_scale > 0.0f ? g_screen_emissive / g_emissive_scale * (float)step / 8.0f : 0.0f;
      material = material_for(&textures[d.texture], false, nullptr, true, d.by_alpha ? kGlow : kLit, strength);
    } else if (!d.alpha_test) {
      if (mean[3] < 0.02f) continue;
      material = material_for(&textures[d.texture], false, nullptr, false, kSeeThrough);
      see_through_blend(mean, &blend);
      blended = true;
    } else {
      material = material_for(&textures[d.texture], true, nullptr);
    }
    if (!material) continue;
    std::vector<remixapi_HardcodedVertex> vertices(d.vertices.size());
    for (size_t i = 0; i < d.vertices.size(); ++i) {
      const SpriteVertex& src = d.vertices[i];
      remixapi_HardcodedVertex& v = vertices[i];
      memcpy(v.position, src.position, 12);
      float n[3] = { g_sprite_camera.x - src.position[0], g_sprite_camera.y - src.position[1], g_sprite_camera.z - src.position[2] };
      float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (int k = 0; k < 3; ++k) v.normal[k] = len > 1e-4f ? n[k] / len : (k == 2 ? 1.0f : 0.0f);
      v.texcoord[0] = src.uv[0];
      v.texcoord[1] = src.uv[1];
      v.color = (uint32_t)src.rgba[3] << 24 | (uint32_t)src.rgba[0] << 16 | (uint32_t)src.rgba[1] << 8 | src.rgba[2];
    }
    remixapi_MeshInfoSurfaceTriangles tris = {};
    tris.vertices_values = vertices.data();
    tris.vertices_count = vertices.size();
    tris.indices_values = d.indices.data();
    tris.indices_count = d.indices.size() - d.indices.size() % 3;
    tris.material = material;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    ++serial;
    info.hash = fnv1a64(&serial, sizeof serial, 0x5350524954455f31ull);
    info.surfaces_values = &tris;
    info.surfaces_count = 1;
    remixapi_MeshHandle mesh = nullptr;
    if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS || !mesh) continue;
    g_sprite_meshes.push_back(mesh);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = mesh;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.doubleSided = 1;
    if (d.additive || blended) inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
    if (blended) inst.pNext = &blend;
    g_api.DrawInstance(&inst);
    ++g_stat_sprites;
  }
  if (g_stat_sprites > g_stat_sprites_most) g_stat_sprites_most = g_stat_sprites;
}

// Splits one object draw into rigid parts, one per node transform, and caches the meshes.
// `lit_layer`: the for-the-eye part of a self-lit object part (see g_emissive_light).
static const std::vector<Part>& parts_for(const ObjectDraw& d, bool glow_layer = false, bool as_glass = false, bool lit_layer = false) {
  const ModelMesh& model = scene_models()[d.model];
  const auto& textures = scene_textures();
  int tex_index = glow_layer && d.glow_texture >= 0 ? d.glow_texture : d.texture;
  const SceneTexture* tex = tex_index >= 0 ? &textures[tex_index] : nullptr;
  uint64_t key = model.hash;
  key = fnv1a64(&d.start, sizeof d.start, key);
  key = fnv1a64(&d.count, sizeof d.count, key);
  uint64_t tex_hash = tex ? tex->hash : 0;
  key = fnv1a64(&tex_hash, sizeof tex_hash, key);
  key ^= d.alpha_test ? 0x9e3779b97f4a7c15ull : 0;
  // A part the engine does not shade (a lamp face, a screen) is bright whatever the
  // lighting: texture times the material's colour, and a dark colour is how the engine
  // shows a lamp that is off. So its brightness here follows that colour, in linear light.
  // A shaded part gets its material's "illumination" added to the light on it: at 1 it is
  // as good as full-bright. (Models only: the level's shader does not read that value.)
  bool self_lit = d.unshaded || d.illumination >= 0.5f;
  float object_tint = d.color[0] > d.color[1] ? (d.color[0] > d.color[2] ? d.color[0] : d.color[2]) : (d.color[1] > d.color[2] ? d.color[1] : d.color[2]);
  object_tint = object_tint < 0.0f ? 0.0f : object_tint > 1.0f ? 1.0f : object_tint;
  uint32_t object_tint_step = (uint32_t)(object_tint * 16.0f + 0.5f);
  if (self_lit && object_tint_step == 0 && !lit_layer) self_lit = false;
  // One the engine lays over what is behind by its alpha shows that much of itself.
  if (self_lit && d.alpha_blend && d.color[3] < 0.99f) {
    object_tint *= d.color[3] < 0.0f ? 0.0f : d.color[3];
    object_tint_step = (uint32_t)(object_tint * 16.0f + 0.5f);
  }
  float object_lit = powf((float)object_tint_step / 16.0f, 2.2f);
  bool see_through = !glow_layer && !lit_layer && d.alpha_blend && d.color[3] < 0.99f;
  if (lit_layer) key ^= 0x4c49545f4f424a31ull;
  bool glass = see_through && as_glass;
  bool decal = d.alpha_blend && !glow_layer && !see_through;   // opaque-alpha blended draws: signs, blood
  if (glass) {
    uint32_t q = (uint32_t)(d.color[3] * 16.0f + 0.5f);
    key = fnv1a64(&q, sizeof q, key ^ 0x474c4153535f5f31ull);
  } else if (see_through) {
    key ^= 0x5345455448525531ull;
  }
  key ^= self_lit ? 0x7f4a7c159e3779b9ull : 0;
  if (self_lit || lit_layer) key = fnv1a64(&object_tint_step, sizeof object_tint_step, key);
  key ^= decal ? 0x4355544f55545f31ull : 0;
  if (glow_layer) {
    uint32_t q = (uint32_t)(d.glow_strength * kGlowSteps + 0.5f);
    key = fnv1a64(&q, sizeof q, key ^ 0x474c4f574c415952ull);
  }
  if (tex && tex->solid) {   // flat-coloured: the tint is part of the identity
    uint8_t q[3] = { (uint8_t)(d.color[0] * 255.0f + 0.5f), (uint8_t)(d.color[1] * 255.0f + 0.5f), (uint8_t)(d.color[2] * 255.0f + 0.5f) };
    key = fnv1a64(q, sizeof q, key);
  }
  bool mirror = d.env_mapped && !glow_layer && !lit_layer && !glass;
  if (mirror) {
    uint8_t q[4] = { (uint8_t)(d.color[0] * 32.0f + 0.5f), (uint8_t)(d.color[1] * 32.0f + 0.5f), (uint8_t)(d.color[2] * 32.0f + 0.5f), (uint8_t)(d.lod_bias * 2.0f + 0.5f) };
    key = fnv1a64(q, sizeof q, key ^ 0x4d4952524f525f31ull);
  }
  bool polished = d.env_layer && !mirror;
  key ^= polished ? 0x474c4f5353595f31ull : 0;
  auto it = g_parts.find(key);
  if (it != g_parts.end()) return it->second;

  struct Build { std::vector<remixapi_HardcodedVertex> vertices; std::vector<uint32_t> indices; std::map<uint32_t, uint32_t> remap; };
  std::map<uint32_t, Build> groups;
  const float* m = d.uv_matrix;
  for (uint32_t i = d.start; i + 2 < d.start + d.count && i + 2 < model.indices.size(); i += 3) {
    // Winding is reversed: the game's world is right-handed and Remix's view is left-handed,
    // so a triangle that faces the camera in the engine would otherwise face away in Remix.
    uint32_t tri[3] = { model.indices[i], model.indices[i + 2], model.indices[i + 1] };
    if (tri[0] >= model.vertices.size() || tri[1] >= model.vertices.size() || tri[2] >= model.vertices.size()) continue;
    Build& b = groups[model.vertices[tri[0]].transform_id];
    for (uint32_t vi : tri) {
      auto r = b.remap.find(vi);
      if (r == b.remap.end()) {
        const ModelVertex& src = model.vertices[vi];
        remixapi_HardcodedVertex v = {};
        memcpy(v.position, src.position, sizeof v.position);
        memcpy(v.normal, src.normal, sizeof v.normal);
        if (glow_layer || lit_layer) for (int k = 0; k < 3; ++k) v.position[k] += src.normal[k] * kGlowLift;
        v.texcoord[0] = m[0] * src.uv[0] + m[4] * src.uv[1] + m[8];
        v.texcoord[1] = m[1] * src.uv[0] + m[5] * src.uv[1] + m[9];
        v.color = 0xFFFFFFFF;
        r = b.remap.emplace(vi, (uint32_t)b.vertices.size()).first;
        b.vertices.push_back(v);
      }
      b.indices.push_back(r->second);
    }
  }
  std::vector<Part> parts;
  // The glow layer is the same geometry again, as light added in proportion to the texture's alpha.
  remixapi_MaterialHandle material = lit_layer  ? material_for(tex, false, d.color, true, kLit, (1.0f - g_emissive_light) * object_lit)
                                     : glass      ? glass_material(d.color[3])
                                     : mirror     ? mirror_material(d.color, d.lod_bias)
                                     : see_through ? material_for(tex, false, d.color, self_lit, kSeeThrough)
                                     // (a glow whose texture has no see-through parts is pure added light, not a covering)
                                     : glow_layer ? material_for(tex, false, d.color, true, tex && tex->has_mean && tex->mean_alpha > 0.9f ? kLit : kGlow,
                                                                 screen_strength((float)(uint32_t)(d.glow_strength * kGlowSteps + 0.5f) / kGlowSteps))
                                     : decal    ? material_for(tex, false, d.color, self_lit, kDecal)
                                                : material_for(tex, d.alpha_test, d.color, self_lit, kSurface, self_lit ? g_emissive_light * object_lit : 1.0f,
                                                               polished ? 1 : 0);
  for (auto& kv : groups) {
    Build& b = kv.second;
    remixapi_MeshInfoSurfaceTriangles t = {};
    t.vertices_values = b.vertices.data();
    t.vertices_count = b.vertices.size();
    t.indices_values = b.indices.data();
    t.indices_count = b.indices.size();
    t.material = material;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = fnv1a64(&kv.first, sizeof kv.first, key);
    info.surfaces_values = &t;
    info.surfaces_count = 1;
    remixapi_MeshHandle mesh = nullptr;
    if (g_api.CreateMesh(&info, &mesh) == REMIXAPI_ERROR_CODE_SUCCESS && mesh) {
      Part part = { kv.first, mesh };
      // Planar if every triangle's own normal (from its corners, not the stored vertex
      // normals) is parallel to the first one's.
      bool have_axis = false;
      part.planar = true;
      for (size_t i = 0; i + 2 < b.indices.size() && part.planar; i += 3) {
        const float* p0 = b.vertices[b.indices[i]].position;
        const float* p1 = b.vertices[b.indices[i + 1]].position;
        const float* p2 = b.vertices[b.indices[i + 2]].position;
        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-12f) continue;   // a degenerate triangle says nothing
        for (float& c : n) c /= len;
        if (!have_axis) { memcpy(part.axis, n, sizeof n); have_axis = true; continue; }
        float dot = n[0] * part.axis[0] + n[1] * part.axis[1] + n[2] * part.axis[2];
        if (dot < 0.98f && dot > -0.98f) part.planar = false;
      }
      if (!have_axis) part.planar = false;
      parts.push_back(part);
    }
  }
  return g_parts.emplace(key, std::move(parts)).first->second;
}

static std::map<uint64_t, remixapi_MeshHandle> g_skinned_meshes;
static size_t g_stat_characters;

// Characters go to Remix as bind-pose meshes with four bone influences per vertex; the
// engine's bone matrices pose them each frame (Remix does the skinning).
static remixapi_MeshHandle skinned_mesh_for(const SkinnedDraw& d) {
  const SkinnedMesh& model = scene_skinned_models()[d.model];
  const auto& textures = scene_textures();
  const SceneTexture* tex = d.texture >= 0 ? &textures[d.texture] : nullptr;
  uint64_t key = fnv1a64(&d.start, sizeof d.start, model.hash ^ 0x534b494eull);
  key = fnv1a64(&d.count, sizeof d.count, key);
  uint64_t tex_hash = tex ? tex->hash : 0;
  key = fnv1a64(&tex_hash, sizeof tex_hash, key);
  auto it = g_skinned_meshes.find(key);
  if (it != g_skinned_meshes.end()) return it->second;

  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices, joints;
  std::vector<float> weights, first_weight;
  size_t more_than_two = 0;
  std::map<uint32_t, uint32_t> remap;
  const float* m = d.uv_matrix;
  for (uint32_t i = d.start; i < d.start + d.count && i < model.indices.size(); ++i) {
    uint32_t k = i - d.start;
    uint32_t src_i = d.start + k - k % 3 + (k % 3 == 1 ? 2 : k % 3 == 2 ? 1 : 0);   // reversed winding
    if (src_i >= model.indices.size()) break;
    uint32_t vi = model.indices[src_i];
    if (vi >= model.vertices.size()) { indices.clear(); break; }
    auto r = remap.find(vi);
    if (r == remap.end()) {
      const SkinnedVertex& src = model.vertices[vi];
      remixapi_HardcodedVertex v = {};
      memcpy(v.position, src.position, sizeof v.position);
      memcpy(v.normal, src.normal, sizeof v.normal);
      v.texcoord[0] = m[0] * src.uv[0] + m[4] * src.uv[1] + m[8];
      v.texcoord[1] = m[1] * src.uv[0] + m[5] * src.uv[1] + m[9];
      v.color = 0xFFFFFFFF;
      r = remap.emplace(vi, (uint32_t)vertices.size()).first;
      vertices.push_back(v);
      // Two strongest influences, renormalised (see the note on the weight layout below).
      int a = 0, b = -1;
      for (int k = 1; k < 4; ++k) if (src.weights[k] > src.weights[a]) a = k;
      for (int k = 0; k < 4; ++k) if (k != a && src.weights[k] && (b < 0 || src.weights[k] > src.weights[b])) b = k;
      int influences = 0;
      for (int k = 0; k < 4; ++k) influences += src.weights[k] != 0;
      if (influences > 2) ++more_than_two;
      float wa = src.weights[a], wb = b >= 0 ? src.weights[b] : 0.0f;
      joints.push_back(src.joints[a]);
      joints.push_back(b >= 0 ? src.joints[b] : src.joints[a]);
      first_weight.push_back(wa + wb > 0.0f ? wa / (wa + wb) : 1.0f);
    }
    indices.push_back(r->second);
  }
  // Remix 1.6.1 reads vertex i's explicit weights starting at float i of the array (its
  // weight buffer has a stride of one float whatever the bone count), which only works
  // out for two bones per vertex: one explicit weight each, the second is the remainder.
  // So the array is laid out as one weight per vertex, padded to the size the API expects.
  weights = first_weight;
  weights.resize(vertices.size() * 2, 0.0f);
  if (more_than_two)
    logf("remix: skinned mesh %016llx: %zu of %zu vertices have more than two bone influences and were reduced to two", (unsigned long long)key,
         more_than_two, vertices.size());
  remixapi_MeshHandle mesh = nullptr;
  if (indices.size() >= 3) {
    remixapi_MeshInfoSurfaceTriangles t = {};
    t.vertices_values = vertices.data();
    t.vertices_count = vertices.size();
    t.indices_values = indices.data();
    t.indices_count = indices.size() - indices.size() % 3;
    t.skinning_hasvalue = 1;
    t.skinning_value.bonesPerVertex = 2;
    t.skinning_value.blendWeights_values = weights.data();
    t.skinning_value.blendWeights_count = (uint32_t)weights.size();
    t.skinning_value.blendIndices_values = joints.data();
    t.skinning_value.blendIndices_count = (uint32_t)joints.size();
    t.material = material_for(tex, d.alpha_test, d.color);
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = key;
    info.surfaces_values = &t;
    info.surfaces_count = 1;
    if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) mesh = nullptr;
  }
  g_skinned_meshes[key] = mesh;
  return mesh;
}

static void submit_characters() {
  const auto& draws = scene_frame_skinned();
  const auto& bones = scene_frame_bones();
  g_stat_characters = 0;
  for (const SkinnedDraw& d : draws) {
    if (d.blended) continue;
    remixapi_MeshHandle mesh = skinned_mesh_for(d);
    if (!mesh || d.bone_count == 0 || d.bone_count > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) continue;
    static_assert(sizeof(remixapi_Transform) == 12 * sizeof(float), "bone matrices are passed through as-is");
    remixapi_InstanceInfoBoneTransformsEXT ext = {};
    ext.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    ext.boneTransforms_values = reinterpret_cast<const remixapi_Transform*>(&bones[(size_t)d.bone_offset * 12]);
    ext.boneTransforms_count = d.bone_count;
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.pNext = &ext;
    inst.mesh = mesh;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.doubleSided = 1;
    g_api.DrawInstance(&inst);
    ++g_stat_characters;
  }
}

// One object draw to Remix. `nodes` points at its node matrices (16 floats each, column-major).
static void submit_object(const ObjectDraw& d, const float* nodes) {
  // A blended draw whose material is fully opaque (alpha 1) is a sign, a stain or a
  // blood pool: a decal over whatever is behind it.
  bool opaque_decal = d.alpha_blend && d.color[3] >= 0.99f;
  // See-through surfaces (alpha-blended with a material alpha below 1) keep the engine's
  // own blend and opacity; one lying flat on a surface (a stain, a floor's sheen) is a
  // decal. With Glass=1 the upright ones become thin glass instead.
  bool see_through = g_see_through && d.alpha_blend && !opaque_decal && d.color[3] > 0.02f;
  // Anything else blended (additive passes that are not a glow layer) is left out.
  if (d.blended && !opaque_decal && !see_through) { ++g_stat_translucent; return; }
  remixapi_InstanceInfoBlendEXT blend = {};
  if (see_through) see_through_blend(d.color, &blend);
  auto place = [&](const Part& part, remixapi_InstanceInfo* inst) {
    const float* m = nodes + (size_t)part.transform_id * 16;
    inst->sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst->mesh = part.mesh;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) inst->transform.matrix[r][c] = m[c * 4 + r];
    return m;
  };
  for (const Part& part : parts_for(d, false, see_through && g_glass && !d.unshaded)) {
    if (part.transform_id >= d.node_count) continue;
    remixapi_InstanceInfo inst = {};
    const float* m = place(part, &inst);
    bool lying_flat = false;
    if (see_through && part.planar) {   // the node may scale unevenly, so the turned normal is measured
      const float* ax = part.axis;
      float w[3] = { m[0] * ax[0] + m[4] * ax[1] + m[8] * ax[2], m[1] * ax[0] + m[5] * ax[1] + m[9] * ax[2],
                     m[2] * ax[0] + m[6] * ax[1] + m[10] * ax[2] };
      float len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
      lying_flat = len > 0.0f && fabsf(w[2]) > 0.9f * len;
    }
    bool glass = see_through && g_glass && !d.unshaded;
    if (glass && lying_flat) continue;   // as glass a flat sheet would be a mirror patch with a hard edge
    if (see_through) ++g_stat_see_through;
    // Objects keep the engine's culling: the player can stand inside one (a booth, a
    // lift) and must see out through its back faces, as in the engine.
    inst.doubleSided = d.two_sided || g_force_two_sided || see_through ? 1 : 0;
    if (opaque_decal || (see_through && lying_flat)) inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
    if (see_through && !glass) inst.pNext = &blend;
    g_api.DrawInstance(&inst);
    ++g_stat_instances;
  }
  if ((d.unshaded || d.illumination >= 0.5f) && !d.blended && g_emissive_light < 1.0f) {
    for (const Part& part : parts_for(d, false, false, true)) {
      if (part.transform_id >= d.node_count) continue;
      remixapi_InstanceInfo inst = {};
      place(part, &inst);
      inst.doubleSided = d.two_sided || g_force_two_sided ? 1 : 0;
      g_api.DrawInstance(&inst);
    }
  }
  if (d.glow && g_glow_layers) {
    for (const Part& part : parts_for(d, true)) {
      if (part.transform_id >= d.node_count) continue;
      remixapi_InstanceInfo inst = {};
      place(part, &inst);
      inst.doubleSided = d.two_sided || g_force_two_sided ? 1 : 0;
      g_api.DrawInstance(&inst);
      ++g_stat_glow;
    }
  }
}

// A sphere round a model in its own space, for the test against the view.
static void model_bounds(int model, float centre[3], float* radius) {
  static std::vector<std::array<float, 4>> cache;
  if ((size_t)model >= cache.size()) cache.resize((size_t)model + 1, std::array<float, 4>{ 0, 0, 0, -1 });
  std::array<float, 4>& c = cache[model];
  if (c[3] < 0) {
    const ModelMesh& mesh = scene_models()[model];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const auto& v : mesh.vertices)
      for (int k = 0; k < 3; ++k) { if (v.position[k] < lo[k]) lo[k] = v.position[k]; if (v.position[k] > hi[k]) hi[k] = v.position[k]; }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) { c[k] = mesh.vertices.empty() ? 0 : (lo[k] + hi[k]) * 0.5f; float h = (hi[k] - lo[k]) * 0.5f; r2 += h * h; }
    c[3] = mesh.vertices.empty() ? 0 : sqrtf(r2);
  }
  memcpy(centre, c.data(), 12);
  *radius = c[3];
}

// True when the sphere lies wholly inside the camera's view (its four side planes).
static bool wholly_in_view(const float centre[3], float radius) {
  const Camera& cam = scene_camera();
  float m[16];   // projection * view, column-major
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += cam.proj[k * 4 + r] * cam.view[c * 4 + k];
      m[c * 4 + r] = sum;
    }
  auto row = [&](int r, float out[4]) { for (int c = 0; c < 4; ++c) out[c] = m[c * 4 + r]; };
  float x[4], y[4], w[4];
  row(0, x); row(1, y); row(3, w);
  for (int side = 0; side < 4; ++side) {
    const float* axis = side < 2 ? x : y;
    float sign = side % 2 ? -1.0f : 1.0f;
    float plane[4];
    for (int k = 0; k < 4; ++k) plane[k] = w[k] + sign * axis[k];
    float len = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (!(len > 0.0f)) return false;
    float distance = (plane[0] * centre[0] + plane[1] * centre[1] + plane[2] * centre[2] + plane[3]) / len;
    if (distance < radius) return false;
  }
  return true;
}

static void submit_objects() {
  const auto& objects = scene_frame_objects();
  const auto& nodes = scene_frame_nodes();
  g_stat_instances = 0;
  g_stat_translucent = 0;
  g_stat_see_through = 0;
  g_stat_glow = 0;
  g_stat_kept = 0;
  for (const ObjectDraw& d : objects) {
    const float* own = &nodes[(size_t)d.node_offset * 16];
    submit_object(d, own);
    if (!g_keep_objects) continue;
    size_t node_bytes = (size_t)d.node_count * 16 * sizeof(float);
    int ids[4] = { d.model, (int)d.start, (int)d.count, d.texture };
    uint64_t shape = fnv1a64(ids, sizeof ids);
    uint64_t key = fnv1a64(own, node_bytes, shape);
    auto found = g_kept.find(key);
    if (found != g_kept.end()) {
      KeptObject& k = found->second;
      k.steady = k.seen + 1 == g_frames ? k.steady + 1 : 1;
      k.seen = g_frames;
      k.draw = d;   // colours and glow may change while it stands still
      continue;
    }
    if (g_kept.size() > 6000) continue;
    KeptObject k;
    k.draw = d;
    k.nodes.assign(own, own + (size_t)d.node_count * 16);
    k.seen = g_frames;
    k.steady = 1;
    k.shape = shape;
    float local[3], r;
    model_bounds(d.model, local, &r);
    for (int i = 0; i < 3; ++i) k.centre[i] = own[i] * local[0] + own[4 + i] * local[1] + own[8 + i] * local[2] + own[12 + i];
    float scale = sqrtf(own[0] * own[0] + own[1] * own[1] + own[2] * own[2]);
    k.radius = r * (scale > 1.0f ? scale : 1.0f);
    // The same geometry close by, not drawn this frame, is this object before it moved.
    for (auto it = g_kept.begin(); it != g_kept.end();) {
      const KeptObject& o = it->second;
      float dx = o.centre[0] - k.centre[0], dy = o.centre[1] - k.centre[1], dz = o.centre[2] - k.centre[2];
      float reach = o.radius + k.radius + 0.5f;
      if (o.shape == shape && o.seen != g_frames && dx * dx + dy * dy + dz * dz < reach * reach) it = g_kept.erase(it);
      else ++it;
    }
    g_kept.emplace(key, std::move(k));
  }
  if (!g_keep_objects) return;
  for (auto it = g_kept.begin(); it != g_kept.end();) {
    KeptObject& k = it->second;
    if (k.seen == g_frames) { ++it; continue; }
    // Never stood still (it was moving, or just passing), or wholly in view and yet not drawn: let it go.
    if (k.steady < 20 || wholly_in_view(k.centre, k.radius)) { it = g_kept.erase(it); continue; }
    k.draw.node_offset = 0;
    submit_object(k.draw, k.nodes.data());
    ++g_stat_kept;
    ++it;
  }
}

static void take_shot(double seconds) {
  if (!g_shot_surf) {   // first use, or released by a resize
    if (g_shot_sys) { g_shot_sys->Release(); g_shot_sys = nullptr; }
    g_dev->CreateTexture(g_width, g_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_shot_tex, nullptr);
    if (g_shot_tex) g_shot_tex->GetSurfaceLevel(0, &g_shot_surf);
    g_dev->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_shot_sys, nullptr);
  }
  if (!g_shot_surf || !g_shot_sys) return;
  if (g_api.dxvk_CopyRenderingOutput(g_shot_surf, REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR) != REMIXAPI_ERROR_CODE_SUCCESS) return;
  if (FAILED(g_dev->GetRenderTargetData(g_shot_surf, g_shot_sys))) return;
  D3DLOCKED_RECT lr = {};
  if (FAILED(g_shot_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
  wchar_t name[64];
  swprintf(name, 64, L"remix-%d.bmp", g_shot_index++);
  bool ok = save_bmp(output_dir() + name, static_cast<const uint8_t*>(lr.pBits), (int)g_width, (int)g_height, lr.Pitch);
  g_shot_sys->UnlockRect();
  logf("remix: readback shot %ls at %.1fs (frame %llu, light scale %.1f) %s", name, seconds, (unsigned long long)g_frames, g_light_scale,
       ok ? "saved" : "FAILED");
  if (!g_light_sweep.empty()) {   // next shot uses the next scale: drop the lights so they are recreated
    g_light_scale = g_light_sweep.front();
    g_light_sweep.erase(g_light_sweep.begin());
    for (auto h : g_light_handles) if (h) g_api.DestroyLight(h);
    g_light_handles.clear();
  }
}

static bool ensure_started() {
  if (g_running) return true;
  if (g_tried_start || !scene_world().loaded || !scene_camera().valid) return false;
  g_tried_start = true;
  g_running = start();
  if (g_running) {
    g_composite_wanted = config_flag(L"Composite");
    g_split_screen = config_flag(L"Split");
    logf("remix: compositing into the game frame is %s", g_composite_wanted ? "ON" : "off (viewer only)");
  }
  return g_running;
}

// Drops every mesh and material so the next frame rebuilds them (used after the material
// override table changes). Costs one slow frame.
static void forget_scene_objects() {
  if (!g_running) return;
  if (g_level_mesh) { g_api.DestroyMesh(g_level_mesh); g_level_mesh = nullptr; }
  if (g_level_overlay_mesh) { g_api.DestroyMesh(g_level_overlay_mesh); g_level_overlay_mesh = nullptr; }
  if (g_level_lit_mesh) { g_api.DestroyMesh(g_level_lit_mesh); g_level_lit_mesh = nullptr; }
  if (g_sky_mesh) { g_api.DestroyMesh(g_sky_mesh); g_sky_mesh = nullptr; }
  g_sky_built = ~0u;
  drop_animated_meshes();
  g_level_built_revision = ~0u;
  g_level_built_at = -1e9;
  for (auto& kv : g_parts) for (auto& part : kv.second) if (part.mesh) g_api.DestroyMesh(part.mesh);
  g_parts.clear();
  for (auto& kv : g_skinned_meshes) if (kv.second) g_api.DestroyMesh(kv.second);
  g_skinned_meshes.clear();
  for (auto& kv : g_materials) if (kv.second) g_api.DestroyMaterial(kv.second);
  g_materials.clear();
  logf("remix: materials and meshes dropped for rebuild");
}

// Submits the current scene and lets Remix render one frame.
// Follows the engine's ambient level. Materials carry it, so a change means remaking them;
// it is only taken up once it has held for a while (it is a property of the level).
static void follow_ambient() {
  const float* a = scene_ambient();
  // The floor the engine's lightmaps never go under, taken to linear light: a surface that
  // gives off 0.32 of its colour shows at full brightness, so this much shows as the
  // engine's ambient level. (The ambient the engine lights objects with is another number
  // and can be far higher; it is not used for the level.)
  float floor_level = g_baked_floor >= 0.0f ? g_baked_floor : 0.0f;
  float wanted = 0.32f * powf(floor_level, 2.2f) * g_ambient_scale;
  wanted = floorf(wanted * 65536.0f + 0.5f) / 65536.0f;
  static float candidate = -1.0f;
  static int held = 0;
  if (wanted == g_ambient_emissive) { held = 0; return; }
  if (wanted != candidate) { candidate = wanted; held = 0; return; }
  if (++held < 60) return;
  held = 0;
  logf("remix: engine ambient %.3f %.3f %.3f: surfaces now give off %.4f of their colour", a[0], a[1], a[2], wanted);
  g_ambient_emissive = wanted;
  forget_scene_objects();
}

static void render_frame(double seconds) {
  refresh_overrides(seconds);
  follow_ambient();
  const World& w = scene_world();
  bool stale = w.loaded && (w.vb_hash != g_level_built_hash || w.revision != g_level_built_revision);
  if (stale && (seconds - g_level_built_at > 0.25 || w.vb_hash != g_level_built_hash)) build_level(seconds);

  setup_camera();
  scene_animate_lights(seconds);
  // Debug: Skip=<letters> leaves parts of the scene out (l lights, o objects, c characters),
  // to find which of them something on screen comes from.
  static const std::wstring skip = config_text(L"Skip");
  if (skip.find(L'l') == std::wstring::npos) submit_lights(g_camera_pos);
  static const bool only_characters = config_text(L"Only") == L"characters";   // debug: hide everything else
  if (!only_characters && skip.find(L'o') == std::wstring::npos) submit_objects();
  if (skip.find(L'c') == std::wstring::npos) submit_characters();
  if (seconds - g_last_stats > 10.0) {
    g_last_stats = seconds;
    logf("remix: frame %llu: %zu object draws -> %zu instances (%zu see-through) + %zu glow layers (%zu translucent skipped, %zu kept from out of view), %zu character draws -> %zu, %zu models, %zu lights, %zu materials",
         (unsigned long long)g_frames, scene_frame_objects().size(), g_stat_instances, g_stat_see_through, g_stat_glow, g_stat_translucent, g_stat_kept, scene_frame_skinned().size(),
         g_stat_characters, scene_models().size() + scene_skinned_models().size(), scene_lights().size(), g_materials.size());
    if (g_sprites_most) logf("remix: up to %u sprite draws per frame inside the world pass in the last 10 s (not in the path-traced image)", g_sprites_most);
    if (g_stat_sprites_most) logf("remix: up to %zu batches of particles and sparks per frame in the path-traced scene in the last 10 s", g_stat_sprites_most);
    g_stat_sprites_most = 0;
    g_sprites_most = 0;
    const float* amb = scene_ambient();
    logf("remix: camera at (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f); engine ambient %.3f %.3f %.3f, %zu directional and %zu moving lights",
         g_camera_pos.x, g_camera_pos.y, g_camera_pos.z, g_camera_forward.x, g_camera_forward.y, g_camera_forward.z, amb[0], amb[1], amb[2],
         scene_far_lights().size(), scene_dynamic_lights().size());
    if (g_composites)
      logf("remix: %llu frames composited, mean %.2f ms max %.1f ms per frame for render + hand-off, %llu failures", (unsigned long long)g_composites,
           g_composite_ms_total / g_composites, g_composite_ms_max, (unsigned long long)g_composite_failures);
    g_composite_ms_max = 0;
  }
  if (g_level_mesh && !only_characters) {
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = g_level_mesh;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.doubleSided = 1;
    g_api.DrawInstance(&inst);
    if (g_level_overlay_mesh) {
      inst.mesh = g_level_overlay_mesh;
      inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
      g_api.DrawInstance(&inst);
    }
    if (g_level_lit_mesh) {
      inst.mesh = g_level_lit_mesh;
      inst.categoryFlags = 0;
      g_api.DrawInstance(&inst);
    }
    submit_animated_polygons();
    if (g_glow_layers) submit_glow_polygons();
    submit_sky();
    g_sprite_camera = g_camera_pos;
    submit_sprites();
  }
  HRESULT hr = g_dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  if (FAILED(hr) && g_frames < 5) logf("remix: PresentEx -> 0x%08lx", (unsigned long)hr);
  ++g_frames;
  g_rendered_this_frame = true;
  if (!g_shot_at.empty() && seconds >= g_shot_at.front()) {
    g_shot_at.erase(g_shot_at.begin());
    take_shot(seconds);
  }
  // Debug aid: ShotOnCharacters=N saves a readback the first N times characters are being drawn.
  static int auto_shots = (int)config_number(L"ShotOnCharacters", 0);
  static double last_auto_shot = -1e9;
  if (auto_shots > 0 && g_stat_characters > 0 && seconds - last_auto_shot > 3.0) {
    --auto_shots;
    last_auto_shot = seconds;
    take_shot(seconds);
  }
}

static void release_shared() {
  if (g_d9_shared_surf) { g_d9_shared_surf->Release(); g_d9_shared_surf = nullptr; }
  if (g_d9_shared) { g_d9_shared->Release(); g_d9_shared = nullptr; }
  if (g_d11_shared) { g_d11_shared->Release(); g_d11_shared = nullptr; }
  g_shared_w = g_shared_h = 0;
}

static bool ensure_shared(ID3D11Device* dev, unsigned w, unsigned h) {
  if (g_d11_shared && g_shared_w == w && g_shared_h == h) return true;
  release_shared();
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &g_d11_shared);
  HANDLE handle = nullptr;
  if (SUCCEEDED(hr)) {
    IDXGIResource* res = nullptr;
    hr = g_d11_shared->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    if (SUCCEEDED(hr)) { hr = res->GetSharedHandle(&handle); res->Release(); }
  }
  if (FAILED(hr) || !handle) { logf("remix: shared texture %ux%u on D3D11 failed 0x%08lx", w, h, (unsigned long)hr); release_shared(); return false; }
  hr = g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8B8G8R8, D3DPOOL_DEFAULT, &g_d9_shared, &handle);
  if (SUCCEEDED(hr)) hr = g_d9_shared->GetSurfaceLevel(0, &g_d9_shared_surf);
  if (SUCCEEDED(hr) && !g_event_query) hr = g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_event_query);
  if (FAILED(hr)) { logf("remix: importing the shared texture into Remix failed 0x%08lx", (unsigned long)hr); release_shared(); return false; }
  g_shared_w = w; g_shared_h = h;
  logf("remix: hand-off texture %ux%u ready", w, h);
  return true;
}

static bool wait_remix_gpu(double timeout_ms) {
  g_event_query->Issue(D3DISSUE_END);
  BOOL done = FALSE;
  double t0 = now_ms();
  for (;;) {
    HRESULT hr = g_event_query->GetData(&done, sizeof done, D3DGETDATA_FLUSH);
    if (hr == S_OK) return true;
    if (hr != S_FALSE || now_ms() - t0 > timeout_ms) return false;
    YieldProcessor();
  }
}

static double g_present_seconds;   // the frame clock handed in at Present
static double g_settle_until;      // after path tracing is switched back on: when its image is shown again

bool remix_link_before_draw(ID3D11DeviceContext* ctx, const ShaderRecord* vs, const ShaderRecord* ps) {
  if (g_running && g_enabled && !g_sprites && vs && vs->cls == ShaderClass::WorldSpace && !g_composited_this_frame && scene_world_target()) ++g_sprites_this_frame;
  if (!g_running || !g_composite_wanted || !g_enabled || !vs || vs->cls != ShaderClass::Post) return false;
  ID3D11Texture2D* target = scene_world_target();
  if (!target) return false;   // no world rendered this frame (menu, video)
  if (!g_composited_this_frame) {
    // First post-process draw after the engine's opaque world pass: the scene for this
    // frame is complete. Render it with Remix and replace the engine's world image.
    g_composited_this_frame = true;
    double t0 = now_ms();
    D3D11_TEXTURE2D_DESC td;
    target->GetDesc(&td);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = dev && td.SampleDesc.Count == 1 && ensure_shared(dev, td.Width, td.Height);
    if (dev) dev->Release();
    if (ok) {
      if (td.Width != g_width || td.Height != g_height) {
        // Remix renders at its back-buffer size; follow the game's world target.
        g_width = td.Width; g_height = td.Height;
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = g_width; pp.BackBufferHeight = g_height;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = g_hwnd;
        pp.Windowed = TRUE;
        if (g_event_query) { g_event_query->Release(); g_event_query = nullptr; }
        if (g_shot_surf) { g_shot_surf->Release(); g_shot_surf = nullptr; }
        if (g_shot_tex) { g_shot_tex->Release(); g_shot_tex = nullptr; }
        release_shared();
        HRESULT hr = g_dev->ResetEx(&pp, nullptr);
        logf("remix: output resized to %ux%u -> 0x%08lx", g_width, g_height, (unsigned long)hr);
        ID3D11Device* d2 = nullptr;
        ctx->GetDevice(&d2);
        ok = SUCCEEDED(hr) && d2 && ensure_shared(d2, td.Width, td.Height);
        if (d2) d2->Release();
      }
    }
    if (ok) {
      render_frame(g_present_seconds);
      ok = g_api.dxvk_CopyRenderingOutput(g_d9_shared_surf, REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR) == REMIXAPI_ERROR_CODE_SUCCESS &&
           wait_remix_gpu(g_composites < 3 ? 8000 : 2000);   // Remix's first frames of a level build its acceleration structures
      // Just switched back on: Remix renders, but the engine's image stays up until its
      // lighting has settled again (about a second of a dim picture otherwise).
      if (ok && g_present_seconds < g_settle_until) {
      } else if (ok && g_split_screen) {
        D3D11_BOX right_half = { td.Width / 2, 0, 0, td.Width, td.Height, 1 };
        ctx->CopySubresourceRegion(target, 0, td.Width / 2, 0, 0, g_d11_shared, 0, &right_half);
      } else if (ok) {
        ctx->CopyResource(target, g_d11_shared);
      }
    }
    if (ok) {
      double ms = now_ms() - t0;
      ++g_composites;
      g_composite_ms_total += ms;
      g_recent_ms = g_recent_ms > 0.0 ? g_recent_ms * 0.95 + ms * 0.05 : ms;
      if (ms > g_composite_ms_max) g_composite_ms_max = ms;
    } else if (g_composite_failures++ < 5) {
      logf("remix: composite failed this frame (target %ux%u format %d samples %u)", td.Width, td.Height, (int)td.Format, td.SampleDesc.Count);
    }
  }
  // Ambient occlusion on top of a path-traced image would darken it twice. (In split-screen
  // mode the engine half keeps its ambient occlusion, so the passes run.)
  return !g_split_screen && g_present_seconds >= g_settle_until && ps && ps->name.compare(0, 5, "SSAO_") == 0;
}

void remix_link_set_enabled(bool enabled) {
  if (enabled != g_enabled) logf("remix: path-traced image %s", enabled ? "ON" : "OFF (engine rendering shown)");
  if (enabled && !g_enabled) g_settle_until = g_present_seconds + 1.5;
  g_enabled = enabled;
}

bool remix_link_enabled() { return g_enabled; }
bool remix_link_started() { return g_tried_start; }

void remix_link_mark(double seconds) {
  static int marks;
  const World& w = scene_world();
  unsigned dark = 0, changing = 0;
  for (const SceneLight& l : scene_lights()) {
    changing += l.anim_mode >= 0;
    dark += l.has_live && !(l.live[0] > 0.0f || l.live[1] > 0.0f || l.live[2] > 0.0f);
  }
  logf("MARK %d at %.1fs: camera (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f); %zu object instances, %zu of them kept from out of view, %zu characters; "
       "%zu lights (%u changing on their own, %u dark now), %zu moving lights; level %016llx, %zu polygons seen, %zu with swapping textures, %zu glowing",
       ++marks, seconds, g_camera_pos.x, g_camera_pos.y, g_camera_pos.z, g_camera_forward.x, g_camera_forward.y, g_camera_forward.z,
       g_stat_instances, g_stat_kept, g_stat_characters, scene_lights().size(), changing, dark, scene_dynamic_lights().size(),
       (unsigned long long)w.vb_hash, w.ranges.size(), w.animated.size(), w.glowing.size());
  for (const SceneLight& l : scene_dynamic_lights())
    logf("MARK   moving light at (%.1f, %.1f, %.1f) colour %.2f %.2f %.2f reach %.1f", l.position[0], l.position[1], l.position[2], l.color[0],
         l.color[1], l.color[2], l.radius);
  g_shot_at.insert(g_shot_at.begin(), seconds);
}

void remix_link_toggle_glow() {
  g_glow_layers = !g_glow_layers;
  g_level_built_revision = ~0u;
  logf("remix: glow layers %s", g_glow_layers ? "ON" : "OFF");
}

// Raw-input registrations are per process: one made by Remix replaces or overrides the
// game's own. Logged once Remix has settled, and again if it changes.
static void log_raw_input(double seconds) {
  static double next_check = 0;
  static std::string last;
  if (!g_running || seconds < next_check) return;
  next_check = seconds + 5.0;
  RAWINPUTDEVICE devices[16];
  UINT count = 16;
  UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
  std::string now;
  if (got == (UINT)-1) {
    now = "query failed";
  } else {
    char item[96];
    for (UINT i = 0; i < got; ++i) {
      const char* what = devices[i].usUsagePage == 1 && devices[i].usUsage == 2 ? "mouse" : devices[i].usUsagePage == 1 && devices[i].usUsage == 6 ? "keyboard" : "other";
      snprintf(item, sizeof item, "%s%s flags 0x%lx%s target %p", i ? "; " : "", what, (unsigned long)devices[i].dwFlags,
               (devices[i].dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY && devices[i].usUsage == 6 ? " (NO LEGACY KEY MESSAGES)" : "", (void*)devices[i].hwndTarget);
      now += item;
    }
    if (got == 0) now = "none";
  }
  if (now != last) { last = now; logf("input: raw input registrations: %s", now.c_str()); }
  // Which DLSS components Remix has actually loaded says more than its options do.
  static std::string last_ngx;
  std::string ngx;
  static const struct { const wchar_t* dll; const char* what; } parts[] = {
    { L"nvngx_dlss.dll", "super resolution" }, { L"nvngx_dlssd.dll", "ray reconstruction" }, { L"nvngx_dlssnr.dll", "neural rendering" },
    { L"nvngx_dlssg.dll", "frame generation" }, { L"libxess.dll", "XeSS" },
  };
  for (const auto& part : parts) if (GetModuleHandleW(part.dll)) { if (!ngx.empty()) ngx += ", "; ngx += part.what; }
  if (ngx.empty()) ngx = "none";
  if (ngx != last_ngx) { last_ngx = ngx; logf("remix: DLSS components loaded: %s", ngx.c_str()); }
}

void remix_link_frame(double seconds) {
  g_present_seconds = seconds;
  log_raw_input(seconds);
  if (ensure_started() && !g_rendered_this_frame && g_enabled && !g_composite_wanted) render_frame(seconds);   // viewer mode
  g_composited_this_frame = false;
  g_rendered_this_frame = false;
  if (g_sprites_this_frame > g_sprites_most) g_sprites_most = g_sprites_this_frame;
  g_sprites_this_frame = 0;
}

// ---- settings panel ----------------------------------------------------------------------
//
// What the in-game panel (menu.cpp) shows. Each row is a heading, a choice or a number.
// A row has a key in ss2rtx.ini where its value is kept, and either a Remix option it
// drives or a function that applies it to the adapter.

struct Setting {
  enum Kind { kHeading, kChoice, kNumber } kind;
  const wchar_t* label;
  const wchar_t* ini;                      // key in ss2rtx.ini, or null if it is not kept
  const char* option;                      // Remix option it sets, or null
  std::vector<const wchar_t*> names;       // choices as shown
  std::vector<const char*> values;         // choices as stored and as given to Remix
  double low, high, step;                  // numbers
  int decimals;
  void (*apply)(Setting&);                 // for rows that drive the adapter itself
  int choice;
  double number;
  bool touched;                            // changed in the panel this session
};

static void lights_restated() { for (auto& c : g_light_built_color) c.x = -1.0f; }

static std::string setting_value(const Setting& s) {
  if (s.kind == Setting::kChoice) return s.values[(size_t)s.choice];
  char buf[32];
  snprintf(buf, sizeof buf, "%.*f", s.decimals, s.number);
  return buf;
}

// DLSS mode "auto": full resolution up to 1200 lines of output, Remix's own choice above.
static void apply_dlss(Setting& s) {
  const char* v = s.values[(size_t)s.choice];
  if (!strcmp(v, "conf")) return;
  if (!strcmp(v, "auto")) v = g_height <= 1200 ? "5" : "4";
  if (g_running || g_dev) set_remix_option("rtx.qualityDLSS", v);
}

// The picture rows. "Look" is a shorthand that fills in the three rows under it; those can
// be set on their own, which makes the look "custom".
//   tone mapping: off (what the path tracer computes goes to the screen as it is, which is
//     what matches the engine's brightness), Remix's local tone mapper (filmic, lifts
//     shadows and holds highlights area by area), Remix's global curve (fits the whole
//     picture's range, so it changes with the view). (A plain filmic curve on its own is
//     not to be had: the runtime's ACES pass only runs inside its tone mappers.)
//   auto exposure: Remix adapts brightness to what is on screen.
//   exposure: in stops. With auto exposure off, 1.65 is the engine's brightness whichever
//     tone mapping is chosen: each tone mapper needs its own value for the same mid-grey,
//     and the offsets below (measured against the engine on two views) hide that.
static Setting *g_look_row, *g_tone_row, *g_auto_row, *g_exposure_row;
// Measured on a dark and a bright view (mean level, engine 21.5 and 74.9): the local tone
// mapper at 3.0 gives 17.6 and 88.1, the nearest one value gets to both, since it adds
// contrast; the global curve at the plain 1.65 stops gives 22.2 and 72.4.
static const float kLocalFixedExposure = 3.0f;    // rtx.localtonemap.exposure for the engine's brightness at 1.65 stops

static void apply_picture() {
  if (!(g_running || g_dev) || !g_tone_row || !g_auto_row || !g_exposure_row) return;
  const char* tone = g_tone_row->values[(size_t)g_tone_row->choice];
  bool local = !strcmp(tone, "local"), global = !strcmp(tone, "global");
  bool automatic = g_auto_row->choice == 1;
  float stops = (float)g_exposure_row->number;
  char buf[32];
  set_remix_option("rtx.tonemappingMode", local ? "1" : "0");
  set_remix_option("rtx.tonemap.tonemappingEnabled", global ? "True" : "False");
  set_remix_option("rtx.autoExposure.enabled", automatic ? "True" : "False");
  snprintf(buf, sizeof buf, "%.3f", stops);
  set_remix_option("rtx.tonemap.exposureBias", buf);
  snprintf(buf, sizeof buf, "%.4f", automatic ? 0.75f * powf(2.0f, stops) : kLocalFixedExposure * powf(2.0f, stops - 1.65f));
  set_remix_option("rtx.localtonemap.exposure", buf);
}

static std::string setting_value(const Setting& s);

static void store_row(Setting* row) {
  row->touched = true;
  if (!row->ini) return;
  std::string v = setting_value(*row);
  config_set(row->ini, std::wstring(v.begin(), v.end()));
}

// 0 engine's brightness, 1 the same with Remix's tone mapping, 2 Remix's own, 3 custom.
static void apply_look(Setting& s) {
  if (g_tone_row && g_auto_row && g_exposure_row && s.choice < 3) {
    g_tone_row->choice = s.choice == 0 ? 0 : 1;
    g_auto_row->choice = s.choice == 2 ? 1 : 0;
    g_exposure_row->number = s.choice == 2 ? 0.0 : 1.65;
    if (s.touched) { store_row(g_tone_row); store_row(g_auto_row); store_row(g_exposure_row); }
  }
  apply_picture();
}

// One of the rows under "Look" was set by hand.
static void apply_picture_row(Setting& s) {
  if (s.touched && g_look_row && g_look_row->choice != 3) { g_look_row->choice = 3; store_row(g_look_row); }
  if (s.touched && &s == g_auto_row && g_exposure_row) {   // the exposure means something else with auto exposure: start it at that mode's neutral value
    g_exposure_row->number = s.choice == 1 ? 0.0 : 1.65;
    store_row(g_exposure_row);
  }
  apply_picture();
}

static std::vector<Setting>& settings() {
  static std::vector<Setting> rows;
  if (!rows.empty()) return rows;
  auto heading = [&](const wchar_t* label) { rows.push_back({ Setting::kHeading, label, nullptr, nullptr, {}, {}, 0, 0, 0, 0, nullptr, 0, 0, false }); };
  auto choice = [&](const wchar_t* label, const wchar_t* ini, const char* option, std::vector<const wchar_t*> names, std::vector<const char*> values,
                    int initial, void (*apply)(Setting&) = nullptr) {
    rows.push_back({ Setting::kChoice, label, ini, option, std::move(names), std::move(values), 0, 0, 0, 0, apply, initial, 0, false });
  };
  auto number = [&](const wchar_t* label, const wchar_t* ini, const char* option, double low, double high, double step, int decimals, double initial,
                    void (*apply)(Setting&) = nullptr) {
    rows.push_back({ Setting::kNumber, label, ini, option, {}, {}, low, high, step, decimals, apply, 0, initial, false });
  };
  const std::vector<const wchar_t*> on_off = { L"On", L"Off" };
  const std::vector<const char*> true_false = { "True", "False" };
  const std::vector<const char*> one_zero = { "1", "0" };

  heading(L"PATH TRACING");
  choice(L"Path tracing", nullptr, nullptr, on_off, one_zero, 0, [](Setting& s) { remix_link_set_enabled(s.choice == 0); });

  heading(L"DLSS");
  choice(L"Upscaler", L"Upscaler", "rtx.upscalerType", { L"DLSS", L"XeSS", L"TAA-U", L"NIS", L"None (native)" }, { "1", "4", "3", "2", "0" }, 0);
  choice(L"DLSS mode", L"Dlss", nullptr,
         { L"Auto", L"Ultra performance", L"Performance", L"Balanced", L"Quality", L"Full resolution (DLAA)", L"As rtx.conf says" },
         { "auto", "0", "1", "2", "3", "5", "conf" }, 0, apply_dlss);
  choice(L"Ray reconstruction", L"RayReconstruction", "rtx.enableRayReconstruction", on_off, true_false, 0);
  choice(L"Neural rendering", L"NeuralRendering", "rtx.dlssNeuralRendering.enable", { L"Off", L"On" }, { "False", "True" }, 0);
  choice(L"  model", L"NeuralModel", "rtx.dlssNeuralRendering.model", { L"A", L"B", L"C" }, { "0", "1", "2" }, 0);
  number(L"  intensity", L"NeuralIntensity", "rtx.dlssNeuralRendering.intensity", 0.0, 1.0, 0.05, 2, 1.0);
  number(L"  detail, shadows, materials", L"NeuralStructure", "rtx.dlssNeuralRendering.structuralStrength", 0.0, 1.0, 0.05, 2, 0.7);
  number(L"  tone", L"NeuralTone", "rtx.dlssNeuralRendering.toneStrength", 0.0, 1.0, 0.05, 2, 0.3);

  heading(L"LIGHT TRANSPORT");
  choice(L"Indirect light", L"IndirectMode", "rtx.integrateIndirectMode",
         { L"Radiance cache (SHaRC)", L"Neural radiance cache", L"ReSTIR GI", L"Plain (noisy, reference)" }, { "3", "2", "1", "0" }, 0);
  number(L"Bounces", L"Bounces", "rtx.pathMaxBounces", 1, 12, 1, 0, 4);
  choice(L"Direct light sampling (RTXDI)", L"Rtxdi", "rtx.useRTXDI", on_off, true_false, 0);

  heading(L"PICTURE");
  choice(L"Look", L"Look", nullptr,
         { L"Engine's brightness", L"Engine's brightness, Remix tone mapping", L"Remix (auto exposure, tone mapped)", L"Custom (the three rows below)" },
         { "engine", "engine-tonemapped", "remix", "custom" }, 0, apply_look);
  choice(L"  tone mapping", L"ToneMapping", nullptr, { L"Off", L"Remix local (filmic)", L"Remix global curve (follows the view)" },
         { "off", "local", "global" }, 0, apply_picture_row);
  choice(L"  auto exposure", L"AutoExposure", nullptr, { L"Off", L"On" }, { "0", "1" }, 0, apply_picture_row);
  number(L"  exposure (stops)", L"Exposure", nullptr, -3.0, 5.0, 0.25, 2, 1.65, apply_picture_row);
  choice(L"Bloom (Remix's, on top of the game's own)", L"Bloom", "rtx.bloom.enable", on_off, true_false, 1);
  choice(L"Volumetric light", L"Volumetrics", "rtx.volumetrics.enable", on_off, true_false, 0);
  choice(L"Motion blur", L"MotionBlur", "rtx.postfx.enableMotionBlur", { L"Off", L"On" }, { "False", "True" }, 0);

  heading(L"SCENE");
  number(L"Light brightness", L"LightScale", nullptr, 5, 200, 5, 0, g_light_scale, [](Setting& s) { g_light_scale = (float)s.number; lights_restated(); });
  number(L"Lamp faces", L"Emissive", nullptr, 0.1, 3.0, 0.1, 1, g_emissive_scale,
         [](Setting& s) { g_emissive_scale = (float)s.number; if (s.touched) forget_scene_objects(); });
  number(L"Screens and lit signs", L"ScreenEmissive", nullptr, 0.1, 3.0, 0.1, 1, g_screen_emissive,
         [](Setting& s) { g_screen_emissive = (float)s.number; if (s.touched) forget_scene_objects(); });
  number(L"Light cast by lamps and screens", L"EmissiveLight", nullptr, 0.0, 1.0, 0.05, 2, g_emissive_light,
         [](Setting& s) { g_emissive_light = (float)s.number; if (s.touched) forget_scene_objects(); });
  choice(L"Water", L"Water", nullptr, { L"As the engine textures it", L"Mirror-smooth", L"See-through skin", L"Bends light" }, { "0", "1", "2", "3" }, g_water_mode,
         [](Setting& s) { g_water_mode = s.choice; if (s.touched) forget_scene_objects(); });
  choice(L"Particles and sparks", L"Sprites", nullptr, on_off, one_zero, g_sprites ? 0 : 1, [](Setting& s) { g_sprites = s.choice == 0; });
  choice(L"Fit each light to its room (large rooms as bright as the engine's)", L"FitLights", nullptr, on_off, one_zero, g_fit_lights ? 0 : 1,
         [](Setting& s) { g_fit_lights = s.choice == 0; });
  choice(L"Stand lights off from walls (softer pools)", L"StandOff", nullptr, on_off, one_zero, g_stand_off ? 0 : 1,
         [](Setting& s) { g_stand_off = s.choice == 0; });
  choice(L"Glow layers", L"GlowLayers", nullptr, on_off, one_zero, g_glow_layers ? 0 : 1, [](Setting& s) { g_glow_layers = s.choice == 0; });
  choice(L"Flashing and flickering lights", L"LiveLights", nullptr, on_off, one_zero, g_use_live_lights ? 0 : 1,
         [](Setting& s) { g_use_live_lights = s.choice == 0; });
  choice(L"Keep objects that leave the view", L"KeepObjects", nullptr, on_off, one_zero, g_keep_objects ? 0 : 1,
         [](Setting& s) { g_keep_objects = s.choice == 0; if (!g_keep_objects) g_kept.clear(); });
  choice(L"See-through surfaces", L"SeeThrough", nullptr, on_off, one_zero, g_see_through ? 0 : 1, [](Setting& s) { g_see_through = s.choice == 0; });
  for (Setting& s : rows) {
    if (!s.ini) continue;
    if (!wcscmp(s.ini, L"Look")) g_look_row = &s;
    if (!wcscmp(s.ini, L"ToneMapping")) g_tone_row = &s;
    if (!wcscmp(s.ini, L"AutoExposure")) g_auto_row = &s;
    if (!wcscmp(s.ini, L"Exposure")) g_exposure_row = &s;
  }
  return rows;
}

static void setting_apply(Setting& s) {
  if (s.apply) { s.apply(s); return; }
  if (s.option && (g_running || g_dev)) set_remix_option(s.option, setting_value(s).c_str());
}

// At start: take each row's stored value from ss2rtx.ini, and give Remix the ones that
// were stored or changed. A Remix option nobody has touched is left to rtx.conf.
static void settings_start() {
  bool picture_stored = false;
  for (Setting& s : settings()) {
    if (s.kind == Setting::kHeading) continue;
    bool stored = false;
    if (s.ini) {
      std::wstring v = config_text(s.ini);
      if (!v.empty() && !s.touched) {
        if (s.kind == Setting::kNumber) {
          double n = _wtof(v.c_str());
          if (n >= s.low && n <= s.high) { s.number = n; stored = true; }
        } else {
          for (size_t i = 0; i < s.values.size(); ++i) {
            std::wstring candidate(s.values[i], s.values[i] + strlen(s.values[i]));
            if (v == candidate || (v == L"1" && candidate == L"True") || (v == L"0" && candidate == L"False")) { s.choice = (int)i; stored = true; break; }
          }
        }
      }
    }
    bool is_dlss = s.ini && !wcscmp(s.ini, L"Dlss");   // "auto" still means something when nothing is stored
    bool picture = &s == g_look_row || &s == g_tone_row || &s == g_auto_row || &s == g_exposure_row;
    if (picture) {
      // The look first fills in its three rows; each of those that is stored itself then
      // overrides it. All of it goes to Remix once, below, and only if something is stored:
      // otherwise rtx.conf has the say.
      if (stored || s.touched) picture_stored = true;
      if (&s == g_look_row && (stored || s.touched) && s.choice < 3 && g_tone_row && g_auto_row && g_exposure_row) {
        g_tone_row->choice = s.choice == 0 ? 0 : 1;
        g_auto_row->choice = s.choice == 2 ? 1 : 0;
        g_exposure_row->number = s.choice == 2 ? 0.0 : 1.65;
      }
      continue;
    }
    if (s.option ? (stored || s.touched) : (is_dlss || s.touched)) setting_apply(s);
  }
  if (picture_stored) apply_picture();
}

int remix_settings_count() { return (int)settings().size(); }

bool remix_settings_row(int i, std::wstring* label, std::wstring* value) {
  auto& rows = settings();
  if (i < 0 || (size_t)i >= rows.size()) return true;
  const Setting& s = rows[(size_t)i];
  *label = s.label;
  value->clear();
  if (s.kind == Setting::kHeading) return true;
  if (s.kind == Setting::kChoice) {
    if (!s.ini && !s.option && s.apply) const_cast<Setting&>(s).choice = g_enabled ? 0 : 1;   // the path tracing switch follows its key too
    *value = s.names[(size_t)s.choice];
  } else {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.*f", s.decimals, s.number);
    *value = buf;
  }
  return false;
}

void remix_settings_step(int i, int direction) {
  auto& rows = settings();
  if (i < 0 || (size_t)i >= rows.size() || rows[(size_t)i].kind == Setting::kHeading) return;
  Setting& s = rows[(size_t)i];
  if (s.kind == Setting::kChoice) {
    int n = (int)s.names.size();
    s.choice = ((s.choice + (direction > 0 ? 1 : -1)) % n + n) % n;
  } else {
    double v = s.number + (direction > 0 ? s.step : -s.step);
    v = floor(v / s.step + 0.5) * s.step;
    s.number = v < s.low ? s.low : v > s.high ? s.high : v;
  }
  s.touched = true;
  setting_apply(s);
  std::string v = setting_value(s);
  if (s.ini) config_set(s.ini, std::wstring(v.begin(), v.end()));
  logf("settings: %ls = %s", s.label, v.c_str());
}

std::wstring remix_settings_status() {
  wchar_t buf[160];
  if (!g_running) return L"Path tracing starts when a level is on screen";
  if (!g_enabled) return L"Path tracing is off: the engine's own picture";
  swprintf(buf, 160, L"Path tracing %.1f ms a frame at %ux%u", g_recent_ms, g_width, g_height);
  return buf;
}

}  // namespace ss2rtx
