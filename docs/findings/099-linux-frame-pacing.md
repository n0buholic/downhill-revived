# 099: Linux frame pacing and Wayland startup

The Linux windowed port intermittently slows down in both menus and races. Tested on
an i5-11400H / RTX 3050 laptop, Plasma Wayland, with the local USA disc. No game data
is included here.

## Changes

- Linux desktop defaults to the existing clock-paced VBLANK path (`DHD_LATENCY=0`).
  The just-in-time path moves its next deadline to follow a late presenter; repeated
  scheduler/rendering delays can therefore lower the simulation rate. Clock pacing
  keeps the 59.94 Hz grid through short delays (kernel.c's existing two-period reset
  handles long stalls). This trades some input latency for stable simulation timing.
  An explicit `DHD_LATENCY=1`, in the environment or dhd.ini, still selects JIT.
  Windows, Android, the virtual-clock benchmarks and the game core are unchanged.
- The POSIX deadline's final millisecond uses a CPU pause instruction, as Windows
  does, instead of repeatedly entering the scheduler with `sched_yield()`.
- Linux shows/raises/minimizes its new window through `sdl_host_run`, on the same
  SDL thread that creates it and pumps events. Two benchmark launches hung before
  `Ready`; a stack sample showed the game thread inside Wayland's window-show
  roundtrip while SDL pumped events on another thread.

## Measurements and limits

Use the native renderer explicitly with `DHD_SETTINGS=none`: without a settings
file the renderer otherwise defaults to exact. The initial exact-renderer menu
test measured 26.649 fields/s; it is **not** a before/after comparison against
the native renderer. Native JIT menu runs varied from 54.854 to 59.634 fields/s.

An exploratory native menu run using clock pacing measured 59.940 fields/s,
p99 16.737 ms (fields 400–1000). Trying `-O3` for the VU and native graphics
translation units gave no clear throughput benefit, so that build change was
discarded. The final executable keeps the existing optimization flags.

A sequential race comparison used the same O2 executable with the wait/window fixes,
changing only `DHD_LATENCY`. Native renderer, scale 2, 896x672 Wayland window,
no sound device, no memory card, seed 81257, the `state_trace_run.py` race navigation,
`DHD_RACE_PAD=2:1200:cross`, bench fields 6000–6600:

| Pacing | Simulation fields/s | Simulation p99 ms | Displayed fps | Present p99 ms |
| --- | ---: | ---: | ---: | ---: |
| JIT | 59.785 | 20.105 | 59.77 | 19.13 |
| Clock | 59.942 | 16.783 | 59.94 | 18.80 |

These are one sequential pair, not a universal throughput claim. Display jitter is
still possible: stable simulation intervals do not guarantee identical compositor
presentation intervals. The render thread remains asynchronous.

The final O2 binary, using the player's existing 1920x1080/2x/native settings and
SDL audio, measured 59.940 simulation fields/s (p99 16.833 ms), 59.94 displayed
fps (present p99 18.95 ms), and zero audio underruns, dropped or skipped audio
over the 400–1000 menu window. The log confirms `pacing: wait before VBLANK`.

## Checks

- `port/host/test_posix_pacing.c`: Linux default, environment/INI overrides and
  precedence, 100 wall deadlines that never return early, and an expired deadline.
- `host64_selftest.py --linux --optimized --negative-controls`: memory mirrors,
  out-of-bounds traps, floating-point bits, fiber context, and negative controls pass.
- `state_diff.py`: all columns match for the first 4,000 fields of the deterministic
  race trace, with the existing three-word mask unchanged. The baseline completed
  5,000 fields; the candidate received SIGTERM after 4,002 rows, so only the common
  complete 4,000-field prefix was compared. This is not a 5,000-field trace pass.

## The same icon as Windows

`build_host64_linux.py` embeds `port/res/icon.png` (the source of Windows'
`port/res/dhd.ico`) in the SDL game; no external icon file is needed for its
window. The video thread loads it with SDL 3.4's PNG loader and sets the window
icon. The application ID is `app.skuza.downhillrevived`.

The Tauri launcher's first PNG points directly at `port/res/icon.png`, so its
Unix window uses the full 512x512 Windows source instead of the first 32x32 PNG.
Its Windows ICO and macOS ICNS remain in the shared bundle configuration.
The AppImage embeds the same artwork; no external shortcut or icon is generated.

Checks: the game executable contains the exact source PNG. A live Plasma Wayland
run sent `create_icon`, the matching `set_app_id`, and `set_icon` to the compositor
and exited cleanly. The launcher release rebuild and version/path checks pass.

X11 game presentation remains limited by the existing renderer's RGBA-only
surface requirement: this machine's X11 Vulkan surface offers BGRA instead.
That is separate from icon handling; this change does not claim X11 gameplay works.

## Research

- [Linux sched_yield(2)](https://man7.org/linux/man-pages/man2/sched_yield.2.html):
  yielding under ordinary scheduling is unspecified and can cause unnecessary
  context switches; it is not the CPU pause instruction used for a short spin.
- [SDL_ShowWindow](https://wiki.libsdl.org/SDL3/SDL_ShowWindow) and
  [SDL_RaiseWindow](https://wiki.libsdl.org/SDL3/SDL_RaiseWindow): video calls belong
  on SDL's main/video thread. This port already marshals window creation through
  `sdl_host_run`; showing the Linux window now follows the same ownership.
- [SDL's Wayland integration](https://wiki.libsdl.org/SDL3/README-wayland):
  window-icon protocol support and the matching desktop-entry application ID.

No third-party code was copied.
