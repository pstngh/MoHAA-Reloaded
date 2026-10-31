//! The launcher's logic, without UI: the game's config file, the settings the launcher edits, the game's command
//! line, the crosshair's geometry and the launcher's own settings. launcher/macos's LauncherCore is the same logic
//! in Swift; `cargo test` checks this one.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const GAME_COUNT: usize = 3;
pub const GAME_TITLES: [&str; GAME_COUNT] = ["Allied Assault", "Spearhead", "Breakthrough"];
/// Each game's data folder, which is also its folder for configs
pub const GAME_DIRS: [&str; GAME_COUNT] = ["main", "mainta", "maintt"];

/// Lowercase cvar name -> value
pub type Cvars = BTreeMap<String, String>;

/// The values a config's `seta`/`set` lines leave, as the game reads them: quotes, `;` and `//` as the game's own
/// command parser handles them. Names are lowercased (cvar names don't depend on case).
pub fn parse_config(text: &str) -> Cvars {
    fn take(tokens: &mut Vec<String>, out: &mut Cvars) {
        if tokens.len() >= 3 {
            let cmd = tokens[0].to_ascii_lowercase();
            if matches!(cmd.as_str(), "set" | "seta" | "sets" | "setu") {
                out.insert(tokens[1].to_lowercase(), tokens[2..].join(" "));
            }
        }
        tokens.clear();
    }

    let b = text.as_bytes();
    let n = b.len();
    let at = |k: usize| if k < n { b[k] } else { 0 };
    let mut out = Cvars::new();
    let mut tokens = Vec::new();
    let mut star_comment = false;
    let mut i = 0;
    while i < n {
        let c = b[i];
        if star_comment {
            if c == b'*' && at(i + 1) == b'/' {
                star_comment = false;
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }
        if c == b'\n' || c == b'\r' || c == b';' {
            // the end of a command (the game's command buffer splits there outside quotes)
            take(&mut tokens, &mut out);
            i += 1;
            continue;
        }
        if c <= b' ' {
            i += 1;
            continue;
        }
        if c == b'/' && at(i + 1) == b'/' {
            while i < n && b[i] != b'\n' && b[i] != b'\r' {
                i += 1;
            }
            continue;
        }
        if c == b'/' && at(i + 1) == b'*' {
            star_comment = true;
            i += 2;
            continue;
        }
        let start;
        let end;
        if c == b'"' {
            // a quoted token runs to the closing quote or the end of the line
            start = i + 1;
            i = start;
            while i < n && b[i] != b'"' && b[i] != b'\n' && b[i] != b'\r' {
                i += 1;
            }
            end = i;
            if i < n && b[i] == b'"' {
                i += 1;
            }
        } else {
            start = i;
            while i < n
                && b[i] > b' '
                && b[i] != b'"'
                && b[i] != b';'
                && !(b[i] == b'/' && (at(i + 1) == b'/' || at(i + 1) == b'*'))
            {
                i += 1;
            }
            end = i;
        }
        tokens.push(String::from_utf8_lossy(&b[start..end]).into_owned());
    }
    take(&mut tokens, &mut out);
    out
}

pub fn read_config(path: &Path) -> Option<Cvars> {
    let bytes = std::fs::read(path).ok()?;
    Some(parse_config(&String::from_utf8_lossy(&bytes)))
}

/// The first config that exists: for the game's own data folder, then for Allied Assault's (an expansion's first
/// start inherits it), in each home folder in turn, `omconfig.cfg` before the original game's `unnamedsoldier.cfg`.
pub fn find_config(homes: &[PathBuf], game: usize) -> Option<PathBuf> {
    let mut dirs = vec![GAME_DIRS[game]];
    if game != 0 {
        dirs.push(GAME_DIRS[0]);
    }
    for name in ["omconfig.cfg", "unnamedsoldier.cfg"] {
        for dir in &dirs {
            for home in homes {
                let path = home.join(dir).join("configs").join(name);
                if path.is_file() {
                    return Some(path);
                }
            }
        }
    }
    None
}

/// The settings the launcher edits. Each holds the value the game's config had when it was last read and the one
/// shown in the launcher; only the ones the player changed in the launcher go to the game, so a change made in the
/// game (its menus, the console) is never undone by starting it from here.
#[derive(Clone, Debug)]
pub struct CvarEdits {
    entries: Vec<Entry>,
}

#[derive(Clone, Debug)]
struct Entry {
    /// lowercase, to find it (cvar names don't depend on case)
    key: String,
    /// as written, to pass it
    name: String,
    def: String,
    value: String,
    loaded: String,
}

impl CvarEdits {
    pub fn new(defaults: &[(&str, &str)]) -> Self {
        let entries = defaults
            .iter()
            .map(|(name, def)| Entry {
                key: name.to_lowercase(),
                name: name.to_string(),
                def: def.to_string(),
                value: def.to_string(),
                loaded: def.to_string(),
            })
            .collect();
        CvarEdits { entries }
    }

    fn find(&self, name: &str) -> Option<&Entry> {
        let key = name.to_lowercase();
        self.entries.iter().find(|e| e.key == key)
    }

    pub fn get(&self, name: &str) -> &str {
        self.find(name).map_or("", |e| e.value.as_str())
    }

    pub fn set(&mut self, name: &str, value: &str) {
        let key = name.to_lowercase();
        if let Some(e) = self.entries.iter_mut().find(|e| e.key == key) {
            e.value = value.to_string();
        }
    }

    #[cfg(test)]
    pub fn changed(&self, name: &str) -> bool {
        self.find(name).is_some_and(|e| e.value != e.loaded)
    }

    /// Takes the config's values (the defaults for the ones it doesn't have); a value changed in the launcher stays
    pub fn load(&mut self, config: &Cvars) {
        for e in &mut self.entries {
            let changed = e.value != e.loaded;
            e.loaded = config.get(&e.key).cloned().unwrap_or_else(|| e.def.clone());
            if !changed {
                e.value = e.loaded.clone();
            }
        }
    }

    /// The changed values, in the order of the defaults, by their names as written
    pub fn changes(&self) -> Vec<(String, String)> {
        self.entries.iter().filter(|e| e.value != e.loaded).map(|e| (e.name.clone(), e.value.clone())).collect()
    }

    /// The game was started with the changes: they are the config's values now
    pub fn applied(&mut self) {
        for e in &mut self.entries {
            e.loaded = e.value.clone();
        }
    }
}

/// One value on the game's command line. The game joins its arguments into one line, quoting the ones with a space,
/// and splits that line into commands at each `+` and `;` outside quotes: a value is cleaned of quotes and control
/// characters, and quoted when it has none of its own (no space) but could be split or cut (`+`, `;`, `//`).
pub fn engine_arg(value: &str) -> String {
    let v: String = value
        .chars()
        .filter_map(|c| match c {
            '\t' => Some(' '),
            '"' => None,
            c if (c as u32) < 0x20 || c as u32 == 0x7f => None,
            c => Some(c),
        })
        .collect();
    if v.contains(' ') {
        return v; // the game puts it in quotes itself
    }
    if v.is_empty() || v.contains('+') || v.contains(';') || v.contains("//") || v.contains("/*") {
        return format!("\"{v}\"");
    }
    v
}

#[derive(Clone, Debug, Default)]
pub struct Launch {
    /// fs_homepath: where the game writes everything (the game folder), if not empty
    pub homepath: String,
    /// fs_basepath: the game folder when the game isn't in it, else empty
    pub basepath: String,
    pub game: usize,
    pub cvars: Vec<(String, String)>,
    pub address: String,
    pub password: String,
    pub rcon: String,
}

/// The game's arguments: +set fs_homepath, +set fs_basepath, +set com_target_game N, the cvars, the passwords,
/// +connect
pub fn build_args(launch: &Launch) -> Vec<String> {
    let mut args: Vec<String> = Vec::new();
    let mut set = |name: &str, value: &str| {
        args.extend(["+set".to_string(), name.to_string(), engine_arg(value)]);
    };
    if !launch.homepath.is_empty() {
        set("fs_homepath", &launch.homepath);
    }
    if !launch.basepath.is_empty() {
        set("fs_basepath", &launch.basepath);
    }
    set("com_target_game", &launch.game.to_string());
    for (name, value) in &launch.cvars {
        set(name, value);
    }
    if !launch.password.is_empty() {
        set("password", &launch.password);
    }
    if !launch.rcon.is_empty() {
        set("rconPassword", &launch.rcon);
    }
    if !launch.address.is_empty() {
        args.push("+connect".into());
        args.push(engine_arg(&launch.address));
    }
    args
}

pub fn same_address(a: &str, b: &str) -> bool {
    fn key(s: &str) -> String {
        let k = s.trim().to_lowercase();
        if k.contains(':') {
            k
        } else {
            k + ":12203" // the game's default port
        }
    }
    key(a) == key(b)
}

/// "FFFFFF", "#ffffff" -> 0xFFFFFF; None if it isn't 6 hex digits (the game then draws white)
pub fn parse_color(text: &str) -> Option<u32> {
    let hex = text.trim();
    let hex = hex.strip_prefix('#').unwrap_or(hex);
    if hex.len() != 6 || !hex.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u32::from_str_radix(hex, 16).ok()
}

pub fn color_hex(rgb: u32) -> String {
    format!("{:06X}", rgb & 0xFF_FFFF)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CrossArm {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// The crosshair as cg_drawtools.cpp lays it out, in HUD pixels: the four arms around a center square of the
/// thickness, whose top left corner is (0, 0), and the width (and height) the arms span
pub fn crosshair_boxes(length: f64, gap: f64, thickness: f64) -> ([CrossArm; 4], i32) {
    fn pixels(value: f64, maximum: f64) -> i32 {
        let v = if value.is_nan() || value < 1.0 { 1.0 } else { value.min(maximum) };
        (v + 0.5) as i32
    }
    let l = pixels(length, 128.0);
    let t = pixels(thickness, 16.0);
    let s = (pixels(gap, 64.0) - t / 2).max(1);
    let arms = [
        CrossArm { x: -s - l, y: 0, w: l, h: t },
        CrossArm { x: t + s, y: 0, w: l, h: t },
        CrossArm { x: 0, y: -s - l, w: t, h: l },
        CrossArm { x: 0, y: t + s, w: t, h: l },
    ];
    (arms, 2 * (s + l) + t)
}

/// The game's window modes on the Vulkan port, as r_fullscreen and r_noborder pick them
pub const MODE_TITLES: [&str; 3] = ["Fullscreen", "Borderless window", "Window"];
pub const MODE_FULLSCREEN: usize = 0;
pub const MODE_BORDERLESS: usize = 1;
pub const MODE_WINDOW: usize = 2;

fn int(s: &str) -> i64 {
    s.trim().parse::<f64>().map_or(0, |v| v as i64)
}

pub fn mode_from_cvars(fullscreen: &str, noborder: &str) -> usize {
    if int(fullscreen) != 0 {
        MODE_FULLSCREEN
    } else if int(noborder) != 0 {
        MODE_BORDERLESS
    } else {
        MODE_WINDOW
    }
}

pub fn mode_to_cvars(mode: usize, edits: &mut CvarEdits) {
    match mode {
        MODE_FULLSCREEN => {
            // r_noborder only counts for a window
            if int(edits.get("r_fullscreen")) == 0 {
                edits.set("r_fullscreen", "1");
            }
        }
        MODE_BORDERLESS => {
            edits.set("r_fullscreen", "0");
            edits.set("r_noborder", "1");
        }
        _ => {
            edits.set("r_fullscreen", "0");
            edits.set("r_noborder", "0");
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Resolution {
    pub width: i32,
    pub height: i32,
}

/// The sizes to offer: the display's, the common ones that fit it, largest first, without duplicates
pub fn resolution_list(display_modes: &[Resolution], desktop: Option<Resolution>) -> Vec<Resolution> {
    const COMMON: [(i32, i32); 18] = [
        (3840, 2160), (3440, 1440), (2560, 1600), (2560, 1440), (2560, 1080), (1920, 1200), (1920, 1080),
        (1680, 1050), (1600, 900), (1440, 900), (1366, 768), (1280, 1024), (1280, 960), (1280, 800),
        (1280, 720), (1024, 768), (800, 600), (640, 480),
    ];
    let mut list: Vec<Resolution> = Vec::new();
    let mut add = |r: Resolution| {
        if r.width >= 640 && r.height >= 480 && !list.contains(&r) {
            list.push(r);
        }
    };
    if let Some(d) = desktop {
        add(d);
    }
    for &r in display_modes {
        add(r);
    }
    for (w, h) in COMMON {
        if desktop.is_none_or(|d| w <= d.width && h <= d.height) {
            add(Resolution { width: w, height: h });
        }
    }
    list.sort_by(|a, b| b.width.cmp(&a.width).then(b.height.cmp(&a.height)));
    list
}

/// An entry of the resolution menu: the desktop's (r_mode -2), a size (r_mode -1 with r_customwidth and
/// r_customheight), or a numbered mode a config holds
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResolutionEntry {
    pub mode: i64,
    pub width: i32,
    pub height: i32,
}

impl ResolutionEntry {
    pub fn label(&self) -> String {
        match self.mode {
            -2 => "Desktop resolution".into(),
            -1 => format!("{} \u{00D7} {}", self.width, self.height),
            m => format!("r_mode {m}"),
        }
    }
}

/// The menu's entries for the sizes offered and the config's own (r_mode, r_customwidth, r_customheight), and the
/// index of the config's
pub fn resolution_entries(sizes: &[Resolution], edits: &CvarEdits) -> (Vec<ResolutionEntry>, usize) {
    let mut entries = vec![ResolutionEntry { mode: -2, width: 0, height: 0 }];
    entries.extend(sizes.iter().map(|r| ResolutionEntry { mode: -1, width: r.width, height: r.height }));
    let mode = int(edits.get("r_mode"));
    let current = if mode == -1 {
        ResolutionEntry {
            mode: -1,
            width: int(edits.get("r_customwidth")) as i32,
            height: int(edits.get("r_customheight")) as i32,
        }
    } else {
        ResolutionEntry { mode: if mode < -1 { -2 } else { mode }, width: 0, height: 0 }
    };
    if let Some(k) = entries.iter().position(|e| *e == current) {
        return (entries, k);
    }
    // the config's own size where it belongs in the list, or its numbered mode at the end
    let k = if current.mode == -1 {
        entries
            .iter()
            .skip(1)
            .position(|e| e.width < current.width || (e.width == current.width && e.height < current.height))
            .map_or(entries.len(), |k| k + 1)
    } else {
        entries.len()
    };
    entries.insert(k, current);
    (entries, k)
}

/// The launcher's own settings, as the game folder's mohlauncher.json holds them (the macOS launcher's are the same)
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub game: usize,
    pub slots: Vec<GameSlot>,
}

/// Per game: the server fields and the servers played on
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct GameSlot {
    pub address: String,
    pub password: String,
    pub rcon: String,
    pub servers: Vec<Server>,
}

/// A server in the list: what it's called there (its address until it's renamed), its address and the passwords
/// used on it. The player's own name isn't part of it: that's the game's.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Server {
    pub label: String,
    pub address: String,
    pub password: String,
    pub rcon: String,
}

impl Settings {
    pub fn from_json(text: &str) -> Settings {
        let mut s: Settings = serde_json::from_str(text).unwrap_or_default();
        s.normalize();
        s
    }

    fn normalize(&mut self) {
        if self.game >= GAME_COUNT {
            self.game = 0;
        }
        self.slots.resize_with(GAME_COUNT, GameSlot::default);
        for slot in &mut self.slots {
            slot.servers.retain(|s| !s.address.trim().is_empty());
            for s in &mut slot.servers {
                if s.label.trim().is_empty() {
                    s.label = s.address.clone();
                }
            }
        }
    }

    /// Keeps the server in the game's list (added at the end), with the passwords used
    pub fn remember_server(&mut self, game: usize, address: &str, password: &str, rcon: &str) {
        let servers = &mut self.slots[game].servers;
        if let Some(s) = servers.iter_mut().find(|s| same_address(&s.address, address)) {
            s.address = address.to_string();
            s.password = password.to_string();
            s.rcon = rcon.to_string();
        } else {
            servers.push(Server {
                label: address.to_string(),
                address: address.to_string(),
                password: password.to_string(),
                rcon: rcon.to_string(),
            });
        }
    }
}

/// The servers the launcher comes with, first in their game's list (game, name, address): they can't be renamed or
/// removed
pub const BUILTIN_SERVERS: [(usize, &str, &str); 1] = [(0, "Forte FFA", "151.80.234.90:12203")];

/// A server as the list shows it
#[derive(Clone, Debug, PartialEq)]
pub struct Listed {
    /// with the passwords last used on it
    pub server: Server,
    /// its place in the game's saved servers, if it's there
    pub saved: Option<usize>,
    pub builtin: bool,
}

/// The game's list: its built-in servers, then the ones the player joined (a built-in one's entry, which keeps the
/// passwords used, isn't listed twice)
pub fn listed_servers(settings: &Settings, game: usize) -> Vec<Listed> {
    let saved = &settings.slots[game].servers;
    let mut list = Vec::new();
    for &(_, name, address) in BUILTIN_SERVERS.iter().filter(|b| b.0 == game) {
        let k = saved.iter().position(|s| same_address(&s.address, address));
        let (password, rcon) = k.map_or_else(Default::default, |k| (saved[k].password.clone(), saved[k].rcon.clone()));
        let server = Server { label: name.into(), address: address.into(), password, rcon };
        list.push(Listed { server, saved: k, builtin: true });
    }
    for (k, s) in saved.iter().enumerate() {
        if !BUILTIN_SERVERS.iter().any(|b| b.0 == game && same_address(b.2, &s.address)) {
            list.push(Listed { server: s.clone(), saved: Some(k), builtin: false });
        }
    }
    list
}

/// A path without the verbatim prefix Windows' canonical paths have (`\\?\C:\x`, `\\?\UNC\server\share\x`), for the
/// game's fs_homepath and fs_basepath: the game appends to the path it's given and merges repeated separators, which
/// turns `\\?\` into `\?\`, a folder it can't create. Any other path, and one that isn't Unicode, is kept as it is.
pub fn plain_path(path: &Path) -> PathBuf {
    let Some(text) = path.to_str() else { return path.to_path_buf() };
    if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        return PathBuf::from(format!(r"\\{rest}"));
    }
    if let Some(rest) = text.strip_prefix(r"\\?\") {
        let b = rest.as_bytes();
        if b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':' {
            return PathBuf::from(rest);
        }
    }
    path.to_path_buf()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_configs() {
        let text = "// generated by OpenMoHAA, do not modify\n\
            unbindall\n\
            bind TAB \"+scores\"\n\
            seta name \"Some Body\"\r\n\
            seta cg_crosshair_length \"5\"\n\
            SETA CG_Crosshair_Color \"#00ff00\" // green\n\
            set r_mode -1; seta r_customwidth \"2560\"; seta r_customheight 1440\n\
            seta password \"a;b//c\"\n\
            /* seta r_fullscreen \"0\" */ seta r_noborder \"1\"\n\
            seta empty \"\"\n\
            seta unterminated \"abc\n\
            seta spaced one two\n\
            seta \u{00FC}n\u{00EF}code \"\u{00E4}\"\n";
        let c = parse_config(text);
        assert_eq!(c["name"], "Some Body");
        assert_eq!(c["cg_crosshair_length"], "5");
        assert_eq!(c["cg_crosshair_color"], "#00ff00");
        assert_eq!(c["r_mode"], "-1");
        assert_eq!(c["r_customwidth"], "2560");
        assert_eq!(c["r_customheight"], "1440");
        assert_eq!(c["password"], "a;b//c");
        assert!(!c.contains_key("r_fullscreen"));
        assert_eq!(c["r_noborder"], "1");
        assert_eq!(c["empty"], "");
        assert_eq!(c["unterminated"], "abc");
        assert_eq!(c["spaced"], "one two");
        assert_eq!(c["\u{00FC}n\u{00EF}code"], "\u{00E4}");
        assert!(!c.contains_key("tab"));
    }

    #[test]
    fn keeps_the_launchers_changes_only() {
        let mut e = CvarEdits::new(&[("name", ""), ("cg_crosshair_length", "4"), ("r_fullscreen", "1")]);
        assert_eq!(e.get("cg_crosshair_length"), "4");
        assert!(e.changes().is_empty());

        e.load(&Cvars::from([("name".into(), "Player".into()), ("cg_crosshair_length".into(), "6".into())]));
        assert_eq!(e.get("name"), "Player");
        assert_eq!(e.get("cg_crosshair_length"), "6");
        assert_eq!(e.get("r_fullscreen"), "1");
        assert!(e.changes().is_empty());

        e.set("cg_crosshair_length", "8");
        assert!(e.changed("cg_crosshair_length"));
        // a change made in the game to another setting comes in; the launcher's own change stays
        e.load(&Cvars::from([("name".into(), "Renamed".into()), ("cg_crosshair_length".into(), "7".into())]));
        assert_eq!(e.get("name"), "Renamed");
        assert_eq!(e.get("cg_crosshair_length"), "8");
        assert_eq!(e.changes(), vec![("cg_crosshair_length".to_string(), "8".to_string())]);

        e.set("cg_crosshair_length", "7");
        assert!(e.changes().is_empty());

        e.set("r_fullscreen", "0");
        e.applied();
        assert!(e.changes().is_empty());
        e.load(&Cvars::from([("r_fullscreen".into(), "0".into())]));
        assert_eq!(e.get("r_fullscreen"), "0");
        assert!(!e.changed("r_fullscreen"));
    }

    #[test]
    fn builds_the_command_line() {
        assert_eq!(engine_arg("plain"), "plain");
        assert_eq!(engine_arg("with space"), "with space");
        assert_eq!(engine_arg("a;b"), "\"a;b\"");
        assert_eq!(engine_arg("a+b"), "\"a+b\"");
        assert_eq!(engine_arg("http://x"), "\"http://x\"");
        assert_eq!(engine_arg("say \"hi\""), "say hi");
        assert_eq!(engine_arg(""), "\"\"");
        assert_eq!(engine_arg("tab\there"), "tab here");
        assert_eq!(engine_arg("\u{00FC}n\u{00EF}"), "\u{00FC}n\u{00EF}");

        let launch = Launch {
            homepath: String::new(),
            basepath: String::new(),
            game: 1,
            cvars: vec![("name".into(), "A B".into()), ("cg_crosshair_color".into(), "00FF00".into())],
            address: "1.2.3.4:12203".into(),
            password: "pw;1".into(),
            rcon: "r".into(),
        };
        let want = [
            "+set", "com_target_game", "1", "+set", "name", "A B", "+set", "cg_crosshair_color", "00FF00", "+set",
            "password", "\"pw;1\"", "+set", "rconPassword", "r", "+connect", "1.2.3.4:12203",
        ];
        assert_eq!(build_args(&launch), want);
        assert_eq!(build_args(&Launch::default()), ["+set", "com_target_game", "0"]);
        let elsewhere = Launch { homepath: "/games/MOH AA".into(), basepath: "/games/MOH AA".into(), ..Default::default() };
        assert_eq!(
            build_args(&elsewhere),
            ["+set", "fs_homepath", "/games/MOH AA", "+set", "fs_basepath", "/games/MOH AA", "+set", "com_target_game", "0"]
        );
    }

    #[test]
    fn misc() {
        assert!(same_address("Host.Example", "host.example:12203"));
        assert!(!same_address("host:12204", "host"));
        assert_eq!(parse_color("#00ff7F"), Some(0x00FF7F));
        assert_eq!(parse_color("12345"), None);
        assert_eq!(parse_color("GGGGGG"), None);
        assert_eq!(color_hex(0x0A0B0C), "0A0B0C");

        let (arms, extent) = crosshair_boxes(4.0, 3.0, 1.0);
        assert_eq!(arms[0], CrossArm { x: -7, y: 0, w: 4, h: 1 });
        assert_eq!((arms[1].x, arms[3].y, arms[2].y, extent), (4, 4, -7, 15));
        let (arms, extent) = crosshair_boxes(5.0, 3.0, 2.0);
        assert_eq!((arms[1].x, arms[0].x, extent), (4, -7, 16));
        let (arms, _) = crosshair_boxes(0.0, 1000.0, 99.0);
        assert_eq!((arms[1].w, arms[0].h, arms[1].x), (1, 16, 16 + 56));

        assert_eq!(mode_from_cvars("1", "1"), MODE_FULLSCREEN);
        assert_eq!(mode_from_cvars("0", "1"), MODE_BORDERLESS);
        assert_eq!(mode_from_cvars("0", "0"), MODE_WINDOW);
        let mut e = CvarEdits::new(&[("r_fullscreen", "1"), ("r_noborder", "0")]);
        mode_to_cvars(MODE_BORDERLESS, &mut e);
        assert_eq!((e.get("r_fullscreen"), e.get("r_noborder")), ("0", "1"));
        mode_to_cvars(MODE_FULLSCREEN, &mut e);
        assert_eq!((e.get("r_fullscreen"), e.get("r_noborder")), ("1", "1"));

        let r = |width, height| Resolution { width, height };
        let list = resolution_list(&[r(1920, 1080), r(1280, 720), r(320, 200)], Some(r(1920, 1080)));
        assert_eq!(list[0], r(1920, 1080));
        assert_eq!(list.iter().filter(|&&x| x == r(1920, 1080)).count(), 1);
        assert!(!list.contains(&r(2560, 1440)) && !list.contains(&r(320, 200)));

        let mut e = CvarEdits::new(&[("r_mode", "-2"), ("r_customwidth", "1600"), ("r_customheight", "1024")]);
        let sizes = [r(1920, 1080), r(1280, 720)];
        assert_eq!(resolution_entries(&sizes, &e).1, 0);
        e.set("r_mode", "-1");
        let (entries, k) = resolution_entries(&sizes, &e);
        assert_eq!((entries.len(), k, entries[k].width), (4, 2, 1600));
        e.set("r_mode", "4");
        let (entries, k) = resolution_entries(&sizes, &e);
        assert_eq!((k, entries[k].label()), (3, "r_mode 4".to_string()));
    }

    #[test]
    fn finds_configs() {
        let root = std::env::temp_dir().join(format!("mohl-core-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let (home, base) = (root.join("home"), root.join("base"));
        let write = |p: PathBuf, text: &str| {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, text).unwrap();
        };
        let aa = home.join("main/configs/omconfig.cfg");
        write(aa.clone(), "seta name a\n");
        write(base.join("maintt/configs/unnamedsoldier.cfg"), "seta name b\n");
        let homes = [home.clone(), base.clone()];
        assert_eq!(find_config(&homes, 0), Some(aa.clone()));
        assert_eq!(find_config(&homes, 1), Some(aa.clone()));
        assert_eq!(find_config(&homes, 2), Some(aa.clone()));
        let bt = home.join("maintt/configs/omconfig.cfg");
        write(bt.clone(), "seta name c\n");
        assert_eq!(find_config(&homes, 2), Some(bt.clone()));
        assert_eq!(find_config(&[root.join("none")], 0), None);
        assert_eq!(read_config(&bt).unwrap()["name"], "c");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn reads_and_keeps_settings() {
        // the macOS launcher's launcher.json reads the same
        let s = Settings::from_json(
            r#"{"game": 1, "slots": [{"address": "a:1", "servers": [{"label": "", "address": "a:1"}, {"address": " "}]}]}"#,
        );
        assert_eq!(s.game, 1);
        assert_eq!(s.slots.len(), GAME_COUNT);
        assert_eq!(s.slots[0].servers, vec![Server { label: "a:1".into(), address: "a:1".into(), ..Default::default() }]);
        assert_eq!(Settings::from_json("not json").slots.len(), GAME_COUNT);

        let mut s = Settings::from_json("{}");
        s.remember_server(0, "host", "pw", "");
        s.remember_server(0, "HOST:12203", "pw2", "r");
        assert_eq!(s.slots[0].servers.len(), 1);
        assert_eq!((s.slots[0].servers[0].address.as_str(), s.slots[0].servers[0].password.as_str()), ("HOST:12203", "pw2"));
        let json = serde_json::to_string(&s).unwrap();
        assert!(json.contains("\"slots\"") && json.contains("\"label\"") && !json.contains("gameFolder"));
        assert!(!json.contains("\"name\"")); // a server has a label; the player's name is the game's
    }

    #[test]
    fn lists_the_builtin_servers_first() {
        let mut s = Settings::from_json("{}");
        s.remember_server(0, "a:1", "", "");
        let list = listed_servers(&s, 0);
        assert_eq!(list.len(), 2);
        assert!(list[0].builtin && list[0].saved.is_none());
        assert_eq!((list[0].server.label.as_str(), list[0].server.address.as_str()), ("Forte FFA", "151.80.234.90:12203"));
        assert_eq!((list[1].server.address.as_str(), list[1].saved), ("a:1", Some(0)));
        // joined with a password: listed once, with it
        s.remember_server(0, "151.80.234.90:12203", "pw", "");
        let list = listed_servers(&s, 0);
        assert_eq!(list.len(), 2);
        assert_eq!((list[0].saved, list[0].server.password.as_str()), (Some(1), "pw"));
        // the other games don't have it
        assert!(listed_servers(&s, 1).is_empty());
    }

    #[test]
    fn plain_paths() {
        let plain = |p: &str| plain_path(Path::new(p)).to_str().unwrap().to_string();
        assert_eq!(plain(r"\\?\C:\Games\MOHAA"), r"C:\Games\MOHAA");
        assert_eq!(plain(r"\\?\d:\"), r"d:\");
        assert_eq!(plain(r"\\?\UNC\server\share\MOHAA"), r"\\server\share\MOHAA");
        assert_eq!(plain(r"C:\Games\MOHAA"), r"C:\Games\MOHAA");
        assert_eq!(plain(r"\\?\Volume{0}\MOHAA"), r"\\?\Volume{0}\MOHAA");
        assert_eq!(plain("/home/player/Games/MOHAA"), "/home/player/Games/MOHAA");
    }
}
