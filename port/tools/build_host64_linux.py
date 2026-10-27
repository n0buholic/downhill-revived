"""Build a wasm2c module and the native Linux host as dhd64.

Accepts .wasm or WABT .c units (module name must be core). The module is the
same portable core the Windows x64 build uses; the host is the POSIX
backend (port/host/host_posix.c, POSIX threads). No force-link or missing stubs. All generated C is
prepared before compilation; all output stays in build/.

With --sdl-lib the SDL window, input, audio and Vulkan renderer are enabled.
DHD_SOUND=sdl also enables SDL automatically; DHD_SOUND=null disables audio.
Without SDL, the null sound/GPU backends are linked and no window opens. Run it through
port/tools/state_trace_run.py --linux.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_host64_imports import generate
from prepare_wasm2c import check_launcher_exports, module_units, prepare_units

ROOT = Path(__file__).resolve().parents[2]
WABT = Path('/opt/wabt')
SOURCE = ROOT / 'port/host64'

# The host units, in link order: the Windows set (host64_census.HOST_UNITS)
# with each OS backend swapped for its Linux counterpart. The sound backend
# follows DHD_SOUND (sdl: SDL audio, else the null backend);
# port/host/sdl_host.c joins the list when the SDL window is enabled.
HOST_UNITS = [
    'port/host/host_posix.c', 'port/host/__sound__', 'port/runtime/sha256.c',
    'port/hle/host.c', 'port/hle/graphics_queue.c', 'port/hle/vif.c', 'port/runtime/vu0_interp.c',
    'port/gs/memory.c', 'port/gs/gs.c', 'port/gs/display.c', 'port/gs/present.c', 'port/gs/gpu_null.c',
    'port/gs/texpack_linux.c', 'port/gs/capture.c',
    'port/hle/gamepad.c', 'port/hle/keyboard.c', 'port/hle/text_input.c', 'port/hle/movie.c',
    'port/hle/splash.c', 'port/hle/update.c', 'port/hle/fps.c',
    'port/net/client.c', 'port/net/hash.c', 'port/net/json.c', 'port/net/net.c',
    'port/net/ws_posix.c',
] + ['port/third_party/libmpeg2/%s.c' % n for n in (
    'alloc', 'cpu_accel', 'cpu_state', 'decode', 'header', 'idct', 'motion_comp', 'slice')]
# port/hle/gs_queue.inc is included by graphics_queue.c.


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('module', type=Path, nargs='+')
    p.add_argument('--out', type=Path, default=ROOT / 'build/host64-linux')
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--num-outputs', type=int, default=1, help='WABT split C output count for .wasm input')
    p.add_argument('--sdl-lib', type=Path,
                   help='Linux x64 SDL3 static library: the SDL window and input (sdl3.py --linux)')
    p.add_argument('--sdl-include', type=Path, help='its include directory (with --sdl-lib)')
    p.add_argument('--sdl-libs', default=None, metavar='LIBS',
                   help='system libraries for the static SDL3, one quoted string '
                        '(default: from sdl3.py --linux)')
    p.add_argument('--wabt', type=Path, default=WABT)
    p.add_argument('--simde', type=Path, default=Path('/opt/simde-src'),
                   help='SIMDe headers (the wasm SIMD shim, as in build_host64.py)')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if str(out).startswith('/mnt/c/'):
        p.error('build output must stay off C:')
    # The sound output: the null backend, or SDL3's audio stream. SDL audio is the default
    # whenever a window is enabled, like build_host64.py does on Windows.
    sdl_sound = {'sdl': True, 'null': False}.get(os.environ.get('DHD_SOUND'), a.sdl_lib is not None)
    sdl_lib, sdl_include, sdl_libs = a.sdl_lib, a.sdl_include, a.sdl_libs
    if sdl_sound and sdl_lib is None:
        from sdl3 import linux_build
        sdl_lib, sdl_include, default_libs = linux_build()
        if sdl_libs is None:
            sdl_libs = default_libs
    sdl = sdl_lib is not None
    if sdl and sdl_include is None:
        p.error('--sdl-lib needs --sdl-include')
    if sdl and sdl_libs is None:
        from sdl3 import linux_build
        _, _, sdl_libs = linux_build()  # cached: no rebuild
    elif isinstance(sdl_libs, str):
        sdl_libs = shlex.split(sdl_libs)
    sound = 'port/host/sound_out_sdl.c' if sdl_sound else 'port/host/sound_out_null.c'
    host_units = [u.replace('port/host/__sound__', sound) for u in HOST_UNITS]
    if sdl:
        host_units.insert(2, 'port/host/sdl_host.c')  # the SDL thread, with the sound unit
        # the Rust renderer replaces the null GPU backend (finding 098): its surface sits on
        # the SDL window (X11 or Wayland), headless runs keep working through DHD_HEADLESS
        host_units[host_units.index('port/gs/gpu_null.c')] = 'port/gs/gpu_rust.c'
        host_units.insert(host_units.index('port/gs/texpack_linux.c') + 1, 'port/gs/font_sharp.c')
    env = dict(os.environ, TMPDIR=str(out / 'tmp'))
    (out / 'tmp').mkdir(exist_ok=True)
    commands = []

    def run(cmd, timeout=300):
        cmd = list(map(str, cmd))
        commands.append(cmd)
        r = subprocess.run(cmd, cwd=out, env=env, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout)
        if r.returncode:
            raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        return r.stdout + r.stderr

    rt = a.wabt / 'share/wabt/wasm2c'
    wasm = a.module[0].resolve() if len(a.module) == 1 and a.module[0].suffix == '.wasm' else None
    units = module_units(out, run, a.wabt / 'bin/wasm2c', wasm, a.module, a.num_outputs)
    rows, exports = generate(out / 'core.h', out, linux=True)
    check_launcher_exports(exports)
    prepare_units(units, out)
    common = ['clang', '-c', '-O2', '-march=x86-64-v2', '-fPIC',
              '-ffile-prefix-map=' + str(Path.home()) + '=home',
              '-ffile-prefix-map=' + str(ROOT) + '=.',
              '-ffp-model=strict', '-ffp-contract=off',
              # wasm-rt.h defaults Linux to guard-page checks, which our mirror allocator
              # replaces: keep every generated OOB access a trapping check (finding 082).
              '-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1',
              '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing',
              # One section per function/data, as the Windows build's /Gy: the host keeps
              # legacy guest-extern helpers that only some builds reference, and the link
              # below garbage-collects the unreferenced ones (/opt:ref there).
              '-ffunction-sections', '-fdata-sections',
              '-fwrapv', '-pthread', '-w',
              '-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_MAX_CALL_STACK_DEPTH=512',
              # Traps can come from any EE fiber: report and end (launcher.c).
              '-DWASM_RT_TRAP_HANDLER=host64_trap',
              '-I' + str(out), '-I' + str(a.wabt / 'include'), '-I' + str(rt), '-I' + str(SOURCE),
              '-I' + str(ROOT / 'port/runtime'),
              '-I' + str(ROOT / 'port/include'), '-I' + str(ROOT / 'port/host'),
              '-I' + str(a.simde)]
    objdir = out / 'obj'
    objdir.mkdir(exist_ok=True)
    # The units that include SDL3's headers (host64_census.SDL_UNITS); gpu_rust.c too, for the
    # window's back end (gs_gpu_surface).
    sdl_units = ('port/hle/gamepad.c', 'port/hle/text_input.c', 'port/gs/present.c',
                 'port/gs/gpu_rust.c', 'port/host/sdl_host.c', 'port/host/sound_out_sdl.c')

    # VU1's straight-line blocks as C (vu1_aot.inc and vu1_native_clip.inc, from the game's
    # .vutext: DHD_VUTEXT or assets/vutext.bin); vu0_interp.c runs them
    import vu1_aot
    gen = out / 'gen'
    gen.mkdir(exist_ok=True)
    vu1_aot.generate(gen / 'vu1_aot.inc')
    if sdl:
        icon = (ROOT / 'port/res/icon.png').read_bytes()
        (gen / 'dhd_icon.inc').write_text('static const unsigned char dhd_icon_png[] = {' +
                                        ','.join(map(str, icon)) + '};\n')

    def compile_one(source):
        source = Path(source)
        rel = str(source.relative_to(ROOT)) if source.is_relative_to(ROOT) else source.name
        extra = ['-I' + str(gen)] if rel.startswith('port/') else []
        if sdl and rel in sdl_units:  # the SDL3 headers (sdl3.py)
            extra += ['-DDHD_HAVE_SDL', '-I' + str(Path(sdl_include).resolve())]
        if rel.endswith('gs/gpu_rust.c'):
            extra += ['-I' + str(ROOT / 'port/render/include')]  # dhd_render.h
        if rel.endswith('vu0_interp.c'):
            extra += ['-DPS2_VU_AOT']
        if 'libmpeg2/' in rel:
            extra += ['-I' + str(ROOT / 'port/third_party/libmpeg2')]
        obj = objdir / (rel.replace('/', '_') + '.o')
        run(common + extra + [str(source), '-o', str(obj)], timeout=1800)
        return obj

    try:
        with ThreadPoolExecutor(a.jobs) as pool:
            host_objects = list(pool.map(compile_one, [ROOT / u for u in host_units]))
        library = out / 'libhost64.a'
        run(['rm', '-f', str(library)])  # 'ar r' keeps stale members from an earlier sound unit
        run(['ar', 'rcs', str(library), *[str(o) for o in host_objects]])
        objects = []
        glue = [rt / 'wasm-rt-impl.c', out / 'imports_gen.c'] + [SOURCE / n for n in
                ('guest_memory.c', 'guest_memory_posix.c', 'fiber_posix.c', 'fp_mode.c', 'bridge.c',
                 'diagnostics.c', 'launcher.c')]
        with ThreadPoolExecutor(a.jobs) as pool:
            objects = list(pool.map(compile_one, units + glue))
        link = [str(x) for x in objects + [library]]
        rust_inputs = []
        if sdl:
            import rustgs  # cargo through rustgs only (its flags, version gate, checks and cache)
            try:
                report = rustgs.build(rustgs.LINUX)
            except rustgs.Refused as why:
                raise RuntimeError('rustgs: refused: %s' % why)
            # rustc's list for the static library (checked by rustgs against
            # port/render/native-static-libs-x86_64-unknown-linux-gnu.txt)
            link.append(report['lib'])
            rust_inputs = [x for x in report['native_static_libs'] if x not in ('-lc',)]
        sdl_inputs = [os.path.abspath(str(sdl_lib))] + list(sdl_libs) if sdl else []
        run(['clang', '-O2', '-march=x86-64-v2', '-pthread', '-rdynamic',
             '-Wl,--gc-sections',
             *link, *sdl_inputs, *rust_inputs,
             '-o', str(out / 'dhd64'), '-ldl', '-lm'])
        (out / 'failure.log').unlink(missing_ok=True)
        print(f'PASS: {len(rows)} generated imports; {len(host_objects)} Linux host objects'
              f'{" (SDL window/input" + (", SDL audio" if sdl_sound else ", null audio") + ")" if sdl else " (null audio, no window)"}; {out / "dhd64"}',
              flush=True)
    except (RuntimeError, ValueError) as error:
        (out / 'failure.log').write_text(str(error))
        raise SystemExit(str(error))
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
