//! Starting the game beside the launcher and waiting for it.
//!
//! The game is told it was started by the launcher (DHD_LAUNCHER=1): it then neither looks for
//! updates itself (DHD_AUTO_UPDATE=off; the launcher did) nor starts the launcher again, and the
//! online menu's Update Now ends it with UPDATE_EXIT_CODE so the launcher installs the update.

use std::path::{Path, PathBuf};
use std::process::{Child, Command};

#[cfg(windows)]
pub const GAME_EXE: &str = "dhd.exe";
#[cfg(not(windows))]
pub const GAME_EXE: &str = "dhd";

/// "UPDT": the game asks for an update (port/hle/update.c).
pub const UPDATE_EXIT_CODE: u32 = 0x5550_4454;

pub fn appimage() -> Option<PathBuf> {
    #[cfg(target_os = "linux")]
    return std::env::var_os("APPIMAGE").filter(|p| !p.is_empty()).map(PathBuf::from);
    #[cfg(not(target_os = "linux"))]
    None
}

fn data_dir(xdg: Option<PathBuf>, home: Option<PathBuf>) -> PathBuf {
    xdg.filter(|p| p.is_absolute())
        .unwrap_or_else(|| home.expect("HOME is required for AppImage saves").join(".local/share"))
        .join("downhill-revived")
}

/// The folder of the game: the launcher's own, or DHD_GAME_DIR (development).
pub fn dir() -> PathBuf {
    if let Some(d) = std::env::var_os("DHD_GAME_DIR") {
        return PathBuf::from(d);
    }
    if appimage().is_some() {
        return data_dir(std::env::var_os("XDG_DATA_HOME").map(PathBuf::from),
                        std::env::var_os("HOME").map(PathBuf::from));
    }
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

pub fn exe(dir: &Path) -> PathBuf {
    if appimage().is_some() {
        return std::env::current_exe().expect("launcher executable").with_file_name(GAME_EXE);
    }
    dir.join(GAME_EXE)
}

pub fn start(dir: &Path) -> std::io::Result<Child> {
    let mut command = Command::new(exe(dir));
    command.current_dir(dir)
        .env("DHD_LAUNCHER", "1")
        .env("DHD_AUTO_UPDATE", "off");
    if appimage().is_some() {
        command.env("DHD_SETTINGS", crate::ini::path(dir));
        let settings = crate::ini::read(&crate::ini::path(dir));
        if std::env::var_os("DHD_MEMCARD_DIR").is_none()
            && !settings.contains_key("dhd_memcard_dir") {
            command.env("DHD_MEMCARD_DIR", dir.join("saves"));
        }
        if std::env::var_os("DHD_TEXTURE_PACK").is_none()
            && settings.get("texture_pack").is_none_or(|p| p.is_empty())
            && !settings.contains_key("dhd_texture_pack") {
            command.env("DHD_TEXTURE_PACK", dir.join("textures"));
        }
        let log = std::fs::File::create(dir.join("dhd.log"))?;
        command.stdout(log.try_clone()?).stderr(log);
    }
    command.spawn()
}

/// The exit code as the game set it (a u32 on Windows, where UPDATE_EXIT_CODE is negative as i32).
pub fn exit_code(status: std::process::ExitStatus) -> Option<u32> {
    status.code().map(|c| c as u32)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn appimage_data_is_writable_and_stable() {
        assert_eq!(data_dir(Some("/data".into()), None), PathBuf::from("/data/downhill-revived"));
        assert_eq!(data_dir(None, Some("/users/player".into())),
                   PathBuf::from("/users/player/.local/share/downhill-revived"));
        assert_eq!(data_dir(Some("relative".into()), Some("/users/player".into())),
                   data_dir(None, Some("/users/player".into())));
    }
}
