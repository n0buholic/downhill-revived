# State

Goal: readable, **matching** C/C++ decompilation of Downhill Domination (PS2, USA `SCUS_971.77`):
source that rebuilds the retail executable byte for byte. Later it can serve as the base for a PC
port. PCSX2 is the behavioural reference.

Progress: `python tools/report.py` (objdiff report `progress/report.json`, README table).
2026-10-12: game 98.17% and engine 92.61% of code matched, 4,526 of 4,593 functions (the SDK and
libgcc regions are library code and stay assembly). Left: 28 functions in the queue, 7
hand-written, 164 eight-byte remnants that aren't functions.

## Phase 0 — compiler identification (done)
`docs/findings/002-compiler-sn-prodg.md`. Game code: **SN ProDG 3.01** — cc1 "2.95.3 SN BUILD
1.36" + SN assembler `ps2eeas`, **`-O2 -G8 -fopt-stack`**; C++ `-fno-exceptions` (finding 003).
Library regions are identified (finding 003, `config/libmatch.txt`) and not decompiled.

## Phase 1 — project skeleton (done)
`docs/findings/003-build-system.md`. The retail image rebuilds byte-identical from splat asm +
C/C++ (`python tools/configure.py && python -m ninja` → MATCH), checked by `tools/check_image.py`.
Tooling: `tools/queue.py` (work queue + claims), `tools/try_func.py` (per-function trial build,
with a fast compile-only check first), `tools/diff_func.py`, `tools/promote.py` (into `src/` only
if the whole image still matches; refuses TU merges that would turn C back into asm),
`tools/lost_check.py`, `tools/unit_merge.py` (merge existing units into a wider translation
unit), m2c drafts. Guide for contributors: `docs/WORKER.md`.

## Phase 2 — decompilation (finishing; 28 functions left)
Functions are claimed from the queue, decompiled one at a time against the retail bytes and
promoted only when `try_func` prints MATCH and the whole image still matches. Finding 004
covers how the assembler addresses small data, finding 007 what made trials fast.

2026-10-06: `func_00134CC8` and `func_00240358` promoted after exact function and
whole-image checks. The latter required integration into its existing full translation unit.
Finding 009 distinguishes an assigned-function implementation from a matching assembly stub,
and records serialization of report generation with source promotion. A fresh full-image check
matched all eight checked sections.

2026-10-11: six more matches (00137300, 00137B70, 00148F60, 001554C0, 00171B28,
00228BE8), including two translation-unit merges. The rebuilt image matches all eight
checked retail sections.

2026-10-21 (finding 074): the 25 functions not matched yet have behaviour-equivalent C under
`#ifdef NONMATCHING` next to their `INCLUDE_ASM` (18 new single-function units). The default build
keeps the asm and still matches; `python tools/configure.py --nonmatching` builds the C into
build-nonmatching/ (links, not checked; sizes and the moved ranges are in the finding).
func_00220DA0 is now listed in config/handwritten.txt (trapping adds, pmfhi/pmflo).

Open items:
- `engine` region 0x201000–0x243000 (1,202 functions): same toolchain and flags as game code;
  what it is (engine/middleware) is still open.
- Real TU boundaries: single-function units are merged when evidence appears (shared `$gp`
  data, `.rodata` ownership). func_001F24B8 and func_001C8380 have matching C that still needs
  its translation unit worked out before it can be promoted.
- Per-file flags: two small clusters were compiled `-O0 -fno-opt-stack` (0x21E9A8–0x21EAA0,
  0x2212A0–0x221608); rule in WORKER.md.
- Language: the game is C++ (exception tables, vtables, `this`-style calls). Matching is done in
  whatever form is fastest (mostly C, byte-identical to the method form); a later phase turns
  shared structs into classes and merges functions into reconstructed TUs.
- Hand-written assembly (e.g. func_0021B8A8: registers saved to the scratchpad, `$gp` used as
  scratch) stays `.s`.
- `.vutext`/DVP overlays (VU microcode) stay binary for now.

## Phase 3 — PC port: Downhill Revived (current)
Plan: `docs/findings/015-pc-port-plan.md`. Port code lives in `port/` and is applied at build
time; the matching sources and checks don't change. Build steps and requirements:
`docs/BUILDING.md`. Players' text: `port/release/README.txt`. The round-by-round log of how the
port got here (2026-10-12 to 2026-10-25) is in the findings (015-088) and in this file's history
(`git show v0.3.1:docs/STATE.md`).

Releases: 0.3.1 (2026-10-25) is the last 32-bit release (tag `v0.3.1`; it can always be
rebuilt from there). 0.4.0 is the first 64-bit release, the build below; 0.3.1's updater installs
it unchanged (finding 088, CL3).

### The builds
- One portable game core: `port/tools/build_wasm.py` compiles every `src/` unit (rewritten at
  build time by `port/tools/transform.py`, with the PC patches of `port/patches*.py`), the
  asm-only functions recompiled to C (`port/tools/recomp.py`), the module-side runtime and HLE
  and the data image's fixups into one wasm32 module, `build/wasm/dhd_core.wasm` (findings 081,
  082). Everything comes from the checkout, `asm/` and `assets/vutext.bin`.
- Windows x64: `port/tools/package_release.py` runs `build_host64.py` (wasm2c, the x64 host
  `port/host64` + `port/host`, the Rust renderer) and writes the players' zip
  (`build/release/DownhillRevived-<version>-win.zip`, `latest.json`) after its checks: version,
  no absolute path, no server but `dhd.skuza.app`, no retail bytes (`scan_retail.py`), Windows
  DLL imports only, the Rust library (`check_rust_lib.py`), pinned `dxcompiler.dll`, generated
  Rust crate notices. It builds only from committed source.
  Guest RAM mirrors: the OS views plus `memory_access.h`, with one inline window test per base
  value per access site (`prepare_wasm2c.py split_accesses`) and the fold out of line (finding 090 OP1).
- Browser: `port/web/build.py build/wasm/dhd_core.wasm` (into `build/web/`). The graphics side
  (VIF1/VU1, GS front end, renderer) runs in its own worker on shared memory by default
  (`--no-threads` for one worker; finding 087, BS2). It draws only with the Rust renderer
  (`--renderer rust` is still accepted; the JS/WebGPU renderer `www/gpu.js` is gone, finding 088
  CL5). Speed toward 60 fps: finding 087. Its PC SETTINGS (finding 087 WO1): resolution 1x-8x
  (applied at once), scaling, widescreen, font, frame rate (60-144 paced to the core's period).
- Linux x64 (`build_host64_linux.py`, a Linux or WSL host): headless for state traces
  (`DHD_HEADLESS=1`) and, with a window and the renderer, the native Linux game (finding 098):
  SDL3 static (`sdl3.py --linux`, X11 + Wayland), the Rust renderer on Vulkan through the
  Xlib/Wayland surface kinds (`DHDR_WINDOW_XLIB`/`_WAYLAND`), SDL audio out, the POSIX net.
  Runs end to end on Wayland (RTX 3050): disc, title, menus, movie, PC settings, in-game
  rendering. Finding 099 uses clock-paced VBLANK by default on Linux to prevent JIT deadline
  drift, uses CPU pauses for the final deadline wait, and shows the Wayland window on its SDL
  thread. Game/launcher icons reuse the Windows artwork.
  Finding 100 requests compositor fullscreen on Linux, explicitly restores the launcher
  after game exit, and makes movies/loading cards follow the selected picture aspect.
  Finding 101 fixes early Rider Selected skip at high FPS: the skip delay now follows
  elapsed game fields, preventing the active animation blend from resetting the seek.
  Early/late Cross regression runs pass at 60 and 240 FPS.
  Known open items: a rare early-boot SIGSEGV in the game thread (not seen under gdb), X11
  surfaces that offer only BGRA.
  Finding 102 packages the launcher/game as one AppImage with the Windows icon,
  persistent XDG player data, and atomic AppImage updates.
- Launcher (finding 093, 098): `port/launcher`, the Tauri window players see before the game.
  One codebase builds both platforms (`port/tools/build_launcher.py`): the Windows
  `DownhillRevived.exe` in the release zip and a Linux `DownhillRevived` built the same way;
  updates follow the package format (Windows zip, Linux AppImage or legacy Linux zip).
  A release without the platform's package is reported as unavailable, not a failed check.
- Android arm64 (finding 091): `build_host64_android.py` makes `libmain.so` (wasm2c core, the
  POSIX host on arm64, the Rust renderer on Vulkan, SDL3 as `libSDL3.so`) and
  `port/android/build_apk.py` a debug-signed APK with Gradle (no Android Studio). On the Mi 11i:
  title, menus and a race draw right, background/foreground works, about 25 fields/s in a race
  (60 is the goal: speed work next). Gamepad or keyboard only (no touch); online off (no TLS in
  `ws_posix.c`).
- Retired (finding 088): the 32-bit i686 build (`link.py`, `build_clean.py`, the x87/FP-trap
  runtime), the Direct3D 11 renderer, the C wgpu-native renderer and the browser's JS renderer.
  No D3D11 fallback.

### How the port runs the game
- `port/hle/` replaces the PS2 system: EE kernel (threads as fibers, semaphores, interrupts),
  crt0, SIF RPC, disc and file I/O (`DHD_ISO`), DMA, VIF0/VIF1, timers, pads, memory card
  (`mc.c`), the sound driver (989snd replaced by `sound.c`, finding 022), movies (`movie.c`,
  libmpeg2, finding 055).
- The game data image is read at start from the player's `SCUS_971.77` and checked by SHA-1
  (`game_data.c`); the exe holds only the words the port changes (finding 053, section 4).
- Inline asm runs on an emulated EE/FPU/VU0 register file (`port/runtime/ps2asm.c`, plus
  `ps2asm_aot.py`); EE float rounding follows PCSX2's (toward zero, FTZ/DAZ, finding 026,
  `eefloat.py`). VU0 microprograms are native (finding 016); VU1 runs AOT blocks (`vu1_aot.py`,
  MAC/status updates no flag read can see made lazily) and native geometry kernels
  (`vu1_native_clip.py`, `vu1_native_*.h`), checked against PCSX2's interpreter
  (`test_vu1_pcsx2.py`) and on captured run sequences against our interpreter
  (`vu1_seq_diff.py`, x64; finding 090 VU1R).
- GS (`port/gs/`): GS memory and registers in C; the Rust renderer (`port/render`, wgpu: DX12
  or Vulkan, WARP last; WebGPU in the browser) behind the shared C front ends `gpu_exact.inc`
  (exact: the PS2's pixels) and `native_core.inc` (native: upscaled, the default). Glue:
  `gpu_rust.c`, `gpu_rust_native.inc`, `port/web/gpu_rust_web*.{c,inc}`, with what both share in
  `gpu_rust_shared.inc` (findings 085, 086, 088).
- Host services (`port/host`): SDL3 window, sound and controllers (`sdl3.py`, static), the
  keyboard (finding 059), text input, the start window (finding 070).

### What players get
PC SETTINGS in the game's options, saved to dhd.ini (finding 064): resolution and scale,
borderless/windowed, Hor+ widescreen with the HUD anchored (finding 056), frame rate (60, or real
offline simulation steps at higher rates, finding 067), texture packs in PCSX2's format (finding 057), the sharp
font (finding 058), movies, vibration, EXIT GAME. The updater (`port/hle/update.c`, finding 054)
and online races (below). The port's own pages (PC SETTINGS, the exit question, ONLINE and the
race room) share one centred layer in the game's popup style (`menu_page.c`), and the mouse works
in them and in the game's own popups: hover selects, left click X, right click back, the wheel
scrolls or changes values (finding 092; the shell's own menus are not bridged yet).

### Checks and tools
- Native unit tests: `python port/tools/test_runtime.py` (still 32-bit test programs, built from
  the checkout; they don't need the retired build).
- Renderer: replay goldens recorded locally (`gs_golden.py record`/`compare`; they hold pictures
  of the game, so they are not in the repository), `replay_gs.py`, `test_gs_wgpu.py`,
  `test_native_transfers.py`, `web_gs_replay.mjs` for the browser, `gs_verdict.py` (the pass rule)
  and `harness_negative.py` (its negative controls).
- Game state: per-field state traces (`port/hle/state_trace.c`, `state_trace_run.py`,
  `state_diff.py`, `trace_columns.py`; `codeptrs_guard.py`), the wasm offset scan
  (`wasm_offset_scan.py`, run by `build_wasm.py`), `test_fps_race.py` for high rates.
- Retail comparisons with PCSX2 (`compare_pcsx2.py`, `race_compare.py`, finding 024), local and
  online race runs (`lockstep_local.py`, `online_race.py`), timing (`bench_race.py`,
  `sample_profile.py`); crash and trap reports name RVAs with `host64_rva.py`.

### Release steps
Bump `port/include/version.h`, `build_wasm.py`, `package_release.py`, publish the zip and
`latest.json` as `v<version>`, then raise `MIN_VERSION` in
`online/server/wrangler.jsonc` and deploy when older versions should stop racing (finding 054,
section 6; finding 088, CL3).

2026-10-26 (finding 090, SP3): native GS vertex conversion and texture-page key iteration
cost less CPU; one x64 race A/B measured GS busy 4.571 -> 3.807 ms/field.
Exact, native and interpolated replay goldens and all 198 transfer golden items match;
x64 packaging and the Rust browser build/layout pass. Exact/probe paths and game core unchanged.

2026-10-26 (finding 090, SP6): fixed a retired-queue observation race that could wait after
completion; added opt-in per-field cycles and host/GPU stall attribution. One race pair observed p99 73.803 -> 17.763 ms (not an isolated speedup estimate), with unchanged-counter
timeouts 2,443 -> 513. All 4,500 race state-trace fields match with the existing mask; x64 packaging,
Rust browser build/layout and GS FIFO stress pass. Details and the legacy VIF fixture link limit
are in finding 090. No game core or GPU operation changed.

2026-10-26 (finding 090, SP7): x64 core and packaging pass. Two race pairs retain padded queue counters
and the 512-pause retired/full-ring spin policy: 104.548 -> 141.226 and
121.783 -> 124.349 fields/s. EE spin falls in both; second-pair p99 worsens
21.869 -> 24.024 ms. Run-to-run variability prevents an isolated speedup claim.
Full busy/spin/park tables and limitations are in the SP7 section.

2026-10-26 (finding 090, FPS1): at 240 the two counters that stayed per-update now
advance per field behind `port_fps_high()`: the particle/effects frame clock
`D_0029DBAC` (dust, snow and spray waves, glow phases) and the sound/skat queue age
`obj->x60C`; the rate audit (`DHD_RATE_AUDIT`, off by default) now measures them at a quarter
of their 60 Hz travel per update (0.250 and 1.002 against 1.000 and 4.004 before), which is
time-based motion at 240. Boot 600- and race 5000-field state traces against the base build
match in every column with the mask unchanged (3); x64 packaging, Rust browser build and layout
pass.
Measured words, attribution and the audit's false positives are in the FPS1 section.

### Open
- Browser speed: about 25-30 fields/s on the title, 17-25 in a race before finding 087's work;
  the game's worker is CPU-bound.
- Finding 067's high-rate (120/144) AI and physics equivalence limits.

## Facts

- `.text` 0x10a000–0x263bc0 (~1.35 MB), stripped. C++ (only 4 TUs with exception info).
- 5,690 functions (splat): game 3,534, engine 1,202, sdk/libc/libm 802, libgcc 147, crt0 5.
- `_gp` = 0x2A40F0. Single PT_LOAD at file offset 0x1000.
- Disc: `SYSTEM.CNF` BOOT2 = `cdrom0:\SCUS_971.77;1`, VER 1.00.

## Environment
Windows (Git Bash or PowerShell); the build runs natively. Toolchain: `python
tools/setup_toolchain.py` (pinned + hashed in `toolchain.lock.json`: SN ProDG 3.01, ps2dev
binutils 2.45.1, SDK 2.4 libs for identification). m2c: clone into `tools/ext/m2c`. The retail ELF
is not in the repo: extract it from your own disc (`7z e "<USA iso>" SCUS_971.77` into `iso/`,
sha256 adfda7b7…f0cf0c).

## Online play
Shipped since 0.1.0; the original plan is `docs/ONLINE-PLAN.md` (historical). The round log is in
findings 039-047, 053-054, 065 and 072 and in this file's history.
- Server: `online/server/` (Cloudflare Workers + Durable Objects) at `wss://dhd.skuza.app`, the
  game's built-in default (dhd.ini `server =` or `DHD_NET_SERVER` override it). Lobbies, join
  codes, the public list, the race relay (4 players per room by default), desync reports, usage stats
  in Workers Analytics Engine. `MIN_VERSION` (`wrangler.jsonc`, now 0.6.3) turns older clients
  away with "update to play online". Protocol: `docs/online-protocol.md`. Steps:
  `online/server/README.md`.
- Client: `port/net/` (lobby client, WebSocket over WinHTTP, or `ws_posix.c`, hashes; `net.h`), in
  the Windows exe.
  Races use state sync, not input lockstep (finding 041 has why): each PC runs its own one-player
  race and shows the others, and on guests the host's AI riders, as puppet riders written from
  snapshots (`port/hle/puppet.c`, finding 044). Each player sends all its riders in one message.
  Grid, rider/bike looks, the countdown on the server's clock, hits between humans, finishes and
  results are shared (finding 047); a player who leaves becomes an AI rider.
- In the game's own menus: name and code entry, public list, rider picker (finding 065); restart,
  new level and rematch flows (finding 072). Create/Join waits on ONLINE, then the rider screen
  once and the room over it, with no bounce and no second title flight (finding 092); the shop's
  bike/jersey pages are still skipped online.
- Tests: `port/net/build_tests.py` (unit and loopback), the server's own tests (`npm test`),
  `port/tools/online_race.py` (two local games through a relay, optionally `nettool` bots).
