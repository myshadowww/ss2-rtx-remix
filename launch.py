#!/usr/bin/env python3
"""Launch the private SS2 copy under Proton with the ss2rtx adapter installed.

  python3 launch.py                       # play; everything is closed when the game exits
  python3 launch.py --seconds 45          # automated smoke run: closes itself after 45 s
  python3 launch.py --stock               # no adapter at all (stock game, for comparison)
  python3 launch.py --passthrough         # adapter loaded but with SS2RTX_DISABLE=1

Whatever happens (normal exit, timeout, Ctrl-C), every window and Wine process of the
private prefix is shut down before this script returns. Evidence goes to run/game/.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from runtest import PROTON, ROOT, close_everything, gpu_uuid, prefix_processes, screenshot, winpath

GAME = ROOT / 'game'
RUN = ROOT / 'run/game'

REMIX_RUNTIME = ROOT / 'runtime/remix-1.6.1-x64'

# Remix settings for SS2. The first two are required under Proton (see README.md).
REMIX_CONF = [
    'rtx.dlssPreset = 2',
    'rtx.dlfg.enable = False',
    # Remix's default GUI input method registers raw keyboard input with RIDEV_NOLEGACY for
    # the whole process, which stops the game receiving key presses, and takes over raw
    # mouse input. Must be off whenever Remix shares a process with the game.
    'rtx.useNewGuiInputMethod = False',
    'rtx.texturemanager.neverDowngradeTextures = True',   # streaming otherwise leaves every texture at a small mip
    # Remix adds these camera effects by default; the game has none of them and they read as blur.
    'rtx.postfx.enableMotionBlur = False',
    'rtx.postfx.enableChromaticAberration = False',
    'rtx.postfx.enableVignette = False',
    'rtx.bloom.enable = False',   # the game blooms the finished frame itself
    # Brightness as the engine's: a fixed exposure and no tone curve. The adapter's light scale
    # makes an engine light value of x come out as radiance albedo * x / pi, so an exposure of
    # log2(pi) = 1.65 stops puts it back on screen at the engine's level. Remix's defaults
    # (auto-exposure and a local tone mapper) brighten dark rooms and flatten the image.
    'rtx.tonemappingMode = 0',
    'rtx.autoExposure.enabled = False',
    'rtx.tonemap.tonemappingEnabled = False',
    'rtx.tonemap.exposureBias = 1.65',
    'rtx.zUp = True',            # Dark Engine world: Z up, right-handed
    'rtx.sceneScale = 0.0328',   # 1 cm per game unit; a Dark Engine unit is about one foot
]

# Unattended routes into a level. Keys are posted by the adapter to the game's own window.
# 'ramsey': skip the intro films, Main menu -> Load Game -> Quick Save (Ramsey Center, the
# opening area). The level is on screen about 25 s after launch.
_MENU = '14:down;15.5:enter;17.5:down;18.1:down'   # main menu -> Load Game -> cursor on Quick Save
SCENARIOS = {
    'ramsey': (['-skipmovies'], _MENU + ';19.2:enter'),
    # Slot saves made by the user on 22 Nov 2026 (copies in saves/):
    'medsci-arrival': (['-skipmovies'], _MENU + ';18.7:down;19.8:enter'),          # slot 1, just arrived on the MedSci deck
    'medsci': (['-skipmovies'], _MENU + ';18.7:down;19.3:down;20.4:enter'),        # slot 2, MedSci a little later
    'pistol': (['-skipmovies'], _MENU + ';18.7:down;19.3:down;19.9:down;21:enter'),          # slot 3: a copy of the Steam quick-save (pistol in hand)
    # New Game in the campaign made by tools/tour.py (run that first): New Game, Campaign,
    # pick the second entry, Tab twice to Confirm, Start Game, skip the intro. In the level
    # at about 34 s.
    'tour': (['-skipmovies'], '14:enter;16:up;17:up;18:enter;20:down;21:down;22:enter;24:tab;24.5:tab;25.5:enter;'
                              '28:down;28.6:down;29.6:enter;33:esc'),   # one Esc for the intro film: a second would open the game's menu
}


def memory_sample():
    """(resident MiB, GPU MiB) of the game process, or None if it is not running.

    Several processes of the prefix carry the game's name on their command line (the
    Proton launcher among them); the game itself is the one using the most memory.
    """
    best = None
    for pid in prefix_processes():
        try:
            if b'SystemShock2Remastered.exe' not in Path(f'/proc/{pid}/cmdline').read_bytes():
                continue
            rss = int(Path(f'/proc/{pid}/statm').read_text().split()[1]) * os.sysconf('SC_PAGE_SIZE') // (1 << 20)
        except (OSError, ValueError):
            continue
        if best is None or rss > best[1]:
            best = (pid, rss)
    if best is None:
        return None
    out = subprocess.run(['nvidia-smi', '--query-compute-apps=pid,used_memory', '--format=csv,noheader,nounits'],
                         capture_output=True, text=True).stdout
    gpu = sum(int(l.split(',')[1]) for l in out.splitlines() if l.split(',')[0].strip() == str(best[0]))
    return best[1], gpu


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds', type=float, help='close the game after this many seconds (automated runs)')
    p.add_argument('--screenshot-at', type=float, action='append', default=[], help='seconds after launch; repeatable')
    p.add_argument('--stock', action='store_true', help='run without the adapter')
    p.add_argument('--installed', action='store_true',
                   help='run the game folder exactly as install.py left it: settings from ss2rtx.ini, nothing copied or overridden')
    p.add_argument('--passthrough', action='store_true', help='adapter loaded with all hooks disabled')
    p.add_argument('--game-arg', action='append', default=[], help='extra game command-line argument; repeatable')
    p.add_argument('--keys', help='adapter key script, e.g. "16:2;19:enter" (seconds after first frame : key)')
    p.add_argument('--scenario', choices=sorted(SCENARIOS), help='named way into a level; sets game args and keys')
    p.add_argument('--remix', action='store_true', help='start the Remix runtime inside the game and feed it the scene')
    p.add_argument('--composite', action='store_true', help='put the Remix image into the game frame (implies --remix)')
    p.add_argument('--split', action='store_true', help='engine image on the left half, path-traced on the right (implies --composite)')
    p.add_argument('--remix-shot-at', help='seconds at which the adapter saves a readback of the Remix image, e.g. "40,48"')
    p.add_argument('--conf', action='append', default=[], metavar='rtx.option=value', help='extra rtx.conf line; repeatable')
    p.add_argument('--census-at', help='seconds after the first frame at which to capture a draw-stream census, e.g. "40,44"')
    p.add_argument('--env', action='append', default=[], metavar='K=V', help='extra environment for the game')
    p.add_argument('--track-memory', type=float, metavar='SECONDS', help='sample the game process RSS and GPU memory at this interval')
    p.add_argument('--teleport', action='append', default=[], metavar='X,Y,Z',
                   help='bind a teleport to that spot to the next of the keys y u o p n b (use them in --keys, after one press of h, which allows it); repeatable')
    p.add_argument('--live-input', action='store_true', help='let a scripted run (--keys/--scenario) react to the real keyboard and mouse as well')
    p.add_argument('--gpu', type=int, default=0)
    p.add_argument('--proton-log', action='store_true')
    a = p.parse_args()

    exe = GAME / 'SystemShock2Remastered.exe'
    if not exe.exists():
        sys.exit('run setup_game.py first')
    if RUN.exists():
        shutil.rmtree(RUN)
    RUN.mkdir(parents=True)

    installed = GAME / 'dxgi.dll'
    built = ROOT / 'adapter/build/dxgi.dll'
    if a.installed:
        if not (GAME / 'ss2rtx/install-manifest.json').exists():
            sys.exit('not installed: run  python3 install.py install --target game')
    elif a.stock:
        installed.unlink(missing_ok=True)
    else:
        if not built.exists():
            sys.exit('adapter not built; run build.py adapter')
        shutil.copy2(built, installed)

    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('VK_', 'VKD3D_', 'DXVK_', 'DLSSNR_', 'SS2_NATIVE_', 'DLSS5VK_', 'SS2RTX_')) or key in (
                'VKLayer_DLSS5', 'VKLAYER_DLSS5', 'LD_PRELOAD', 'WINEDLLOVERRIDES', 'WINEDEBUG', 'PROTON_LOG'):
            env.pop(key, None)
    env.update(
        STEAM_COMPAT_DATA_PATH=str(ROOT / 'prefix'),
        STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / '.local/share/Steam'),
        SteamAppId='866570', SteamGameId='866570',
        PROTON_FORCE_NVAPI='1', PROTON_ENABLE_WAYLAND='0',
        # Our dxgi.dll proxy and (later) the Remix d3d9.dll must load as native DLLs.
        WINEDLLOVERRIDES=('' if a.stock else 'dxgi=n,b;') + 'd3d9=n,b;winedbg.exe=d',
        DLSSNR_DISABLE='1',
        DISABLE_VK_LAYER_VALVE_steam_overlay_1='1',
        DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1',
        PROTON_LOG='1' if a.proton_log else '0', PROTON_LOG_DIR=str(RUN),
        WINEDEBUG='-all,err+all,warn+module',
        DXVK_LOG_PATH=str(RUN), DXVK_LOG_LEVEL='info',
        DXVK_FILTER_DEVICE_UUID=gpu_uuid(a.gpu),
        RTX_SENTRY_TEST_DSN='http://none@127.0.0.1:9/0',  # see runtest.py: never upload crash reports
        SS2RTX_LOG=winpath(RUN / 'ss2rtx.log'),
    )
    if a.installed:
        env.pop('SS2RTX_LOG')   # default location: <game>/ss2rtx/ss2rtx.log
    if a.passthrough:
        env['SS2RTX_DISABLE'] = '1'
    if not a.installed:
        env['SS2RTX_NOSAVE'] = '1'   # the settings panel does not write an ini into the test copy
    if a.scenario:
        args, keys = SCENARIOS[a.scenario]
        a.game_arg = args + a.game_arg
        a.keys = keys + (';' + a.keys if a.keys else '')   # extra keys run after the scenario's own
    if a.keys:
        env['SS2RTX_KEYS'] = a.keys
    if a.live_input:
        env['SS2RTX_LIVEINPUT'] = '1'   # by default a scripted run keeps the real keyboard and mouse out
    if a.census_at:
        env['SS2RTX_CENSUS_AT'] = a.census_at
    conf_file = GAME / 'rtx.conf'
    a.composite = a.composite or a.split
    a.remix = a.remix or a.composite
    if a.split:
        env['SS2RTX_SPLIT'] = '1'
    if a.composite:
        env['SS2RTX_COMPOSITE'] = '1'
    if a.installed:
        pass   # rtx.conf and ss2rtx.ini come from the installer
    elif a.remix:
        env['SS2RTX_REMIXDIR'] = winpath(REMIX_RUNTIME)
        if a.remix_shot_at:
            env['SS2RTX_REMIX_SHOT_AT'] = a.remix_shot_at
        conf = list(REMIX_CONF)
        conf_file.write_text('\n'.join(conf + a.conf) + '\n')
    else:
        conf_file.unlink(missing_ok=True)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v

    # Test-only bindings in the private profile so the autopilot can turn the player
    # (the stock turn keys are on the numeric keypad, which posted key messages do not reach).
    bnd = ROOT / 'prefix/pfx/drive_c/users/steamuser/Saved Games/Nightdive Studios/System Shock 2 Remastered/user.bnd'
    if bnd.exists():
        text = bnd.read_text(errors='replace')
        # --teleport X,Y,Z (repeatable) binds the game's own teleport command to y, u, o, p, n, b
        # in that order, for use in --keys; earlier teleport lines are dropped.
        kept = [line for line in text.splitlines() if '"teleport ' not in line and '"enable_cheats ' not in line]
        wanted = ['bind j +turnleftfast', 'bind k +turnrightfast', 'bind g +fire_weapon']
        wanted += [f'bind {key} "teleport {spot}"' for key, spot in zip('yuopnb', a.teleport)]
        if a.teleport:
            wanted.append('bind h "enable_cheats 1"')   # the game wants this first: press h once
        extra = [line for line in wanted if line not in kept]
        if extra or len(kept) != len(text.splitlines()):
            bnd.write_text('\n'.join(kept + extra) + '\n')

    stale = close_everything(env)
    started = time.time()
    pending = sorted(a.screenshot_at)
    samples = []
    next_sample = 30.0
    shots = []
    timed_out = False
    killed = 0
    with (RUN / 'launch.log').open('w') as log:
        proc = subprocess.Popen([str(PROTON), 'run', str(exe), *a.game_arg], cwd=GAME, env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while proc.poll() is None:
                elapsed = time.time() - started
                if pending and elapsed >= pending[0]:
                    at = pending.pop(0)
                    name = f'window-{int(at):03d}s.png' if float(at).is_integer() else f'window-{at:05.1f}s.png'
                    try:
                        shots.append((name, screenshot('System Shock 2', RUN / name)))
                    except Exception as e:
                        shots.append((name, f'error: {e}'))
                if a.track_memory and elapsed >= next_sample:
                    next_sample = elapsed + a.track_memory
                    sample = memory_sample()
                    if sample:
                        samples.append((round(elapsed, 1), *sample))
                if a.seconds is not None and elapsed >= a.seconds:
                    timed_out = True
                    break
                time.sleep(0.25)
        except KeyboardInterrupt:
            pass
        finally:
            killed = close_everything(env)
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, 9)
                except OSError:
                    pass
    rc = proc.wait()
    remaining = prefix_processes()
    mode = 'stock' if a.stock else 'passthrough' if a.passthrough else 'adapter'
    (RUN / 'result.json').write_text(json.dumps({
        'utc': datetime.now(timezone.utc).isoformat(), 'mode': mode, 'exit_code': rc,
        'seconds': round(time.time() - started, 1), 'closed_by_timer': timed_out,
        'screenshots': shots, 'processes_killed_at_cleanup': killed, 'processes_left': len(remaining),
        'memory_samples_seconds_rssMiB_gpuMiB': samples,
    }, indent=2) + '\n')
    print(f'--- game ({mode}): exit {rc} after {time.time() - started:.0f}s'
          f'{", closed by timer" if timed_out else ""}; screenshots {shots}')
    print(f'cleanup: stale before start {stale}, force-killed {killed}, still running {len(remaining)}')
    log_file = GAME / 'ss2rtx/ss2rtx.log' if a.installed else RUN / 'ss2rtx.log'
    if log_file.exists():
        print('--- ss2rtx.log')
        print(log_file.read_text(errors='replace').rstrip())
    elif not a.stock:
        print('no ss2rtx.log: the adapter was not loaded')
    print('files:', ', '.join(sorted(x.name for x in RUN.iterdir())))


if __name__ == '__main__':
    main()
