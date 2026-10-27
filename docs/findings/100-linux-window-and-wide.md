# Linux window lifecycle and picture shape

2026-10-28. Linux startup ignored `DHD_WINDOW`; applying fullscreen later only resized an
undecorated window. `present.c` now creates an SDL fullscreen window for that setting and
uses `SDL_SetWindowFullscreen` when settings change. Window changes run through the SDL
thread. This requests compositor fullscreen, including removal of desktop panels, without
making the game permanently always-on-top.

The launcher now hides while its child runs and explicitly shows and unminimizes after the
child exits, before requesting focus. Previously it relied only on minimize/unminimize.

The native renderer independently forced movies to 4:3 and forced consecutive flat/loading
fields to 4:3. Both overrides are removed: the selected picture shape applies throughout.
Explicit 4:3 still works. Original 4:3 movie artwork stretches when wide is selected; this
does not manufacture additional scene content. Existing menu text/HUD shape compensation
and gameplay field of view remain as before.

Validation:
- Linux game and release launcher built successfully.
- Wayland protocol trace: `set_fullscreen(wl_output)` followed by compositor configuration
  at 1920x1080; the 650-field run exited normally. A 1920x1080 menu capture was inspected.
- A second, movie-enabled run completed 1200 fields and exited normally.
- Both binaries installed in the player's game folder, with backups and
  SHA-256 verification.
- CPU viewport regression `port/gs/test_native_aspect.c`: wide, explicit 4:3, stretch and
  raw-pixel aspect pass. Build with clang `-O2 -ffunction-sections -fdata-sections
  -Iport/render/include -Iport/include -Wno-format`, link `-Wl,--gc-sections -lm`.
- Launcher tests: five passed, one existing network test ignored. The install test needs
  permission to set executable bits and passes outside the restricted filesystem sandbox.
- Interactive launcher play/exit and every selection menu have not been visually tested:
  the session's prescribed GUI executable, `orca-ide`, is not installed.

References consulted (no external code copied):
- [SDL fullscreen API](https://wiki.libsdl.org/SDL3/SDL_SetWindowFullscreen): compositor
  request, asynchronous transitions and video-thread requirements.
- [Tauri WebviewWindow](https://docs.rs/tauri/latest/tauri/webview/struct.WebviewWindow.html):
  show, hide, unminimize and focus methods; local Tao Linux implementation checked too.
