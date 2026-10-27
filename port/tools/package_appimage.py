"""Package the Linux game and launcher as one AppImage (Tauri/linuxdeploy).

    python port/tools/package_appimage.py --game build/host64-linux/dhd64

Build the game first. For distributable builds, run on the oldest supported Linux baseline;
bundling a newer glibc-linked binary does not make it compatible with older distributions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import build_launcher

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=ROOT / 'build/host64-linux/dhd64')
    parser.add_argument('--skip-launcher-build', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('AppImages must be packaged on Linux')
    game = args.game.resolve()
    data = game.read_bytes() if game.is_file() else b''
    if data[:4] != b'\x7fELF':
        parser.error('--game must be a built Linux executable')
    for path in (ROOT, Path.home()):
        if str(path).encode() in data:
            parser.error('--game contains build-machine paths; rebuild with build_host64_linux.py')
    launcher = build_launcher.EXE if args.skip_launcher_build else build_launcher.build()
    problems = build_launcher.problems(launcher)
    if problems:
        raise SystemExit('\n'.join(problems))
    out = ROOT / 'build/appimage'
    out.mkdir(parents=True, exist_ok=True)
    files = {'/usr/bin/dhd': str(game)}
    for source in ('LICENSE', 'port/release/THIRD-PARTY-NOTICES.txt', 'port/release/README.txt'):
        files['/usr/share/doc/downhill-revived/' + Path(source).name] = str(ROOT / source)
    config = out / 'tauri.appimage.json'
    config.write_text(json.dumps({'version': build_launcher.version(), 'bundle': {
        'active': True, 'targets': ['appimage'], 'category': 'Game',
        'linux': {'appimage': {'files': files}}
    }}, indent=2) + '\n')
    env = os.environ.copy()
    # Tools can extract themselves on hosts without FUSE; don't strip modern ELF sections
    # with linuxdeploy's older binutils. Release binaries are already built/stripped.
    env.update(APPIMAGE_EXTRACT_AND_RUN='1', NO_STRIP='1')
    subprocess.run([shutil.which('npx'), 'tauri', 'bundle', '--bundles', 'appimage',
                    '--config', str(config)], cwd=build_launcher.LAUNCHER, env=env, check=True)
    bundles = launcher.parent / 'bundle/appimage'
    images = list(bundles.glob('*.AppImage'))
    if len(images) != 1:
        raise SystemExit(f'expected one AppImage in {bundles}, found {len(images)}')
    release = ROOT / 'build/release'
    release.mkdir(parents=True, exist_ok=True)
    target = release / f'DownhillRevived-{build_launcher.version()}-x86_64.AppImage'
    staged = target.with_suffix(".AppImage.tmp")
    shutil.copy2(images[0], staged)
    staged.chmod(0o755)
    staged.replace(target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_suffix('.AppImage.sha256').write_text(f'{digest}  {target.name}\n')
    (release / 'latest-linux-appimage.json').write_text(json.dumps({
        'version': build_launcher.version(), 'url': target.name,
        'sha256': digest, 'size': target.stat().st_size,
        'page': 'https://github.com/skuza-oss/downhill-revived/releases/latest'
    }, indent=2) + '\n')
    print(target, flush=True)


if __name__ == '__main__':
    main()
