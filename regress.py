#!/usr/bin/env python3
"""Regression run: play a fixed scenario and compare what is on screen with stored baselines.

  python3 regress.py                 # run, compare with tests/baseline/, report
  python3 regress.py --update        # run and store the result as the new baseline

One run of the `ramsey` scenario in split-screen mode, with screenshots at fixed times
from a camera that does not move. Each screenshot holds the engine's own image (left half)
and the path-traced image (right half), so one comparison covers both: an engine-side
difference means the adapter disturbed the game, a Remix-side difference means the scene
we submit changed.

Images are compared after shrinking to 64x36, which averages out path-tracing noise; the
reported number is the mean absolute difference on a 0-255 scale. It needs the GPU
(free the GPU first: gpu_session.py begin).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent
BASELINE = ROOT / 'tests/baseline'
RUN = ROOT / 'run/game'
TIMES = [30, 36, 42]
THRESHOLD = 6.0   # mean absolute difference per half, 0-255


def halves(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    left = im.crop((0, 0, w // 2, h)).resize((32, 36), Image.BILINEAR)
    right = im.crop((w // 2, 0, w, h)).resize((32, 36), Image.BILINEAR)
    return left, right


def difference(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    return sum(abs(x - y) for x, y in zip(pa, pb)) / len(pa)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--update', action='store_true', help='store this run as the baseline')
    a = p.parse_args()
    cmd = [sys.executable, str(ROOT / 'launch.py'), '--seconds', str(TIMES[-1] + 4), '--scenario', 'ramsey', '--split']
    for t in TIMES:
        cmd += ['--screenshot-at', str(t)]
    run = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    shots = [RUN / f'window-{t:03d}s.png' for t in TIMES]
    missing = [s.name for s in shots if not s.exists()]
    if missing:
        print(run.stdout[-2000:])
        sys.exit(f'run produced no screenshot for: {missing}')
    log = (RUN / 'ss2rtx.log').read_text(errors='replace') if (RUN / 'ss2rtx.log').exists() else ''
    composited = 'frames composited' in log
    failures = log.count('composite failed')

    if a.update:
        BASELINE.mkdir(parents=True, exist_ok=True)
        for s in shots:
            shutil.copy2(s, BASELINE / f'ramsey-{s.name}')
        print(f'baseline updated: {len(shots)} images in {BASELINE}')
        return
    results, ok = [], composited and failures == 0
    for s in shots:
        base = BASELINE / f'ramsey-{s.name}'
        if not base.exists():
            sys.exit(f'no baseline {base.name}; run with --update first')
        (bl, br), (cl, cr) = halves(base), halves(s)
        engine, remix = difference(bl, cl), difference(br, cr)
        passed = engine <= THRESHOLD and remix <= THRESHOLD
        ok = ok and passed
        results.append({'image': s.name, 'engine_half_diff': round(engine, 2), 'remix_half_diff': round(remix, 2), 'pass': passed})
        print(f"{s.name}: engine half {engine:5.2f}, path-traced half {remix:5.2f}  {'ok' if passed else 'DIFFERS'}")
    print(f"composited: {composited}, composite failures: {failures}")
    (RUN / 'regress.json').write_text(json.dumps({'pass': ok, 'threshold': THRESHOLD, 'results': results}, indent=2) + '\n')
    print('REGRESSION PASS' if ok else 'REGRESSION FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
