//! What differs between Windows and Linux: where the game and the launcher keep their files, and the display's sizes.

use crate::logic::Resolution;
use std::path::{Path, PathBuf};

#[cfg(windows)]
pub const GAME_EXE: &str = "openmohaa.exe";
#[cfg(not(windows))]
pub const GAME_EXE: &str = "openmohaa";

fn env_path(name: &str) -> Option<PathBuf> {
    std::env::var_os(name).filter(|v| !v.is_empty()).map(PathBuf::from)
}

/// The folders the game keeps its configs in, as its Sys_DefaultHomeConfigPath finds them
pub fn engine_homes() -> Vec<PathBuf> {
    let mut homes = Vec::new();
    #[cfg(windows)]
    if let Some(app_data) = env_path("APPDATA") {
        homes.push(app_data.join("openmohaa"));
    }
    #[cfg(not(windows))]
    {
        let home = env_path("HOME");
        if let Some(xdg) = env_path("XDG_CONFIG_HOME") {
            homes.push(xdg.join("openmohaa"));
        } else if let Some(home) = &home {
            homes.push(home.join(".config").join("openmohaa"));
        }
        if let Some(home) = &home {
            homes.push(home.join(".openmohaa")); // upstream's layout before XDG
        }
    }
    homes
}

/// launcher.json, beside the game's configs
pub fn settings_path() -> PathBuf {
    #[cfg(windows)]
    let base = env_path("APPDATA").unwrap_or_else(|| PathBuf::from("."));
    #[cfg(not(windows))]
    let base = env_path("XDG_CONFIG_HOME")
        .or_else(|| env_path("HOME").map(|h| h.join(".config")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("openmohaa").join("launcher.json")
}

/// The folder the launcher is in: the game folder, if the game is beside it
pub fn own_folder() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.canonicalize().ok().or(Some(exe)))
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// A data folder (main, mainta, maintt), in any case: retail folders sometimes come as MAIN or MainTA
pub fn has_subdir(folder: &Path, name: &str) -> bool {
    if folder.join(name).is_dir() {
        return true;
    }
    std::fs::read_dir(folder).is_ok_and(|entries| {
        entries.flatten().any(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name) && e.path().is_dir())
    })
}

/// The display's own sizes and the desktop's
#[cfg(windows)]
pub fn display_sizes() -> (Vec<Resolution>, Option<Resolution>) {
    use windows_sys::Win32::Graphics::Gdi::{EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS};
    let mut modes = Vec::new();
    let mut desktop = None;
    // SAFETY: DEVMODEW is plain data; dmSize tells the call its size
    unsafe {
        let mut dm: DEVMODEW = std::mem::zeroed();
        dm.dmSize = std::mem::size_of::<DEVMODEW>() as u16;
        if EnumDisplaySettingsW(std::ptr::null(), ENUM_CURRENT_SETTINGS, &mut dm) != 0 {
            desktop = Some(Resolution { width: dm.dmPelsWidth as i32, height: dm.dmPelsHeight as i32 });
        }
        let mut i = 0;
        while EnumDisplaySettingsW(std::ptr::null(), i, &mut dm) != 0 {
            modes.push(Resolution { width: dm.dmPelsWidth as i32, height: dm.dmPelsHeight as i32 });
            i += 1;
        }
    }
    (modes, desktop)
}

/// The display's own sizes and the desktop's, from the kernel's list of each connected output's modes (under X11
/// and Wayland alike); the first mode is the output's preferred one
#[cfg(not(windows))]
pub fn display_sizes() -> (Vec<Resolution>, Option<Resolution>) {
    let mut modes = Vec::new();
    let mut desktop = None;
    let mut outputs: Vec<PathBuf> = std::fs::read_dir("/sys/class/drm")
        .map(|entries| entries.flatten().map(|e| e.path()).collect())
        .unwrap_or_default();
    outputs.sort();
    for output in outputs {
        let connected = std::fs::read_to_string(output.join("status")).is_ok_and(|s| s.trim() == "connected");
        if !connected {
            continue;
        }
        let Ok(list) = std::fs::read_to_string(output.join("modes")) else { continue };
        for line in list.lines() {
            let line = line.trim_end_matches(|c: char| !c.is_ascii_digit());
            if let Some((w, h)) = line.split_once('x') {
                if let (Ok(width), Ok(height)) = (w.parse(), h.parse()) {
                    let r = Resolution { width, height };
                    desktop.get_or_insert(r);
                    modes.push(r);
                }
            }
        }
    }
    (modes, desktop)
}
