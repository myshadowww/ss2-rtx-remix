// Scene capture: turns the game's D3D11 draws into a renderer-independent description
// (level geometry, textures, camera) that remix_link.cpp submits to Remix.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace ss2rtx {

struct SceneTexture {
  ID3D11Texture2D* tex = nullptr;  // holds a reference so the pointer stays unique
  uint64_t hash = 0;
  std::wstring path;               // exported .dds, empty if export failed
  UINT width = 0, height = 0;
  bool solid = false;              // one flat colour: use it as a constant instead of a texture
  float solid_rgba[4] = { 1, 1, 1, 1 };
  bool uniform = false;            // blank: one transparent block repeated (an empty layer)
};

// One world polygon as the engine draws it: a run of indices with one texture.
struct WorldRange {
  uint32_t start = 0, count = 0;
  int texture = -1;        // index into scene_textures(), -1 if none
  bool alpha_test = false;
  float color[4] = { 1, 1, 1, 1 };   // DarkMaterialConstants.Color
  float scale[2] = { 1, 1 };         // DarkNewParams.uTerrainScale at the time it was drawn
  float uv_matrix[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
  // Some level surfaces are drawn in two layers: this one, then a second texture
  // alpha-blended over it (panelling over a blank base, lettering over a wall).
  bool unshaded = false;             // DarkMaterialConstants.Shade == 0: full-bright, a screen or a sign
  float illumination = 0.0f;         // DarkMaterialConstants.Illumination
  bool overlay_unshaded = false;
  // The engine swaps the texture of some polygons while they are on show (screens).
  // Those are kept out of the level mesh and drawn as small meshes of their own.
  bool animated = false;
  const void* last_view = nullptr;   // identity of the texture view last seen; never dereferenced
  uint64_t seen_frame = ~0ull;       // frame of the latest draw, to tell a second pass from a new frame
  uint8_t later_passes_checked = 0;
  int overlay_texture = -1;
  float overlay_scale[2] = { 1, 1 };
  float overlay_uv_matrix[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
  // A full-bright texture the engine adds on top (the writing on a screen, a lamp's glow).
  int glow_texture = -1;
  float glow_strength = 0.0f;        // the pass's colour alpha times its brightest channel, as last drawn
  uint64_t glow_frame = ~0ull;       // frame the glow pass was last drawn in
  float glow_scale[2] = { 1, 1 };
  float glow_uv_matrix[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
};

// Layout of the engine's level vertex buffer (DarkWorldRep.shader), 48 bytes.
struct WorldVertex {
  float position[3];
  float normal[3];
  float uv_unused[2];      // not what the shader samples with; see scene.cpp
  float lightmap_uv[2];
  uint32_t flags;          // low 19 bits: polygon id
  uint32_t flags2;
};

struct World {
  bool loaded = false;
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  uint64_t vb_hash = 0;
  uint32_t revision = 0;             // bumps whenever `ranges` gains a polygon
  std::vector<WorldVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<float> uv;             // 2 per vertex, valid for vertices of known polygons
  std::map<uint32_t, WorldRange> ranges;  // keyed by start index
  std::vector<uint32_t> animated;         // start indices of the polygons with `animated` set
  std::vector<uint32_t> glowing;          // start indices of the polygons with a glow layer
  // The sky box: the engine keeps a cube (1200 units to a side from its middle) at the end
  // of the level's buffers and draws it round the camera, at the far plane, wherever the
  // level leaves a hole. One face per entry, with its texture once the engine has drawn it.
  struct SkyFace { uint32_t start, count; int texture; };
  std::vector<SkyFace> sky;
  uint32_t sky_revision = 0;
};

// Layout of a rigid object's vertex buffer (DarkMDModel.shader), 40 bytes.
struct ModelVertex {
  float position[3];
  float normal[3];
  float uv[2];
  uint32_t transform_id;   // which of the object's nodes moves this vertex
  uint32_t shapekey_id;
};

struct ModelMesh {
  ID3D11Buffer* vb = nullptr;   // references held so the pointers stay unique
  ID3D11Buffer* ib = nullptr;
  uint64_t hash = 0;
  std::vector<ModelVertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t max_transform_id = 0;
};

// One object draw of the current frame: part of a model, with one texture and the node
// transforms that place it in the world.
struct ObjectDraw {
  int model = -1;
  uint32_t start = 0, count = 0;
  int texture = -1;
  bool alpha_test = false;
  bool blended = false;       // drawn with alpha blending enabled
  bool unshaded = false;      // DarkMaterialConstants.Shade == 0: full-bright in the engine (lamps, screens)
  bool two_sided = false;     // the engine draws it without back-face culling
  bool additive = false;      // blended as SRC_ALPHA + ONE
  bool alpha_blend = false;   // blended as SRC_ALPHA + INV_SRC_ALPHA
  bool glow = false;          // the engine repeats this geometry as a blended, self-lit pass
                              // masked by the texture's alpha: its glowing parts
  int glow_texture = -1;      // texture of that pass
  float glow_strength = 0;    // its colour alpha times its brightest channel
  float illumination = 0;     // DarkMaterialConstants.Illumination
  float color[4] = { 1, 1, 1, 1 };   // DarkMaterialConstants.Color
  float uv_matrix[12];
  uint32_t node_offset = 0;   // into scene_frame_nodes(), 16 floats per node
  uint32_t node_count = 0;
};

// Layout of a skinned character's vertex buffer (DarkEMMModel.shader), 44 bytes.
#pragma pack(push, 1)
struct SkinnedVertex {
  float position[3];
  float uv[2];
  float normal[3];
  uint8_t joints[4];
  uint8_t weights[4];      // unsigned normalised
  uint32_t shapekey_id;
};
#pragma pack(pop)

struct SkinnedMesh {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  uint64_t hash = 0;
  std::vector<SkinnedVertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t max_joint = 0;
};

// One character draw of the current frame, with the bone matrices that pose it.
struct SkinnedDraw {
  int model = -1;
  uint32_t start = 0, count = 0;
  int texture = -1;
  bool alpha_test = false;
  bool blended = false;
  float color[4] = { 1, 1, 1, 1 };
  float uv_matrix[12];
  uint32_t bone_offset = 0;   // into scene_frame_bones(), 12 floats per bone (3 rows of 4)
  uint32_t bone_count = 0;
};

// A light as the engine hands it to its object shader: position and colour, with the
// engine's linear (1/distance) falloff.
struct SceneLight {
  float position[3];
  float color[3];
  bool spot = false;       // from the mission's light table
  float direction[3] = { 0, 0, -1 };
  float inner = -1, outer = 0;   // cosines of the cone half-angles
  float radius = 0;        // the engine's cut-off distance, 0 for none
  // What the engine is lighting objects with right now, when an object near this light has
  // been drawn: lights flicker, pulse and get switched off. Kept until seen again.
  bool has_live = false;
  float live[3] = { 0, 0, 0 };
  // From the mission file, for a light that flashes, slides or flickers on its own: its
  // rhythm is replayed here (the engine drops a light from its lists while it is dark, so
  // the dark half of a flashing light cannot be seen that way). -1: not such a light.
  int anim_mode = -1;
  int rise_ms = 0, fall_ms = 0, phase_ms = 0;
  float anim_low = 0, anim_high = 1;   // as fractions of the table brightness
  // Where it is in its rhythm: up (at or heading for its brightest) or down, the time left
  // in that state, the level it is at and, for the random kinds, the level it is heading for.
  bool anim_up = true;
  float anim_left_ms = 0, anim_level = 1, anim_from = 1, anim_to = 1, anim_span_ms = 1;
  // Where the engine keeps this light's brightness of the moment for the level's own
  // lightmaps (an entry of the level shader's brightness buffer), or -1. When there is one
  // the light follows it instead of the replay: the engine's phase, and its switches.
  int feed = -1;
  float feed_full[3] = { 0, 0, 0 };   // its colour when the engine's value is 1
};

struct Camera {
  bool valid = false;
  float view[16];   // world -> view, column-major, OpenGL conventions (camera looks down -Z)
  float proj[16];
  uint64_t frame = 0;
};

void scene_set_device(ID3D11Device* dev, ID3D11DeviceContext* immediate);

struct ShaderRecord;

// Called from the draw hooks for every indexed draw on the immediate context, with the
// records of the bound vertex and pixel shaders (either may be null).
void scene_on_draw_indexed(ID3D11DeviceContext* ctx, const ShaderRecord* vs, const ShaderRecord* ps, UINT count, UINT start, INT base_vertex);

// The colour target the engine rendered the level into this frame (null before the first
// world draw of the frame). No reference is added.
ID3D11Texture2D* scene_world_target();

// Texture coordinates the level shader would give a level vertex under the given
// per-draw constants (terrain scale and UV matrix).
void scene_level_uv(uint32_t vertex_index, const float scale[2], const float uv_matrix[12], float out[2]);

// Call once per presented frame.
void scene_frame_end();
// Advances the lights that change on their own to `seconds` (any steadily running clock).
void scene_animate_lights(double seconds);

const World& scene_world();
const std::vector<SceneTexture>& scene_textures();
const Camera& scene_camera();
const std::vector<ModelMesh>& scene_models();
const std::vector<ObjectDraw>& scene_frame_objects();   // cleared by scene_frame_end()
const std::vector<float>& scene_frame_nodes();          // column-major 4x4 matrices
const std::vector<SkinnedMesh>& scene_skinned_models();
const std::vector<SkinnedDraw>& scene_frame_skinned();   // cleared by scene_frame_end()
const std::vector<float>& scene_frame_bones();           // bind space -> world, row-major 3x4
const std::vector<SceneLight>& scene_lights();          // accumulated for the current level
// The ambient colour the engine lit objects with in the last frame (averaged over the
// objects drawn), and the directional lights it used (position holds the direction).
const float* scene_ambient();
const std::vector<SceneLight>& scene_far_lights();
// The engine's moving lights this frame (muzzle flashes, sparks, carried lights).
const std::vector<SceneLight>& scene_dynamic_lights();
unsigned scene_lights_generation();                     // bumps when the list is replaced, not when it grows

}  // namespace ss2rtx
