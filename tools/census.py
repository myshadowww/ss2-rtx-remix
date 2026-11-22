#!/usr/bin/env python3
"""Summarise a draw-stream census captured by the ss2rtx adapter.

  python3 tools/census.py run/game/census-0.jsonl            # passes, shaders, geometry
  python3 tools/census.py run/game/census-0.jsonl --draws    # one line per draw as well
  python3 tools/census.py run/game/census-0.jsonl --cb 12    # constant buffers of draw 12

Shader names come from matching each shader's DXBC checksum against the compiled blobs
the game ships in hlsl.kpf.
"""
import argparse
import collections
import json
import struct
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DXGI = {2: 'RGBA32F', 6: 'RGB32F', 10: 'RGBA16F', 16: 'RG32F', 24: 'RGB10A2', 28: 'RGBA8', 29: 'RGBA8_SRGB', 30: 'RGBA8U', 34: 'RG16F',
        40: 'D32F', 41: 'R32F', 42: 'R32U', 45: 'D24S8', 54: 'R16F', 56: 'R16', 57: 'R16U', 61: 'R8', 71: 'BC1', 72: 'BC1_SRGB',
        74: 'BC2', 77: 'BC3', 78: 'BC3_SRGB', 87: 'BGRA8', 91: 'BGRA8_SRGB', 98: 'BC7', 99: 'BC7_SRGB', 49: 'RG8', 11: 'RGBA16',
        31: 'RGBA8S', 80: 'BC4', 83: 'BC5', 95: 'BC6H', 19: 'D32FS8', 20: 'R32FX8', 39: 'R32T', 44: 'R24G8T', 46: 'R24X8', 26: 'R11G11B10F'}


def shader_names():
    names = {}
    kpf = ROOT / 'game/hlsl.kpf'
    if not kpf.exists():
        return names
    with zipfile.ZipFile(kpf) as z:
        for entry in z.namelist():
            data = z.read(entry)
            off = data.find(b'DXBC')
            while off >= 0 and off + 20 <= len(data):
                names.setdefault(data[off + 4:off + 20].hex(), Path(entry).name.removesuffix('.bin'))
                off = data.find(b'DXBC', off + 4)
    return names


def fmt_res(r):
    if not r:
        return '-'
    if 'tex2d' in r:
        return f"{r['tex2d'][0]}x{r['tex2d'][1]} {DXGI.get(r['fmt'], r['fmt'])}" + (f" x{r['samples']}" if r.get('samples', 1) > 1 else '')
    if 'buffer' in r:
        return f"buf {r['buffer']}B/{r['stride']}"
    return str(r)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture')
    p.add_argument('--draws', action='store_true')
    p.add_argument('--cb', type=int, help='print the constant buffers of this draw sequence number')
    a = p.parse_args()
    events = [json.loads(l) for l in Path(a.capture).read_text().splitlines() if l.strip()]
    names = shader_names()
    draws = [e for e in events if e['ev'] == 'draw']
    blobs = {e['id']: bytes.fromhex(e['hex']) for e in events if e['ev'] == 'blob'}
    layouts = {e['ptr']: e['elements'] for e in events if e['ev'] == 'layout'}
    present = next((e for e in events if e['ev'] == 'present'), {})

    def name(s):
        if not s:
            return '-'
        return names.get(s['id'], s['id'][:10] + ('' if s['id'] in ('unseen', 'not-dxbc') else '?'))

    known = sum(1 for e in events if e['ev'] == 'shader' and e['id'] in names)
    total_shaders = sum(1 for e in events if e['ev'] == 'shader')
    print(f'{len(draws)} draws, {sum(d["deferred"] for d in draws)} on deferred contexts; '
          f'{sum(1 for e in events if e["ev"] == "execute_command_list")} command lists executed this frame; '
          f'{present.get("deferred_contexts", 0)} deferred contexts created so far')
    print(f'shaders created so far: {total_shaders}, of which {known} match a blob in hlsl.kpf ({len(names)} blobs)')
    unseen = sum(1 for d in draws for k in ('vs', 'ps') if d[k] and d[k]['id'] == 'unseen')
    if unseen:
        print(f'WARNING: {unseen} shader uses were created before the hooks were installed')

    if a.cb is not None:
        d = next(x for x in draws if x['seq'] == a.cb)
        print(f"draw {a.cb}: vs {name(d['vs'])} ps {name(d['ps'])}")
        for stage in ('vs_cb', 'ps_cb'):
            for slot, cb in enumerate(d[stage]):
                if not cb:
                    continue
                data = blobs.get(cb['blob'], b'')
                print(f"  {stage}[{slot}] {cb['bytes']} bytes blob {cb['blob']}")
                for off in range(0, min(len(data), 256), 16):
                    f = struct.unpack_from('<4f', data, off)
                    i = struct.unpack_from('<4i', data, off)
                    print(f'    +{off:3d}  ' + ' '.join(f'{x:12.5f}' for x in f) + '   | ' + ' '.join(f'{x:11d}' for x in i))
        return

    # Passes: runs of consecutive draws with the same colour and depth targets.
    print('\n== passes (consecutive draws sharing render targets) ==')
    passes = []
    for e in events:
        if e['ev'] == 'draw':
            key = (tuple((r or {}).get('ptr') for r in e['rt']), (e['ds'] or {}).get('ptr'))
            if not passes or passes[-1]['key'] != key:
                passes.append({'key': key, 'draws': [], 'rt': e['rt'], 'ds': e['ds'], 'clears': 0})
            passes[-1]['draws'].append(e)
    for i, ps in enumerate(passes):
        d = ps['draws']
        hist = collections.Counter(name(x['vs']).split('-')[0].removesuffix('_vs_5_0') for x in d)
        rts = ', '.join(fmt_res(r) for r in ps['rt'] if r) or 'none'
        print(f"{i:3d}. seq {d[0]['seq']:4d}-{d[-1]['seq']:4d}  {len(d):4d} draws  rt [{rts}] ds {fmt_res(ps['ds'])}")
        print('       ' + ', '.join(f'{k} x{v}' for k, v in hist.most_common(8)))

    print('\n== shader pairs ==')
    pairs = collections.Counter((name(d['vs']), name(d['ps'])) for d in draws)
    for (vs, ps), n in pairs.most_common(40):
        idx = sum(d['count'] * d['instances'] for d in draws if (name(d['vs']), name(d['ps'])) == (vs, ps))
        print(f'{n:5d} draws {idx:9d} indices/verts  {vs}  |  {ps}')

    print('\n== geometry by vertex shader family ==')
    fams = collections.defaultdict(list)
    for d in draws:
        fams[name(d['vs']).split('-')[0].removesuffix('_vs_5_0')].append(d)
    for fam, ds in sorted(fams.items(), key=lambda kv: -len(kv[1])):
        vbs = {}
        for d in ds:
            for slot, vb in enumerate(d['vb']):
                if vb:
                    vbs[(slot, vb['buf']['ptr'])] = (vb['buf']['bytes'], vb['stride'], vb['buf']['usage'])
        ibs = {d['ib']['buf']['ptr']: (d['ib']['buf']['bytes'], d['ib']['fmt']) for d in ds if d['ib']['buf']}
        lay = collections.Counter(d['layout'] for d in ds)
        print(f'{fam}: {len(ds)} draws, {len(vbs)} vertex buffers, {len(ibs)} index buffers, kinds {dict(collections.Counter(d["kind"] for d in ds))}')
        for (slot, ptr), (size, stride, usage) in sorted(vbs.items(), key=lambda kv: -kv[1][0])[:4]:
            print(f'    vb slot {slot} {ptr}: {size} bytes, stride {stride} ({size // max(stride, 1)} verts), usage {usage}')
        for ptr, (size, f) in sorted(ibs.items(), key=lambda kv: -kv[1][0])[:2]:
            print(f'    ib {ptr}: {size} bytes, {"u16" if f == 57 else "u32" if f == 42 else f}')
        for ptr, n in lay.most_common(2):
            els = layouts.get(ptr)
            desc = ', '.join(f'{e[0]}{e[1]}:{DXGI.get(e[2], e[2])}@{e[3]}+{e[4]}' for e in els) if els else 'layout not seen'
            print(f'    layout x{n}: {desc}')
        d0 = ds[0]
        print('    first draw: vs_cb ' + str([c['bytes'] if c else 0 for c in d0['vs_cb']]) + ' ps_cb ' + str([c['bytes'] if c else 0 for c in d0['ps_cb']])
              + ' vs_srv ' + str([fmt_res(r) for r in d0['vs_srv'] if r]) + ' ps_srv ' + str([fmt_res(r) for r in d0['ps_srv'] if r]))

    if a.draws:
        print('\n== draws ==')
        for d in draws:
            vb = d['vb'][0]
            print(f"{d['seq']:4d} {'D' if d['deferred'] else 'I'} {d['kind'][:12]:12s} n={d['count']:6d} start={d['start']:7d} base={d['base']:6d} "
                  f"{name(d['vs'])[:44]:44s} tex0 {fmt_res(d['ps_srv'][0]):22s} vb {vb['buf']['ptr'][-6:] if vb else '-':6s} "
                  f"blend {d.get('blend', ['-'])[0]} depth {d.get('depth', '-')}")


if __name__ == '__main__':
    main()
