#!/usr/bin/env python3
"""Turn the module+offset frames from a probe/adapter stack dump into function names.

  python3 tools/symbolize.py run/t01_triangle_r1/probe.log [--all]

Frames in the Remix runtime are resolved with NVIDIA's published PDBs. Idle worker
threads (parked in a wait) are hidden unless --all is given.
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOLIZER = ROOT / 'toolchain/llvm-mingw/bin/llvm-symbolizer'
# module name (lower case) -> (binary, pdb)
MODULES = {
    'd3d9_x64.dll': (ROOT / 'runtime/remix-1.6.1-x64/d3d9_x64.dll', ROOT / 'runtime/remix-1.6.1-x64-symbols/d3d9_x64.pdb'),
    'd3d9.dll': (ROOT / 'runtime/remix-1.6.1-x64/d3d9.dll', ROOT / 'runtime/remix-1.6.1-x64-symbols/d3d9.pdb'),
}
FRAME = re.compile(r'^\s+#(\d+) (\S+)\+0x([0-9a-f]+)\s*$')


def image_base(binary):
    out = subprocess.run([str(ROOT / 'toolchain/llvm-mingw/bin/llvm-readobj'), '--file-headers', str(binary)],
                         capture_output=True, text=True).stdout
    m = re.search(r'ImageBase: (0x[0-9A-Fa-f]+)', out)
    return int(m.group(1), 16) if m else 0x180000000


def resolve(module, offsets):
    binary, pdb = MODULES[module]
    # llvm-symbolizer finds the PDB next to the binary; stage a symlink pair once.
    stage = ROOT / 'run/symbols'
    stage.mkdir(parents=True, exist_ok=True)
    for src in (binary, pdb):
        link = stage / src.name
        if not link.exists():
            link.symlink_to(src)
    base = image_base(binary)
    addrs = [hex(base + o) for o in offsets]
    out = subprocess.run([str(SYMBOLIZER), '--obj', str(stage / binary.name), '--functions=short', '--inlines',
                          '--demangle', *addrs], capture_output=True, text=True, cwd=stage).stdout
    names = []
    for block in out.strip().split('\n\n'):
        lines = [l for l in block.splitlines() if l.strip()]
        # pairs of (function, file:line); inlined frames first
        fns = [f'{lines[i]} ({Path(lines[i + 1].rsplit(":", 2)[0].replace(chr(92), "/")).name}:{lines[i + 1].rsplit(":", 2)[1]})'
               for i in range(0, len(lines) - 1, 2)]
        names.append(' <- '.join(fns) if fns else '??')
    return dict(zip(offsets, names))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log')
    p.add_argument('--all', action='store_true', help='also show idle worker threads')
    a = p.parse_args()
    lines = Path(a.log).read_text(errors='replace').splitlines()
    wanted = {}
    for line in lines:
        m = FRAME.match(line)
        if m and m.group(2).lower() in MODULES:
            wanted.setdefault(m.group(2).lower(), set()).add(int(m.group(3), 16))
    table = {mod: resolve(mod, sorted(offs)) for mod, offs in wanted.items()}

    threads, current = [], None
    for line in lines:
        if line.startswith('thread '):
            current = [line]
            threads.append(current)
        elif current is not None and FRAME.match(line):
            m = FRAME.match(line)
            mod, off = m.group(2).lower(), int(m.group(3), 16)
            name = table.get(mod, {}).get(off)
            current.append(f'  #{m.group(1)} {m.group(2)}+0x{off:x}' + (f'  {name}' if name else ''))
        elif line.startswith('==='):
            current = None
    idle = re.compile(r'DxvkPipelineWorkers|condition_variable|runWorker|WaitForSingleObject|Sleep|std::thread|thread::threadProc', re.I)
    shown = 0
    for t in threads:
        body = '\n'.join(t)
        resolved = [l for l in t[1:] if '  ' in l.strip() and ('(' in l)]
        if not a.all and resolved and all(idle.search(l) for l in resolved[:1]) and 'winevulkan' not in body and 'nv' not in body.lower().split('d3d9_x64')[0]:
            continue
        print(body)
        shown += 1
    print(f'-- {shown} of {len(threads)} threads shown' + ('' if a.all else ' (idle workers hidden; --all shows them)'))


if __name__ == '__main__':
    main()
