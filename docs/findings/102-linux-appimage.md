# Linux AppImage packaging and update availability

`port/tools/package_appimage.py` bundles the existing launcher, native game, licence
and notices through Tauri/linuxdeploy. The Windows icon is embedded in the image;
no desktop shortcut or separate installed icon is generated. ISO files, saves and
player settings are excluded. Build instructions are in `docs/BUILDING.md`.

The bundled game is resolved beside the mounted launcher. Writable settings, logs,
saves and textures use `$XDG_DATA_HOME/downhill-revived` (otherwise
`$HOME/.local/share/downhill-revived`); explicit player overrides remain supported.
Compiler prefix maps remove checkout/toolchain home paths from shipped binaries.
Packaging replaces its output atomically, including when an older image is running.

AppImage updates use `latest-linux-appimage.json` or a matching GitHub asset with
its SHA-256 digest. The installer checks the digest and AppImage header, stages the
file beside the original image, then renames it atomically. Existing zip installs
retain their platform-specific update path.

A successful GitHub response with no matching package now returns an unavailable
status rather than an error. On verification, official v0.6.4 published only the
Windows zip and manifest. The launcher keeps Play available and says no update
package is published for this platform. Network errors, invalid manifests and
matching assets without a digest still fail explicitly.

Validation: launcher tests pass (8 tests, one network test ignored), including
missing-platform versus malformed-release cases, AppImage replacement preserving
the old file on rejection, and persistent data paths. The final AppDir scan found no personal home paths in 466 files.
The bundled game passes early/late rider skip at 60 and 240 FPS (41/71 and
57/254 fields respectively); its executable code section matches the input game.
The AppImage starts without extraction overrides and creates isolated XDG data.
Packaging and runtime checks are local Linux checks; older distributions and Windows runtime remain untested.
AppImages must be built on the oldest supported glibc baseline for distribution.

References (no external code copied):
- https://v2.tauri.app/distribute/appimage/
- https://docs.appimage.org/packaging-guide/overview.html
- https://api.github.com/repos/skuza-oss/downhill-revived/releases/latest
