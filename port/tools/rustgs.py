"""rustgs.py: every cargo use of the Rust GS renderer (port/render, crate dhd_render) goes through here
(plan section 3.1, with the flag and seed contract in port/tools/rustgs_selftest.py's docstring).

    python port/tools/rustgs.py build --target x86_64-pc-windows-msvc [--features test-hooks]
                                      [--profile release|fixloop] [--no-cache] [--report FILE]
    python port/tools/rustgs.py build --target wasm32-unknown-unknown [--profile ...]   # cdylib + wasm-bindgen
    python port/tools/rustgs.py build --target aarch64-linux-android [--profile ...]    # Android (finding 091)
    python port/tools/rustgs.py test  --target x86_64-pc-windows-msvc [--features ...] -- --test device_smoke [filter]
    python port/tools/rustgs.py run   --target x86_64-pc-windows-msvc --bin optrace_replay -- <args>
    python port/tools/rustgs.py tree  [--target T]      # the version gate, printed
    python port/tools/rustgs.py selftest [--keep]       # port/tools/rustgs_selftest.py

Scope: x64 Windows, the browser (wasm32) and Android arm64 (aarch64-linux-android, finding 091: a
staticlib, libdhd_render.a, linked into libmain.so by build_host64_android.py; Vulkan only). The i686
build is retired.

What every cargo call gets
--------------------------
- cwd port/render, `--locked --offline -j 2`, `--target T --profile P`.
- build, native: `cargo rustc --lib --crate-type staticlib --features native[,...] -- --print
  native-static-libs`; wasm: `cargo rustc --lib --crate-type cdylib --no-default-features --features
  web[,...]`, then `wasm-bindgen --target web --keep-lld-exports --no-typescript` into
  build/web/renderer (dhd_render.js, dhd_render_bg.wasm).
- CARGO_ENCODED_RUSTFLAGS is the only flag source (RUSTFLAGS and every other flag, profile, target-dir
  and toolchain override are removed from cargo's environment):
    msvc: -Ctarget-feature=+crt-static;
    all:  --remap-path-prefix=<CARGO_HOME>=cargo and <RUSTUP_HOME>=rustup, backslash and forward-slash
          spellings (no remap of the worktree or the target dir: the flags are the same in every
          worktree; check_rust_lib.py refuses a leak of either instead);
    wasm: -Clink-arg= for the memory layout (WASM_LINK_ARGS).
- CARGO_INCREMENTAL is removed (sccache refuses it; the profiles decide). sccache, if used, comes from a
  local .cargo/config.toml; rustgs neither needs nor overrides it.
- CARGO_TARGET_DIR = DHD_DEVCACHES/dhd-cargo/<sha1(worktree)[:8]>. On first use of <triple>/<profile>
  there, DHD_DEVCACHES/dhd-cargo/seed/<triple>/<profile>/ is copied in (mtimes kept) when its seed.json
  matches this build's rustflags, LF-normalised Cargo.lock and rustc -vV.

Gates (each prints `rustgs: refused: ...` and exits 2; a refused or failed build writes no report)
-------------------------------------------------------------------------------------------------
- `--features no-fp-guard` (the guard's negative control is run by hand with cargo, finding 085);
- versions: `cargo tree -e normal -i <pkg>` must give exactly PINS (native graph), and the wasm graph
  must hold none of naga / wgpu-core / wgpu-hal;
- an `include_str!` / `include_bytes!` / `include!` of an absolute path in port/render/src (pre-check);
  after the build, cargo's dep-info must list no input outside the tree, CARGO_HOME, RUSTUP_HOME and
  the target dir;
- port/tools/check_rust_lib.py on the built library (paths, markers, /defaultlib:msvcrt,
  native-static-libs outside the S1 list; Android: outside native-static-libs-aarch64-linux-android.txt).

The one rewrite of rustc's output (x64): thin LTO re-emits the std crates' modules, which carry
CodeView, and LLVM writes each object's S_OBJNAME record (in .debug$S, which never reaches the exe's
image) with the object's path under the per-worktree target dir. The flags may not name that dir, so
normalise_objnames rewrites the target-dir prefix of exactly those records to `target` (NUL-padded,
same length) and recomputes their section's checksum (refusing if the old one was not the JamCRC
of the bytes). Every other byte is rustc's; check_rust_lib then refuses any remaining mention.

Cache
-----
The built library (and the wasm glue) is kept in the shared object cache (objcache.DIR, default
DHD_DEVCACHES/dhd-objcache). The key is sha256 over rustc -vV, cargo -V (and wasm-bindgen -V for wasm),
Cargo.lock, every Cargo.toml, rust-toolchain.toml, the crate's .cargo/config.toml, every file under
port/render/src, the four WGSL files, the encoded flags, triple, features, profile and the resolved
versions; paths in it are relative to the tree, so every worktree has the same key. A hit copies the
files out instead of running cargo. `--no-cache` builds with cargo and stores nothing.
DHD_OBJCACHE=0 turns the cache off (every build a miss).

Outputs: native: <target_dir>/rustgs/<triple>/<profile>/<features>/dhd_render.lib (Android:
libdhd_render.a); wasm:
build/web/renderer/. `--report FILE` writes {lib, sha256, cache, key, rustflags, target_dir, glue,
native_static_libs, triple, profile, features, versions}.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import struct
import tempfile
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import check_rust_lib  # noqa: E402
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

ROOT = TOOLS.parents[1]
RENDER = ROOT / 'port' / 'render'
GS = ROOT / 'port' / 'gs'
WGSL = ('raster.wgsl', 'present.wgsl', 'native.wgsl', 'native_dual.wgsl')
CARGO_TARGETS = (DEVCACHES / 'dhd-cargo')
SEED = CARGO_TARGETS / 'seed'
WEB_OUT = ROOT / 'build' / 'web' / 'renderer'
X64 = 'x86_64-pc-windows-msvc'
WASM = 'wasm32-unknown-unknown'
ANDROID = 'aarch64-linux-android'  # the Android build (build_host64_android.py, finding 091)
LINUX = 'x86_64-unknown-linux-gnu'  # the Linux host build (build_host64_linux.py, finding 098)
TARGETS = (X64, WASM, ANDROID, LINUX)   # i686 is retired
PROFILES = ('release', 'fixloop')
FEATURES = ('test-hooks',)       # what a caller may add; native / web are rustgs's own
REFUSED_FEATURES = ('no-fp-guard',)
JOBS = '2'
VERSION = 'rustgs 1'
# G1: the native graph's wgpu-native family (plan section 0 D5; finding 085, Pins).
PINS = {'naga': '29.0.3', 'wgpu-core': '29.0.3', 'wgpu-hal': '29.0.3', 'wgpu-types': '29.0.3'}
WASM_ABSENT = ('naga', 'wgpu-core', 'wgpu-hal')
WASM_LINK_ARGS = ('--import-memory', '--export-memory', '--no-stack-first', '--global-base=1610612736',
                  '-z', 'stack-size=1048576', '--initial-memory=1761607680', '--max-memory=2147483648',
                  '--export=__heap_base', '--export=__data_end', '--export=__stack_pointer')
WASM_BINDGEN_ARGS = ('--target', 'web', '--keep-lld-exports', '--no-typescript', '--out-name', 'dhd_render')
# Removed from cargo's environment: every other way to pass flags or change the profile, the target
# dir or the toolchain.
ENV_DROP = ('RUSTFLAGS', 'CARGO_BUILD_RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTDOCFLAGS',
            'CARGO_ENCODED_RUSTDOCFLAGS', 'CARGO_BUILD_RUSTDOCFLAGS', 'CARGO_INCREMENTAL',
            'CARGO_BUILD_INCREMENTAL', 'CARGO_TARGET_DIR', 'CARGO_BUILD_TARGET_DIR', 'CARGO_BUILD_TARGET',
            'CARGO_BUILD_JOBS', 'RUSTUP_TOOLCHAIN', 'RUSTC', 'CARGO_BUILD_RUSTC', 'RUSTC_BOOTSTRAP',
            'CARGO_BUILD_DEP_INFO_BASEDIR')
ENV_DROP_PREFIX = ('CARGO_PROFILE_', 'CARGO_TARGET_')
ABS_INCLUDE = re.compile(r'\binclude(?:_str|_bytes)?\s*!\s*\(\s*r?#*"((?:[A-Za-z]:)?[\\/][^"]*)"')


class Refused(Exception):
    pass


def refuse(why):
    raise Refused(why)


def log(msg):
    print('rustgs: ' + msg, flush=True)


# ---------------------------------------------------------------------------------------------------
# environment and flags

def home(var):
    value = os.environ.get(var)
    if not value:
        if os.name != 'nt':  # a Linux host has the toolchain where it lives (default paths are fine)
            return ''
        refuse('%s is not set (the Rust toolchain must live off the C: drive)' % var)
    path = os.path.normpath(value)
    if os.path.splitdrive(path)[0].upper() == 'C:':
        refuse('%s is on C: (%s); the system drive stays free' % (var, path))
    return path


def spellings(path):
    p = os.path.normpath(str(path))
    return list(dict.fromkeys((p, p.replace('\\', '/'))))


def rustflags(triple):
    flags = []
    if triple.endswith('-msvc'):
        flags.append('-Ctarget-feature=+crt-static')
    for var, name in (('CARGO_HOME', 'cargo'), ('RUSTUP_HOME', 'rustup')):
        if var not in os.environ and os.name != 'nt':
            value = os.path.expanduser('~/.cargo' if var == 'CARGO_HOME' else '~/.rustup')
        else:
            value = home(var)
        for s in spellings(value):
            flags.append('--remap-path-prefix=%s=%s' % (s, name))
    if triple == WASM:
        flags += ['-Clink-arg=' + a for a in WASM_LINK_ARGS]
    return flags


def target_dir():
    path = CARGO_TARGETS / hashlib.sha1(os.path.normcase(str(ROOT)).encode()).hexdigest()[:8]
    if path.resolve().drive.upper() == 'C:':
        refuse('the cargo target dir %s is on C:' % path)
    return path


def cargo_env(triple):
    env = {k: v for k, v in os.environ.items()
           if k not in ENV_DROP and not k.startswith(ENV_DROP_PREFIX)}
    env['CARGO_TARGET_DIR'] = str(target_dir())
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(rustflags(triple))
    return env


def tool_version(cmd, env=None):
    r = subprocess.run(cmd, cwd=str(RENDER), env=env, capture_output=True, text=True, errors='replace')
    if r.returncode:
        refuse('%s failed: %s' % (' '.join(cmd), (r.stderr or r.stdout).strip()[-500:]))
    return r.stdout.replace('\r\n', '\n')


def lf_sha(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def check_features(features):
    for f in features:
        if f in REFUSED_FEATURES:
            refuse('--features %s: the unguarded build is a negative control, run by hand with cargo '
                   'only (rustgs never builds it)' % f)
    for f in features:
        if f not in FEATURES:
            refuse('--features %s: unknown (allowed: %s)' % (f, ', '.join(FEATURES)))


def parse_features(text):
    return sorted({f.strip() for f in (text or '').replace(' ', ',').split(',') if f.strip()})


# ---------------------------------------------------------------------------------------------------
# gates

def tree_versions(triple, env):
    """{package: [versions]} of the gated packages in the normal-dependency graph of triple."""
    features = ['--features', 'native'] if triple != WASM else ['--no-default-features', '--features', 'web']
    names = list(PINS) if triple != WASM else list(WASM_ABSENT)
    found = {}
    for name in names:
        cmd = ['cargo', 'tree', '--locked', '--offline', '-e', 'normal', '-i', name, '--target', triple,
               *features, '--depth', '0', '--prefix', 'none']
        r = subprocess.run(cmd, cwd=str(RENDER), env=env, capture_output=True, text=True, errors='replace')
        if r.returncode:
            refuse('version gate: `%s` failed (%s): %s' % (' '.join(cmd), name,
                                                          (r.stderr or r.stdout).strip()[-800:]))
        versions = sorted({m.group(1) for m in re.finditer(r'^%s v(\S+)' % re.escape(name), r.stdout, re.M)})
        found[name] = versions
    return found


def version_gate(triple, env):
    found = tree_versions(triple, env)
    if triple == WASM:
        bad = {n: v for n, v in found.items() if v}
        if bad:
            refuse('version gate: the browser graph must hold no %s, it holds %s' % (
                ' / '.join(WASM_ABSENT), ', '.join('%s %s' % (n, '/'.join(v)) for n, v in bad.items())))
    else:
        for name, want in PINS.items():
            if found.get(name) != [want]:
                refuse('version gate: %s resolves to %s, the pin is exactly %s (G1)' % (
                    name, ', '.join(found.get(name) or ['nothing']), want))
    return found


def source_files():
    """Every file the key covers, as (relative posix path, absolute path)."""
    files = []
    for p in RENDER.rglob('*'):
        rel = p.relative_to(RENDER)
        if not p.is_file() or rel.parts[0] in ('target',) or '__pycache__' in rel.parts:
            continue
        if p.name in ('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml') or rel.parts[0] == 'src' \
                or rel.as_posix() == '.cargo/config.toml':
            files.append(('port/render/' + rel.as_posix(), p))
    for name in WGSL:
        files.append(('port/gs/' + name, GS / name))
    return sorted(files)


def include_precheck():
    for p in sorted((RENDER / 'src').rglob('*.rs')):
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in ABS_INCLUDE.finditer(text):
            refuse('%s includes a file by an absolute path (%s): every compiled input must be inside '
                   'the tree' % (p.relative_to(ROOT).as_posix(), m.group(1)))


def dep_info_paths(d_file):
    text = Path(d_file).read_text(encoding='utf-8', errors='replace').replace('\\\n', ' ')
    paths = []
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        # "<output>: <input> <input> ..."; spaces inside a path are written "\ "
        m = re.match(r'^(.*?[^\\]):(?:\s|$)', line)
        if not m:
            continue
        rest = line[m.end():]
        for tok in re.split(r'(?<!\\)\s+', rest.strip()):
            if tok:
                paths.append(tok.replace('\\ ', ' '))
    return paths


def inputs_gate(d_file):
    if not Path(d_file).is_file():
        refuse('dep-info %s is missing: the compiled inputs cannot be checked' % d_file)
    allowed = [os.path.normcase(os.path.normpath(str(p))) for p in
               (ROOT, home('CARGO_HOME'), home('RUSTUP_HOME'), target_dir())]
    for path in dep_info_paths(d_file):
        full = os.path.normcase(os.path.normpath(os.path.join(str(RENDER), path)))
        if not any(full == a or full.startswith(a.rstrip('\\/') + os.sep) for a in allowed):
            refuse('the build read %s, outside the tree, CARGO_HOME, RUSTUP_HOME and the target dir (an include of '
                   'a file outside the tree?)' % path)


# ---------------------------------------------------------------------------------------------------
# S_OBJNAME: the one place the target dir reaches the library

S_OBJNAME = 0x1101
OBJNAME_ROOT = b'target'
NUL = bytes(1)


def jamcrc(data):
    """The COFF section checksum LLVM writes into a section's aux symbol (JamCRC, init 0)."""
    return (~zlib.crc32(data, 0xFFFFFFFF)) & 0xFFFFFFFF


def normalise_objnames(lib, tdir):
    """Rewrites, in place, each CodeView S_OBJNAME record of lib's objects that names an object under
    tdir as `target\\...` (what --remap-path-prefix=<tdir>=target would write), NUL-padded to the same
    length, and fixes its section's checksum. Thin LTO re-emits the std crates' modules, which carry
    CodeView, with the object path rustc wrote (the per-worktree target dir); the flags must not name
    that dir (they are the same in every worktree), so the record is normalised here instead. Returns
    the number of records rewritten. Any other mention of tdir stays and check_rust_lib refuses it."""
    data = bytearray(Path(lib).read_bytes())
    if data[:8] != b'!<arch>\n':
        return 0
    prefixes = [s.encode() for s in spellings(tdir)]
    count, pos = 0, 8
    while pos + 60 <= len(data):
        size = int(bytes(data[pos + 48:pos + 58]).strip() or b'0')
        body = pos + 60
        if any(p in data[body:body + size] for p in prefixes):
            count += _objnames_in_member(data, body, size, prefixes)
        pos = body + size + (size & 1)
    if count:
        tmp = Path(str(lib) + '.tmp')
        tmp.write_bytes(bytes(data))
        os.replace(tmp, lib)
    return count


def _objnames_in_member(data, base, size, prefixes):
    obj = memoryview(data)[base:base + size]
    if size < 20:
        return 0
    sig1, sig2 = struct.unpack_from('<HH', obj, 0)
    if sig1 == 0 and sig2 == 0xFFFF:                       # bigobj (an import object has no sections)
        if struct.unpack_from('<H', obj, 4)[0] < 2:
            return 0
        nsec = struct.unpack_from('<I', obj, 44)[0]
        symptr, nsym = struct.unpack_from('<II', obj, 48)
        table, entry, secfmt = 56, 20, '<i'
    else:
        nsec = struct.unpack_from('<H', obj, 2)[0]
        symptr, nsym = struct.unpack_from('<II', obj, 8)
        table, entry, secfmt = 20 + struct.unpack_from('<H', obj, 16)[0], 18, '<h'
    sections = []
    for k in range(nsec):
        h = table + 40 * k
        sections.append((bytes(obj[h:h + 8]).rstrip(NUL),) + struct.unpack_from('<II', obj, h + 16))
    aux = {}                                                 # section index -> its checksum's offset
    i = 0
    while i < nsym:
        e = symptr + entry * i
        secno = struct.unpack_from(secfmt, obj, e + 12)[0]
        sclass, naux = obj[e + entry - 2], obj[e + entry - 1]
        if naux and sclass == 3 and 1 <= secno <= nsec and bytes(obj[e:e + 8]).rstrip(NUL) == sections[secno - 1][0]:
            aux.setdefault(secno - 1, e + entry + 8)
        i += 1 + naux
    changed = 0
    for k, (name, length, ptr) in enumerate(sections):
        if name != b'.debug$S' or not ptr:
            continue
        sec = obj[ptr:ptr + length]
        text = bytes(sec)
        hits = []
        for prefix in prefixes:
            at = text.find(prefix)
            while at >= 0:
                rec = at - 8                                  # reclen(2) kind(2) signature(4) name
                if rec >= 0 and struct.unpack_from('<H', text, rec + 2)[0] == S_OBJNAME:
                    end = rec + 2 + struct.unpack_from('<H', text, rec)[0]
                    nul = text.find(NUL, at, end)
                    if nul < 0:
                        refuse('S_OBJNAME rewrite: a record without its terminator')
                    hits.append((at, nul, prefix))
                at = text.find(prefix, at + 1)
        if not hits:
            continue
        if k not in aux:
            refuse('S_OBJNAME rewrite: the .debug$S section has no section symbol')
        stored = struct.unpack_from('<I', obj, aux[k])[0]
        if stored != jamcrc(text):
            refuse('S_OBJNAME rewrite: the .debug$S checksum is not the JamCRC of its bytes')
        for at, nul, prefix in hits:
            new = OBJNAME_ROOT + text[at + len(prefix):nul]
            sec[at:nul] = new + NUL * (nul - at - len(new))
        struct.pack_into('<I', obj, aux[k], jamcrc(bytes(sec)))
        changed += len(hits)
    return changed


# ---------------------------------------------------------------------------------------------------
# seeds

def seed(triple, profile, flags, rustc_v):
    tdir = target_dir()
    if (tdir / triple / profile).exists():
        return
    src = SEED / triple / profile
    meta = src / 'seed.json'
    if not meta.is_file():
        log('no seed for %s/%s; cold build' % (triple, profile))
        return
    try:
        s = json.loads(meta.read_text(encoding='utf-8'))
    except ValueError:
        log('seed %s unreadable; cold build' % meta)
        return
    why = []
    if s.get('rustflags') != flags:
        why.append('rustflags')
    if s.get('cargo_lock_sha256_lf') != lf_sha(RENDER / 'Cargo.lock'):
        why.append('Cargo.lock')
    if (s.get('rustc') or '').replace('\r\n', '\n') != rustc_v:
        why.append('rustc')
    if why:
        log('seed %s/%s not used (%s differ); cold build' % (triple, profile, ', '.join(why)))
        return
    log('seeding %s from %s' % (tdir, src))
    tdir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, tdir, dirs_exist_ok=True, copy_function=shutil.copy2,
                    ignore=lambda d, names: ['seed.json'] if Path(d) == src else [])


# ---------------------------------------------------------------------------------------------------
# cache

def objcache():
    import objcache as oc
    return oc


def cache_key(triple, profile, features, flags, versions, tools):
    h = hashlib.sha256(VERSION.encode())

    def part(label, data):
        if isinstance(data, str):
            data = data.encode('utf-8', 'surrogateescape')
        h.update(b'%s %d:' % (label.encode(), len(data)))
        h.update(data)
    for name, text in tools:
        part(name, text)
    for rel, path in source_files():
        data = path.read_bytes()
        if not rel.endswith('.wgsl'):   # rustc and cargo read text with CRLF normalised; WGSL is embedded raw
            data = data.replace(b'\r\n', b'\n')
        part('file ' + rel, data)
    part('flags', '\x1f'.join(flags))
    part('triple', triple)
    part('profile', profile)
    part('features', ','.join(features))
    part('versions', json.dumps(versions, sort_keys=True))
    if triple == WASM:
        part('bindgen', ' '.join(WASM_BINDGEN_ARGS))
    return h.hexdigest()


def _sub(key, name):
    return hashlib.sha256(('%s\0%s' % (key, name)).encode()).hexdigest()


def cache_roots():
    return [(home('CARGO_HOME'), 'cargo'), (home('RUSTUP_HOME'), 'rustup'), (str(target_dir()), 'target')]


def cache_fetch(key, out_dir):
    """The cached files of key copied into out_dir: {name: path}, or None."""
    oc = objcache()
    if not oc.ENABLED:
        return None
    tmp = Path(tempfile.mkdtemp(prefix='rustgs-', dir=str(target_dir())))
    try:
        meta_path = tmp / 'meta.json'
        if not oc.fetch(_sub(key, 'meta.json'), str(meta_path)):
            return None
        meta = json.loads(meta_path.read_text(encoding='utf-8'))
        got = {}
        for name, sha in meta['files'].items():
            dst = tmp / name
            if not oc.fetch(_sub(key, name), str(dst)) or check_rust_lib.sha256(dst) != sha:
                return None
            got[name] = dst
        out_dir.mkdir(parents=True, exist_ok=True)
        result = {}
        for name, src in got.items():
            dst = out_dir / name
            shutil.copyfile(src, dst)
            result[name] = dst
        result['meta.json'] = meta
        return result
    except (OSError, ValueError, KeyError):
        return None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cache_store(key, files, extra):
    oc = objcache()
    if not oc.ENABLED:
        return
    roots = cache_roots()
    meta = {'files': {}, **extra}
    for name, path in files.items():
        oc.store(_sub(key, name), str(path), roots)
        meta['files'][name] = check_rust_lib.sha256(path)
    with tempfile.TemporaryDirectory(dir=str(target_dir())) as d:
        mp = Path(d) / 'meta.json'
        mp.write_text(json.dumps(meta, sort_keys=True), encoding='utf-8')
        oc.store(_sub(key, 'meta.json'), str(mp), roots)


# ---------------------------------------------------------------------------------------------------
# commands

def common(triple, profile):
    if triple not in TARGETS:
        refuse('--target %s: rustgs builds %s only (the i686 build is retired)' % (
            triple, ' and '.join(TARGETS)))
    if profile not in PROFILES:
        refuse('--profile %s: one of %s' % (profile, ', '.join(PROFILES)))
    if os.path.splitdrive(str(ROOT))[0].upper() == 'C:':
        refuse('the worktree %s is on C:' % ROOT)
    return cargo_env(triple)


def cargo_args(triple, profile):
    return ['--locked', '--offline', '--target', triple, '--profile', profile, '-j', JOBS]


def feature_args(triple, features):
    if triple == WASM:
        return ['--no-default-features', '--features', ','.join(['web'] + list(features))]
    return ['--features', ','.join(['native'] + list(features))]


def run_cargo(cmd, env):
    print('  + ' + ' '.join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=str(RENDER), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors='replace')
    return r


def build(triple, features=(), profile='release', no_cache=False):
    """Builds (or fetches) the library; returns the report dict. Raises Refused or SystemExit."""
    features = list(parse_features(','.join(features)))
    check_features(features)
    env = common(triple, profile)
    flags = rustflags(triple)
    include_precheck()
    versions = version_gate(triple, env)
    rustc_v = tool_version(['rustc', '-vV'], env)
    tools = [('rustc', rustc_v), ('cargo', tool_version(['cargo', '-V'], env))]
    if triple == WASM:
        tools.append(('wasm-bindgen', tool_version(['wasm-bindgen', '-V'], env)))
    key = cache_key(triple, profile, features, flags, versions, tools)
    tdir = target_dir()
    tdir.mkdir(parents=True, exist_ok=True)
    tag = '+'.join(features) or 'default'
    out_dir = WEB_OUT if triple == WASM else tdir / 'rustgs' / triple / profile / tag
    lib_name = ('dhd_render_bg.wasm' if triple == WASM else 'libdhd_render.a'
                if triple in (ANDROID, LINUX) else 'dhd_render.lib')
    rep = {'triple': triple, 'profile': profile, 'features': features, 'key': key, 'rustflags': flags,
           'target_dir': str(tdir), 'versions': versions}
    hit = None if no_cache else cache_fetch(key, out_dir)
    if hit is not None:
        meta = hit.pop('meta.json')
        rep.update(cache='hit', lib=str(hit[lib_name]), sha256=check_rust_lib.sha256(hit[lib_name]),
                   glue=sorted(str(p) for n, p in hit.items() if n != lib_name),
                   native_static_libs=meta.get('native_static_libs', []))
        log('cache hit %s -> %s' % (key[:16], rep['lib']))
        return rep

    seed(triple, profile, flags, rustc_v)
    crate_type = 'cdylib' if triple == WASM else 'staticlib'
    cmd = ['cargo', 'rustc', '--lib', '--crate-type', crate_type, *cargo_args(triple, profile),
           *feature_args(triple, features)]
    if triple != WASM:
        cmd += ['--', '--print', 'native-static-libs']
    r = run_cargo(cmd, env)
    sys.stdout.write(r.stdout[-8000:] if r.returncode else '')
    if r.returncode:
        raise SystemExit('rustgs: cargo failed (exit %d)' % r.returncode)
    built_dir = tdir / triple / profile
    raw = built_dir / ('dhd_render.wasm' if triple == WASM else lib_name)
    inputs_gate(built_dir / ('libdhd_render.d' if triple in (ANDROID, LINUX) else 'dhd_render.d'))
    sysroot = Path(tool_version(['rustc', '--print', 'sysroot'], env).strip()) / 'lib' / 'rustlib' / triple / 'lib'
    native_libs = []
    if triple != WASM:
        m = re.findall(r'native-static-libs: (.*)', r.stdout)
        if not m:
            refuse('rustc printed no native-static-libs line; the link list cannot be checked')
        native_libs = m[-1].split()
    if out_dir.exists() and triple != WASM:
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    files = {}
    if triple == WASM:
        with tempfile.TemporaryDirectory(prefix='rustgs-bindgen-', dir=str(tdir)) as d:
            bcmd = ['wasm-bindgen', *WASM_BINDGEN_ARGS, '--out-dir', d, str(raw)]
            print('  + ' + ' '.join(bcmd), flush=True)
            b = subprocess.run(bcmd, cwd=str(RENDER), env=env, capture_output=True, text=True, errors='replace')
            if b.returncode:
                raise SystemExit('rustgs: wasm-bindgen failed (exit %d)\n%s' % (b.returncode, b.stderr[-4000:]))
            names = sorted(p.name for p in Path(d).iterdir() if p.is_file())
            problems = []
            for n in names:
                problems += check_rust_lib.problems(Path(d) / n, trees=[ROOT], extra=[tdir], sysroot=sysroot)
            if problems:
                refuse('check_rust_lib: ' + '; '.join(problems[:8]))
            for n in names:
                shutil.copyfile(Path(d) / n, out_dir / n)
                files[n] = out_dir / n
    else:
        lib = out_dir / lib_name
        shutil.copyfile(raw, lib)
        renamed = normalise_objnames(lib, tdir) if triple == X64 else 0  # COFF's records only
        if renamed:
            log('%d S_OBJNAME record(s) normalised to target\\...' % renamed)
        problems = check_rust_lib.problems(lib, trees=[ROOT], extra=[tdir], native_static_libs=native_libs,
                                           sysroot=sysroot, triple=triple)
        if problems:
            lib.unlink()
            refuse('check_rust_lib: ' + '; '.join(problems[:8]))
        files[lib_name] = lib
    if lib_name not in files:
        refuse('the build produced no %s' % lib_name)
    if not no_cache:
        cache_store(key, files, {'native_static_libs': native_libs})
    rep.update(cache='miss', lib=str(files[lib_name]), sha256=check_rust_lib.sha256(files[lib_name]),
               glue=sorted(str(p) for n, p in files.items() if n != lib_name), native_static_libs=native_libs)
    log('built %s (key %s)' % (rep['lib'], key[:16]))
    return rep


def library(target=X64, features=(), profile='release'):
    """The built library's path (for link scripts; A1c). 'x64' / 'wasm' are accepted for the triple."""
    triple = {'x64': X64, 'x86_64': X64, 'wasm': WASM, 'wasm32': WASM, 'android': ANDROID,
              'linux': LINUX}.get(target, target)
    return Path(build(triple, features, profile)['lib'])


def native_static_libs(target=X64, features=(), profile='release'):
    """The libraries rustc says the static library needs (each checked against S1's list)."""
    return build(target, features, profile)['native_static_libs']


def test(triple, features, profile, rest):
    features = parse_features(','.join(features))
    check_features(features)
    if triple == WASM:
        refuse('test: the crate\'s tests run natively (x86_64-pc-windows-msvc)')
    env = common(triple, profile)
    include_precheck()
    version_gate(triple, env)
    seed(triple, profile, rustflags(triple), tool_version(['rustc', '-vV'], env))
    import dxc
    env['DHDR_TEST_DXC'] = str(dxc.dll('x64'))
    cmd = ['cargo', 'test', *cargo_args(triple, profile), *feature_args(triple, features), *rest]
    print('  + ' + ' '.join(cmd), flush=True)
    return subprocess.run(cmd, cwd=str(RENDER), env=env).returncode


def run(triple, features, profile, binary, rest):
    features = parse_features(','.join(features))
    check_features(features)
    if triple == WASM:
        refuse('run: the tools are x64 Windows programs')
    env = common(triple, profile)
    include_precheck()
    version_gate(triple, env)
    seed(triple, profile, rustflags(triple), tool_version(['rustc', '-vV'], env))
    cmd = ['cargo', 'run', '-p', 'dhd_render_tools', '--bin', binary, *cargo_args(triple, profile)]
    if features:
        cmd += ['--features', ','.join('dhd_render/' + f for f in features)]
    cmd += ['--', *rest]
    print('  + ' + ' '.join(cmd), flush=True)
    return subprocess.run(cmd, cwd=str(RENDER), env=env).returncode


def tree(targets):
    for triple in targets:
        env = common(triple, 'release')
        found = version_gate(triple, env)
        log('tree %s: %s' % (triple, ', '.join('%s %s' % (n, '/'.join(v) or 'absent') for n, v in found.items())))
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    if '--' in argv:
        i = argv.index('--')
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('build', 'test', 'run'):
        p = sub.add_parser(name)
        p.add_argument('--target', default=X64)
        p.add_argument('--features', action='append', default=[])
        p.add_argument('--profile', default='release')
        if name == 'build':
            p.add_argument('--no-cache', action='store_true')
            p.add_argument('--report', type=Path)
        if name == 'run':
            p.add_argument('--bin', required=True)
    p = sub.add_parser('tree')
    p.add_argument('--target', action='append')
    p = sub.add_parser('selftest')
    p.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'build':
            if rest:
                refuse('build takes no arguments after --')
            if a.report and a.report.exists():
                a.report.unlink()
            rep = build(a.target, a.features, a.profile, a.no_cache)
            if a.report:
                a.report.parent.mkdir(parents=True, exist_ok=True)
                a.report.write_text(json.dumps(rep, indent=1, sort_keys=True) + '\n', encoding='utf-8')
            print(json.dumps({k: rep[k] for k in ('lib', 'sha256', 'cache', 'key')}), flush=True)
            return 0
        if a.cmd == 'test':
            return test(a.target, a.features, a.profile, rest)
        if a.cmd == 'run':
            return run(a.target, a.features, a.profile, a.bin, rest)
        if a.cmd == 'tree':
            return tree(a.target or list(TARGETS))
        if a.cmd == 'selftest':
            cmd = [sys.executable, str(TOOLS / 'rustgs_selftest.py')] + (['--keep'] if a.keep else [])
            return subprocess.run(cmd, cwd=str(ROOT)).returncode
    except Refused as e:
        print('rustgs: refused: %s' % e, flush=True)
        return 2
    return 1


if __name__ == '__main__':
    sys.exit(main())
