#!/usr/bin/env python3
"""Give the game's surfaces relief and varied gloss: a starter set of material maps.

  python3 tools/author.py                    # for the private test copy (game/)
  python3 tools/author.py --steam            # for the installed Steam game
  python3 tools/author.py --target <folder>  # any other copy of the game
  python3 tools/author.py --remove           # take the generated maps and lines out again

The game's textures are colour only. For each texture the adapter has met so far (it keeps
them in <game>/ss2rtx_cache/textures) this works out, from the texture itself, a normal
map (the relief its light and dark suggest) and a roughness map (how polished it looks,
smoother on raised and worn parts, rougher in seams), writes them to
<game>/ss2rtx/authored/ and adds lines that use them to <game>/ss2rtx/materials.txt,
between two marker lines. Lines of your own in that file are kept and win over the
generated ones. The adapter re-reads the file while the game runs.

Run it again after playing further: decks you have not visited yet have textures it has
not seen. The maps are made on your machine from your copy of the game; none are part of
this project. Needs numpy and Pillow.

materials/handmade.txt in this repository holds values chosen by hand for the textures
that cover most of the decks (what each surface is: steel, enamel, flesh, carpet). They
are merged in: a texture listed there gets its roughness and metalness from the table and
its relief from the script. --hand-only writes those values alone, with no maps.

For the rest this is a starting point, not art: relief guessed from colour is wrong wherever a texture
has painted shadows or writing. Edit or delete single lines of the generated block as you
see fit (a line of your own for the same texture, after the block, replaces it).
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import install  # noqa: E402  (find_game)

BEGIN, END = install.MATERIALS_BEGIN, install.MATERIALS_END
DXGI_BC4, DXGI_BC5 = 80, 83


def blur(a, sigma):
    """Gaussian blur that wraps round the edges, as the textures themselves do."""
    fy = np.fft.fftfreq(a.shape[0])[:, None]
    fx = np.fft.rfftfreq(a.shape[1])[None, :]
    return np.fft.irfft2(np.fft.rfft2(a) * np.exp(-2.0 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy)), s=a.shape)


def bc4(channel):
    """One 8-bit channel as BC4 blocks (two end values and sixteen 3-bit choices between them)."""
    h, w = channel.shape
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        channel = np.pad(channel, ((0, ph - h), (0, pw - w)), mode='edge')
    blocks = channel.reshape(ph // 4, 4, pw // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16).astype(np.float32)
    hi, lo = blocks.max(axis=1), blocks.min(axis=1)
    span = np.maximum(hi - lo, 1e-6)
    t = np.rint((hi[:, None] - blocks) / span[:, None] * 7.0).astype(np.uint64)   # 0 at the high end .. 7 at the low
    index = np.where(t == 0, 0, np.where(t == 7, 1, t + 1)).astype(np.uint64)
    index[hi == lo] = 0
    bits = np.zeros(len(blocks), dtype=np.uint64)
    for i in range(16):
        bits |= index[:, i] << np.uint64(3 * i)
    out = np.zeros((len(blocks), 8), dtype=np.uint8)
    out[:, 0] = hi.astype(np.uint8)
    out[:, 1] = lo.astype(np.uint8)
    for i in range(6):
        out[:, 2 + i] = (bits >> np.uint64(8 * i)) & np.uint64(255)
    return out


def write_dds(path, levels, dxgi):
    """levels: arrays of shape (h, w) for BC4 or (h, w, 2) for BC5, largest first."""
    h, w = levels[0].shape[:2]
    body = []
    for level in levels:
        if dxgi == DXGI_BC5:
            body.append(np.concatenate([bc4(level[..., 0]), bc4(level[..., 1])], axis=1).tobytes())
        else:
            body.append(bc4(level).tobytes())
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, len(body[0]), 0, len(levels))
    header += struct.pack('<II4s20x', 32, 0x4, b'DX10')
    header += struct.pack('<IIII4x', 0x1000 | 0x8 | 0x400000, 0, 0, 0)
    header += struct.pack('<IIIII', dxgi, 3, 0, 1, 0)
    path.write_bytes(header + b''.join(body))


def mip_chain(a):
    """Halved down to 4 texels, by averaging."""
    levels = [a]
    while min(levels[-1].shape[:2]) > 4:
        b = levels[-1].astype(np.float32)
        h, w = b.shape[0] // 2 * 2, b.shape[1] // 2 * 2
        b = b[:h, :w]
        b = (b[0::2, 0::2] + b[1::2, 0::2] + b[0::2, 1::2] + b[1::2, 1::2]) / 4.0
        levels.append(np.rint(b).astype(np.uint8))
    return levels


def hand_table():
    """hash -> (roughness, metallic) from materials/handmade.txt."""
    table = {}
    path = ROOT / 'materials/handmade.txt'
    if path.exists():
        for line in path.read_text().splitlines():
            parts = line.split('#')[0].split()
            if len(parts) == 3:
                table[parts[0].lower()] = (float(parts[1]), float(parts[2]))
    return table


def maps_for(image, relief, base=None):
    """(normal map as two octahedral channels, roughness map, note) or (None, None, why not)."""
    rgba = np.asarray(image.convert('RGBA'), dtype=np.float32) / 255.0
    h, w = rgba.shape[:2]
    if min(h, w) < 64:
        return None, None, 'small'
    if rgba[..., 3].mean() < 0.98:
        return None, None, 'see-through parts'   # cut-outs, decals, glows: not a surface of their own
    if max(h, w) > 512:   # relief guessed from colour holds no finer detail than this
        image = image.convert('RGBA').resize((max(w * 512 // max(h, w), 4) // 4 * 4, max(h * 512 // max(h, w), 4) // 4 * 4), Image.BOX)
        rgba = np.asarray(image, dtype=np.float32) / 255.0
        h, w = rgba.shape[:2]
    rgb = rgba[..., :3]
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    if lum.std() < 0.01:
        return None, None, 'flat colour'
    spread = rgb.max(axis=2) - rgb.min(axis=2)
    # A display or a lit sign: mostly black with strongly coloured, bright writing. Its
    # light and dark are a picture, not relief.
    if float((lum < 0.04).mean()) > 0.3 and float(((spread > 0.45) & (rgb.max(axis=2) > 0.5)).mean()) > 0.01:
        return None, None, 'screens and signs'
    saturation = float(spread.mean())
    # Height: light is raised, dark is sunk, at two scales (panels and seams; scratches and grain).
    broad = lum - blur(lum, max(h, w) / 16.0)
    fine = lum - blur(lum, 1.5)
    height = 0.65 * broad / max(float(broad.std()), 1e-4) + 0.35 * fine / max(float(fine.std()), 1e-4)
    height = np.clip(height / 3.0, -1.0, 1.0)   # within three deviations
    smooth = blur(height, 0.8)
    dx = (np.roll(smooth, -1, axis=1) - np.roll(smooth, 1, axis=1)) * 0.5
    dy = (np.roll(smooth, -1, axis=0) - np.roll(smooth, 1, axis=0)) * 0.5
    rms = float(np.sqrt((dx * dx + dy * dy).mean()))
    k = min(relief / max(rms, 1e-5), 14.0)   # a typical slope of `relief`, without blowing up a nearly flat texture's noise
    nx, ny, nz = -dx * k, -dy * k, np.ones_like(dx)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx / norm, ny / norm, nz / norm
    # Remix's encoding: the hemisphere folded onto a square (octahedral), two channels.
    scale = 1.0 / (np.abs(nx) + np.abs(ny) + nz)
    px, py = nx * scale, ny * scale
    normal = np.stack([(px + py) * 0.5 + 0.5, (px - py) * 0.5 + 0.5], axis=2)
    # Roughness: grey, pale surfaces read as painted metal or plastic (smoother); coloured
    # and dark ones as matt. Raised parts are worn smoother, seams hold dirt.
    note = 'chosen by hand'
    if base is None:
        note = 'guessed'
        base = 0.5 if saturation < 0.12 else 0.62 if saturation < 0.25 else 0.78
        if float(lum.mean()) < 0.1:
            base = max(base, 0.65)
    rough = np.clip(base - 0.16 * height, 0.25, 0.95)
    to8 = lambda a: np.rint(np.clip(a, 0.0, 1.0) * 255.0).astype(np.uint8)
    return to8(normal), to8(rough), f'base roughness {base:.2f}, {note}'


def strip_block(text):
    if BEGIN not in text:
        return text
    head, rest = text.split(BEGIN, 1)
    tail = rest.split(END, 1)[1] if END in rest else ''
    return head.rstrip('\n') + ('\n' if head.strip() else '') + tail.lstrip('\n')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--steam', action='store_true', help='the Steam installation')
    p.add_argument('--target', type=Path, help='game folder (default: the private test copy)')
    p.add_argument('--remove', action='store_true', help='remove the generated maps and lines')
    p.add_argument('--relief', type=float, default=0.12, help='how strong the relief is: the typical slope (default 0.12)')
    p.add_argument('--only', nargs='*', help='texture hashes to (re)make; default: all')
    p.add_argument('--hand-only', action='store_true', help='only the values of materials/handmade.txt, no generated maps')
    a = p.parse_args()
    game = install.find_game() if a.steam else (a.target.resolve() if a.target else ROOT / 'game')
    if not game or not (game / 'SystemShock2Remastered.exe').exists():
        sys.exit('game folder not found')
    table = game / 'ss2rtx/materials.txt'
    out = game / 'ss2rtx/authored'
    own = strip_block(table.read_text()) if table.exists() else ''
    own = '\n'.join(l for l in own.splitlines() if l != install.MATERIALS_DECLINED) + ('\n' if own else '')
    if a.remove:
        shutil.rmtree(out, ignore_errors=True)
        if table.exists():
            table.write_text(own + install.MATERIALS_DECLINED + '\n')
        print(f'removed the generated maps and lines from {game / "ss2rtx"}')
        return
    hand = hand_table()
    if a.hand_only:
        shutil.rmtree(out, ignore_errors=True)
        lines = [f'{h.upper()} roughness={r:g} metallic={m:g}' for h, (r, m) in sorted(hand.items())]
        (game / 'ss2rtx').mkdir(exist_ok=True)
        table.write_text('\n'.join([BEGIN, *lines, END]) + '\n' + ('\n' + own if own.strip() else ''))
        print(f'{len(lines)} hand-made material lines written to {table}')
        return
    cache = game / 'ss2rtx_cache/textures'
    files = sorted(cache.glob('*.dds'))
    if not files:
        sys.exit(f'no textures in {cache}: play the game with ss2rtx first, the adapter keeps what it meets there')
    out.mkdir(parents=True, exist_ok=True)
    lines, skipped = [], {}
    wanted = {x.lower() for x in a.only} if a.only else None
    for n, f in enumerate(files):
        name = f.stem.lower()
        normal_file, rough_file = out / f'{name}_n.dds', out / f'{name}_r.dds'
        metal = f' metallic={hand[name][1]:g}' if name in hand and hand[name][1] > 0 else ''
        if wanted is not None and name not in wanted:
            if normal_file.exists() and rough_file.exists():
                lines.append(f'{name.upper()} normal=authored\\{name}_n.dds roughness_tex=authored\\{name}_r.dds{metal}')
            continue
        try:
            image = Image.open(f)
            image.load()
        except Exception as error:   # a format Pillow cannot read
            skipped['unreadable'] = skipped.get('unreadable', 0) + 1
            continue
        normal, rough, note = maps_for(image, a.relief, hand[name][0] if name in hand else None)
        if normal is None:
            if name in hand:   # no maps for it, but its values are known
                lines.append(f'{name.upper()} roughness={hand[name][0]:g} metallic={hand[name][1]:g}   # chosen by hand')
            skipped[note] = skipped.get(note, 0) + 1
            continue
        write_dds(normal_file, mip_chain(normal), DXGI_BC5)
        write_dds(rough_file, mip_chain(rough), DXGI_BC4)
        lines.append(f'{name.upper()} normal=authored\\{name}_n.dds roughness_tex=authored\\{name}_r.dds{metal}   # {note}')
        if sys.stdout.isatty():
            print(f'\r{n + 1} of {len(files)}', end='', flush=True)
    if sys.stdout.isatty():
        print()
    block = '\n'.join([BEGIN, *lines, END]) + '\n'
    # The generated block first: a later line for the same texture replaces an earlier one.
    table.write_text(block + ('\n' + own if own.strip() else ''))
    size = sum(x.stat().st_size for x in out.glob('*.dds'))
    print(f'{len(lines)} textures given a normal and a roughness map ({size >> 20} MiB in {out})')
    print('left as they are: ' + (', '.join(f'{v} {k}' for k, v in sorted(skipped.items())) or 'none'))
    print(f'lines written to {table}; the adapter picks them up while the game runs')


if __name__ == '__main__':
    main()
