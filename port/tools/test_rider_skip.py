"""Integration regression: early Cross must skip Rider Selected at 60 and 240 FPS.

Usage: python port/tools/test_rider_skip.py --exe build/host64-linux/dhd64 --iso PATH
Uses scripted pad input, isolated settings and no memory card; requires a working display.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--iso', type=Path, required=True)
    args = parser.parse_args()
    exe, iso = args.exe.resolve(), args.iso.resolve()
    # Early input is latched before the half-second blend ends. Late input is
    # the control case, after that blend; both must reach NEW/CONTINUE promptly.
    for fps in (60, 240):
        for settle in (10, fps):
            env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
            env.update(DHD_ISO=str(iso), DHD_SETTINGS='none', DHD_RENDER='native',
                       DHD_WINDOW='windowed', DHD_WINDOW_WIDTH='960', DHD_WINDOW_HEIGHT='540',
                       DHD_WIDESCREEN='1', DHD_ASPECT='wide', DHD_SKIP_MOVIES='1',
                       DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_FPS=str(fps), DHD_UNLOCK_ALL='1',
                       DHD_AUTO_UPDATE='off', DHD_LAUNCHER='1', DHD_STOP_FIELD=str(fps * 7),
                       DHD_NAV=f'0:triangle|cross,15:cross,32:cross,3:cross,33+{settle}:cross')
            with tempfile.TemporaryDirectory(prefix='dhd-rider-skip-') as directory:
                result = subprocess.run([str(exe)], cwd=directory, env=env,
                                        capture_output=True, text=True, timeout=90)
            log = result.stdout + result.stderr
            assert result.returncode == 0, log[-4000:]
            states = {int(step): int(field) for step, field in
                      re.findall(r'compare: nav step ([45]) field=(\d+) menu=', log)}
            assert 4 in states and 5 in states, log[-4000:]
            duration = states[5] - states[4]
            budget = max(settle, fps // 2) + fps // 2
            assert duration <= budget, f'{fps} FPS, settle {settle}: stuck for {duration} fields (budget {budget})'
            print(f'PASS {fps} FPS, Cross after {settle} fields: exited Rider Selected in {duration} fields', flush=True)


if __name__ == '__main__':
    main()
