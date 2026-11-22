// GPU -> CPU readback helpers for the game's D3D11 resources (immediate context only).
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace ss2rtx {

uint64_t fnv1a64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull);

// Copies a buffer's contents to `out`. `max_bytes` 0 means the whole buffer.
bool read_buffer(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* buf, std::vector<uint8_t>* out);

struct TextureDump {
  uint64_t hash = 0;      // of the top mip's bytes
  UINT width = 0, height = 0, mips = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  // For small uncompressed 8-bit textures: true when every texel has the same colour,
  // which is how the engine represents flat-coloured model materials.
  bool solid = false;
  float solid_rgba[4] = { 1, 1, 1, 1 };
  bool already_on_disk = false;   // the .dds was written by an earlier run
  // An empty layer: nearly every block (or texel) of the top mip is the same transparent
  // one. The engine uses such textures as the first pass of layered surfaces.
  bool uniform = false;
};

// Writes all mips of a 2D texture as a DDS file (DX10 header) under `dir`, named by
// content hash, and returns the full path in `path`. `view_format` picks the concrete
// format when the texture itself is typeless. Existing files are not rewritten.
bool dump_texture_dds(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, DXGI_FORMAT view_format,
                      const std::wstring& dir, std::wstring* path, TextureDump* info);

// Writes a 32-bit BGRA image as a .bmp (top-down).
bool save_bmp(const std::wstring& path, const uint8_t* bgra, int w, int h, int pitch);

}  // namespace ss2rtx
