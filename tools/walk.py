#!/usr/bin/env python3
"""Look at several places in every deck, path traced and engine-rendered, without playing.

  python3 tools/walk.py                 # three places in each of the 23 missions (over an hour)
  python3 tools/walk.py medsci2 rec1    # only these
  python3 tools/walk.py --spots 5 eng1

For each mission it reads the level's cells from the mission file and picks roomy air
cells far from each other and from the player's start. For each it starts a new game
there (tools/tour.py --at), takes a path-traced screenshot, switches path tracing off and
takes the engine's. Results go to run/walk/: one side-by-side image per place (left path
traced, right engine), sheet-*.png contact sheets and summary.json with each place's mean
brightness both ways and anything the adapter's log complained about.

A place can turn out to be inside something or over a drop; then both halves show it and
the pair says little. This finds faults (a missing level, a black frame, a failed
hand-off), not beauty.
"""
import json
import math
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageStat

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / 'game'
RUN = ROOT / 'run/game'
OUT = ROOT / 'run/walk'
SOURCES = [GAME / 'sshock2.kpf', GAME / 'mods/scp.kpf']   # later archives override earlier ones


def mission_data(name):
    found = None
    for archive in SOURCES:
        if not archive.exists():
            continue
        with zipfile.ZipFile(archive) as z:
            for entry in z.namelist():
                if Path(entry).name.lower() == name + '.mis':
                    found = z.read(entry)
    return found


def chunks(data):
    inventory = struct.unpack_from('<I', data, 0)[0]
    out = {}
    for i in range(struct.unpack_from('<I', data, inventory)[0]):
        tag, offset, length = struct.unpack_from('<12sII', data, inventory + 4 + i * 20)
        out[tag.split(b'\0')[0].decode('latin1')] = (offset + 24, length)
    return out


def air_cells(data, ch):
    """(centre, radius) of every air cell."""
    kind = next(k for k in ('WREXT', 'WRRGB', 'WR') if k in ch)
    offset, length = ch[kind]
    w = data[offset:offset + length]
    if kind == 'WREXT':
        p, count, texel, render_poly = 28, struct.unpack_from('<I', w, 24)[0], 2 if struct.unpack_from('<I', w, 12)[0] == 0 else 4, 52
    else:
        p, count, texel, render_poly = 8, struct.unpack_from('<I', w, 4)[0], 2 if kind == 'WRRGB' else 1, 48
    cells = []
    for _ in range(count):
        nv, npoly, nrp, _nportal, nplanes, medium = struct.unpack_from('<BBBBBB', w, p)
        nanim = w[p + 13]
        cx, cy, cz, radius = struct.unpack_from('<ffff', w, p + 15)
        if medium == 1:
            cells.append(((cx, cy, cz), radius))
        p += 31 + 12 * nv + 8 * npoly + render_poly * nrp
        p += 4 + struct.unpack_from('<I', w, p)[0] + 16 * nplanes + 2 * nanim
        infos = [struct.unpack_from('<hhHBBIII', w, p + 20 * i) for i in range(nrp)]
        p += 20 * nrp
        for info in infos:
            p += (1 + bin(info[7]).count('1')) * info[3] * info[4] * texel
        p += 4 + 2 * struct.unpack_from('<I', w, p)[0]
    return cells


def player_start(data, ch):
    if 'L$PlayerFac' not in ch or 'P$Position' not in ch or ch['L$PlayerFac'][1] < 14:
        return None
    start = struct.unpack_from('<iiih', data, ch['L$PlayerFac'][0])[1]
    at, end = ch['P$Position'][0], ch['P$Position'][0] + ch['P$Position'][1]
    while at + 8 <= end:
        obj, size = struct.unpack_from('<iI', data, at)
        if obj == start and size >= 12:
            return struct.unpack_from('<fff', data, at + 8)
        at += 8 + size
    return None


def pick_spots(name, wanted):
    data = mission_data(name)
    ch = chunks(data)
    roomy = [c for c, r in air_cells(data, ch) if 5.0 <= r <= 14.0]
    chosen = [player_start(data, ch) or (0.0, 0.0, 0.0)]
    while roomy and len(chosen) <= wanted:   # each time the cell farthest from everything chosen so far
        best = max(roomy, key=lambda c: min(math.dist(c, other) for other in chosen))
        chosen.append(best)
        roomy.remove(best)
    return chosen[1:]


def problems(log):
    out = []
    if not log.exists():
        return ['no adapter log']
    for line in log.read_text(errors='replace').splitlines():
        low = line.lower()
        if any(word in low for word in ('fail', 'could not', 'not trusted', 'unexpected', 'error')) and '0 failures' not in low:
            out.append(line.split('] ', 2)[-1][:160])
    return sorted(set(out))[:8]


def main():
    args = sys.argv[1:]
    spots_each = 3
    if '--spots' in args:
        i = args.index('--spots')
        spots_each = int(args[i + 1])
        del args[i:i + 2]
    names = args or [l for l in subprocess.check_output([sys.executable, str(ROOT / 'tools/tour.py'), 'list'], text=True).split()]
    OUT.mkdir(parents=True, exist_ok=True)
    summary_path = OUT / 'summary.json'
    summary = json.loads(summary_path.read_text()) if summary_path.exists() else {}
    for name in names:
        for n, spot in enumerate(pick_spots(name, spots_each)):
            if subprocess.run([sys.executable, str(ROOT / 'tools/steam_game_running.py')]).returncode == 0:
                sys.exit('the Steam copy of the game is running; stopping')
            key = f'{name}-{n}'
            at = ','.join(f'{v:.1f}' for v in spot)
            subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), name, f'--at={at},0'], check=True, stdout=subprocess.DEVNULL)
            subprocess.run([sys.executable, str(ROOT / 'launch.py'), '--seconds', '51', '--scenario', 'tour', '--composite',
                            '--keys', '44.4:rtx_off', '--screenshot-at', '43.5', '--screenshot-at', '47.6'],
                           cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            info = {'at': at, 'problems': problems(RUN / 'ss2rtx.log')}
            shots = [RUN / 'window-043.5s.png', RUN / 'window-047.6s.png']
            if all(s.exists() for s in shots):
                traced, engine = (Image.open(s).convert('RGB') for s in shots)
                info['path_traced'] = round(ImageStat.Stat(traced.convert('L')).mean[0], 1)
                info['engine'] = round(ImageStat.Stat(engine.convert('L')).mean[0], 1)
                pair = Image.new('RGB', (1280, 360))
                pair.paste(traced.resize((640, 360)), (0, 0))
                pair.paste(engine.resize((640, 360)), (640, 0))
                pair.save(OUT / f'{key}.png')
            else:
                info['problems'].append('no screenshots')
            log = RUN / 'ss2rtx.log'
            if log.exists():
                (OUT / f'{key}.log').write_bytes(log.read_bytes())
            summary[key] = info
            summary_path.write_text(json.dumps(summary, indent=1) + '\n')
            ratio = info['path_traced'] / info['engine'] if info.get('engine') else float('nan')
            print(f"{key:12s} at {at:22s} path traced {info.get('path_traced', '-'):>6} engine {info.get('engine', '-'):>6} ratio {ratio:4.2f} problems {len(info['problems'])}", flush=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), 'off'], stdout=subprocess.DEVNULL)
    pairs = sorted(OUT.glob('*-[0-9]*.png'))
    for sheet in range(0, len(pairs), 12):
        page = Image.new('RGB', (1280 * 2, 360 * 6))
        for i, f in enumerate(pairs[sheet:sheet + 12]):
            page.paste(Image.open(f), ((i % 2) * 1280, (i // 2) * 360))
        page.resize((1280 * 2 // 2, 360 * 6 // 2)).save(OUT / f'sheet-{sheet // 12 + 1}.png')
    print('contact sheets in', OUT)


if __name__ == '__main__':
    main()
