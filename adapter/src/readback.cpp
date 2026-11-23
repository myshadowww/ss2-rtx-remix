#include "readback.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

namespace ss2rtx {

uint64_t fnv1a64(const void* data, size_t size, uint64_t h) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

bool read_buffer(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* buf, std::vector<uint8_t>* out) {
  D3D11_BUFFER_DESC d;
  buf->GetDesc(&d);
  D3D11_BUFFER_DESC sd = {};
  sd.ByteWidth = d.ByteWidth;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* staging = nullptr;
  if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging))) return false;
  ctx->CopyResource(staging, buf);
  D3D11_MAPPED_SUBRESOURCE map = {};
  bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map));
  if (ok) {
    out->assign(static_cast<const uint8_t*>(map.pData), static_cast<const uint8_t*>(map.pData) + d.ByteWidth);
    ctx->Unmap(staging, 0);
  }
  staging->Release();
  return ok;
}

// Bytes per 4x4 block for compressed formats, or per pixel otherwise. 0 = unsupported.
static void format_layout(DXGI_FORMAT f, unsigned* bytes, bool* block) {
  *block = false;
  switch ((int)f) {
    case 70: case 71: case 72: case 79: case 80: case 81: *bytes = 8; *block = true; return;   // BC1, BC4
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84:
    case 94: case 95: case 96: case 97: case 98: case 99: *bytes = 16; *block = true; return;  // BC2, BC3, BC5, BC6H, BC7
    case 27: case 28: case 29: case 30: case 31: case 87: case 88: case 90: case 91: case 92: case 93: *bytes = 4; return;
    case 48: case 49: case 50: case 51: case 52: case 56: *bytes = 2; return;
    case 60: case 61: case 62: case 63: case 64: case 65: *bytes = 1; return;
    case 9: case 10: case 11: *bytes = 8; return;
    default: *bytes = 0; return;
  }
}

// A typeless resource format needs a concrete one in the file.
static DXGI_FORMAT concrete_format(DXGI_FORMAT resource, DXGI_FORMAT view) {
  switch ((int)resource) {
    case 27: case 70: case 73: case 76: case 79: case 82: case 90: case 92: case 94: case 97:
      if (view != DXGI_FORMAT_UNKNOWN && view != resource) return view;
      return (DXGI_FORMAT)((int)resource + 1);  // the UNORM sibling follows the typeless entry
    default:
      return resource;
  }
}

// True when a texture is an empty layer: nearly every block (or texel) is the same one, and
// that one is transparent. The engine uses such textures as the first pass of surfaces
// whose picture is all in a second pass. A few stray texels are allowed; the ones seen in
// the game have about 0.2% of their blocks different.
static bool looks_blank(const std::vector<uint8_t>& top, unsigned unit, int format) {
  if (unit == 0 || top.size() < (size_t)unit * 16) return false;
  const size_t count = top.size() / unit;
  const uint8_t* common = nullptr;
  for (size_t candidate : { (size_t)0, count / 3, count / 2 }) {   // the common block is almost surely one of these
    const uint8_t* c = top.data() + candidate * unit;
    size_t same = 0;
    for (size_t i = 0; i < count; ++i) same += memcmp(c, top.data() + i * unit, unit) == 0;
    if (same * 100 >= count * 99) { common = c; break; }
  }
  if (!common) return false;
  const int kAlmostZero = 8;   // of 255
  if (format >= 97 && format <= 99) {          // BC7: read the alpha endpoints of the two single-subset alpha modes
    uint64_t lo, hi;
    memcpy(&lo, common, 8);
    memcpy(&hi, common + 8, 8);
    if ((lo & 0x7f) == 0x40) {                 // mode 6: 7-bit endpoints and a shared low bit each
      int a0 = (int)((lo >> 49) & 0x7f) << 1 | (int)((lo >> 63) & 1);
      int a1 = (int)((lo >> 56) & 0x7f) << 1 | (int)(hi & 1);
      return a0 <= kAlmostZero && a1 <= kAlmostZero;
    }
    if ((lo & 0x3f) == 0x20 && ((lo >> 6) & 3) == 0) {   // mode 5, no channel rotation: 8-bit alpha endpoints
      int a0 = (int)((lo >> 50) & 0xff);
      int a1 = (int)((lo >> 58) & 0x3f) | (int)(hi & 3) << 6;
      return a0 <= kAlmostZero && a1 <= kAlmostZero;
    }
    return false;
  }
  if (format >= 76 && format <= 78) return common[0] <= kAlmostZero && common[1] <= kAlmostZero;   // BC3: alpha endpoints lead the block
  if ((format >= 27 && format <= 29) || format == 87 || format == 90 || format == 91) return common[3] <= kAlmostZero;   // 8-bit RGBA / BGRA
  return false;
}

// The average colour of one mip level, near enough: for block-compressed formats the mean
// of each block's two end colours (first pair only for BC7's multi-part modes).
static bool rough_mean(const std::vector<uint8_t>& mip, unsigned unit, int format, float* out) {
  double sum[3] = { 0, 0, 0 }, alpha = 0.0;
  size_t n = 0, alphas = 0;
  bool bc1 = format >= 70 && format <= 72, bc23 = format >= 73 && format <= 78, bc7 = format >= 97 && format <= 99;
  bool rgba8 = format >= 27 && format <= 29, bgra8 = format == 87 || format == 88 || (format >= 90 && format <= 93);
  if (bc1 || bc23) {
    for (size_t at = 0; at + unit <= mip.size(); at += unit) {
      const uint8_t* b = mip.data() + at + (bc23 ? 8 : 0);
      if (format >= 76 && format <= 78) { alpha += (mip[at] + mip[at + 1]) / 510.0; ++alphas; }   // BC3: the two alpha ends
      for (int e = 0; e < 2; ++e) {
        unsigned v = b[e * 2] | b[e * 2 + 1] << 8;
        sum[0] += (v >> 11 & 31) / 31.0; sum[1] += (v >> 5 & 63) / 63.0; sum[2] += (v & 31) / 31.0;
        ++n;
      }
    }
  } else if (bc7) {
    // mode: bits before the first colour bit, bits per colour, end colours in the block
    static const int head[8] = { 5, 8, 9, 10, 8, 8, 7, 14 }, bits[8] = { 4, 6, 5, 7, 5, 7, 7, 5 }, ends[8] = { 6, 4, 6, 4, 2, 2, 2, 4 };
    for (size_t at = 0; at + 16 <= mip.size(); at += 16) {
      const uint8_t* b = mip.data() + at;
      int mode = 0;
      while (mode < 8 && !(b[0] >> mode & 1)) ++mode;
      if (mode >= 8) continue;
      auto field = [&](int first, int count) {
        unsigned v = 0;
        for (int i = 0; i < count; ++i) v |= (unsigned)(b[(first + i) >> 3] >> ((first + i) & 7) & 1) << i;
        return v;
      };
      for (int c = 0; c < 3; ++c)
        for (int e = 0; e < 2; ++e) sum[c] += field(head[mode] + (c * ends[mode] + e) * bits[mode], bits[mode]) / (double)((1 << bits[mode]) - 1);
      n += 2;
      // Alpha ends follow the colour ends in the modes that have alpha.
      static const int alpha_bits[8] = { 0, 0, 0, 0, 6, 8, 7, 5 };
      if (alpha_bits[mode]) {
        int first = head[mode] + 3 * ends[mode] * bits[mode];
        alpha += (field(first, alpha_bits[mode]) + field(first + alpha_bits[mode], alpha_bits[mode])) / (2.0 * ((1 << alpha_bits[mode]) - 1));
      } else {
        alpha += 1.0;
      }
      ++alphas;
    }
  } else if (format >= 60 && format <= 62) {   // one 8-bit channel (the engine's incidence maps)
    for (size_t at = 0; at < mip.size(); ++at) {
      for (int c = 0; c < 3; ++c) sum[c] += mip[at] / 255.0;
      ++n;
    }
  } else if (format == 79 || format == 80) {   // BC4: one channel, two ends a block
    for (size_t at = 0; at + 8 <= mip.size(); at += 8) {
      for (int c = 0; c < 3; ++c) sum[c] += (mip[at] + mip[at + 1]) / 510.0;
      ++n;
    }
  } else if (rgba8 || bgra8) {
    for (size_t at = 0; at + 4 <= mip.size(); at += 4) {
      sum[0] += mip[at + (bgra8 ? 2 : 0)] / 255.0; sum[1] += mip[at + 1] / 255.0; sum[2] += mip[at + (bgra8 ? 0 : 2)] / 255.0;
      ++n;
      if (format != 88 && format != 92 && format != 93) { alpha += mip[at + 3] / 255.0; ++alphas; }
    }
  }
  if (!n) return false;
  for (int c = 0; c < 3; ++c) out[c] = (float)(sum[c] / (double)n);
  out[3] = alphas ? (float)(alpha / (double)alphas) : 1.0f;
  return true;
}

bool dds_rough_mean(const std::wstring& path, float* rgba) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  uint32_t header[37];
  bool ok = fread(header, 4, 37, f) == 37 && header[0] == 0x20534444 && header[21] == 0x30315844;
  unsigned width = ok ? header[4] : 0, height = ok ? header[3] : 0, levels = ok ? header[7] : 0;
  int format = ok ? (int)header[32] : 0;
  unsigned unit = 0;
  bool block = false;
  if (ok) format_layout((DXGI_FORMAT)format, &unit, &block);
  ok = ok && unit && width && height && levels && width <= 16384 && height <= 16384;
  std::vector<uint8_t> mip;
  if (ok) {
    // Skip to a mip of 16 texels a side, or the smallest there is.
    unsigned pick = 0;
    while (pick + 1 < levels && ((width >> pick) > 16 || (height >> pick) > 16)) ++pick;
    long offset = 148;
    for (unsigned m = 0; m <= pick; ++m) {
      unsigned w = width >> m ? width >> m : 1, h = height >> m ? height >> m : 1;
      size_t bytes = block ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * unit : (size_t)w * h * unit;
      if (m == pick) {
        mip.resize(bytes);
        ok = fseek(f, offset, SEEK_SET) == 0 && fread(mip.data(), 1, bytes, f) == bytes;
      }
      offset += (long)bytes;
    }
  }
  fclose(f);
  return ok && rough_mean(mip, unit, format, rgba);
}

bool dump_texture_dds(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, DXGI_FORMAT view_format,
                      const std::wstring& dir, std::wstring* path, TextureDump* info) {
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  unsigned unit = 0;
  bool block = false;
  format_layout(d.Format, &unit, &block);
  if (unit == 0 || d.SampleDesc.Count != 1 || d.ArraySize != 1) return false;

  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
  ctx->CopyResource(staging, tex);

  std::vector<std::vector<uint8_t>> mips(d.MipLevels);
  bool ok = true;
  for (UINT m = 0; m < d.MipLevels && ok; ++m) {
    UINT w = d.Width >> m ? d.Width >> m : 1, h = d.Height >> m ? d.Height >> m : 1;
    UINT rows = block ? (h + 3) / 4 : h;
    UINT row_bytes = block ? ((w + 3) / 4) * unit : w * unit;
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(ctx->Map(staging, m, D3D11_MAP_READ, 0, &map))) { ok = false; break; }
    mips[m].resize((size_t)rows * row_bytes);
    for (UINT y = 0; y < rows; ++y)
      memcpy(&mips[m][(size_t)y * row_bytes], static_cast<const uint8_t*>(map.pData) + (size_t)y * map.RowPitch, row_bytes);
    ctx->Unmap(staging, m);
  }
  staging->Release();
  if (!ok) return false;

  info->hash = fnv1a64(mips[0].data(), mips[0].size());
  info->width = d.Width; info->height = d.Height; info->mips = d.MipLevels;
  info->format = concrete_format(d.Format, view_format);
  info->uniform = looks_blank(mips[0], unit, (int)d.Format);
  {
    // A mip of 16 texels a side or the smallest there is: few blocks, already averaged.
    UINT pick = 0;
    while (pick + 1 < d.MipLevels && ((d.Width >> pick) > 16 || (d.Height >> pick) > 16)) ++pick;
    float rgba[4];
    info->has_mean = rough_mean(mips[pick], unit, (int)d.Format, rgba);
    if (info->has_mean) { memcpy(info->mean_rgb, rgba, sizeof info->mean_rgb); info->mean_alpha = rgba[3]; }
  }
  int fid = (int)d.Format;
  bool rgba8 = fid >= 27 && fid <= 29, bgra8 = fid == 87 || fid == 88 || (fid >= 90 && fid <= 93);
  if ((rgba8 || bgra8) && d.Width * d.Height <= 64 * 64 && mips[0].size() >= 4) {
    const uint8_t* px = mips[0].data();
    bool same = true;
    for (size_t i = 4; i + 3 < mips[0].size() && same; i += 4) same = memcmp(px, px + i, 4) == 0;
    if (same) {
      info->solid = true;
      info->solid_rgba[0] = px[bgra8 ? 2 : 0] / 255.0f;
      info->solid_rgba[1] = px[1] / 255.0f;
      info->solid_rgba[2] = px[bgra8 ? 0 : 2] / 255.0f;
      info->solid_rgba[3] = fid == 88 || fid == 92 || fid == 93 ? 1.0f : px[3] / 255.0f;
    }
  }
  wchar_t name[64];
  swprintf(name, 64, L"%016llx.dds", (unsigned long long)info->hash);
  *path = dir + name;
  if (GetFileAttributesW(path->c_str()) != INVALID_FILE_ATTRIBUTES) { info->already_on_disk = true; return true; }

  uint32_t header[32] = {};   // "DDS " + DDS_HEADER (124 bytes) + DDS_HEADER_DXT10 (20 bytes) = 148 bytes = 37 words
  uint32_t dx10[5] = {};
  header[0] = 0x20534444;     // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;  // caps, height, width, pixelformat, mipmapcount, linearsize
  header[3] = d.Height;
  header[4] = d.Width;
  header[5] = (uint32_t)mips[0].size();
  header[7] = d.MipLevels;
  header[19] = 32;            // DDS_PIXELFORMAT.size
  header[20] = 0x4;           // DDPF_FOURCC
  header[21] = 0x30315844;    // "DX10"
  header[27] = 0x1000 | (d.MipLevels > 1 ? 0x400008 : 0);    // texture, plus complex|mipmap
  dx10[0] = (uint32_t)info->format;
  dx10[1] = 3;                // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  dx10[3] = 1;                // array size
  FILE* f = _wfopen(path->c_str(), L"wb");
  if (!f) return false;
  fwrite(header, 4, 32, f);
  fwrite(dx10, 4, 5, f);
  for (const auto& mip : mips) fwrite(mip.data(), 1, mip.size(), f);
  fclose(f);
  return true;
}

bool save_bmp(const std::wstring& path, const uint8_t* bgra, int w, int h, int pitch) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
  ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
  fwrite(&fh, sizeof fh, 1, f);
  fwrite(&ih, sizeof ih, 1, f);
  for (int y = 0; y < h; ++y) fwrite(bgra + (size_t)y * pitch, 4, w, f);
  fclose(f);
  return true;
}

}  // namespace ss2rtx
