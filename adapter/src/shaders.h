// Shader identities. The game ships its compiled shaders, with names, in hlsl.kpf; a
// shader created at runtime is recognised by its DXBC checksum.
#pragma once
#include <windows.h>
#include <string>

namespace ss2rtx {

enum class ShaderClass : unsigned char {
  Unknown,
  WorldRep,   // DarkWorldRep: level geometry
  MDModel,    // DarkMDModel: rigid objects
  MMModel,    // DarkMMModel: skinned characters (older path)
  EMMModel,   // DarkEMMModel: skinned characters
  WorldSpace, // DarkWS*: world-space quads, particles, 2D
  Sky,        // DarkSky: the sky box, drawn between the level and its objects
  Post,       // SSAO, bloom, SMAA, colour correction, blurs
};

struct ShaderRecord {
  char kind = '?';  // 'v' or 'p'
  std::string id;   // 32 hex digits, or "not-dxbc"
  std::string name; // archive name without path and ".bin"; empty if unknown
  ShaderClass cls = ShaderClass::Unknown;
  bool alpha_test = false;  // "_at1" variants
  size_t size = 0;
};

// Reads hlsl.kpf from the working directory (the game folder). Call once.
void shader_table_load();

// Registers a shader object created by the game.
void shader_register(void* shader, char kind, const void* bytecode, SIZE_T size);

// Returns nullptr for a shader created before the hooks were installed.
const ShaderRecord* shader_find(void* shader);

// Visits every registered shader (used by the census to make captures self-contained).
void shader_for_each(void (*fn)(void* shader, const ShaderRecord& rec, void* user), void* user);

}  // namespace ss2rtx
