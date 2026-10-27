"""SDL3 for the PC build: generic controllers (finding 059).

    python port/tools/sdl3.py          prints the static library it built (or had built)

package_release.py calls build(X64_TARGET) for build_host64.py. It downloads SDL's release source
once (pinned version, SHA-256 checked; nothing else is accepted), unpacks it under build/port/sdl3
and builds a static library with CMake and Ninja for 64-bit Windows (x86_64-pc-windows-msvc,
clang, the static C runtime that dhd64.exe uses).
The window (video, Direct3D 11 surface only: no OpenGL, Vulkan or SDL renderer) and the audio
output (WASAPI) are built too (finding 083 / W11). No GPU or camera subsystems.
linux_build() is the same source built for Linux x64 (build_host64_linux.py, WSL only):
the window (X11 and Wayland where their dev packages are present) and audio
(PulseAudio/PipeWire and ALSA where present), same SDL version and subsystem selection.
android_build() (--android) is libSDL3.so for Android arm64 with the NDK, plus SDL's file dialog,
and the release's Java sources (build_host64_android.py, port/android/build_apk.py; finding 091).
SDL is zlib-licensed (port/release/THIRD-PARTY-NOTICES.txt).

The download is kept in build/downloads, so a rebuild needs no network. Needs CMake on PATH (or in its default place), LLVM's clang and the
ninja Python package (requirements.txt)."""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

VERSION = '3.4.18'
URL = 'https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL3-%s.tar.gz' % (VERSION, VERSION)
SHA256 = '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3'

ROOT = Path(__file__).resolve().parents[2]
DOWNLOADS = ROOT / 'build' / 'downloads'
OUT = ROOT / 'build' / 'port' / 'sdl3'
CMAKE_DEFAULT = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'CMake' / 'bin' / 'cmake.exe'

# Everything but the window, audio and controllers off. SDL_JOYSTICK brings the drivers: XInput, RawInput,
# Windows.Gaming.Input, DirectInput (generic pads, with the built-in mapping database) and HIDAPI
# (DualSense, DualShock 4, Switch Pro, 8BitDo and others, rumble included).
OPTIONS = dict(SDL_SHARED='OFF', SDL_STATIC='ON', SDL_TEST_LIBRARY='OFF', SDL_TESTS='OFF', SDL_EXAMPLES='OFF',
               SDL_INSTALL='OFF', SDL_AUDIO='ON', SDL_VIDEO='ON', SDL_OPENGL='OFF', SDL_OPENGLES='OFF',
               SDL_VULKAN='OFF', SDL_GPU='OFF', SDL_RENDER='OFF',
               SDL_CAMERA='OFF', SDL_POWER='OFF', SDL_SENSOR='OFF', SDL_DIALOG='OFF', SDL_TRAY='OFF',
               SDL_HAPTIC='OFF', SDL_JOYSTICK='ON', SDL_HIDAPI='ON', SDL_HIDAPI_JOYSTICK='ON',
               SDL_VIRTUAL_JOYSTICK='OFF', SDL_LIBC='ON', SDL_ASSEMBLY='OFF',
               SDL_DISABLE_INSTALL='ON', SDL_DISABLE_UNINSTALL='ON',
               # SDL's CMake looks for dinput.h only when audio or video is on, which would leave
               # out the DirectInput driver the generic pads need: the Windows SDK has it.
               HAVE_DINPUT_H='1',
               # GameInput (a C++ driver for Microsoft's newest API, off by default on desktop
               # Windows) is left out.
               HAVE_GAMEINPUT_H='0',
               # atan from SDL's own libm (fdlibm) instead of the C runtime's: the runtime's atan
               # brings a table of doubles in which package_release.py's path check reads
               # "Y:/(A6>" (SDL's dynamic API table references every SDL function, SDL_atan too)
               LIBC_HAS_ATAN='0', LIBC_HAS_ATANF='0', LIBC_HAS_ATAN2='0', LIBC_HAS_ATAN2F='0')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch():
    tarball = DOWNLOADS / ('SDL3-%s.tar.gz' % VERSION)
    if not tarball.exists() or sha256(tarball) != SHA256:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        part = tarball.with_suffix('.part')
        print('SDL3: downloading', URL)
        with urllib.request.urlopen(URL, timeout=120) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256(part)
        if got != SHA256:
            part.unlink()
            sys.exit('SDL3: %s has SHA-256 %s, expected %s' % (URL, got, SHA256))
        os.replace(part, tarball)
    return tarball


def tool(name, default=None):
    found = shutil.which(name)
    if found:
        return found
    if default and Path(default).exists():
        return str(default)
    sys.exit('SDL3: %s not found (docs: README.md, Building)' % name)


X64_TARGET = 'x86_64-pc-windows-msvc'  # dhd64.exe (build_host64.py, package_release.py)


def build(target=X64_TARGET):
    """The static library and its include directory; built once per version and target, under
    build/port/sdl3/<version>-<arch> with the same options (the window, audio and controllers)."""
    base = OUT / ('%s-%s' % (VERSION, target.split('-')[0]))
    lib = base / 'build' / 'SDL3-static.lib'
    include = base / 'src' / ('SDL3-%s' % VERSION) / 'include'
    stamp = base / 'built.txt'
    recipe = repr(sorted(OPTIONS.items())) + target
    if lib.exists() and stamp.exists() and stamp.read_text() == recipe:
        return lib, include
    tarball = fetch()
    src_parent = base / 'src'
    if src_parent.exists():
        shutil.rmtree(src_parent)
    src_parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src_parent, filter='data')
    src = src_parent / ('SDL3-%s' % VERSION)
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    import ninja
    cmake = tool('cmake', CMAKE_DEFAULT)
    clang = tool('clang')
    cmd = [cmake, '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_MAKE_PROGRAM=' + os.path.join(ninja.BIN_DIR, 'ninja.exe'),
           '-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_C_COMPILER_TARGET=' + target,
           '-DCMAKE_CXX_COMPILER=' + clang.replace('clang', 'clang++'), '-DCMAKE_CXX_COMPILER_TARGET=' + target,
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
           '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW']
    cmd += ['-D%s=%s' % kv for kv in sorted(OPTIONS.items())]
    for step in (cmd, [cmake, '--build', str(bld)]):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    stamp.write_text(recipe)
    return lib, include


# The Windows libraries the static SDL3 needs at link time.
SYSTEM_LIBS = ['setupapi.lib', 'winmm.lib', 'imm32.lib', 'version.lib', 'ole32.lib', 'oleaut32.lib',
               'advapi32.lib', 'shell32.lib', 'user32.lib', 'gdi32.lib', 'cfgmgr32.lib', 'uuid.lib',
               'dwmapi.lib', 'shcore.lib']


LINUX_DIRNAME = '%s-linux-x86_64' % VERSION  # build/port/sdl3/<version>-linux-x86_64


def linux_build():
    """The Linux x64 static library, its include directory and the system libraries it needs.

    Same SDL version, same subsystem selection (OPTIONS: the window, audio and controllers;
    no GPU/renderer/camera). CMake auto-detects the backends whose dev packages are installed
    (X11, Wayland, PulseAudio/PipeWire, ALSA); the build fails closed if neither X11 nor
    Wayland is found, so a windowless SDL3 never ships silently. The library installs under
    its build dir (build/port/sdl3/<version>-linux-x86_64/install); the system libraries come
    from the installed sdl3.pc (`pkg-config --static --libs`). WSL/Linux only. Output stays
    under build/."""
    import json as _json
    if sys.platform == 'win32':
        sys.exit('SDL3: --linux needs WSL or Linux (run python3 port/tools/sdl3.py --linux there)')
    base = OUT / LINUX_DIRNAME
    prefix = base / 'install'
    lib = prefix / 'lib' / 'libSDL3.a'
    include = prefix / 'include'
    stamp = base / 'built.txt'
    libs_file = base / 'link_libs.json'
    recipe = repr(sorted(OPTIONS.items())) + 'linux-x86_64'
    if lib.exists() and stamp.exists() and libs_file.exists() and stamp.read_text() == recipe:
        return lib, include, _json.loads(libs_file.read_text())
    for name in ('cmake', 'ninja', 'pkg-config'):
        if not shutil.which(name):
            sys.exit('SDL3: %s not found on PATH (apt: cmake, ninja-build, pkg-config)' % name)
    tarball = fetch()
    src_parent = base / 'src'
    if src_parent.exists():
        shutil.rmtree(src_parent)
    src_parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src_parent, filter='data')
    src = src_parent / ('SDL3-%s' % VERSION)
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    if prefix.exists():
        shutil.rmtree(prefix)
    cmd = ['cmake', '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
           '-DCMAKE_INSTALL_LIBDIR=lib']
    cmd += ['-D%s=%s' % kv for kv in sorted(OPTIONS.items())
            if kv[0] not in ('SDL_INSTALL', 'SDL_DISABLE_INSTALL', 'SDL_DISABLE_UNINSTALL',
                             'HAVE_DINPUT_H', 'HAVE_GAMEINPUT_H')]
    # The X screen saver inhibitor alone has no Fedora -devel package preinstalled and the game
    # calls no SDL screensaver function: build SDL3 without it (BSD/Linux screen-saver inhibit is
    # a nice-to-have; Windows ships its compiled library).
    cmd += ['-DSDL_X11_XSCRNSAVER=OFF']
    cmd += ['-DSDL_INSTALL=ON', '-DSDL_DISABLE_INSTALL=OFF', '-DSDL_DISABLE_UNINSTALL=OFF']
    for step in (cmd, ['cmake', '--build', str(bld)], ['cmake', '--install', str(bld)]):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 Linux build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    video = [line for line in (bld / 'CMakeCache.txt').read_text().splitlines()
             if line.startswith(('SDL_X11:', 'SDL_WAYLAND:'))]
    env = dict(os.environ, PKG_CONFIG_PATH=str(prefix / 'lib' / 'pkgconfig'))
    p = subprocess.run(['pkg-config', '--static', '--libs', 'sdl3'], capture_output=True, text=True, env=env)
    if p.returncode:
        sys.exit('SDL3: sdl3.pc missing after install:\n' + (p.stdout + p.stderr)[-2000:])
    libs = [word for word in p.stdout.split() if word != '-lSDL3' and 'SDL3' not in word]
    # The backends' own libraries need not appear above: SDL3 dlopens them
    # (configure logs `dynamic libX11 -> ...`), so the window-backend gate reads
    # the configure result, not the link line. (nm confirms the compiled archive
    # defines X11_bootstrap, Wayland_bootstrap and the audio bootstraps.)
    if not any(line.endswith('=ON') for line in video):
        sys.exit('SDL3: no window backend (X11/Wayland dev packages missing?):\n  ' + '\n  '.join(video))
    libs_file.write_text(_json.dumps(libs) + '\n')
    stamp.write_text(recipe)
    return lib, include, libs


ANDROID_API = 30  # Android 11 (memfd_create, the Mi 11i's first release); build_host64_android.py's too
ANDROID_DIRNAME = '%s-android-arm64' % VERSION  # build/port/sdl3/<version>-android-arm64


def android_ndk():
    """The NDK: ANDROID_NDK_HOME, else the newest under the SDK (ANDROID_HOME/ndk). Never on C:."""
    named = os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
    if named:
        ndk = Path(named)
    else:
        sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or Path.home() / 'Android/Sdk')
        found = sorted((p for p in (sdk / 'ndk').glob('*') if (p / 'source.properties').exists()),
                       key=lambda p: [int(x) for x in p.name.split('.') if x.isdigit()])
        if not found:
            sys.exit('SDL3: no NDK under %s/ndk (docs/findings/091-android.md, Toolchain)' % sdk)
        ndk = found[-1]
    if not (ndk / 'build' / 'cmake' / 'android.toolchain.cmake').exists():
        sys.exit('SDL3: %s is not an NDK' % ndk)
    return ndk


def android_build():
    """libSDL3.so for Android arm64-v8a (API ANDROID_API), its include directory and the Java
    sources of the same release (android-project/app/src/main/java: SDLActivity and friends, which
    build_apk.py puts in the APK). Same subsystem selection as OPTIONS (window, audio, controllers),
    plus SDL's file dialog (the disc image is chosen through Android's document picker). A shared
    library, as SDL's Java side loads it (System.loadLibrary("SDL3")); its own libc++ is static.
    Built with the NDK's CMake toolchain file, CMake and Ninja, under
    build/port/sdl3/<version>-android-arm64."""
    base = OUT / ANDROID_DIRNAME
    src = base / 'src' / ('SDL3-%s' % VERSION)
    lib = base / 'build' / 'libSDL3.so'
    include = src / 'include'
    java = src / 'android-project' / 'app' / 'src' / 'main' / 'java'
    stamp = base / 'built.txt'
    ndk = android_ndk()
    options = {k: v for k, v in OPTIONS.items() if k not in ('HAVE_DINPUT_H', 'HAVE_GAMEINPUT_H')}
    options.update(SDL_SHARED='ON', SDL_STATIC='OFF', SDL_DIALOG='ON')
    recipe = repr(sorted(options.items())) + 'android-arm64-%d %s' % (ANDROID_API, ndk.name)
    if lib.exists() and stamp.exists() and stamp.read_text() == recipe:
        return lib, include, java
    tarball = fetch()
    if src.parent.exists():
        shutil.rmtree(src.parent)
    src.parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src.parent, filter='data')
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    import ninja
    cmake = tool('cmake', CMAKE_DEFAULT)
    cmd = [cmake, '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_MAKE_PROGRAM=' + os.path.join(ninja.BIN_DIR, 'ninja.exe' if os.name == 'nt' else 'ninja'),
           '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build' / 'cmake' / 'android.toolchain.cmake'),
           '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-%d' % ANDROID_API,
           '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release']
    cmd += ['-D%s=%s' % kv for kv in sorted(options.items())]
    for step in (cmd, [cmake, '--build', str(bld), '-j', '2']):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 Android build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    stamp.write_text(recipe)
    return lib, include, java


if __name__ == '__main__':
    if sys.argv[1:] == ['--android']:
        print(*android_build(), sep='\n')
    elif sys.argv[1:] == ['--linux']:
        lib, include, libs = linux_build()
        print('%s\n%s\n%s' % (lib, include, ' '.join(libs)))
    else:
        print(*build())
