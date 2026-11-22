#!/usr/bin/env python3
"""Build the ss2rtx probes (and later the adapter) as Windows x64 binaries with llvm-mingw.

Each probe is built once per Remix SDK header it supports, because the two runtimes
have different API versions and struct layouts.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT / 'toolchain/llvm-mingw/bin'
OUT = ROOT / 'tests/build'

# name -> include dir of the matching remix_c.h
SDKS = {
    'r1': ROOT / 'runtime/sdk-1.6.1/include',      # Remix 1.6.1 release, API 0.6.5
    'r2': ROOT / 'runtime/sdk-mc-rtx2/include',    # minecraft-rtx2 runtime, API 0.16.1
}

# probe -> (source, language, SDKs it is built for, extra link libs)
PROBES = {
    't01_triangle': ('tests/t01_triangle.cpp', 'c++', ('r1', 'r2'), []),
    't02_readback': ('tests/t02_readback.cpp', 'c++', ('r1', 'r2'), []),
    't03_handoff': ('tests/t03_handoff.cpp', 'c++', ('r1', 'r2'), ['d3d11', 'dxgi']),
}


def build(probe, sdk):
    src, lang, _, libs = PROBES[probe]
    cc = BIN / ('x86_64-w64-mingw32-clang' if lang == 'c' else 'x86_64-w64-mingw32-clang++')
    std = '-std=c11' if lang == 'c' else '-std=c++20'
    out = OUT / f'{probe}_{sdk}.exe'
    cmd = [str(cc), std, '-O2', '-g', '-Wall', '-Wno-unused-function', '-municode',
           f'-DSS2RTX_SDK_{sdk.upper()}=1', '-I', str(SDKS[sdk]), '-I', str(ROOT / 'tests'),
           str(ROOT / src), '-o', str(out), '-static', *[f'-l{l}' for l in libs]]
    print(' '.join(cmd[:1] + ['...', src, '->', str(out.relative_to(ROOT))]), flush=True)
    subprocess.run(cmd, check=True)
    return out


def build_adapter():
    """The ss2rtx dxgi.dll proxy that the game loads."""
    src_dir = ROOT / 'adapter/src'
    out_dir = ROOT / 'adapter/build'
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / 'dxgi.dll'
    sources = sorted(str(p) for p in src_dir.glob('*.cpp'))
    cmd = [str(BIN / 'x86_64-w64-mingw32-clang++'), '-std=c++20', '-O2', '-g', '-Wall', '-Wextra',
           '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-shared', '-static', '-I', str(src_dir),
           '-I', str(SDKS['r1']), *sources,
           str(ROOT / 'adapter/ss2rtx.def'), '-lgdi32', '-o', str(out), '-Wl,--pdb=']
    print('adapter:', len(sources), 'sources ->', out.relative_to(ROOT), flush=True)
    subprocess.run(cmd, check=True, cwd=out_dir)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('targets', nargs='*', help="probe names and/or 'adapter'; default: everything")
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for target in args.targets or [*PROBES, 'adapter']:
        if target == 'adapter':
            build_adapter()
            continue
        if target not in PROBES:
            sys.exit(f'unknown target {target}')
        for sdk in PROBES[target][2]:
            build(target, sdk)


if __name__ == '__main__':
    main()
