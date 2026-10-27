//! The launcher's updater, the same rules as the game's (port/hle/update.c, finding 054): the update
//! source is dhd.ini's update_url or the GitHub releases API of skuza-oss/downhill-revived; its answer
//! is GitHub's release JSON (whose latest.json asset, or else its DownhillRevived-*-win.zip asset
//! and that asset's sha256 digest, names the zip) or a latest.json itself. The zip is downloaded
//! into dhd-update/, its SHA-256 checked, and the files of its one folder copied over the game's
//! (never dhd.ini, a log or a folder such as saves/). A file in use (this launcher) is renamed
//! NAME.old first; clean_up deletes those at the next start.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const OFFICIAL_SOURCE: &str = "https://api.github.com/repos/skuza-oss/downhill-revived/releases/latest";
pub const RELEASES_PAGE: &str = "https://github.com/skuza-oss/downhill-revived/releases/latest";
const WORK_DIR: &str = "dhd-update";
/// Which release zip this platform installs (DownhillRevived-*-win.zip / -linux.zip): the game's
/// binaries are not portable between them, so a release's other platform is no update at all.
#[cfg(target_os = "windows")]
pub const ASSET_PLATFORM: &str = "win";
#[cfg(not(target_os = "windows"))]
pub const ASSET_PLATFORM: &str = "linux";
/// Never replaced by an update: the player's own files.
const KEEP: [&str; 4] = ["dhd.ini", "dhd.log", "iso-check.txt", "launcher.log"];

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Release {
    pub version: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub page: String,
    /// The release's notes (GitHub's markdown body), when the source gives them.
    pub notes: String,
    /// When it was published (GitHub's published_at, e.g. 2026-10-26T20:28:33Z), when known.
    pub published: String,
}

fn agent() -> ureq::Agent {
    let mut builder = ureq::AgentBuilder::new();
    if let Ok(tls) = native_tls::TlsConnector::new() {
        builder = builder.tls_connector(std::sync::Arc::new(tls));
    }
    builder
        .timeout_connect(Duration::from_secs(6))
        .timeout_read(Duration::from_secs(20))
        .user_agent(concat!("DownhillRevived-launcher/", env!("DHD_VERSION")))
        .build()
}

pub fn get_json(url: &str) -> Result<serde_json::Value, String> {
    agent()
        .get(url)
        .set("Accept", "application/json")
        .call()
        .map_err(|e| format!("{url}: {e}"))?
        .into_json()
        .map_err(|e| format!("{url} didn't answer with JSON: {e}"))
}

/// url relative to the manifest at base, as update.c's resolve does.
fn resolve(base: &str, url: &str) -> String {
    if url.contains("://") || url.is_empty() {
        return url.to_string();
    }
    let base = base.split('?').next().unwrap_or(base);
    let after = base.find("://").map(|i| i + 3).unwrap_or(0);
    let root = base[after..].find('/').map(|i| i + after);
    match (url.starts_with('/'), root) {
        (true, Some(r)) => format!("{}{url}", &base[..r]),
        (false, Some(_)) => format!("{}{url}", &base[..=base.rfind('/').unwrap()]),
        _ => format!("{base}/{}", url.trim_start_matches('/')),
    }
}

fn text(v: &serde_json::Value, key: &str) -> String {
    v.get(key).and_then(|s| s.as_str()).unwrap_or_default().to_string()
}

fn read_manifest(m: &serde_json::Value, base: &str) -> Result<Release, String> {
    let (version, url, sha256) = (text(m, "version"), text(m, "url"), text(m, "sha256"));
    if version.is_empty() || url.is_empty() || sha256.is_empty() {
        return Err(format!("the update manifest {base} lacks version, url or sha256"));
    }
    Ok(Release {
        version,
        url: resolve(base, &url),
        sha256: sha256.to_ascii_lowercase(),
        size: m.get("size").and_then(|s| s.as_u64()).unwrap_or(0),
        page: text(m, "page"),
        ..Default::default()
    })
}

pub fn parse_version(v: &str) -> Option<[u32; 3]> {
    let mut out = [0u32; 3];
    let mut parts = v.trim().trim_start_matches(['v', 'V']).split('.');
    for slot in &mut out {
        *slot = parts.next()?.parse().ok()?;
    }
    parts.next().is_none().then_some(out)
}

pub fn newer(candidate: &str, current: &str) -> bool {
    matches!((parse_version(candidate), parse_version(current)), (Some(a), Some(b)) if a > b)
}

/// The newest release the source names.
pub fn latest(source: &str) -> Result<Option<Release>, String> {
    let appimage = crate::game::appimage().is_some();
    let body = get_json(source)?;
    release_from_body(source, &body, appimage)
}

fn release_from_body(source: &str, body: &serde_json::Value, appimage: bool) -> Result<Option<Release>, String> {
    let mut r = match body.get("assets").and_then(|a| a.as_array()) {
        None => read_manifest(&body, source)?,
        Some(assets) => {
            let mut manifest = None;
            let mut zip = None;
            for a in assets {
                let name = text(a, "name");
                if name == if appimage { "latest-linux-appimage.json" }
                           else if ASSET_PLATFORM == "linux" { "latest-linux.json" }
                           else { "latest.json" } {
                    manifest = Some(text(a, "browser_download_url"));
                } else if name.starts_with("DownhillRevived-") && name.ends_with(
                    &if appimage { "-x86_64.AppImage".into() } else { format!("-{ASSET_PLATFORM}.zip") }) {
                    zip = Some(a.clone());
                }
            }
            let mut r = if let Some(m) = manifest {
                read_manifest(&get_json(&m)?, &m)?
            } else if let Some(z) = zip {
                if !text(&z, "digest").starts_with("sha256:") {
                    return Err("the update asset has no SHA-256 digest".into());
                }
                Release {
                    version: text(&body, "tag_name").trim_start_matches(['v', 'V']).to_string(),
                    url: text(&z, "browser_download_url"),
                    sha256: text(&z, "digest")[7..].to_ascii_lowercase(),
                    size: z.get("size").and_then(|s| s.as_u64()).unwrap_or(0),
                    ..Default::default()
                }
            } else {
                return Ok(None);
            };
            r.page = text(&body, "html_url");
            r.notes = text(&body, "body");
            r.published = text(&body, "published_at");
            r
        }
    };
    let hex = r.sha256.len() == 64 && r.sha256.bytes().all(|c| c.is_ascii_hexdigit());
    if parse_version(&r.version).is_none() || !hex || !(r.url.starts_with("https://") || r.url.starts_with("http://")) {
        return Err(format!("the update source {source} gave an unusable answer (version \"{}\")", r.version));
    }
    if r.page.is_empty() {
        r.page = if source == OFFICIAL_SOURCE { RELEASES_PAGE.into() } else { source.into() };
    }
    if appimage && !r.url.split('?').next().unwrap_or("").ends_with("-x86_64.AppImage") {
        return Err("the update source does not offer an x86_64 AppImage".into());
    }
    Ok(Some(r))
}

/// Downloads r's zip to dhd-update/ beside the game, calling progress(done, total) as it goes,
/// checks it and puts its files in place; an error names what went wrong or what wasn't replaced.
pub fn install(r: &Release, game_dir: &Path, mut progress: impl FnMut(u64, u64)) -> Result<(), String> {
    let work = game_dir.join(WORK_DIR);
    let _ = fs::remove_dir_all(&work);
    fs::create_dir_all(&work).map_err(|e| format!("can't make {}: {e} (is the game's folder read-only?)", work.display()))?;
    let zip_path = work.join(format!("DownhillRevived-{}.zip", r.version));

    let resp = agent().get(&r.url).call().map_err(|e| format!("download failed: {e}"))?;
    let total = resp.header("Content-Length").and_then(|s| s.parse().ok()).unwrap_or(r.size);
    let mut reader = resp.into_reader();
    let mut file = fs::File::create(&zip_path).map_err(|e| format!("{}: {e}", zip_path.display()))?;
    let mut hash = Sha256::new();
    let mut buf = vec![0u8; 256 * 1024];
    let mut done = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("download failed: {e}"))?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
        file.write_all(&buf[..n]).map_err(|e| format!("can't write the download: {e}"))?;
        done += n as u64;
        progress(done, total);
    }
    drop(file);
    let got: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
    if got != r.sha256 {
        return Err(format!("the download is damaged (SHA-256 {got}, expected {})", r.sha256));
    }

    if let Some(target) = crate::game::appimage() {
        replace_appimage(&zip_path, &target)?;
        let _ = fs::remove_dir_all(&work);
        return Ok(());
    }

    let unpacked = work.join("unpacked");
    let f = fs::File::open(&zip_path).map_err(|e| e.to_string())?;
    zip::ZipArchive::new(f)
        .and_then(|mut z| z.extract(&unpacked))
        .map_err(|e| format!("can't unpack the update: {e}"))?;
    let folder = fs::read_dir(&unpacked)
        .map_err(|e| e.to_string())?
        .flatten()
        .map(|e| e.path())
        .find(|p| p.join(crate::game::GAME_EXE).is_file())
        .ok_or("the update holds no game")?;

    let mut failed = Vec::new();
    for entry in fs::read_dir(&folder).map_err(|e| e.to_string())?.flatten() {
        let src = entry.path();
        let name = entry.file_name();
        if !src.is_file() || KEEP.iter().any(|k| name.eq_ignore_ascii_case(k)) {
            continue;
        }
        let dst = game_dir.join(&name);
        if fs::copy(&src, &dst).is_ok() {
            continue;
        }
        // in use (this launcher): it can be renamed but not overwritten
        let old = old_name(&dst);
        let _ = fs::remove_file(&old);
        if fs::rename(&dst, &old).and_then(|_| fs::copy(&src, &dst)).is_err() {
            failed.push(name.to_string_lossy().into_owned());
        }
    }
    let _ = fs::remove_dir_all(&work);
    if failed.is_empty() {
        Ok(())
    } else {
        Err(format!("couldn't replace {}", failed.join(", ")))
    }
}

/// Stage on the destination filesystem, then atomically replace the running image.
fn replace_appimage(source: &Path, target: &Path) -> Result<(), String> {
    let mut header = [0; 11];
    fs::File::open(source).and_then(|mut f| f.read_exact(&mut header)).map_err(|e| e.to_string())?;
    if &header[..4] != b"\x7fELF" || &header[8..11] != b"AI\x02" {
        return Err("the download is not a type-2 AppImage".into());
    }
    let staged = target.with_extension(format!("AppImage.update-{}", std::process::id()));
    let mut output = fs::OpenOptions::new().write(true).create_new(true).open(&staged)
        .map_err(|e| format!("can't stage {}: {e}", target.display()))?;
    let result = (|| -> std::io::Result<()> {
        std::io::copy(&mut fs::File::open(source)?, &mut output)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            output.set_permissions(fs::Permissions::from_mode(0o755))?;
        }
        output.sync_all()?;
        fs::rename(&staged, target)
    })();
    if result.is_err() { let _ = fs::remove_file(&staged); }
    result.map_err(|e| format!("can't replace {}: {e}", target.display()))
}

fn old_name(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(".old");
    PathBuf::from(s)
}

/// What the last update left behind: *.old files and dhd-update/.
pub fn clean_up(game_dir: &Path) {
    let _ = fs::remove_dir_all(game_dir.join(WORK_DIR));
    if let Ok(dir) = fs::read_dir(game_dir) {
        for e in dir.flatten() {
            if e.file_name().to_string_lossy().ends_with(".old") && e.path().is_file() {
                let _ = fs::remove_file(e.path());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replaces_only_a_valid_appimage() {
        let dir = std::env::temp_dir().join(format!("dhd-appimage-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let image = dir.join("game.AppImage");
        let source = dir.join("download");
        fs::write(&image, b"old").unwrap();
        fs::write(&source, b"not an AppImage").unwrap();
        assert!(replace_appimage(&source, &image).is_err());
        assert_eq!(fs::read(&image).unwrap(), b"old");
        let data = b"\x7fELF\x02\x01\x01\x00AI\x02new payload";
        fs::write(&source, data).unwrap();
        replace_appimage(&source, &image).unwrap();
        assert_eq!(fs::read(&image).unwrap(), data);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(&image).unwrap().permissions().mode() & 0o777, 0o755);
        }
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn missing_platform_is_not_a_failed_check() {
        let body = serde_json::json!({"assets": [
            {"name": "latest.json", "browser_download_url": "https://example.invalid/latest.json"},
            {"name": "DownhillRevived-0.6.4-win.zip"}
        ]});
        assert!(release_from_body(OFFICIAL_SOURCE, &body, true).unwrap().is_none());
        let bad = serde_json::json!({"assets": [{"name": "DownhillRevived-0.6.4-x86_64.AppImage"}]});
        assert!(release_from_body(OFFICIAL_SOURCE, &bad, true).is_err());
        assert!(release_from_body(OFFICIAL_SOURCE, &serde_json::json!({}), true).is_err());
    }

    #[test]
    fn versions() {
        assert!(newer("0.5.1", "0.5.0"));
        assert!(newer("v1.0.0", "0.9.9"));
        assert!(!newer("0.5.0", "0.5.0"));
        assert!(!newer("junk", "0.5.0"));
    }

    #[test]
    fn relative_urls() {
        assert_eq!(resolve("https://a.b/x/latest.json", "z.zip"), "https://a.b/x/z.zip");
        assert_eq!(resolve("https://a.b/x/latest.json?q", "/z.zip"), "https://a.b/z.zip");
        assert_eq!(resolve("https://a.b/x/latest.json", "https://c/z.zip"), "https://c/z.zip");
    }

    /// The official source answers (network: cargo test -- --ignored).
    #[test]
    #[ignore]
    fn official_source() {
        let Some(r) = latest(OFFICIAL_SOURCE).unwrap() else { return };
        println!("{} {} {} bytes, {} chars of notes", r.version, r.url, r.size, r.notes.len());
        assert!(parse_version(&r.version).is_some());
    }

    /// A release served from a local HTTP server: latest.json, then the zip; installed over a
    /// game folder whose dhd.ini must survive.
    #[test]
    fn installs_a_release() {
        use std::io::{BufRead, BufReader, Cursor};
        use std::net::TcpListener;

        let mut zip_bytes = Vec::new();
        {
            let mut z = zip::ZipWriter::new(Cursor::new(&mut zip_bytes));
            let opt = zip::write::SimpleFileOptions::default();
            let exe = crate::game::GAME_EXE;
            for (name, body) in [(exe, "new game"), ("README.txt", "new readme"), ("dhd.ini", "name = WRONG\n")] {
                z.start_file(format!("DownhillRevived-9.9.9/{name}"), opt).unwrap();
                z.write_all(body.as_bytes()).unwrap();
            }
            z.finish().unwrap();
        }
        let sha: String = Sha256::digest(&zip_bytes).iter().map(|b| format!("{b:02x}")).collect();
        let manifest = format!(r#"{{"version":"9.9.9","url":"DownhillRevived-9.9.9-win.zip","sha256":"{sha}","size":{}}}"#, zip_bytes.len());

        let server = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", server.local_addr().unwrap());
        std::thread::spawn(move || {
            for stream in server.incoming().take(2).flatten() {
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                loop {
                    let mut h = String::new();
                    if reader.read_line(&mut h).unwrap() <= 2 {
                        break;
                    }
                }
                let body: &[u8] = if line.contains("latest.json") { manifest.as_bytes() } else { &zip_bytes };
                let mut s = stream;
                write!(s, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).unwrap();
                s.write_all(body).unwrap();
            }
        });

        let game = std::env::temp_dir().join(format!("dhd-launcher-update-{}", std::process::id()));
        let _ = fs::remove_dir_all(&game);
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join(crate::game::GAME_EXE), "old game").unwrap();
        fs::write(game.join("dhd.ini"), "name = SKUZA\n").unwrap();

        let r = latest(&format!("{base}/latest.json")).unwrap().unwrap();
        assert_eq!(r.version, "9.9.9");
        assert!(newer(&r.version, "0.5.0"));
        let mut last = 0;
        install(&r, &game, |done, _| last = done).unwrap();
        assert_eq!(last, r.size);
        assert_eq!(fs::read_to_string(game.join(crate::game::GAME_EXE)).unwrap(), "new game");
        assert_eq!(fs::read_to_string(game.join("README.txt")).unwrap(), "new readme");
        assert_eq!(fs::read_to_string(game.join("dhd.ini")).unwrap(), "name = SKUZA\n");
        assert!(!game.join(WORK_DIR).exists());
        fs::remove_dir_all(&game).unwrap();
    }
}
