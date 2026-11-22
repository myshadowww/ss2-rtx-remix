#!/usr/bin/env python3
"""Run one ss2rtx probe under Proton in the project's private prefix and collect evidence.

  python3 runtest.py t01_triangle r1 --seconds 20 --screenshot-at 12

The probe gets the Windows path of the chosen Remix runtime's d3d9.dll as its first
argument. Logs, the probe's own probe.log and an optional window screenshot land in
run/<probe>_<sdk>/.
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

ROOT = Path(__file__).resolve().parent
PROTON = Path('/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton')
RUNTIMES = {
    'r1': ROOT / 'runtime/remix-1.6.1-x64',
    'r2': ROOT / 'runtime/minecraft-rtx2-a00da12a-win64',
}


def winpath(p):
    return 'Z:' + str(Path(p).resolve()).replace('/', '\\')


def gpu_uuid(index):
    out = subprocess.check_output(['nvidia-smi', '-i', str(index), '--query-gpu=uuid', '--format=csv,noheader'], text=True)
    return out.strip().removeprefix('GPU-').replace('-', '').lower()


def screenshot(title_prefix, dest):
    from Xlib import display, X
    from PIL import Image
    d = display.Display()
    found = []

    def scan(w, depth=0):
        try:
            if (w.get_wm_name() or '').startswith(title_prefix):
                found.append(w)
            if depth < 4:
                for c in w.query_tree().children:
                    scan(c, depth + 1)
        except Exception:
            pass
    scan(d.screen().root)
    if not found:
        return f'no window titled {title_prefix!r}'
    w = found[0]
    g = w.get_geometry()
    data = w.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    Image.frombytes('RGB', (g.width, g.height), data.data, 'raw', 'BGRX').save(dest)
    return f'{g.width}x{g.height}'


def prefix_processes():
    """PIDs of every process that belongs to the project's private Wine prefix."""
    # Proton exports WINEPREFIX with a trailing slash, and helper processes may carry only
    # the compat-data path, so accept either spelling.
    prefix = str(ROOT / 'prefix')
    wanted = {('WINEPREFIX=' + prefix + '/pfx').encode(), ('WINEPREFIX=' + prefix + '/pfx/').encode(),
              ('STEAM_COMPAT_DATA_PATH=' + prefix).encode(), ('STEAM_COMPAT_DATA_PATH=' + prefix + '/').encode()}
    me = os.getpid()
    pids = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit() or int(proc.name) == me:
            continue
        try:
            if wanted & set((proc / 'environ').read_bytes().split(b'\0')):
                pids.append(int(proc.name))
        except OSError:
            pass
    return pids


def close_everything(env):
    """Tear down every window and process this run started. Returns how many had to be killed.

    Killing the launcher is not enough: the Wine processes that own the probe window are
    not in its process group, so a stalled probe would stay on the desktop.
    """
    wineserver = PROTON.parent / 'files/bin/wineserver'
    wenv = dict(env, WINEPREFIX=str(ROOT / 'prefix/pfx'))
    try:
        subprocess.run([str(wineserver), '-k'], env=wenv, timeout=15,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        pass
    deadline = time.time() + 8
    while prefix_processes() and time.time() < deadline:
        time.sleep(0.25)
    left = prefix_processes()
    for pid in left:
        try:
            os.kill(pid, 9)
        except OSError:
            pass
    return len(left)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('probe')
    p.add_argument('sdk', choices=sorted(RUNTIMES))
    p.add_argument('args', nargs='*', help='extra probe arguments')
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--timeout', type=float, default=180, help='hard limit for the whole run')
    p.add_argument('--screenshot-at', type=float, help='seconds after launch to capture the probe window')
    p.add_argument('--window-title', default='ss2rtx', help='window title prefix for the screenshot')
    p.add_argument('--gpu-usage-at', type=float, help='seconds after launch to record which GPU the probe is using')
    p.add_argument('--gpu', type=int, default=0)
    p.add_argument('--proton-log', action='store_true')
    p.add_argument('--env', action='append', default=[], metavar='K=V')
    p.add_argument('--conf', action='append', default=[], metavar='rtx.option=value',
                   help='line for the rtx.conf the Remix runtime reads from the run directory')
    p.add_argument('--no-default-conf', action='store_true', help='do not write the Proton baseline rtx.conf')
    a = p.parse_args()

    exe = ROOT / f'tests/build/{a.probe}_{a.sdk}.exe'
    if not exe.exists():
        sys.exit(f'{exe} not built; run build.py')
    run = ROOT / f'run/{a.probe}_{a.sdk}'
    if run.exists():
        shutil.rmtree(run)
    run.mkdir(parents=True)
    (ROOT / 'prefix').mkdir(exist_ok=True)

    # Baseline Remix settings for running under Proton. Frame generation uses its own present
    # queue and threads and deadlocks swapchain recreation under Wine; we never want it anyway,
    # because the game presents the final frame, not Remix.
    conf = [] if a.no_default_conf else ['rtx.dlssPreset = 2', 'rtx.dlfg.enable = False']
    conf += a.conf
    if conf:
        (run / 'rtx.conf').write_text('\n'.join(conf) + '\n')

    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('VK_', 'VKD3D_', 'DXVK_', 'DLSSNR_', 'SS2_NATIVE_', 'DLSS5VK_')) or key in (
                'VKLayer_DLSS5', 'VKLAYER_DLSS5', 'LD_PRELOAD', 'WINEDLLOVERRIDES', 'WINEDEBUG', 'PROTON_LOG'):
            env.pop(key, None)
    env.update(
        STEAM_COMPAT_DATA_PATH=str(ROOT / 'prefix'),
        STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / '.local/share/Steam'),
        SteamAppId='866570', SteamGameId='866570',
        PROTON_FORCE_NVAPI='1', PROTON_ENABLE_WAYLAND='0',
        # The Remix runtime is itself a d3d9.dll; make sure Wine loads the file we point at.
        # winedbg is disabled so a crash cannot leave a debugger dialog on the desktop.
        WINEDLLOVERRIDES='d3d9=n,b;winedbg.exe=d',
        DLSSNR_DISABLE='1',
        DISABLE_VK_LAYER_VALVE_steam_overlay_1='1',
        DISABLE_VK_LAYER_VALVE_steam_fossilize_1='1',
        PROTON_LOG='1' if a.proton_log else '0', PROTON_LOG_DIR=str(run),
        WINEDEBUG='-all,err+all,warn+module',
        DXVK_LOG_PATH=str(run), DXVK_LOG_LEVEL='info',
        DXVK_FILTER_DEVICE_UUID=gpu_uuid(a.gpu),
        # Remix ships NVIDIA crash/usage reporting. It is consent-gated and we never give consent
        # (do NOT set RTX_AUTOMATION_DISABLE_BLOCKING_DIALOG_BOXES=1: that grants it). As a second
        # guard, point its upload endpoint at a dead local address.
        RTX_SENTRY_TEST_DSN='http://none@127.0.0.1:9/0',
    )
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v

    stale = close_everything(env)  # nothing of ours may be running before we start
    cmd = [str(PROTON), 'run', str(exe), winpath(RUNTIMES[a.sdk] / 'd3d9.dll'), str(a.seconds), *a.args]
    started = time.time()
    shot = None
    timed_out = False
    killed = 0
    gpu_usage = None
    with (run / 'launch.log').open('w') as log:
        proc = subprocess.Popen(cmd, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while proc.poll() is None:
                elapsed = time.time() - started
                if a.screenshot_at is not None and shot is None and elapsed >= a.screenshot_at:
                    try:
                        shot = screenshot(a.window_title, run / 'window.png')
                    except Exception as e:  # evidence only; never fail the run on it
                        shot = f'error: {e}'
                if a.gpu_usage_at is not None and gpu_usage is None and elapsed >= a.gpu_usage_at:
                    gpu_usage = subprocess.run(
                        ['nvidia-smi', '--query-compute-apps=gpu_uuid,pid,used_memory,name', '--format=csv,noheader'],
                        capture_output=True, text=True).stdout.strip().splitlines()
                    gpu_usage = [l for l in gpu_usage if '.exe' in l or 'wine' in l.lower()]
                if elapsed > a.timeout:
                    timed_out = True
                    break
                time.sleep(0.25)
        finally:
            # Always runs: normal exit, timeout, Ctrl-C or an exception in this script.
            killed = close_everything(env)
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, 9)
                except OSError:
                    pass
    rc = proc.wait()
    remaining = prefix_processes()
    probe_log = run / 'probe.log'
    text = probe_log.read_text(errors='replace') if probe_log.exists() else ''
    result = 'PASS' if 'RESULT PASS' in text else 'FAIL' if text else 'NO_PROBE_LOG'
    (run / 'result.json').write_text(json.dumps({
        'utc': datetime.now(timezone.utc).isoformat(), 'probe': a.probe, 'sdk': a.sdk,
        'runtime': str(RUNTIMES[a.sdk]), 'gpu': a.gpu, 'exit_code': rc,
        'seconds': round(time.time() - started, 1), 'screenshot': shot, 'result': result,
        'timed_out': timed_out, 'processes_killed_at_cleanup': killed, 'processes_left': len(remaining),
        'gpu_usage': gpu_usage,
    }, indent=2) + '\n')
    print(f'--- {a.probe} {a.sdk}: {result} (exit {rc}, {time.time() - started:.0f}s, screenshot {shot}'
          f'{", TIMED OUT" if timed_out else ""})')
    print(f'cleanup: stale before start {stale}, force-killed {killed}, still running {len(remaining)}')
    if gpu_usage is not None:
        print('gpu usage (uuid, pid, MiB, name):', gpu_usage)
    print(text.rstrip() or '(no probe.log)')
    print('files:', ', '.join(sorted(x.name for x in run.iterdir())))
    sys.exit(0 if result == 'PASS' else 1)


if __name__ == '__main__':
    main()
