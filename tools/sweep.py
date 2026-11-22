#!/usr/bin/env python3
"""Load every mission path-traced, one after another, and record what happened.

For each mission: tools/tour.py makes New Game start there, launch.py runs the `tour`
scenario in split-screen (engine left, path-traced right), two screenshots are taken (the
start view and one after turning round), and the adapter's log is summarised. Results go
to run/sweep/: one PNG pair per mission, sheet-*.png contact sheets and summary.json.

  python3 tools/sweep.py                 # all missions
  python3 tools/sweep.py medsci2 eng1    # some
  python3 tools/sweep.py --composite     # full-frame path-traced instead of split

Needs the GPU to itself (python3 gpu_session.py begin). About a minute per mission.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'run/sweep'
RUN = ROOT / 'run/game'
ORDER = ['earth', 'station', 'medsci1', 'medsci2', 'eng1', 'eng2', 'hydro1', 'hydro2', 'hydro3', 'ops1', 'ops2', 'ops3', 'ops4',
         'rec1', 'rec2', 'rec3', 'command1', 'command2', 'rick1', 'rick2', 'rick3', 'many', 'shodan']
SHOT_A, TURN_AT, SHOT_B, END = 45, 46.5, 51, 54


def summarise(log):
    text = log.read_text(errors='replace') if log.exists() else ''
    out = {'problems': []}
    m = re.search(r'level loaded: (\d+) vertices, (\d+) indices \((\d+) triangles\), (\d+) polygon bases, vb hash (\w+)', text)
    if m:
        out['level'] = {'vertices': int(m[1]), 'triangles': int(m[3]), 'hash': m[5]}
    m = re.findall(r'mission: (\S+) (?:matched|recognised by its geometry), (\d+) static lights', text)
    if m:
        out['mission'] = m[-1][0]
        out['mission_lights'] = int(m[-1][1])
    m = re.findall(r'remix: frame \d+: (\d+) object draws -> (\d+) instances \((\d+) (?:glass|see-through)\) \+ (\d+) glow layers \((\d+) translucent skipped\), '
                   r'(\d+) character draws -> (\d+), (\d+) models, (\d+) lights, (\d+) materials', text)
    if m:
        keys = ['object_draws', 'instances', 'glass', 'glow_layers', 'see_through_skipped', 'character_draws', 'characters', 'models', 'lights', 'materials']
        out['frame'] = dict(zip(keys, map(int, m[-1])))
    m = re.findall(r'remix: (\d+) frames composited, mean ([\d.]+) ms max ([\d.]+) ms per frame for render \+ hand-off, (\d+) failures', text)
    if m:
        out['composite'] = {'frames': int(m[-1][0]), 'mean_ms': float(m[-1][1]), 'failures': int(m[-1][3])}
    m = re.findall(r'remix: (\d+) level polygons are two-layer surfaces \((\d+) decal groups\), (\d+) glow groups', text)
    if m:
        out['layers'] = {'two_layer_polygons': int(m[-1][0]), 'decal_groups': int(m[-1][1]), 'glow_groups': int(m[-1][2])}
    out['animated_polygons'] = len(re.findall(r'changes texture while shown', text))
    m = re.findall(r'up to (\d+) sprite draws per frame inside the world pass', text)
    if m:
        out['sprite_draws_lost'] = max(map(int, m))
    m = re.findall(r'engine ambient ([\d.]+) ([\d.]+) ([\d.]+), (\d+) directional', text)
    if m:
        out['ambient'] = [float(x) for x in m[-1][:3]]
        out['directional_lights'] = int(m[-1][3])
    m = re.findall(r'of (\d+) mission lights, (\d+) follow the engine.s current value and (\d+) replay their own rhythm; (\d+) are dark', text)
    if m:
        out['live_lights'] = {'followed': int(m[-1][1]), 'replayed': int(m[-1][2]), 'dark': int(m[-1][3])}
    m = re.findall(r'(\d+) dynamic level light\(s\) in use', text)
    if m:
        out['dynamic_level_lights'] = max(map(int, m))
    for line in text.splitlines():
        if re.search(r'FAILED|could not|unexpected|ignored|not readable|-> -\d+$|-> [1-9]\d*$|was NOT accepted|CreateMesh\(|CreateMaterial ->|no mission', line):
            out['problems'].append(line.split('] ', 2)[-1][:160])
    out['problems'] = sorted(set(out['problems']))[:12]
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('missions', nargs='*')
    p.add_argument('--composite', action='store_true')
    a = p.parse_args()
    missions = a.missions or ORDER
    OUT.mkdir(parents=True, exist_ok=True)
    summary_path = OUT / 'summary.json'
    summary = json.loads(summary_path.read_text()) if summary_path.exists() else {}
    for name in missions:
        r = subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), name], capture_output=True, text=True)
        if r.returncode:
            print(name, 'tour.py failed:', r.stderr.strip() or r.stdout.strip())
            continue
        cmd = [sys.executable, str(ROOT / 'launch.py'), '--seconds', str(END), '--scenario', 'tour', '--composite' if a.composite else '--split',
               '--screenshot-at', str(SHOT_A), '--screenshot-at', str(SHOT_B), '--keys', f'{TURN_AT}:k:0.9']
        run = subprocess.run(cmd, capture_output=True, text=True)
        info = summarise(RUN / 'ss2rtx.log')
        info['launcher_exit'] = run.returncode
        result = RUN / 'result.json'
        if result.exists():
            try:
                info['result'] = {k: v for k, v in json.loads(result.read_text()).items() if k in ('exit_code', 'timed_out', 'killed', 'seconds')}
            except ValueError:
                pass
        remix_log = RUN / 'remix-dxvk.log'
        if remix_log.exists():
            info['remix_errors'] = sorted({l.split('err:', 1)[1].strip()[:140] for l in remix_log.read_text(errors='replace').splitlines() if ' err:' in l})[:8]
        for tag, t in (('a', SHOT_A), ('b', SHOT_B)):
            src = RUN / f'window-{int(t):03d}s.png'
            if src.exists():
                shutil.copy(src, OUT / f'{name}-{tag}.png')
        shutil.copy(RUN / 'ss2rtx.log', OUT / f'{name}.log') if (RUN / 'ss2rtx.log').exists() else None
        loaded = info.get('mission', '?')
        ok = name in loaded and info.get('composite', {}).get('failures', 1) == 0
        info['ok'] = ok
        summary[name] = info
        summary_path.write_text(json.dumps(summary, indent=1) + '\n')
        print(f"{name:9s} {'ok  ' if ok else 'LOOK'} loaded {loaded.split(':')[-1]:13s} lights {info.get('mission_lights', '-'):>4} "
              f"tris {info.get('level', {}).get('triangles', '-'):>6} frame {info.get('composite', {}).get('mean_ms', '-')} ms "
              f"problems {len(info['problems'])}", flush=True)
    subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), 'off'], capture_output=True)
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    done = [m for m in ORDER if (OUT / f'{m}-a.png').exists()]
    for page in range(0, len(done), 6):
        chunk = done[page:page + 6]
        sheet = Image.new('RGB', (1280, 360 * len(chunk)))
        draw = ImageDraw.Draw(sheet)
        for row, m in enumerate(chunk):
            for col, tag in enumerate(('a', 'b')):
                f = OUT / f'{m}-{tag}.png'
                if f.exists():
                    sheet.paste(Image.open(f).convert('RGB').resize((640, 360)), (640 * col, 360 * row))
            draw.text((8, 360 * row + 6), m, fill=(255, 255, 0))
        sheet.save(OUT / f'sheet-{page // 6 + 1}.png')
    print('contact sheets in', OUT)


if __name__ == '__main__':
    main()
