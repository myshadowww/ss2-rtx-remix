#!/usr/bin/env python3
"""The same four views path traced and engine-rendered, side by side.

  python3 tools/survey.py medsci2 -12,-190,2,0 icu [extra launch.py arguments...]

Starts a new game in <mission> with the player put at x,y,z (facing the heading, in
degrees), turns in four steps and at each takes a path-traced screenshot and an engine
one (a few seconds apart, so anything that moves differs). Writes run/survey/<tag>.png (left: path traced, right: engine), the adapter log beside
it, and prints each shot's mean brightness. Standing spots: an air cell's middle is safe;
object positions usually are not.
"""
import subprocess, sys, shutil
from pathlib import Path
from PIL import Image, ImageChops, ImageStat
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'run/survey'
OUT.mkdir(parents=True, exist_ok=True)
mission, spot, tag = sys.argv[1:4]
extra = sys.argv[4:]
start = 36.0   # --start=<seconds>: the first shot; later for decks that load slowly
for x in list(extra):
    if x.startswith('--start='):
        start = float(x.split('=', 1)[1])
        extra.remove(x)
subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), mission, f'--at={spot}'], check=True, stdout=subprocess.DEVNULL)
keys, shots = [], []
t = start
# Screenshot times count from launch and key times from the game's first frame, about a
# second later, and a screenshot can lag: so each shot is taken a good two seconds after
# the switch it depends on. Path tracing also needs over a second to settle after it is
# switched back on.
for view in range(4):
    shots += [t, t + 3.2]
    keys += [f'{t + 0.4}:rtx_off', f'{t + 3.6}:rtx_on', f'{t + 3.7}:k:0.33']
    t += 7.5
cmd = [sys.executable, str(ROOT / 'launch.py'), '--seconds', str(int(t + 2)), '--scenario', 'tour', '--composite', '--keys', ';'.join(keys)]
for s in shots: cmd += ['--screenshot-at', str(s)]
subprocess.run(cmd + extra, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run([sys.executable, str(ROOT / 'tools/tour.py'), 'off'], stdout=subprocess.DEVNULL)
files = sorted((ROOT / 'run/game').glob('window-*.png'))
W, H = 640, 360
sheet = Image.new('RGB', (W * 2, H * 4))
for i, f in enumerate(files[:8]):
    im = Image.open(f).convert('RGB')
    mean = ImageStat.Stat(im.convert('L')).mean[0]
    print(f'view {i // 2} {"path traced" if i % 2 == 0 else "engine     "} mean {mean:5.1f}  {f.name}')
    sheet.paste(im.resize((W, H)), ((i % 2) * W, (i // 2) * H))
# The launcher's clock and the game's differ by a second or two from run to run. If a
# switch slipped past its shot, both shots of a view are the same frame: say so.
for view in range(len(files[:8]) // 2):
    a, b = (Image.open(f).convert('RGB') for f in files[view * 2:view * 2 + 2])
    if sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) < 0.05:
        print(f'view {view}: both shots are the same frame; the switch missed its shot, run it again')
sheet.save(OUT / f'{tag}.png')
shutil.copy(ROOT / 'run/game/ss2rtx.log', OUT / f'{tag}.log')
print(OUT / f'{tag}.png')
