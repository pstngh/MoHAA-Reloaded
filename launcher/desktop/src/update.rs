//! Updates: whether the project's release has a newer build of the game than the one the launcher found, and that
//! build's files put in place of the game's. CI replaces the release's downloads with each change to the `vulkan`
//! branch (.github/workflows/branches-build.yml) and writes their commit into its notes, so a build is told by its
//! commit: the game's own is in its version ("0.83.0-unstable+61.c2f07c2", cmake/version.cmake), and GitHub says
//! whether the release's comes after it.

use crate::platform;
use serde::Deserialize;
use std::fmt::Display;
use std::fs::{self, File};
use std::io::{self, BufReader, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// The repository whose latest release has the downloads
pub const REPO: &str = "pstngh/MoHAA-Reloaded";

/// This system's download on the release (its name's start and end): there are x86-64 builds only
const ASSET: Option<(&str, &str)> = if cfg!(not(target_arch = "x86_64")) {
    None
} else if cfg!(windows) {
    Some(("openmohaa-vulkan-", "-windows-x64.zip"))
} else if cfg!(target_os = "linux") {
    Some(("openmohaa-vulkan-", "-linux-amd64.tar.gz"))
} else {
    None
};

/// Where an update downloads and unpacks, in the game's folder (the same disk, so the files move into place by being
/// renamed); the files it replaced wait there too. Removed when the update is done, and what's left at the next check.
pub const WORK_DIR: &str = ".mohlauncher-update";

/// The error of an update stopped by Cancel
pub const CANCELLED: &str = "cancelled";

/// A newer build than the game's
#[derive(Clone, Debug)]
pub struct Update {
    /// The release's commit, in full
    pub commit: String,
    /// The download: its name, address, size and SHA-256 (lowercase hex)
    pub asset: String,
    pub url: String,
    pub size: u64,
    pub sha256: Option<String>,
}

/// How far an update has got
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    /// with the bytes downloaded so far
    Download,
    Unpack,
    /// the files are moving into place: it can't be stopped any more
    Install,
}

/// What the window and an update's thread tell each other
#[derive(Debug, Default)]
pub struct Flags {
    cancel: AtomicBool,
    installing: AtomicBool,
}

impl Flags {
    /// Stops the update (Cancel, or the window closed), unless its files are moving already: then false, and the
    /// window waits for them. The update looks at it before it moves the first file (begin_install).
    pub fn stop(&self) -> bool {
        self.cancel.store(true, Ordering::SeqCst);
        !self.installing.load(Ordering::SeqCst)
    }

    /// Cancel was chosen, or the window closed
    pub fn stopped(&self) -> bool {
        self.cancel.load(Ordering::SeqCst)
    }

    /// The files may move now, unless the update was stopped. With stop's order of the two (each one's own flag
    /// first, then the other's), one of them always sees the other's.
    fn begin_install(&self) -> bool {
        self.installing.store(true, Ordering::SeqCst);
        if self.stopped() {
            self.installing.store(false, Ordering::SeqCst);
            return false;
        }
        true
    }
}

/// The commit in a build's version, as the game's program holds it: "0.83.0-unstable+61.c2f07c2" is major, minor and
/// patch, the stage, then the CI run and the commit abbreviated (q_version.h's PRODUCT_VERSION_FULL). A build made
/// without git's information has none.
pub fn build_commit(binary: &[u8]) -> Option<String> {
    let hex = |c: &u8| c.is_ascii_digit() || (b'a'..=b'f').contains(c);
    for plus in (0..binary.len()).filter(|&i| binary[i] == b'+') {
        if !ends_with_version(&binary[..plus]) {
            continue;
        }
        let rest = &binary[plus + 1..];
        let run = rest.iter().take_while(|c| c.is_ascii_digit()).count();
        if run == 0 || rest.get(run) != Some(&b'.') {
            continue;
        }
        let rest = &rest[run + 1..];
        let digits = rest.iter().take_while(|c| hex(c)).count();
        if (7..=40).contains(&digits) && rest.get(digits).is_none_or(|c| !c.is_ascii_alphanumeric()) {
            return Some(String::from_utf8_lossy(&rest[..digits]).into_owned());
        }
    }
    None
}

/// "0.83.0" or "0.83.0-unstable" at the end
fn ends_with_version(text: &[u8]) -> bool {
    let mut end = text.len();
    let stage = text.iter().rev().take_while(|c| c.is_ascii_alphabetic()).count();
    if stage > 0 {
        if end <= stage || text[end - stage - 1] != b'-' {
            return false;
        }
        end -= stage + 1;
    }
    for part in 0..3 {
        let digits = text[..end].iter().rev().take_while(|c| c.is_ascii_digit()).count();
        if digits == 0 {
            return false;
        }
        end -= digits;
        if part < 2 {
            if end == 0 || text[end - 1] != b'.' {
                return false;
            }
            end -= 1;
        }
    }
    true
}

/// The commit the release's notes say the Vulkan downloads were built from: CI writes it, linked in full, into the
/// sentence "Built by this repository's CI from commit [`c2f07c2b`](https://github.com/.../commit/c2f07c2b...) of the
/// `vulkan` branch" with each upload
pub fn notes_commit(notes: &str) -> Option<String> {
    const SENTENCE: &str = "Built by this repository's CI from commit ";
    let line = notes.lines().find(|l| l.contains(SENTENCE) && l.contains("of the `vulkan` branch"))?;
    let line = &line[line.find(SENTENCE)?..];
    let at = line.find("/commit/")? + "/commit/".len();
    let hex: String = line[at..].chars().take_while(char::is_ascii_hexdigit).collect();
    (hex.len() == 40).then(|| hex.to_ascii_lowercase())
}

/// A download's SHA-256 in the notes' table ("| `name` | `<64 hex digits>` |"), for when GitHub doesn't give it
pub fn notes_sha256(notes: &str, name: &str) -> Option<String> {
    let cell = format!("| `{name}` | `");
    notes.lines().find_map(|line| {
        let rest = &line[line.find(&cell)? + cell.len()..];
        let hex: String = rest.chars().take_while(char::is_ascii_hexdigit).collect();
        (hex.len() == 64).then(|| hex.to_ascii_lowercase())
    })
}

/// A path in the download below its top folder `top` ("openmohaa-vulkan-1.0.0-linux-amd64", the download's name
/// without .tar.gz or .zip): None for the top folder itself, and for a path outside it, with "..", or from the root
fn inner_path(path: &Path, top: &str) -> Option<PathBuf> {
    let mut parts = path.components().filter(|c| *c != Component::CurDir);
    if parts.next()? != Component::Normal(top.as_ref()) {
        return None;
    }
    let mut inner = PathBuf::new();
    for part in parts {
        match part {
            Component::Normal(name) => inner.push(name),
            _ => return None,
        }
    }
    (!inner.as_os_str().is_empty()).then_some(inner)
}

#[derive(Deserialize)]
struct Release {
    body: Option<String>,
    #[serde(default)]
    assets: Vec<Asset>,
}

#[derive(Deserialize)]
struct Asset {
    name: String,
    size: u64,
    digest: Option<String>,
    browser_download_url: String,
}

#[derive(Deserialize)]
struct Comparison {
    status: String,
}

fn agent(total: Option<Duration>, per_read: Option<Duration>) -> ureq::Agent {
    ureq::Agent::config_builder()
        .timeout_global(total)
        .timeout_connect(Some(Duration::from_secs(15)))
        .timeout_per_read(per_read)
        .user_agent(concat!("mohlauncher/", env!("CARGO_PKG_VERSION")))
        .build()
        .into()
}

fn get_json<T: serde::de::DeserializeOwned>(agent: &ureq::Agent, url: &str) -> Result<T, String> {
    let request =
        agent.get(url).header("Accept", "application/vnd.github+json").header("X-GitHub-Api-Version", "2022-11-28");
    // the tests on CI: GitHub limits a shared runner's calls without a token
    #[cfg(test)]
    let request = match std::env::var("MOHLAUNCHER_TEST_GITHUB_TOKEN") {
        Ok(token) if !token.is_empty() => request.header("Authorization", format!("Bearer {token}")),
        _ => request,
    };
    let text = request
        .call()
        .and_then(|mut response| response.body_mut().read_to_string())
        .map_err(|e| format!("{url}: {e}"))?;
    serde_json::from_str(&text).map_err(|e| format!("{url}: {e}"))
}

/// A newer build than the game `exe` is, if the release has one. None too for a game whose version has no commit
/// (built without git), one GitHub doesn't know (built locally) and one that isn't older than the release's (a branch
/// with changes of its own); an error when the question couldn't be asked (no network, no answer).
pub fn check(exe: &Path) -> Result<Option<Update>, String> {
    let Some((prefix, suffix)) = ASSET else { return Ok(None) };
    let binary = fs::read(exe).map_err(|e| format!("{}: {e}", exe.display()))?;
    let Some(installed) = build_commit(&binary) else { return Ok(None) };
    drop(binary);

    let agent = agent(Some(Duration::from_secs(20)), None);
    let release: Release = get_json(&agent, &format!("https://api.github.com/repos/{REPO}/releases/latest"))?;
    let notes = release.body.unwrap_or_default();
    let Some(asset) = release.assets.into_iter().find(|a| a.name.starts_with(prefix) && a.name.ends_with(suffix))
    else {
        return Ok(None);
    };
    let Some(commit) = notes_commit(&notes) else { return Ok(None) };
    if commit.starts_with(&installed) {
        return Ok(None);
    }
    // Only "ahead" is newer: the release has all of the game's commits and more. "behind" and "diverged" are builds
    // of a branch with changes of its own. The second page of one commit each leaves the commits and the changed files
    // out: the answer is a few kB.
    let url = format!("https://api.github.com/repos/{REPO}/compare/{installed}...{commit}?per_page=1&page=2");
    let comparison: Comparison = get_json(&agent, &url)?;
    if comparison.status != "ahead" {
        return Ok(None);
    }
    let sha256 = asset
        .digest
        .as_deref()
        .and_then(|d| d.strip_prefix("sha256:"))
        .map(str::to_ascii_lowercase)
        .or_else(|| notes_sha256(&notes, &asset.name));
    Ok(Some(Update {
        commit,
        url: asset.browser_download_url,
        size: asset.size,
        sha256,
        asset: asset.name,
    }))
}

/// What an earlier update left in the game's folder: a download it didn't finish, or files it replaced that a program
/// still had open (on Windows, the launcher that started the new one)
pub fn clean_up(folder: &Path) {
    let work = folder.join(WORK_DIR);
    if work.exists() {
        remove_what_can_be(&work);
    }
}

/// A folder and everything in it, except what can't be removed: on Windows a file a running program has open stays
/// (and the folders it's in), the rest goes anyway (remove_dir_all stops at the first such file)
fn remove_what_can_be(folder: &Path) {
    if let Ok(entries) = fs::read_dir(folder) {
        for entry in entries.flatten() {
            if entry.file_type().is_ok_and(|t| t.is_dir()) {
                remove_what_can_be(&entry.path());
            } else {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
    let _ = fs::remove_dir(folder);
}

/// The update: `update`'s download into the folder of the game `exe`, unpacked and checked there, then its files in
/// place of the game's (install). `report` hears how far it has got; `flags` can stop it until the files move. The
/// files replaced or added, by their paths in the folder; on an error, the reason, and the game's files are as they
/// were (or the error says where the old ones are).
pub fn run(
    update: &Update,
    exe: &Path,
    flags: &Flags,
    report: &mut dyn FnMut(Stage, u64),
) -> Result<Vec<PathBuf>, String> {
    let folder = exe.parent().ok_or_else(|| format!("{} has no folder", exe.display()))?;
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_millis());
    let work = folder.join(WORK_DIR).join(stamp.to_string());
    fs::create_dir_all(&work).map_err(|e| format!("Can't write to {}: {e}", folder.display()))?;

    let prepared = (|| -> Result<(PathBuf, Vec<PathBuf>), String> {
        let archive = work.join(&update.asset);
        download(update, &archive, flags, &mut |done| report(Stage::Download, done))?;
        report(Stage::Unpack, 0);
        let new = work.join("new");
        let files = unpack(&archive, &new)?;
        if !new.join(platform::GAME_EXE).is_file() {
            return Err(format!("{} doesn't hold {}.", update.asset, platform::GAME_EXE));
        }
        if flags.stopped() {
            return Err(CANCELLED.to_string());
        }
        if platform::is_running(exe) {
            return Err(format!("{} is running: close the game, then update.", platform::GAME_EXE));
        }
        Ok((new, files))
    })();
    let result = match prepared {
        Err(e) if e == CANCELLED => Err(e),
        Err(e) => Err(format!("{e}\n\nThe game's files are as they were.")),
        Ok(_) if !flags.begin_install() => Err(CANCELLED.to_string()),
        Ok((new, files)) => {
            report(Stage::Install, 0);
            install(&new, &files, folder, &work.join("old"))
        }
    };
    // The download and the replaced files go; a file a program still has open stays until the next check. If old
    // files couldn't be put back, install moved them out of here first.
    remove_what_can_be(&folder.join(WORK_DIR));
    result
}

fn download(update: &Update, to: &Path, flags: &Flags, report: &mut dyn FnMut(u64)) -> Result<(), String> {
    let failed = |e: &dyn Display| format!("Couldn't download {}: {e}.", update.asset);
    let unwritable = |e: io::Error| format!("Can't write {}: {e}.", to.display());
    // no time limit for the whole download, a slow line takes its time; a minute without data ends it
    let agent = agent(None, Some(Duration::from_secs(60)));
    let mut response = agent.get(&update.url).call().map_err(|e| failed(&e))?;
    let mut body = response.body_mut().as_reader();
    let mut file = File::create(to).map_err(unwritable)?;
    let mut sha256 = ring::digest::Context::new(&ring::digest::SHA256);
    let mut buffer = vec![0; 1 << 16];
    let mut done = 0u64;
    loop {
        if flags.stopped() {
            return Err(CANCELLED.to_string());
        }
        let n = match body.read(&mut buffer) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(failed(&e)),
        };
        file.write_all(&buffer[..n]).map_err(unwritable)?;
        sha256.update(&buffer[..n]);
        done += n as u64;
        if done > update.size {
            return Err(failed(&"it's bigger than the release says"));
        }
        report(done);
    }
    if done != update.size {
        return Err(failed(&format!("it ended after {done} of {} bytes", update.size)));
    }
    if let Some(want) = &update.sha256 {
        let got: String = sha256.finish().as_ref().iter().map(|b| format!("{b:02x}")).collect();
        if got != *want {
            return Err(format!("{} isn't the file the release lists: its SHA-256 is {got}, not {want}.", update.asset));
        }
    }
    Ok(())
}

/// The download's files under `to`, without its top folder: their paths below it, programs executable on Linux. A
/// path that leaves the top folder makes it an error; links and other special entries are left out (the downloads
/// have none).
fn unpack(archive: &Path, to: &Path) -> Result<Vec<PathBuf>, String> {
    let name = archive.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let failed = |e: &dyn Display| format!("Couldn't unpack {name}: {e}.");
    let file = BufReader::new(File::open(archive).map_err(|e| failed(&e))?);
    let mut files = Vec::new();
    if let Some(top) = name.strip_suffix(".zip") {
        let mut zip = zip::ZipArchive::new(file).map_err(|e| failed(&e))?;
        for i in 0..zip.len() {
            let mut entry = zip.by_index(i).map_err(|e| failed(&e))?;
            if entry.is_dir() || !entry.is_file() {
                continue; // a file's folders are made for it
            }
            let path = entry.enclosed_name().unwrap_or_else(|| PathBuf::from(".."));
            let inner = inner_path(&path, top).ok_or_else(|| failed(&format!("{} is outside {top}", path.display())))?;
            let executable = entry.unix_mode().is_some_and(|mode| mode & 0o111 != 0);
            write_file(to, &inner, &mut entry, executable).map_err(|e| failed(&e))?;
            files.push(inner);
        }
    } else if let Some(top) = name.strip_suffix(".tar.gz") {
        let mut tar = tar::Archive::new(flate2::read::GzDecoder::new(file));
        for entry in tar.entries().map_err(|e| failed(&e))? {
            let mut entry = entry.map_err(|e| failed(&e))?;
            if !entry.header().entry_type().is_file() {
                continue;
            }
            let path = entry.path().map_err(|e| failed(&e))?.into_owned();
            let inner = inner_path(&path, top).ok_or_else(|| failed(&format!("{} is outside {top}", path.display())))?;
            let executable = entry.header().mode().is_ok_and(|mode| mode & 0o111 != 0);
            write_file(to, &inner, &mut entry, executable).map_err(|e| failed(&e))?;
            files.push(inner);
        }
    } else {
        return Err(failed(&"it isn't a .zip or a .tar.gz"));
    }
    Ok(files)
}

fn write_file(root: &Path, inner: &Path, data: &mut dyn Read, executable: bool) -> io::Result<()> {
    let path = root.join(inner);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    io::copy(data, &mut File::create(&path)?)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&path, fs::Permissions::from_mode(if executable { 0o755 } else { 0o644 }))?;
    }
    #[cfg(not(unix))]
    let _ = executable;
    Ok(())
}

/// Two files with the same contents
fn same_contents(a: &Path, b: &Path) -> bool {
    let (Ok(meta_a), Ok(meta_b)) = (fs::metadata(a), fs::metadata(b)) else { return false };
    if !meta_b.is_file() || meta_a.len() != meta_b.len() {
        return false;
    }
    let (Ok(file_a), Ok(file_b)) = (File::open(a), File::open(b)) else { return false };
    let (mut file_a, mut file_b) = (BufReader::new(file_a), BufReader::new(file_b));
    let (mut buf_a, mut buf_b) = (vec![0u8; 1 << 16], vec![0u8; 1 << 16]);
    loop {
        let n = match file_a.read(&mut buf_a) {
            Ok(0) => return true,
            Ok(n) => n,
            Err(_) => return false,
        };
        if file_b.read_exact(&mut buf_b[..n]).is_err() || buf_a[..n] != buf_b[..n] {
            return false;
        }
    }
}

/// `inner` with each folder in the case the game's folder has it: retail copies have MAIN as well as main
fn game_case(folder: &Path, inner: &Path) -> PathBuf {
    let parts: Vec<_> = inner.components().map(|c| c.as_os_str().to_os_string()).collect();
    let mut at = folder.to_path_buf();
    let mut out = PathBuf::new();
    for (i, part) in parts.iter().enumerate() {
        let mut name = part.clone();
        if i + 1 < parts.len() && !at.join(&name).is_dir() {
            let wanted = name.to_string_lossy().into_owned();
            let same =
                |e: &fs::DirEntry| e.file_name().to_string_lossy().eq_ignore_ascii_case(&wanted) && e.path().is_dir();
            let found = fs::read_dir(&at).ok().and_then(|entries| entries.flatten().find(same));
            if let Some(found) = found {
                name = found.file_name();
            }
        }
        at.push(&name);
        out.push(&name);
    }
    out
}

/// The unpacked files (`files`, below `new`) in place of the game's in `folder`. A file that is the same already
/// stays; one that differs moves to `old` first (Windows lets a running program's file, or a loaded library's, be
/// moved but not overwritten). If a file can't be put in place, the ones put so far go back. The files replaced or
/// added, by their paths in `folder`.
fn install(new: &Path, files: &[PathBuf], folder: &Path, old: &Path) -> Result<Vec<PathBuf>, String> {
    // each file's path in the folder, whether the game had one (now in old), and whether the new one is in place
    let mut moved: Vec<(PathBuf, bool, bool)> = Vec::new();
    let mut failure = None;
    for file in files {
        let source = new.join(file);
        let inner = game_case(folder, file);
        let target = folder.join(&inner);
        if same_contents(&source, &target) {
            continue;
        }
        let had = match target.symlink_metadata() {
            // a folder of the game's is never moved: the download has files only
            Ok(meta) if meta.is_dir() => {
                failure = Some(format!("{} is a folder in the game's folder, not a file.", target.display()));
                break;
            }
            Ok(_) => true,
            Err(_) => false,
        };
        let step = (|| -> io::Result<()> {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            if had {
                let kept = old.join(&inner);
                if let Some(parent) = kept.parent() {
                    fs::create_dir_all(parent)?;
                }
                fs::rename(&target, &kept)?;
            }
            moved.push((inner.clone(), had, false));
            fs::rename(&source, &target)?;
            if let Some(last) = moved.last_mut() {
                last.2 = true;
            }
            Ok(())
        })();
        if let Err(e) = step {
            failure = Some(format!("Couldn't put {} in place: {e}.", target.display()));
            break;
        }
    }
    let Some(failure) = failure else {
        return Ok(moved.into_iter().map(|(inner, ..)| inner).collect());
    };

    let mut stuck = Vec::new();
    for (inner, had, placed) in moved.iter().rev() {
        let target = folder.join(inner);
        if *placed {
            let _ = fs::remove_file(&target);
        }
        if *had && fs::rename(old.join(inner), &target).is_err() {
            stuck.push(inner.display().to_string());
        }
    }
    if stuck.is_empty() {
        return Err(format!("{failure}\n\nThe game's files are as they were."));
    }
    // out of the update's folder, which is removed next
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let keep = folder.join(format!("mohlauncher-old-files-{stamp}"));
    let kept_in = if fs::rename(old, &keep).is_ok() { keep } else { old.to_path_buf() };
    Err(format!(
        "{failure}\n\n{} couldn't be put back: the game's old files are in {}.",
        stuck.join(", "),
        kept_in.display()
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mohl-update-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    #[test]
    fn reads_the_build_commit() {
        let binary = b"\x7fELF\0x+1.abcdef0\0OpenMoHAA 0.83.0-unstable+61.c2f07c2 linux-x86_64 Nov 02 2026 UTC\0";
        assert_eq!(build_commit(binary).as_deref(), Some("c2f07c2"));
        assert_eq!(build_commit(b"\x000.83.0-unstable+0.783b7414\0").as_deref(), Some("783b7414"));
        assert_eq!(build_commit(b"\x001.2.3+5.0123456789abcdef0123456789abcdef01234567\0").as_deref(),
            Some("0123456789abcdef0123456789abcdef01234567"));
        // without git's information, too short, run on, no version before it
        assert_eq!(build_commit(b"\x000.83.0-unstable+0\0"), None);
        assert_eq!(build_commit(b"\x000.83.0-unstable+61.c2f07c\0"), None);
        assert_eq!(build_commit(b"\x000.83.0-unstable+61.c2f07c2x\0"), None);
        assert_eq!(build_commit(b"\0unstable+61.c2f07c2\0"), None);
        assert_eq!(build_commit(b"\x0083.0-unstable+61.c2f07c2\0"), None);
        assert_eq!(build_commit(b"+61.c2f07c2"), None);
    }

    #[test]
    fn reads_the_notes() {
        let commit = "c2f07c2b122a9b0f9836f9af9c61723d04e69a40";
        let notes = format!(
            "Built from the `metal` branch at the tagged commit.\n\
             Built by this repository's CI from commit [`aaaaaaaa`](https://github.com/x/y/commit/{}) of the `metal` branch\n\
             | `openmohaa-vulkan-1.0.0-linux-amd64.tar.gz` | 64-bit Linux |\n\
             Built by this repository's CI from commit [`c2f07c2b`](https://github.com/pstngh/MoHAA-Reloaded/commit/{commit}) \
             of the `vulkan` branch, with OpenMoHAA 0.83.0 (upstream `a2f34019`) as its base.\n\
             | File | SHA-256 |\n\
             | `openmohaa-vulkan-1.0.0-linux-amd64.tar.gz` | `EA8E48375fd9d4b1545e4a3f359c754213fa48ea60b8f87bae657a2478671876` |\n\
             | `openmohaa-vulkan-1.0.0-windows-x64.zip` | `34251e03` |\n",
            "a".repeat(40)
        );
        assert_eq!(notes_commit(&notes).as_deref(), Some(commit));
        assert_eq!(notes_commit("Built by this repository's CI from commit [`c2f07c2b`] of the `vulkan` branch"), None);
        assert_eq!(
            notes_sha256(&notes, "openmohaa-vulkan-1.0.0-linux-amd64.tar.gz").as_deref(),
            Some("ea8e48375fd9d4b1545e4a3f359c754213fa48ea60b8f87bae657a2478671876")
        );
        assert_eq!(notes_sha256(&notes, "openmohaa-vulkan-1.0.0-windows-x64.zip"), None);
        assert_eq!(notes_sha256(&notes, "other.zip"), None);
    }

    #[test]
    fn keeps_paths_inside_the_top_folder() {
        let top = "openmohaa-vulkan-1.0.0-linux-amd64";
        let inner = |p: &str| inner_path(Path::new(p), top);
        assert_eq!(inner("openmohaa-vulkan-1.0.0-linux-amd64/openmohaa"), Some(PathBuf::from("openmohaa")));
        assert_eq!(
            inner("./openmohaa-vulkan-1.0.0-linux-amd64/main/sound/x.sofa"),
            Some(PathBuf::from("main/sound/x.sofa"))
        );
        assert_eq!(inner("openmohaa-vulkan-1.0.0-linux-amd64/"), None);
        assert_eq!(inner("openmohaa"), None);
        assert_eq!(inner("other/openmohaa"), None);
        assert_eq!(inner("openmohaa-vulkan-1.0.0-linux-amd64/../x"), None);
        assert_eq!(inner("/openmohaa-vulkan-1.0.0-linux-amd64/x"), None);
    }

    /// The same files in a tarball and a zip, as misc/release/package.sh makes them
    #[test]
    fn unpacks_both_downloads() {
        let dir = temp_dir("unpack");
        let files: [(&str, &[u8], u32); 3] = [
            ("openmohaa", b"game", 0o755),
            ("cgame.so", b"cgame", 0o644),
            ("main/sound/prom/hrtf/README.txt", b"hrtf", 0o644),
        ];

        let top = "openmohaa-vulkan-1.0.0-linux-amd64";
        let tarball = dir.join(format!("{top}.tar.gz"));
        {
            let gz = flate2::write::GzEncoder::new(File::create(&tarball).unwrap(), flate2::Compression::default());
            let mut tar = tar::Builder::new(gz);
            let mut dir_header = tar::Header::new_gnu();
            dir_header.set_entry_type(tar::EntryType::Directory);
            dir_header.set_mode(0o755);
            dir_header.set_size(0);
            tar.append_data(&mut dir_header, format!("{top}/"), io::empty()).unwrap();
            for (name, data, mode) in files {
                let mut header = tar::Header::new_gnu();
                header.set_mode(mode);
                header.set_size(data.len() as u64);
                tar.append_data(&mut header, format!("{top}/{name}"), data).unwrap();
            }
            let mut link = tar::Header::new_gnu();
            link.set_entry_type(tar::EntryType::Symlink);
            link.set_size(0);
            tar.append_link(&mut link, format!("{top}/link"), "/etc/passwd").unwrap();
            tar.into_inner().unwrap().finish().unwrap();
        }
        let out = dir.join("tar");
        let got = unpack(&tarball, &out).unwrap();
        assert_eq!(got, files.iter().map(|f| PathBuf::from(f.0)).collect::<Vec<_>>());
        assert_eq!(fs::read(out.join("main/sound/prom/hrtf/README.txt")).unwrap(), b"hrtf");
        assert!(!out.join("link").exists());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = |p: &str| fs::metadata(out.join(p)).unwrap().permissions().mode() & 0o777;
            assert_eq!((mode("openmohaa"), mode("cgame.so")), (0o755, 0o644));
        }

        let top = "openmohaa-vulkan-1.0.0-windows-x64";
        let zipped = dir.join(format!("{top}.zip"));
        {
            let mut zip = zip::ZipWriter::new(File::create(&zipped).unwrap());
            let options = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
            zip.add_directory(format!("{top}/"), options).unwrap();
            for (name, data, mode) in files {
                zip.start_file(format!("{top}/{name}"), options.unix_permissions(mode)).unwrap();
                zip.write_all(data).unwrap();
            }
            zip.finish().unwrap();
        }
        let out = dir.join("zip");
        assert_eq!(unpack(&zipped, &out).unwrap().len(), 3);
        assert_eq!(fs::read(out.join("openmohaa")).unwrap(), b"game");

        // a file outside the top folder: nothing of it is used
        let bad = dir.join(format!("{top}-bad.zip"));
        {
            let mut zip = zip::ZipWriter::new(File::create(&bad).unwrap());
            zip.start_file("elsewhere/openmohaa", zip::write::SimpleFileOptions::default()).unwrap();
            zip.write_all(b"x").unwrap();
            zip.finish().unwrap();
        }
        assert!(unpack(&bad, &dir.join("bad")).unwrap_err().contains("outside"));
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn installs_and_puts_back() {
        let dir = temp_dir("install");
        let (new, game, old) = (dir.join("new"), dir.join("game"), dir.join("old"));
        for (name, text) in [("same", "same"), ("changed", "new"), ("added", "added"), ("main/sound/x", "new x")] {
            write(&new.join(name), text);
        }
        write(&game.join("same"), "same");
        write(&game.join("changed"), "old");
        write(&game.join("MAIN/pak0.pk3"), "pak");
        // Windows' folders (and macOS's) have no case
        let caseless = game.join("main").is_dir();
        write(&game.join("mohlauncher.json"), "{}");
        let files: Vec<PathBuf> = ["same", "changed", "added", "main/sound/x"].iter().map(PathBuf::from).collect();

        let done = install(&new, &files, &game, &old).unwrap();
        // the game's MAIN, in its case
        let main = if caseless { "main/sound/x" } else { "MAIN/sound/x" };
        assert_eq!(done, ["changed", "added", main].iter().map(PathBuf::from).collect::<Vec<_>>());
        let read = |p: &str| fs::read_to_string(game.join(p)).unwrap();
        let contents = [read("same"), read("changed"), read("added"), read("MAIN/sound/x")];
        assert_eq!(contents, ["same", "new", "added", "new x"]);
        assert_eq!(read("mohlauncher.json"), "{}");
        assert_eq!(fs::read_to_string(old.join("changed")).unwrap(), "old");
        assert!(!game.join("main").exists() || caseless);

        // a folder where the download has a file: it stays, and nothing changes
        write(&game.join("added2/inside"), "keep");
        write(&new.join("added2"), "file");
        let error = install(&new, &[PathBuf::from("added2")], &game, &old).unwrap_err();
        assert!(error.contains("is a folder") && error.contains("as they were"), "{error}");
        assert_eq!(read("added2/inside"), "keep");

        // the second file can't be put aside (a file where its folder in old goes): the first goes back
        let (new, game, old) = (dir.join("new2"), dir.join("game2"), dir.join("old2"));
        write(&new.join("a"), "new a");
        write(&new.join("sub/b"), "new b");
        write(&game.join("a"), "old a");
        write(&game.join("sub/b"), "old b");
        write(&old.join("sub"), "in the way");
        let files: Vec<PathBuf> = ["a", "sub/b"].iter().map(PathBuf::from).collect();
        let error = install(&new, &files, &game, &old).unwrap_err();
        assert!(error.contains("as they were"), "{error}");
        assert_eq!(fs::read_to_string(game.join("a")).unwrap(), "old a");
        assert_eq!(fs::read_to_string(game.join("sub/b")).unwrap(), "old b");
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn stops_before_the_files_move_only() {
        let flags = Flags::default();
        assert!(flags.begin_install());
        assert!(!flags.stop()); // moving: the window waits
        let flags = Flags::default();
        assert!(flags.stop());
        assert!(!flags.begin_install());
    }

    /// A game folder that can't be written (Linux; Windows' Program Files without admin rights is the same): the
    /// update says so before it downloads anything
    #[test]
    #[cfg(unix)]
    fn says_when_the_folder_cant_be_written() {
        use std::os::unix::fs::PermissionsExt;
        let dir = temp_dir("readonly");
        let exe = dir.join(platform::GAME_EXE);
        fs::write(&exe, "game").unwrap();
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o555)).unwrap();
        let root = File::create(dir.join("probe")).is_ok(); // writes anyway
        let update = Update {
            commit: "c".repeat(40),
            asset: "openmohaa-vulkan-1.0.0-linux-amd64.tar.gz".into(),
            url: "http://127.0.0.1:9/".into(),
            size: 1,
            sha256: None,
        };
        let result = run(&update, &exe, &Flags::default(), &mut |_, _| panic!("it went on"));
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o755)).unwrap();
        if !root {
            let error = result.unwrap_err();
            assert!(error.starts_with("Can't write to "), "{error}");
            assert!(!dir.join(WORK_DIR).exists());
        }
        fs::remove_dir_all(&dir).unwrap();
    }

    /// Set for the copy of this test program that stands in for a running game
    const WAIT_ENV: &str = "MOHLAUNCHER_TEST_WAIT";

    /// The stand-in game: waits to be killed when it's that copy, else passes at once
    #[test]
    fn waits_when_asked() {
        if std::env::var_os(WAIT_ENV).is_some() {
            std::thread::sleep(Duration::from_secs(60));
        }
    }

    /// A program running from the game's file: is_running sees it, and install still puts the new file in its place.
    /// On Windows the running one can only be moved aside, as the launcher's own is when an update replaces it.
    #[test]
    #[cfg(any(windows, target_os = "linux"))]
    fn replaces_a_running_program() {
        use std::process::{Command, Stdio};
        let dir = temp_dir("running");
        let (new, game, old) = (dir.join("new"), dir.join("game"), dir.join("old"));
        let exe = game.join(platform::GAME_EXE);
        fs::create_dir_all(&game).unwrap();
        // Linux: sleep (also from a multi-call coreutils); Windows: this test program, whose debug symbols are in a
        // file of their own, waiting in waits_when_asked
        #[cfg(target_os = "linux")]
        let mut command = {
            use std::os::unix::process::CommandExt;
            fs::copy("/bin/sleep", &exe).unwrap();
            let mut command = Command::new(&exe);
            command.arg0("sleep").arg("60");
            command
        };
        #[cfg(windows)]
        let mut command = {
            fs::copy(std::env::current_exe().unwrap(), &exe).unwrap();
            let mut command = Command::new(&exe);
            command.args(["--exact", "update::tests::waits_when_asked"]).env(WAIT_ENV, "1");
            command
        };
        assert!(!platform::is_running(&exe));
        let mut child = command.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().unwrap();
        let started = std::time::Instant::now();
        while !platform::is_running(&exe) {
            assert!(started.elapsed() < Duration::from_secs(10), "is_running doesn't see {}", exe.display());
            std::thread::sleep(Duration::from_millis(50));
        }

        write(&new.join(platform::GAME_EXE), "the new game");
        let done = install(&new, &[PathBuf::from(platform::GAME_EXE)], &game, &old);
        let still_running = child.try_wait().unwrap().is_none();
        let _ = child.kill();
        let _ = child.wait();
        assert_eq!(done.unwrap(), [PathBuf::from(platform::GAME_EXE)]);
        assert!(still_running, "the program stopped while its file moved");
        assert_eq!(fs::read_to_string(&exe).unwrap(), "the new game");
        assert!(old.join(platform::GAME_EXE).is_file());
        assert!(!platform::is_running(&exe));
        let _ = fs::remove_dir_all(&dir);
    }

    /// The whole update against the real release, into a folder whose game is an older build (its version names a
    /// commit the release's comes after): `cargo test -- --ignored` (downloads this system's build)
    #[test]
    #[ignore]
    fn updates_from_the_release() {
        let dir = temp_dir("release");
        let exe = dir.join(platform::GAME_EXE);
        fs::write(&exe, b"\0OpenMoHAA 0.83.0-unstable+55.6f4c673 linux-x86_64\0").unwrap();
        write(&dir.join("main/Pak0.pk3"), "");
        write(&dir.join("mohlauncher.json"), "{\"game\": 1}");

        if ASSET.is_none() {
            return; // no download for this system
        }
        let update = check(&exe).unwrap().expect("the release is newer than 6f4c673");
        assert_eq!(update.commit.len(), 40);
        assert!(update.size > 1_000_000 && update.sha256.is_some(), "{update:?}");

        let mut last = (Stage::Download, 0);
        let changed = run(&update, &exe, &Flags::default(), &mut |stage, done| last = (stage, done)).unwrap();
        assert_eq!(last.0, Stage::Install);
        assert!(changed.contains(&PathBuf::from(platform::GAME_EXE)), "{changed:?}");
        let commit = build_commit(&fs::read(&exe).unwrap()).unwrap();
        assert!(update.commit.starts_with(&commit));
        assert!(!dir.join(WORK_DIR).exists());
        assert_eq!(fs::read_to_string(dir.join("mohlauncher.json")).unwrap(), "{\"game\": 1}");
        assert!(check(&exe).unwrap().is_none(), "up to date now");
        fs::remove_dir_all(&dir).unwrap();
    }
}
