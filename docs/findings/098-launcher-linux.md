# 098: the launcher on Linux

Finding 093's Tauri launcher is one codebase for Windows, macOS and Linux; nothing about the
window, the Rust commands or the front end is Windows-only (`game.rs` already named the Linux
game binary `dhd`, `index.html` already drags with `data-tauri-drag-region`). What was missing
was Linux *building* and *updating*: every tooling step assumed an .exe.

## What changed

- `port/tools/build_launcher.py` is platform-aware: the executable name is `DownhillRevived`
  without `.exe` and without `-Ctarget-feature=+crt-static` on Linux (glibc has no static CRT a
  native-tls webview process can use); the CARGO_HOME/RUSTUP_HOME remap stays, the drive-letter
  (C:/F:) check is a Windows one.
- `update.rs` picks its release asset by platform: `DownhillRevived-*-win.zip` on Windows,
  `-linux.zip` on Linux. Before, a Linux launcher would have pushed Windows binaries over a
  Linux game (or refused an official release that only ships them). The error names the missing
  platform. `installs_a_release` now packs `crate::game::GAME_EXE`, so it tests on both.
  Releases still ship no Linux zip: Linux players get the check's "no linux zip with a digest"
  until one is packaged (`package_release.py` is a Windows flow for a Linux zip too).

## How it builds and runs (Fedora-like host: webkit2gtk-4.1, gtk3, node, cargo ≥ 1.97)

    cd port/launcher && npm install
    python3 port/tools/build_launcher.py   # prints .../target/release/DownhillRevived

Rust's `rust-version = "1.97"` is required (the machine's 1.93 stable needed `rustup update`).
`cargo test` passes on Linux (5 tests). The binary runs under Wayland too: GTK's Wayland
dispatch can end in a "protocol error 71" with this no-decoration window (the titlebar is set
after the window realizes, a tao/GTK quirk), so main() sets `GDK_BACKEND=x11` when needed —
X11 there is XWayland. WebKitGTK's dmabuf renderer can also blank the page wholesale
("Failed to create GBM buffer", some driver stacks): main() sets
`WEBKIT_DISABLE_DMABUF_RENDERER=1` for the webview (both only when the player set no value).
Snapshot names the game binary per platform; the footer's "isn't in this folder" says `dhd`,
the Settings hint names no binary.

## Open

- `package_release.py` and the release flow are Windows-only: a Linux zip (and an AppImage, per
  finding 093) needs that script to learn a second platform.
- The launcher's own on-screen look was not verified by screenshot on this host, only that the
  process runs and the checks pass — same HTML/CSS as Windows by construction.

## The native Linux game (2026-10-28)

The same full pipeline Windows has, on a Fedora-like Linux host, no Wine: wasm2c core + POSIX
host + the Rust renderer on Vulkan + SDL3, linked as `build/host64-linux/dhd64`. Verified by
running: disc load, title, menus, movies, PC settings, in-game rendering (RTX 3050 laptop,
Plasma Wayland).

- Tools: wasi-sdk 34.0 (Linux), WABT 1.0.42 and SIMDe under `~/DevCaches`
  (`WASI_SYSROOT`/`WABT_DIR`/`SIMDE_DIR`); `clang` for the data image's COFF check needs a
  system target list, so `build_wasm.py` takes `DHD_COFF_CLANG` (defaults to `clang`).
- The Rust renderer gained an `x86_64-unknown-linux-gnu` target (`rustgs.py` LINUX: the same
  flags and gates; `cargo fetch` once, offline after; its `~/.cargo` paths remapped like
  CARGO_HOME's). Its allow-list is `port/render/native-static-libs-x86_64-unknown-linux-gnu.txt`.
- Surfaces: `surface.rs` builds `XlibWindowHandle` (kind 3, `a` = the X11 Window, `b` = the
  `Display*`) and `WaylandWindowHandle` (kind 4, `a` = wl_surface*, `b` = wl_display*); ffi.rs
  allows 1/3/4 on non-Android. `gpu_rust.c` reads SDL's `SDL_PROP_WINDOW_*` properties and picks
  the kind per session; `present.c`'s Linux init mirrors the Windows one (splash, minimise,
  window settings) and `gs_window_apply` is real on SDL as on _WIN32.
- SDL3 was built for Linux by `sdl3.py --linux` (XSCrnSaver off: the game never calls a screen
  saver function, and the Fedora -devel packages for it are not installed).
- `build_host64_linux.py` first working shape: the GLUE swaps gpu_null → gpu_rust and adds
  font_sharp when SDL is on; `DHD_SOUND` defaults to sdl with a window; the archive is recreated
  (`rm -f` before `ar rcs` — 'r' keeps stale members, which once silently linked the *null*
  sound backend over the SDL one); `core.c` is compiled as `--num-outputs 6` parallel units
  (one whole `core.c` takes > 15 min single-threaded); the SDL lib path is passed absolute.
- The launch needs no install: `DHD_ISO=<your iso> ./dhd` (dhd.ini's `iso =` does the same).
  Speech/music/music movies out through PulseAudio (`989snd: SDL audio output started
  ... backend pulseaudio`).

Open: a rare early-boot SIGSEGV in the game thread (guest memory base + 4, once in the first
run, not reproduced in a 180 s gdb run); occasional slow motion (frame pacing); the Linux zip
for the launcher's updates (above).

Follow-up: finding 099 addresses the pacing issue; finding 102 supplies the single
AppImage package and treats an absent Linux release as unavailable rather than an error.
