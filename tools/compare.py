#!/usr/bin/env python3
"""Side-by-side of the game's own frame and the Remix readback from the same run.

  python3 tools/compare.py run/game            # writes run/game/compare.png
"""
import sys
from pathlib import Path
from PIL import Image

run = Path(sys.argv[1] if len(sys.argv) > 1 else 'run/game')
shots = sorted(run.glob('window-*.png'))
remix = sorted(run.glob('remix-*.bmp'))
if not shots or not remix:
    sys.exit('need a window-*.png and a remix-*.bmp in ' + str(run))
a = Image.open(shots[-1]).convert('RGB')
tiles = [a] + [Image.open(r).convert('RGB').resize(a.size) for r in remix]
cols = 2
rows = (len(tiles) + cols - 1) // cols
out = Image.new('RGB', (a.width * cols + 8 * (cols - 1), a.height * rows + 8 * (rows - 1)), (40, 40, 40))
for i, t in enumerate(tiles):
    out.paste(t, ((i % cols) * (a.width + 8), (i // cols) * (a.height + 8)))
out = out.resize((out.width // 2, out.height // 2))
out.save(run / 'compare.png')
print(run / 'compare.png', 'first tile: game', shots[-1].name, '| then Remix:', ', '.join(r.name for r in remix))
