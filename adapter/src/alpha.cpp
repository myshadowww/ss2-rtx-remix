// Alpha decoding for block-compressed textures (see alpha.h). No Windows headers: this
// file is also built for the host by tests/alpha_check.py and compared with a reference.
#include "alpha.h"

#include <string.h>

namespace ss2rtx {

namespace {

struct Bits {
  const uint8_t* b;
  unsigned at = 0;
  unsigned get(unsigned count) {
    unsigned v = 0;
    for (unsigned i = 0; i < count; ++i, ++at) v |= (unsigned)(b[at >> 3] >> (at & 7) & 1) << i;
    return v;
  }
};

const uint8_t kWeights2[4] = { 0, 21, 43, 64 };
const uint8_t kWeights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
const uint8_t kWeights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

inline uint8_t mix(unsigned a, unsigned b, unsigned weight) { return (uint8_t)(((64 - weight) * a + weight * b + 32) >> 6); }

// Which of two subsets each texel of a block belongs to, for BC7's 64 two-subset shapes
// (bit i: texel i), and the second subset's anchor texel.
const uint16_t kShape2[64] = {
  0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
  0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
  0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
  0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
const uint8_t kAnchor2[64] = {
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
  15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};

// One BC7 block's alpha, sixteen texels. Modes 0 to 3 have none: opaque.
void bc7_alpha(const uint8_t* block, uint8_t* out) {
  int mode = 0;
  while (mode < 8 && !(block[0] >> mode & 1)) ++mode;
  if (mode < 4 || mode >= 8) { memset(out, 255, 16); return; }
  Bits in = { block, (unsigned)mode + 1 };
  if (mode == 4 || mode == 5) {
    // One subset; colour and alpha with index sets of their own, and a "rotation" that
    // swaps alpha with one colour channel after decoding.
    unsigned rotation = in.get(2), swap_sets = mode == 4 ? in.get(1) : 0;
    unsigned colour_bits = mode == 4 ? 5 : 7, alpha_bits = mode == 4 ? 6 : 8;
    unsigned ends[4][2];
    for (int c = 0; c < 3; ++c)
      for (int e = 0; e < 2; ++e) {
        unsigned v = in.get(colour_bits);
        ends[c][e] = mode == 4 ? (v << 3 | v >> 2) : (v << 1 | v >> 6);
      }
    for (int e = 0; e < 2; ++e) {
      unsigned v = in.get(alpha_bits);
      ends[3][e] = mode == 4 ? (v << 2 | v >> 4) : v;
    }
    // Two index sets: the first two bits a texel (both modes), the second three (mode 4)
    // or two (mode 5); texel 0 of each has one bit fewer.
    unsigned first[16], second[16];
    for (int i = 0; i < 16; ++i) first[i] = in.get(i == 0 ? 1 : 2);
    unsigned second_bits = mode == 4 ? 3 : 2;
    for (int i = 0; i < 16; ++i) second[i] = in.get(i == 0 ? second_bits - 1 : second_bits);
    // Which set the wanted channel uses: alpha's own data sits in the second set, colour's
    // in the first, unless mode 4 swaps them. With a rotation the alpha we want was stored
    // as a colour channel.
    int channel = rotation == 0 ? 3 : (int)rotation - 1;
    bool is_colour = channel != 3;
    bool use_first = is_colour != (swap_sets != 0);
    const unsigned* index = use_first ? first : second;
    const uint8_t* weights = use_first ? kWeights2 : (second_bits == 3 ? kWeights3 : kWeights2);
    for (int i = 0; i < 16; ++i) out[i] = mix(ends[channel][0], ends[channel][1], weights[index[i]]);
    return;
  }
  if (mode == 6) {
    unsigned ends[2];
    in.at = 7 + 42;                       // past the mode bits and six 7-bit colour ends
    unsigned a0 = in.get(7), a1 = in.get(7), p0 = in.get(1), p1 = in.get(1);
    ends[0] = a0 << 1 | p0;
    ends[1] = a1 << 1 | p1;
    for (int i = 0; i < 16; ++i) out[i] = mix(ends[0], ends[1], kWeights4[in.get(i == 0 ? 3 : 4)]);
    return;
  }
  // Mode 7: two subsets, 5-bit ends with a shared low bit each, 2-bit indices.
  unsigned shape = in.get(6);
  in.at += 60;                            // twelve 5-bit colour ends
  unsigned a[4], p[4], ends[4];
  for (int e = 0; e < 4; ++e) a[e] = in.get(5);
  for (int e = 0; e < 4; ++e) p[e] = in.get(1);
  for (int e = 0; e < 4; ++e) { unsigned v = a[e] << 1 | p[e]; ends[e] = v << 2 | v >> 4; }
  for (int i = 0; i < 16; ++i) {
    unsigned subset = kShape2[shape] >> i & 1;
    bool anchor = i == 0 || (unsigned)i == kAnchor2[shape];
    out[i] = mix(ends[subset * 2], ends[subset * 2 + 1], kWeights2[in.get(anchor ? 1 : 2)]);
  }
}

// BC3's alpha block (BC4 layout): two ends and sixteen 3-bit choices.
void bc4_block(const uint8_t* block, uint8_t* out) {
  unsigned a0 = block[0], a1 = block[1], table[8] = { a0, a1 };
  if (a0 > a1) for (unsigned i = 1; i < 7; ++i) table[i + 1] = ((7 - i) * a0 + i * a1) / 7;
  else { for (unsigned i = 1; i < 5; ++i) table[i + 1] = ((5 - i) * a0 + i * a1) / 5; table[6] = 0; table[7] = 255; }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= (uint64_t)block[2 + i] << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = (uint8_t)table[bits >> (3 * i) & 7];
}

}  // namespace

bool decode_alpha(int format, unsigned width, unsigned height, const uint8_t* data, size_t size, std::vector<uint8_t>* alpha) {
  bool bc1 = format >= 70 && format <= 72, bc2 = format >= 73 && format <= 75, bc3 = format >= 76 && format <= 78, bc7 = format >= 97 && format <= 99;
  bool rgba8 = format >= 27 && format <= 29, bgra8 = format == 87 || format == 90 || format == 91;
  if (!width || !height) return false;
  alpha->assign((size_t)width * height, 255);
  if (rgba8 || bgra8) {
    if (size < (size_t)width * height * 4) return false;
    for (size_t i = 0; i < (size_t)width * height; ++i) (*alpha)[i] = data[i * 4 + 3];
    return true;
  }
  if (!bc1 && !bc2 && !bc3 && !bc7) return false;
  unsigned unit = bc1 ? 8 : 16, bw = (width + 3) / 4, bh = (height + 3) / 4;
  if (size < (size_t)bw * bh * unit) return false;
  for (unsigned by = 0; by < bh; ++by)
    for (unsigned bx = 0; bx < bw; ++bx) {
      const uint8_t* block = data + ((size_t)by * bw + bx) * unit;
      uint8_t texels[16];
      if (bc7) {
        bc7_alpha(block, texels);
      } else if (bc3) {
        bc4_block(block, texels);
      } else if (bc2) {
        for (int i = 0; i < 16; ++i) texels[i] = (uint8_t)((block[i >> 1] >> ((i & 1) * 4) & 15) * 17);
      } else {   // BC1: see-through only in its three-colour form, index 3
        unsigned c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
        uint32_t index = block[4] | block[5] << 8 | block[6] << 16 | (uint32_t)block[7] << 24;
        for (int i = 0; i < 16; ++i) texels[i] = c0 <= c1 && (index >> (2 * i) & 3) == 3 ? 0 : 255;
      }
      for (unsigned y = 0; y < 4 && by * 4 + y < height; ++y)
        for (unsigned x = 0; x < 4 && bx * 4 + x < width; ++x) (*alpha)[(size_t)(by * 4 + y) * width + bx * 4 + x] = texels[y * 4 + x];
    }
  return true;
}

}  // namespace ss2rtx
