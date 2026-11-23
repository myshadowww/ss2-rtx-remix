// The alpha channel of a texture level, decoded on the CPU from the formats the game uses.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace ss2rtx {

// Decodes the alpha of one mip level (`data`, as stored: DXGI format `format`) into one
// byte a texel, row by row. Formats: BC7, BC3, BC2, BC1 (its one-bit alpha), 8-bit RGBA
// and BGRA. Returns false for anything else or for short data.
bool decode_alpha(int format, unsigned width, unsigned height, const uint8_t* data, size_t size, std::vector<uint8_t>* alpha);

}  // namespace ss2rtx
