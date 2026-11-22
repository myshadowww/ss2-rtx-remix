#!/usr/bin/env python3
"""Inventory of the game's mission files, and a look at how findable the light table is.

  python3 tools/missions.py

Reliable facts printed per mission: which archive it is in, whether it is stored
uncompressed (the adapter can only read stored entries) and which world chunk it has.

The last column tries to locate the static light table from structure alone (two counts,
then 768 records of 48 bytes with a zero tail). That is ambiguous or fails for most
missions, so it proves nothing either way. The adapter does not work like this: it
searches for the exact position of a light it has seen at runtime and then checks that
the other lights it has seen are in the same table. That has been verified in the game
for Ramsey Center (mods/scp.kpf:earth.mis) only.
"""
import math
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / 'game'
RECORD = 48
MAX_LIGHTS = 768


def chunks(data):
    inv = struct.unpack_from('<I', data, 0)[0]
    count = struct.unpack_from('<I', data, inv)[0]
    out = {}
    for i in range(count):
        name, off, length = struct.unpack_from('<12sII', data, inv + 4 + i * 20)
        out[name.split(b'\0')[0].decode('latin1')] = (off, length)
    return out


def plausible(rec):
    pos, direction, color, inner, outer, radius = rec[0:3], rec[3:6], rec[6:9], rec[9], rec[10], rec[11]
    if not all(math.isfinite(x) for x in rec):
        return False
    if any(abs(x) > 20000 for x in pos):
        return False
    length = math.sqrt(sum(x * x for x in direction))
    if not (length < 1e-4 or abs(length - 1.0) < 1e-2):      # a unit direction, or none
        return False
    if any(x < 0 or x > 1000 for x in color):
        return False
    if not (inner == -1.0 or 0.0 <= inner <= 1.0):           # -1 marks an omnidirectional light
        return False
    return -1.0 <= outer <= 1.0 and 0 <= radius < 100000


def find_table(blob):
    """Offset and count of the light table, found without any outside knowledge.

    The table is a fixed array of 768 records after two counts; the unused tail is zero.
    """
    zero = bytes(RECORD)
    candidates = []
    for off in range(max(8, len(blob) - 2_000_000), len(blob) - MAX_LIGHTS * RECORD - 8):
        ns, nd = struct.unpack_from('<II', blob, off)
        if not (1 <= ns <= MAX_LIGHTS and nd <= 64):
            continue
        start = off + 8
        # cheap rejections first
        if not plausible(struct.unpack_from('<12f', blob, start)) or not plausible(struct.unpack_from('<12f', blob, start + (ns - 1) * RECORD)):
            continue
        used = ns + nd
        if used < MAX_LIGHTS and blob[start + used * RECORD:start + MAX_LIGHTS * RECORD].strip(b'\0'):
            continue
        recs = [struct.unpack_from('<12f', blob, start + i * RECORD) for i in range(ns)]
        if not all(plausible(r) for r in recs):
            continue
        lit = sum(1 for r in recs if max(r[6:9]) > 0)
        candidates.append((start, ns, lit, recs))
    if len(candidates) != 1:
        return None if not candidates else ('ambiguous', len(candidates))
    return candidates[0]


def main():
    archives = sorted(GAME.glob('mods/*.kpf')) + sorted(GAME.glob('*.kpf'))
    total = ok = 0
    for archive in archives:
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                if not info.filename.lower().endswith('.mis'):
                    continue
                total += 1
                data = z.read(info.filename)
                ch = chunks(data)
                world = next((c for c in ('WREXT', 'WRRGB', 'WR') if c in ch), None)
                label = f'{archive.relative_to(GAME)}:{info.filename}'
                if not world:
                    print(f'{label:44s} no world chunk')
                    continue
                off, length = ch[world]
                blob = data[off:off + 24 + length]
                found = find_table(blob)
                if not found or found[0] == 'ambiguous':
                    why = 'LIGHT TABLE NOT FOUND' if not found else f'AMBIGUOUS ({found[1]} candidates)'
                    print(f'{label:44s} {world:5s} stored={info.compress_type == 0}  {why}')
                    continue
                start, ns, lit, recs = found
                spots = sum(1 for r in recs if r[9] != -1.0)
                ok += 1
                print(f'{label:44s} {world:5s} stored={info.compress_type == 0}  {ns:3d} static lights ({lit} lit, {spots} spots) at chunk+{start}')
    print(f'{total} missions; light table unambiguous from structure alone in {ok} (not what the adapter relies on, see the docstring)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
