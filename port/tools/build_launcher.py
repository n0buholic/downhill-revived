"""The player launcher, DownhillRevived (port/launcher, finding 093), built for a release.

    python port/tools/build_launcher.py          # prints the executable's path

`npx tauri build --no-bundle` in port/launcher (npm install first when node_modules is missing),
with the renderer's flags (port/tools/rustgs.py): on Windows the static C runtime, and always
CARGO_HOME / RUSTUP_HOME remapped to cargo / rustup so the crates' panic locations name no folder
of this machine. The executable is refused if it still holds one of those folders or the
repository's path, or wasn't built with port/include/version.h's version.
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

WINDOWS = sys.platform == 'win32'
ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / 'port' / 'launcher'
EXE = LAUNCHER / 'src-tauri' / 'target' / 'release' / ('DownhillRevived.exe' if WINDOWS else 'DownhillRevived')
VERSION_H = ROOT / 'port' / 'include' / 'version.h'


def spellings(path):
    p = os.path.normpath(str(path))
    return list(dict.fromkeys((p, p.replace('\\', '/'))))


def homes():
    """The Rust toolchain's folders, when set (on Windows they must exist, off C:)."""
    out = {}
    for var, name in (('CARGO_HOME', 'cargo'), ('RUSTUP_HOME', 'rustup')):
        value = os.environ.get(var)
        if not value:
            if WINDOWS:
                sys.exit('%s must be set (the Rust toolchain lives on F:)' % var)
            value = str(Path.home() / ('.cargo' if var == 'CARGO_HOME' else '.rustup'))
        if WINDOWS and os.path.splitdrive(os.path.normpath(value))[0].upper() == 'C:':
            sys.exit('%s must be off C:' % var)
        out[name] = value
    return out


def rustflags():
    flags = ['-Ctarget-feature=+crt-static'] if WINDOWS else []
    for name, value in homes().items():
        flags += ['--remap-path-prefix=%s=%s' % (s, name) for s in spellings(value)]
    if not WINDOWS:
        flags += ['--remap-path-prefix=%s=.' % ROOT]
    return flags


def version():
    m = re.search(r'#define DHD_VERSION "([0-9.]+)"', VERSION_H.read_text(encoding='utf-8'))
    return m.group(1)


def problems(exe):
    """Local folders or a wrong version in the exe."""
    data = exe.read_bytes()
    out = []
    folders = [s for v in homes().values() for s in spellings(v)] + spellings(ROOT)
    for folder in folders:
        for enc in ('utf-8', 'utf-16-le'):
            if folder.encode(enc).lower() in data.lower():
                out.append('names %s (%s)' % (folder, enc))
    if ('DownhillRevived-launcher/%s' % version()).encode() not in data:
        out.append('was not built with version %s (port/include/version.h)' % version())
    return out


def build():
    npx = shutil.which('npx') or sys.exit('npx (Node.js) is needed to build the launcher')
    if not (LAUNCHER / 'node_modules').is_dir():
        subprocess.run([shutil.which('npm'), 'install', '--no-audit', '--no-fund'], cwd=str(LAUNCHER), check=True)
    env = {k: v for k, v in os.environ.items() if k not in ('RUSTFLAGS', 'CARGO_BUILD_RUSTFLAGS', 'CARGO_TARGET_DIR')}
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(rustflags())
    print('building the launcher (port/launcher)', flush=True)
    r = subprocess.run([npx, 'tauri', 'build', '--no-bundle'], cwd=str(LAUNCHER), env=env,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        sys.exit('the launcher build failed:\n' + (r.stdout + r.stderr)[-4000:])
    found = problems(EXE)
    if found:
        sys.exit('%s is not fit to publish:\n  %s' % (EXE, '\n  '.join(found)))
    return EXE


if __name__ == '__main__':
    print(build())
