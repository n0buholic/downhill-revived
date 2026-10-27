//! Downhill Revived launcher (finding 093): the window players see before the game. It writes the
//! game's own settings file (dhd.ini), keeps the game up to date, sets the rider name, and starts
//! dhd.exe. The front end is ../ui; everything that touches files, network or processes is here.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod disc;
mod game;
mod ini;
mod update;

use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Child;
use std::sync::Mutex;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

const VERSION: &str = env!("DHD_VERSION");
const DISCORD: &str = "https://discord.gg/XrSeTyzfr2";
/// The race server when dhd.ini's server is empty (the game's built-in one, port/hle/host.c).
const OFFICIAL_SERVER: &str = "wss://dhd.skuza.app";

struct App {
    dir: PathBuf,
    game: Mutex<Option<Child>>,
}

#[derive(Serialize)]
struct Snapshot {
    version: &'static str,
    /// The game's binary name on this platform (dhd.exe / dhd), for the messages that name it.
    game: &'static str,
    settings: BTreeMap<String, String>,
    game_found: bool,
    running: bool,
    /// Why dhd.ini's disc image can't be used, when it names one and the game would refuse it.
    disc_problem: Option<String>,
    /// A texture pack is there: texture_pack's folder, or textures/SCUS-97177/replacements.
    texture_pack: bool,
}

#[derive(Serialize)]
struct ServerStatus {
    /// Public races open now and the riders in them (private rooms aren't listed).
    races: usize,
    riders: u64,
    /// The oldest version the server lets race.
    min_version: String,
}

#[derive(Serialize)]
struct UpdateCheck {
    release: Option<update::Release>,
    newer: bool,
}

#[derive(Clone, Serialize)]
struct Progress {
    done: u64,
    total: u64,
}

#[derive(Clone, Serialize)]
struct Exited {
    code: Option<u32>,
    wants_update: bool,
}

#[tauri::command]
fn snapshot(app: State<App>) -> Snapshot {
    let settings = ini::read(&ini::path(&app.dir));
    let iso = settings.get("iso").filter(|s| !s.is_empty());
    let pack = settings
        .get("texture_pack")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| app.dir.join("textures").join("SCUS-97177").join("replacements"));
    Snapshot {
        version: VERSION,
        game: game::GAME_EXE,
        disc_problem: iso.and_then(|p| disc::problem(std::path::Path::new(p))),
        texture_pack: std::fs::read_dir(pack).is_ok_and(|mut d| d.next().is_some()),
        game_found: game::exe(&app.dir).is_file(),
        running: app.game.lock().unwrap().is_some(),
        settings,
    }
}

/// Keys are the game's (letters, digits, _); a value is one line.
#[tauri::command]
fn save_setting(app: State<App>, key: String, value: String) -> Result<(), String> {
    let key_ok = !key.is_empty() && key.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_');
    if !key_ok || value.contains(['\n', '\r']) {
        return Err(format!("not a setting: {key}"));
    }
    ini::save(&ini::path(&app.dir), &key, &value).map_err(|e| format!("can't save dhd.ini: {e}"))
}

#[tauri::command]
async fn check_update(app: State<'_, App>) -> Result<UpdateCheck, String> {
    let settings = ini::read(&ini::path(&app.dir));
    let source = settings
        .get("update_url")
        .filter(|s| !s.is_empty())
        .cloned()
        .or_else(|| std::env::var("DHD_UPDATE_URL").ok())
        .unwrap_or_else(|| update::OFFICIAL_SOURCE.into());
    let release = tauri::async_runtime::spawn_blocking(move || update::latest(&source))
        .await
        .map_err(|e| e.to_string())??;
    let newer = release.as_ref().is_some_and(|r| update::newer(&r.version, VERSION));
    Ok(UpdateCheck { release, newer })
}

/// Installs the release, then starts the new launcher and ends this one.
#[tauri::command]
async fn install_update(handle: AppHandle, release: update::Release) -> Result<(), String> {
    let dir = handle.state::<App>().dir.clone();
    let h = handle.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        update::install(&release, &dir, |done, total| {
            if done - last >= 256 * 1024 || done == total {
                last = done;
                let _ = h.emit("update-progress", Progress { done, total });
            }
        })
    })
    .await
    .map_err(|e| e.to_string())??;
    let exe = game::appimage().map(Ok).unwrap_or_else(std::env::current_exe).map_err(|e| e.to_string())?;
    std::process::Command::new(exe).spawn().map_err(|e| format!("updated, but can't restart: {e}"))?;
    handle.exit(0);
    Ok(())
}

#[tauri::command]
fn play(handle: AppHandle, app: State<App>) -> Result<(), String> {
    let mut slot = app.game.lock().unwrap();
    if slot.is_some() {
        return Ok(());
    }
    let child = game::start(&app.dir).map_err(|e| format!("can't start {}: {e}", game::GAME_EXE))?;
    *slot = Some(child);
    if let Some(w) = handle.get_webview_window("main") {
        let _ = w.hide();
    }
    let h = handle.clone();
    std::thread::spawn(move || loop {
        std::thread::sleep(Duration::from_millis(400));
        let app = h.state::<App>();
        let mut slot = app.game.lock().unwrap();
        let status = match slot.as_mut().map(|c| c.try_wait()) {
            Some(Ok(Some(s))) => Some(s),
            Some(Ok(None)) => continue,
            _ => None,
        };
        *slot = None;
        drop(slot);
        let code = status.and_then(game::exit_code);
        let _ = h.emit("game-exited", Exited { code, wants_update: code == Some(game::UPDATE_EXIT_CODE) });
        if let Some(w) = h.get_webview_window("main") {
            let _ = w.show();
            let _ = w.unminimize();
            let _ = w.set_focus();
        }
        return;
    });
    Ok(())
}

#[tauri::command]
fn stop_game(app: State<App>) {
    if let Some(c) = app.game.lock().unwrap().as_mut() {
        let _ = c.kill();
    }
}

/// The race server's public lobby list (online/server: GET /v1/lobbies), from dhd.ini's server.
#[tauri::command]
async fn server_status(app: State<'_, App>) -> Result<ServerStatus, String> {
    let server = ini::read(&ini::path(&app.dir)).get("server").filter(|s| !s.is_empty()).cloned();
    let base = server.as_deref().unwrap_or(OFFICIAL_SERVER).trim_end_matches('/');
    let url = format!("{}/v1/lobbies", base.replacen("wss://", "https://", 1).replacen("ws://", "http://", 1));
    let body = tauri::async_runtime::spawn_blocking(move || update::get_json(&url))
        .await
        .map_err(|e| e.to_string())??;
    let lobbies = body.get("lobbies").and_then(|l| l.as_array()).cloned().unwrap_or_default();
    Ok(ServerStatus {
        races: lobbies.len(),
        riders: lobbies.iter().filter_map(|l| l.get("players")?.as_u64()).sum(),
        min_version: body.get("minVersion").and_then(|v| v.as_str()).unwrap_or_default().to_string(),
    })
}

/// The game's folder in the file manager (the fix the "dhd.exe isn't here" message offers).
#[tauri::command]
fn open_folder(handle: AppHandle) -> Result<(), String> {
    let dir = handle.state::<App>().dir.clone();
    handle.opener().open_path(dir.to_string_lossy(), None::<&str>).map_err(|e| e.to_string())
}

/// The disc image, chosen in a file dialog, checked as the game checks it and saved as iso; None
/// when the player cancels.
#[tauri::command]
async fn pick_disc(handle: AppHandle) -> Result<Option<String>, String> {
    let picked = handle
        .dialog()
        .file()
        .set_title("Choose your Downhill Domination (USA) disc image")
        .add_filter("Disc images", &["iso"])
        .blocking_pick_file();
    let Some(path) = picked.and_then(|p| p.into_path().ok()) else { return Ok(None) };
    if let Some(problem) = disc::problem(&path) {
        return Err(problem);
    }
    let text = path.to_string_lossy().into_owned();
    ini::save(&ini::path(&handle.state::<App>().dir), "iso", &text).map_err(|e| format!("can't save dhd.ini: {e}"))?;
    Ok(Some(text))
}

/// Only the project's own pages open from the launcher.
#[tauri::command]
fn open_link(handle: AppHandle, url: String) -> Result<(), String> {
    let url = if url == "discord" { DISCORD.to_string() } else { url };
    let allowed = ["https://discord.gg/", "https://github.com/skuza-oss/", "https://downhill-revived.pages.dev/"];
    if !allowed.iter().any(|a| url.starts_with(a)) {
        return Err(format!("not opening {url}"));
    }
    handle.opener().open_url(url, None::<&str>).map_err(|e| e.to_string())
}

fn main() {
    // GTK's Wayland dispatch can end in "protocol error 71" with this no-decoration window
    // (tao, finding 098): prefer X11 (XWayland) unless the player chose a backend themselves.
    #[cfg(target_os = "linux")]
    if std::env::var_os("GDK_BACKEND").is_none() && std::env::var_os("WAYLAND_DISPLAY").is_some() {
        std::env::set_var("GDK_BACKEND", "x11");
    }
    // WebKitGTK's dmabuf renderer goes blank ("Failed to create GBM buffer") on some driver
    // stacks (WebKitGTK 2.44, finding 098): fall back to its plain renderer for the webview.
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
    let dir = game::dir();
    if game::appimage().is_some() {
        std::fs::create_dir_all(&dir).expect("create player data folder");
    }
    update::clean_up(&dir);
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(App { dir, game: Mutex::new(None) })
        .invoke_handler(tauri::generate_handler![
            snapshot,
            save_setting,
            check_update,
            install_update,
            play,
            stop_game,
            pick_disc,
            server_status,
            open_folder,
            open_link
        ])
        .run(tauri::generate_context!())
        .expect("the launcher window");
}
