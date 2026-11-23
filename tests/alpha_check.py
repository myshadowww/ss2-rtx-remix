#!/usr/bin/env python3
"""Check the adapter's alpha decoder (adapter/src/alpha.cpp) against Pillow's.

  python3 tests/alpha_check.py [folder of .dds files]     # default: the test copy's texture cache

Builds the decoder for this machine, runs it over every texture's largest level and
compares the result texel by texel with what Pillow decodes. Needs g++, numpy and Pillow.
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
MAIN = r'''
#include "alpha.h"
#include <stdio.h>
int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; i += 2) {
    FILE* f = fopen(argv[i], "rb");
    if (!f) return 2;
    unsigned header[37];
    if (fread(header, 4, 37, f) != 37) return 3;
    std::vector<unsigned char> data(64 << 20);
    size_t got = fread(data.data(), 1, data.size(), f);
    fclose(f);
    std::vector<unsigned char> alpha;
    bool ok = ss2rtx::decode_alpha((int)header[32], header[4], header[3], data.data(), got, &alpha);
    FILE* o = fopen(argv[i + 1], "wb");
    if (ok) fwrite(alpha.data(), 1, alpha.size(), o);
    fclose(o);
  }
  return 0;
}
'''


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'game/ss2rtx_cache/textures'
    files = sorted(folder.glob('*.dds'))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'main.cpp').write_text(MAIN)
        exe = tmp / 'alpha_check'
        subprocess.run(['g++', '-std=c++17', '-O2', '-I', str(ROOT / 'adapter/src'), str(tmp / 'main.cpp'), str(ROOT / 'adapter/src/alpha.cpp'),
                        '-o', str(exe)], check=True)
        args = []
        for f in files:
            args += [str(f), str(tmp / (f.stem + '.a'))]
        for i in range(0, len(args), 400):
            subprocess.run([str(exe), *args[i:i + 400]], check=True)
        by_format, wrong, modes = {}, [], {}
        for f in files:
            header = f.read_bytes()[:148]
            fmt = struct.unpack_from('<I', header, 128)[0]
            ours = np.frombuffer((tmp / (f.stem + '.a')).read_bytes(), dtype=np.uint8)
            image = Image.open(f)
            reference = np.asarray(image.convert('RGBA'))[..., 3].reshape(-1) if 'A' in image.getbands() else np.full(image.size[0] * image.size[1], 255, np.uint8)
            stats = by_format.setdefault(fmt, [0, 0, 0])
            stats[0] += 1
            if ours.size != reference.size:
                stats[2] += 1
                continue
            worst = int(np.abs(ours.astype(int) - reference.astype(int)).max()) if ours.size else 0
            if worst > 1:
                wrong.append((f.name, fmt, worst, float((ours != reference).mean())))
            else:
                stats[1] += 1
    for fmt, (count, same, undecoded) in sorted(by_format.items()):
        print(f'format {fmt}: {count} textures, {same} decoded the same as Pillow (within one step), {undecoded} not decoded')
    for name, fmt, worst, share in wrong[:20]:
        print(f'  differs: {name} format {fmt}, worst texel off by {worst}, {share:.1%} of texels differ')
    sys.exit(1 if wrong else 0)


if __name__ == '__main__':
    main()
