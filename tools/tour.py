#!/usr/bin/env python3
"""Start the private game copy in any deck, without a save from that deck.

The game loads mods from .kpf archives (plain zips). This writes a private mod,
game/mods/zz_tour.kpf, that supplies the chosen deck's mission file under the name of the
first level (earth.mis) and puts it first on the private profile's mod path, so that
"New Game" starts in that deck. `python3 tools/tour.py off` removes it again.

  python3 tools/tour.py medsci2        # New Game now starts in medsci2
  python3 tools/tour.py medsci2 --at=-12,-190,2,90   # ...at that spot, facing 90 degrees
  python3 tools/tour.py list           # mission names
  python3 tools/tour.py off

Only the private copy under remix/game and the private profile under remix/prefix are
touched. The player starts wherever that mission's own start marker is, without the
equipment a real playthrough would have.
"""
import json
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / 'game'
MOD = GAME / 'FMs/ss2rtx_tour.kpf'
CFG = ROOT / 'prefix/pfx/drive_c/users/steamuser/Saved Games/Nightdive Studios/System Shock 2 Remastered/cam_user.cfg'
ENTRY = 'kpf:/mods/zz_tour'
# Later archives override earlier ones for the same mission, as the game's own loader does.
SOURCES = [GAME / 'sshock2.kpf', GAME / 'mods/scp.kpf']


def missions():
    found = {}
    for archive in SOURCES:
        if not archive.exists():
            continue
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():
                if name.lower().endswith('.mis'):
                    found[Path(name).name.lower()[:-4]] = (archive, name)
    return found


def set_mod_path(enabled):
    lines = CFG.read_text(errors='replace').splitlines()
    out = []
    for line in lines:
        if line.startswith('mod_path '):
            parts = [p for p in line[len('mod_path '):].split('+') if p and p != ENTRY]
            if enabled:
                parts.insert(0, ENTRY)
            line = 'mod_path ' + '+'.join(parts)
        out.append(line)
    CFG.write_text('\n'.join(out) + '\n')


def chunk(data, name):
    """Offset and length of a chunk's contents (after its 24-byte header)."""
    inventory = struct.unpack_from('<I', data, 0)[0]
    for i in range(struct.unpack_from('<I', data, inventory)[0]):
        tag, offset, length = struct.unpack_from('<12sII', data, inventory + 4 + i * 20)
        if tag.split(b'\0')[0].decode('latin1') == name:
            return offset + 24, length
    return None


def move_start(data, spot):
    """The player starts at the object that has a player-factory link; put it at `spot`
    (x, y, z and optionally a heading in degrees)."""
    links = chunk(data, 'L$PlayerFac')
    positions = chunk(data, 'P$Position')
    if not links or not positions or links[1] < 14:
        sys.exit('this mission has no player start object to move')
    start = struct.unpack_from('<iiih', data, links[0])[1]
    at, end = positions[0], positions[0] + positions[1]
    data = bytearray(data)
    while at + 8 <= end:
        obj, size = struct.unpack_from('<iI', data, at)
        if obj == start and size >= 22:
            struct.pack_into('<fff', data, at + 8, *spot[:3])
            struct.pack_into('<i', data, at + 20, -1)   # which cell it is in: for the game to work out
            if len(spot) > 3:
                struct.pack_into('<H', data, at + 28, int(spot[3] / 360.0 * 65536) & 0xFFFF)
            return bytes(data)
        at += 8 + size
    sys.exit('the player start object has no position record')


def main():
    spot = None
    for arg in sys.argv[2:]:
        if arg.startswith('--at='):
            spot = [float(v) for v in arg[5:].split(',')]
            sys.argv.remove(arg)
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    what = sys.argv[1].lower()
    known = missions()
    if what == 'list':
        print(' '.join(sorted(known)))
        return
    if what == 'off':
        MOD.unlink(missing_ok=True)
        set_mod_path(False)
        print('tour mod removed')
        return
    if what not in known:
        sys.exit(f'unknown mission {what!r}; try: {" ".join(sorted(known))}')
    archive, name = known[what]
    with zipfile.ZipFile(archive) as z:
        data = z.read(name)
    if spot:
        data = move_start(data, spot)
    info = {'title': f'ss2rtx tour: {what}', 'version': '1', 'author': 'local test',
            'description': f'Test only: New Game starts in {what}.', 'requirements': []}
    with zipfile.ZipFile(MOD, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('modinfo.json', json.dumps(info, indent=1))
        z.writestr('earth.mis', data)
    set_mod_path(True)
    print(f'New Game now starts in {what} ({len(data) >> 20} MiB from {archive.name})')


if __name__ == '__main__':
    main()
