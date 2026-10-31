//! MoHAA Launcher for Windows and Linux. It starts the game beside it (openmohaa.exe, openmohaa) for the game
//! picked on the left, with the player's name, the server, the crosshair and the display set here.

#![windows_subsystem = "windows"]

mod logic;
mod platform;

use crate::logic::*;
use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::{Duration, SystemTime};

slint::include_modules!();

const CVAR_DEFAULTS: [(&str, &str); 10] = [
    ("name", ""),
    ("cg_crosshair_length", "4"),
    ("cg_crosshair_gap", "3"),
    ("cg_crosshair_thickness", "1"),
    ("cg_crosshair_color", "FFFFFF"),
    ("r_mode", "-2"),
    ("r_customwidth", "1600"),
    ("r_customheight", "1024"),
    ("r_fullscreen", "1"),
    ("r_noborder", "0"),
];

/// The launcher's settings, in the game folder
const SETTINGS_FILE: &str = "mohlauncher.json";

const COLOR_PRESETS: [(&str, u32); 7] = [
    ("White", 0xFFFFFF),
    ("Green", 0x00FF00),
    ("Yellow", 0xFFFF00),
    ("Cyan", 0x00FFFF),
    ("Red", 0xFF0000),
    ("Magenta", 0xFF00FF),
    ("Black", 0x000000),
];

/// What the dialog over the window is for
#[derive(Clone, Copy, PartialEq)]
enum Dialog {
    None,
    Message,
    ChooseFolder,
    Rename(usize),
    Remove(usize),
    CustomColor,
}

struct State {
    /// The launcher's settings: mohlauncher.json in the game folder
    settings: Settings,
    own_folder: PathBuf,
    /// The game folder chosen with the folder dialog, for this session: it's kept nowhere outside the game folder
    chosen: Option<PathBuf>,
    game: usize,
    edits: CvarEdits,
    sizes: Vec<Resolution>,
    resolution_entries: Vec<ResolutionEntry>,
    running: bool,
    /// The config last read, and when it was written: read again when it changes
    config_seen: (Option<PathBuf>, Option<SystemTime>),
    dialog: Dialog,
}

fn modified(path: Option<&Path>) -> Option<SystemTime> {
    path.and_then(|p| std::fs::metadata(p).ok()).and_then(|m| m.modified().ok())
}

impl State {
    fn new() -> State {
        let (modes, desktop) = platform::display_sizes();
        let mut state = State {
            game: 0,
            settings: Settings::from_json("{}"),
            own_folder: platform::own_folder(),
            chosen: None,
            edits: CvarEdits::new(&CVAR_DEFAULTS),
            sizes: resolution_list(&modes, desktop),
            resolution_entries: Vec::new(),
            running: false,
            config_seen: (None, None),
            dialog: Dialog::None,
        };
        state.load_settings();
        state
    }

    /// The game folder's mohlauncher.json (none yet: the defaults), and its config
    fn load_settings(&mut self) {
        self.settings = self
            .game_folder()
            .and_then(|folder| std::fs::read_to_string(folder.join(SETTINGS_FILE)).ok())
            .map_or_else(|| Settings::from_json("{}"), |text| Settings::from_json(&text));
        self.game = self.settings.game;
        // an expansion picked last time whose data has gone
        if !self.available(self.game) {
            self.game = 0;
        }
        self.reload_config();
    }

    /// The game folder: the first with the game's data (main/Pak0.pk3) of the one chosen in this session, the
    /// launcher's own and the current folder. Everything the launcher and the game write goes there.
    fn game_folder(&self) -> Option<PathBuf> {
        let current = std::env::current_dir().ok();
        [self.chosen.clone(), Some(self.own_folder.clone()), current]
            .into_iter()
            .flatten()
            .find(|f| platform::has_game_data(f))
    }

    /// The game: beside the launcher (the download's folder), else in the game folder
    fn game_exe(&self) -> Option<PathBuf> {
        [Some(self.own_folder.clone()), self.game_folder()]
            .into_iter()
            .flatten()
            .map(|folder| folder.join(platform::GAME_EXE))
            .find(|exe| exe.is_file())
    }

    fn can_start(&self) -> bool {
        !self.running && self.game_folder().is_some() && self.game_exe().is_some()
    }

    /// An expansion without its data can't start
    fn available(&self, game: usize) -> bool {
        game == 0 || self.game_folder().is_none_or(|folder| platform::has_subdir(&folder, GAME_DIRS[game]))
    }

    /// The game's config: the game folder's (the game is started with its home there), else the one an earlier
    /// start without the launcher left where the game keeps it by default, which the next start copies over
    fn config_path(&self) -> Option<PathBuf> {
        let mut homes: Vec<PathBuf> = self.game_folder().into_iter().collect();
        homes.extend(platform::engine_homes());
        find_config(&homes, self.game)
    }

    /// The configs the game kept outside the game folder before, copied into it once (for the data folders it has),
    /// so that the settings carry over; nothing is written outside the game folder
    fn import_configs(&self, folder: &Path) {
        for dir in GAME_DIRS {
            let target = folder.join(dir).join("configs").join("omconfig.cfg");
            if target.exists() || !platform::has_subdir(folder, dir) {
                continue;
            }
            let old = platform::engine_homes().into_iter().map(|h| h.join(dir).join("configs").join("omconfig.cfg"));
            if let Some(old) = old.into_iter().find(|p| p.is_file()) {
                if let Some(parent) = target.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                let _ = std::fs::copy(&old, &target);
            }
        }
    }

    fn reload_config(&mut self) {
        let path = self.config_path();
        let cvars = path.as_deref().and_then(read_config).unwrap_or_default();
        self.edits.load(&cvars);
        self.config_seen = (path.clone(), modified(path.as_deref()));
    }

    fn config_changed(&self) -> bool {
        let path = self.config_path();
        let stamp = modified(path.as_deref());
        (path, stamp) != self.config_seen
    }

    fn slot(&self) -> &GameSlot {
        &self.settings.slots[self.game]
    }

    fn slot_mut(&mut self) -> &mut GameSlot {
        &mut self.settings.slots[self.game]
    }

    fn status(&self) -> String {
        let Some(folder) = self.game_folder() else {
            return "No game found: choose your MOHAA folder.".into();
        };
        let folder = folder.display().to_string();
        // the home folder as ~ on Linux
        #[cfg(not(windows))]
        let folder = match std::env::var("HOME") {
            Ok(home) if !home.is_empty() && folder.starts_with(&home) => format!("~{}", &folder[home.len()..]),
            _ => folder,
        };
        if self.game_exe().is_none() {
            format!("{} isn't beside the launcher or in {folder}.", platform::GAME_EXE)
        } else if self.running {
            format!("{} is running.", GAME_TITLES[self.game])
        } else {
            folder
        }
    }

    /// mohlauncher.json in the game folder, and nowhere else (with no game folder, nowhere)
    fn save(&mut self) {
        self.settings.game = self.game;
        let Some(folder) = self.game_folder() else { return };
        let Ok(json) = serde_json::to_string_pretty(&self.settings) else { return };
        // a new file in place of the old one, readable by the player only: it holds passwords
        let path = folder.join(SETTINGS_FILE);
        let tmp = folder.join(format!("{SETTINGS_FILE}.tmp"));
        if std::fs::write(&tmp, json).is_ok() {
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let _ = std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o600));
            }
            if std::fs::rename(&tmp, &path).is_err() {
                let _ = std::fs::remove_file(&tmp);
            }
        }
    }

    fn crosshair_rgb(&self) -> u32 {
        parse_color(self.edits.get("cg_crosshair_color")).unwrap_or(0xFFFFFF)
    }
}

type Shared = Rc<RefCell<State>>;

fn int(value: &str) -> i32 {
    value.trim().parse::<f64>().map_or(0, |v| v.round() as i32)
}

fn strings(list: impl IntoIterator<Item = String>) -> ModelRc<SharedString> {
    ModelRc::new(VecModel::from(list.into_iter().map(SharedString::from).collect::<Vec<_>>()))
}

fn show_status(ui: &LauncherWindow, st: &State) {
    ui.set_game(st.game as i32);
    ui.set_sh_available(st.available(1));
    ui.set_bt_available(st.available(2));
    ui.set_status(st.status().into());
    let can_start = st.can_start();
    ui.set_can_start(can_start);
    ui.set_running(st.running);
    ui.set_can_connect(can_start && !st.slot().address.trim().is_empty());
}

fn show_fields(ui: &LauncherWindow, st: &State) {
    let slot = st.slot();
    ui.set_address(slot.address.as_str().into());
    ui.set_password(slot.password.as_str().into());
    ui.set_rcon(slot.rcon.as_str().into());
}

fn show_servers(ui: &LauncherWindow, st: &State) {
    let address = st.slot().address.trim();
    let listed = listed_servers(&st.settings, st.game);
    ui.set_can_bookmark(!address.is_empty());
    ui.set_bookmarked(!address.is_empty() && listed.iter().any(|l| same_address(&l.server.address, address)));
    let items: Vec<ServerItem> = listed
        .iter()
        .map(|l| ServerItem {
            name: l.server.label.as_str().into(),
            address: l.server.address.as_str().into(),
            selected: !address.is_empty() && same_address(&l.server.address, address),
            builtin: l.builtin,
        })
        .collect();
    ui.set_servers(ModelRc::new(VecModel::from(items)));
}

fn show_crosshair(ui: &LauncherWindow, st: &State) {
    let e = &st.edits;
    let (length, gap, thickness) = (e.get("cg_crosshair_length"), e.get("cg_crosshair_gap"), e.get("cg_crosshair_thickness"));
    ui.set_length(int(length).clamp(1, 128));
    ui.set_gap(int(gap).clamp(1, 64));
    ui.set_thickness(int(thickness).clamp(1, 16));

    let rgb = st.crosshair_rgb();
    let preset = COLOR_PRESETS.iter().position(|(_, c)| *c == rgb);
    let mut names: Vec<String> = COLOR_PRESETS.iter().map(|(n, _)| n.to_string()).collect();
    names.push(match preset {
        Some(_) => "Custom\u{2026}".into(),
        None => format!("#{}", color_hex(rgb)),
    });
    ui.set_color_names(strings(names));
    ui.set_color_index(preset.unwrap_or(COLOR_PRESETS.len()) as i32);
    ui.set_crosshair_color(slint::Color::from_rgb_u8((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8));
    let luma = ((rgb >> 16) * 3 + ((rgb >> 8) & 0xFF) * 6 + (rgb & 0xFF)) / 10;
    ui.set_crosshair_dark(luma < 64);

    // three preview pixels to a HUD pixel as long as it fits, else scaled down to fit
    let parse = |s: &str| s.trim().parse::<f64>().unwrap_or(0.0);
    let (arms, extent) = crosshair_boxes(parse(length), parse(gap), parse(thickness));
    let size = 96.0_f32; // ui/app.slint's Theme.preview
    let room = size - 8.0;
    let mut zoom = 3.0_f32;
    if extent as f32 * zoom > room {
        zoom = room / extent as f32;
        if zoom >= 1.0 {
            zoom = zoom.floor();
        }
    }
    let center = arms[2].w as f32 * zoom; // the center square is as wide as the arms are thick
    let (x0, y0) = (((size - center) / 2.0).floor(), ((size - center) / 2.0).floor());
    let boxes: Vec<ArmBox> = arms
        .iter()
        .map(|b| ArmBox {
            x: (x0 + b.x as f32 * zoom).round(),
            y: (y0 + b.y as f32 * zoom).round(),
            width: (b.w as f32 * zoom).round().max(1.0),
            height: (b.h as f32 * zoom).round().max(1.0),
        })
        .collect();
    ui.set_arms(ModelRc::new(VecModel::from(boxes)));
}

fn show_display(ui: &LauncherWindow, st: &mut State) {
    let (entries, index) = resolution_entries(&st.sizes, &st.edits);
    ui.set_resolutions(strings(entries.iter().map(ResolutionEntry::label)));
    ui.set_resolution_index(index as i32);
    st.resolution_entries = entries;
    ui.set_modes(strings(MODE_TITLES.iter().map(|s| s.to_string())));
    ui.set_mode_index(mode_from_cvars(st.edits.get("r_fullscreen"), st.edits.get("r_noborder")) as i32);
}

fn show_cvars(ui: &LauncherWindow, st: &mut State) {
    if ui.get_name().as_str() != st.edits.get("name") {
        ui.set_name(st.edits.get("name").into());
    }
    show_crosshair(ui, st);
    show_display(ui, st);
}

fn show_all(ui: &LauncherWindow, st: &mut State) {
    show_status(ui, st);
    show_fields(ui, st);
    show_servers(ui, st);
    show_cvars(ui, st);
}

fn show_dialog(ui: &LauncherWindow, st: &mut State, dialog: Dialog, title: &str, text: &str, ok: &str, cancel: &str) {
    st.dialog = dialog;
    ui.set_dialog_title(title.into());
    ui.set_dialog_text(text.into());
    ui.set_dialog_ok(ok.into());
    ui.set_dialog_cancel(cancel.into());
    ui.set_dialog_input(false);
    ui.set_dialog_visible(true);
}

fn show_input(ui: &LauncherWindow, st: &mut State, dialog: Dialog, title: &str, text: &str, value: &str, ok: &str) {
    show_dialog(ui, st, dialog, title, text, ok, "Cancel");
    ui.set_dialog_value(value.into());
    ui.set_dialog_input(true);
}

fn close_dialog(ui: &LauncherWindow, st: &mut State) {
    st.dialog = Dialog::None;
    ui.set_dialog_visible(false);
}

fn select_game(ui: &LauncherWindow, st: &mut State, game: usize) {
    if game == st.game || !st.available(game) {
        return;
    }
    st.game = game;
    st.reload_config();
    st.save();
    show_all(ui, st);
}

const FIND_GAME_TEXT: &str = "Choose the folder Medal of Honor: Allied Assault is installed in: the one with main/ \
     (Pak0.pk3 to Pak6.pk3), and mainta/ and maintt/ for the expansions.";

/// The prompt for the game folder, over the window
fn ask_for_folder(ui: &LauncherWindow, st: &mut State, title: &str, text: &str) {
    show_dialog(ui, st, Dialog::ChooseFolder, title, text, "Choose folder\u{2026}", "Not now");
}

/// The native folder dialog, modal to the window; the state isn't borrowed while it is open
fn choose_folder(ui: &LauncherWindow, state: &Shared) {
    let start_dir = {
        let st = state.borrow();
        st.game_folder().unwrap_or_else(|| st.own_folder.clone())
    };
    let Some(folder) = rfd::FileDialog::new()
        .set_title("Choose your MOHAA folder: the one with main/")
        .set_directory(start_dir)
        .set_parent(&ui.window().window_handle())
        .pick_folder()
    else {
        return;
    };
    let mut st = state.borrow_mut();
    if !platform::has_game_data(&folder) {
        let text = format!("{} has no main/ with Pak0.pk3.\n\n{FIND_GAME_TEXT}", folder.display());
        ask_for_folder(ui, &mut st, "No game there", &text);
        return;
    }
    let same = folder.canonicalize().ok() == st.own_folder.canonicalize().ok();
    st.chosen = if same { None } else { Some(folder) };
    st.load_settings();
    show_all(ui, &mut st);
}

fn start(ui: &LauncherWindow, state: &Shared, connect: bool) {
    let mut st = state.borrow_mut();
    if st.running || st.dialog != Dialog::None {
        return;
    }
    let Some(folder) = st.game_folder() else {
        ask_for_folder(ui, &mut st, "Where is MOHAA?", FIND_GAME_TEXT);
        return;
    };
    let Some(exe) = st.game_exe() else {
        let text = format!(
            "{} isn't beside the launcher or in {}: keep MoHAA Launcher with the files of the download it came in.",
            platform::GAME_EXE,
            folder.display()
        );
        show_dialog(ui, &mut st, Dialog::Message, "The game isn't here", &text, "OK", "");
        return;
    };
    let slot = st.slot().clone();
    let address = slot.address.trim().to_string();
    if connect && address.is_empty() {
        return;
    }

    // the settings an earlier start kept outside the game folder, carried over once; then what the config holds
    // now, so that only the changes made here are passed
    st.import_configs(&folder);
    st.reload_config();
    // everything the game writes (configs, saves, screenshots, demos, logs) goes into the game folder
    let mut launch = Launch { game: st.game, homepath: folder.display().to_string(), ..Default::default() };
    // the game finds its data beside itself; elsewhere it's told where
    if exe.parent().and_then(|p| p.canonicalize().ok()) != folder.canonicalize().ok() {
        launch.basepath = folder.display().to_string();
    }
    for (name, value) in st.edits.changes() {
        if name == "name" {
            let name_value = value.trim();
            if !name_value.is_empty() {
                launch.cvars.push((name, name_value.to_string()));
            }
        } else {
            launch.cvars.push((name, value));
        }
    }
    if connect {
        launch.address = address.clone();
        launch.password = slot.password.clone();
        launch.rcon = slot.rcon.clone();
        let game = st.game;
        st.settings.remember_server(game, &address, &slot.password, &slot.rcon);
    }
    st.save();

    let child = std::process::Command::new(&exe).args(build_args(&launch)).current_dir(&folder).spawn();
    match child {
        Ok(mut child) => {
            st.running = true;
            st.edits.applied();
            let weak = ui.as_weak();
            std::thread::spawn(move || {
                let _ = child.wait();
                let _ = weak.upgrade_in_event_loop(|ui| ui.invoke_game_exited());
            });
        }
        Err(error) => {
            let text = format!("{}: {error}", exe.display());
            show_dialog(ui, &mut st, Dialog::Message, "The game didn't start", &text, "OK", "");
        }
    }
    show_status(ui, &st);
    show_servers(ui, &st);
}

fn dialog_accepted(ui: &LauncherWindow, st: &mut State) {
    let dialog = st.dialog;
    let value = ui.get_dialog_value().to_string();
    close_dialog(ui, st);
    match dialog {
        Dialog::Rename(i) => {
            if let Some(server) = st.slot_mut().servers.get_mut(i) {
                let name = value.trim();
                server.label = if name.is_empty() { server.address.clone() } else { name.to_string() };
            }
            st.save();
            show_servers(ui, st);
        }
        Dialog::Remove(i) => {
            if i < st.slot().servers.len() {
                st.slot_mut().servers.remove(i);
            }
            st.save();
            show_servers(ui, st);
        }
        Dialog::CustomColor => match parse_color(&value) {
            Some(rgb) => {
                st.edits.set("cg_crosshair_color", &color_hex(rgb));
                show_crosshair(ui, st);
            }
            None => {
                let text = "Six hexadecimal digits, as #RRGGBB: #00FF7F is spring green.";
                show_input(ui, st, Dialog::CustomColor, "Crosshair color", text, &value, "Use");
            }
        },
        Dialog::ChooseFolder | Dialog::Message | Dialog::None => {}
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let ui = LauncherWindow::new()?;
    let state: Shared = Rc::new(RefCell::new(State::new()));
    {
        let mut st = state.borrow_mut();
        show_all(&ui, &mut st);
        // no game data beside the launcher, in the current folder or where it was last: ask where it is
        if st.game_folder().is_none() {
            ask_for_folder(&ui, &mut st, "Where is MOHAA?", FIND_GAME_TEXT);
        }
    }

    // Wires a callback to a closure that gets the window and the state
    macro_rules! on {
        ($setter:ident, |$ui:ident, $st:ident $(, $arg:ident)*| $body:block) => {{
            let weak = ui.as_weak();
            let state = state.clone();
            ui.$setter(move |$($arg),*| {
                let Some($ui) = weak.upgrade() else { return };
                #[allow(unused_mut)]
                let mut $st = state.borrow_mut();
                $body
            });
        }};
    }

    on!(on_select_game, |ui, st, game| { select_game(&ui, &mut st, game as usize) });
    on!(on_name_edited, |_ui, st, text| { st.edits.set("name", &text) });
    on!(on_address_edited, |ui, st, text| {
        st.slot_mut().address = text.to_string();
        show_servers(&ui, &st);
        show_status(&ui, &st);
    });
    on!(on_password_edited, |_ui, st, text| { st.slot_mut().password = text.to_string() });
    on!(on_rcon_edited, |_ui, st, text| { st.slot_mut().rcon = text.to_string() });
    on!(on_server_clicked, |ui, st, index| {
        if let Some(server) = listed_servers(&st.settings, st.game).get(index as usize).map(|l| l.server.clone()) {
            let slot = st.slot_mut();
            slot.address = server.address;
            slot.password = server.password;
            slot.rcon = server.rcon;
            show_fields(&ui, &st);
            show_servers(&ui, &st);
            show_status(&ui, &st);
        }
    });
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_server_connect(move |index| {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut st = state.borrow_mut();
                let listed = listed_servers(&st.settings, st.game);
                let Some(server) = listed.get(index as usize).map(|l| l.server.clone()) else { return };
                let slot = st.slot_mut();
                slot.address = server.address;
                slot.password = server.password;
                slot.rcon = server.rcon;
                show_fields(&ui, &st);
            }
            start(&ui, &state, true);
        });
    }
    // the server in the fields, kept in the list without joining it (or its passwords updated)
    on!(on_bookmark, |ui, st| {
        let slot = st.slot().clone();
        let address = slot.address.trim();
        if address.is_empty() {
            return;
        }
        let game = st.game;
        st.settings.remember_server(game, address, &slot.password, &slot.rcon);
        st.save();
        show_servers(&ui, &st);
    });
    // a built-in server has neither
    on!(on_server_rename, |ui, st, index| {
        let listed = listed_servers(&st.settings, st.game);
        if let Some(Listed { server, saved: Some(i), builtin: false }) = listed.get(index as usize).cloned() {
            let text = format!("A name for {}.", server.address);
            show_input(&ui, &mut st, Dialog::Rename(i), "Rename server", &text, &server.label, "Rename");
        }
    });
    on!(on_server_remove, |ui, st, index| {
        let listed = listed_servers(&st.settings, st.game);
        if let Some(Listed { server, saved: Some(i), builtin: false }) = listed.get(index as usize).cloned() {
            let text = format!("{} and its passwords leave the list.", server.label);
            show_dialog(&ui, &mut st, Dialog::Remove(i), "Remove server?", &text, "Remove", "Cancel");
        }
    });

    let crosshair_edit = |cvar: &'static str, max: i32| {
        let weak = ui.as_weak();
        let state = state.clone();
        move |value: i32| {
            let Some(ui) = weak.upgrade() else { return };
            let mut st = state.borrow_mut();
            st.edits.set(cvar, &value.clamp(1, max).to_string());
            show_crosshair(&ui, &st);
        }
    };
    ui.on_length_edited(crosshair_edit("cg_crosshair_length", 128));
    ui.on_gap_edited(crosshair_edit("cg_crosshair_gap", 64));
    ui.on_thickness_edited(crosshair_edit("cg_crosshair_thickness", 16));
    on!(on_color_selected, |ui, st, index| {
        if let Some((_, rgb)) = COLOR_PRESETS.get(index as usize) {
            st.edits.set("cg_crosshair_color", &color_hex(*rgb));
            show_crosshair(&ui, &st);
        } else {
            let value = format!("#{}", color_hex(st.crosshair_rgb()));
            show_input(&ui, &mut st, Dialog::CustomColor, "Crosshair color", "Any color as #RRGGBB.", &value, "Use");
        }
    });
    on!(on_reset_crosshair, |ui, st| {
        for (name, value) in CVAR_DEFAULTS.iter().filter(|(n, _)| n.starts_with("cg_crosshair_")) {
            st.edits.set(name, value);
        }
        show_crosshair(&ui, &st);
    });
    on!(on_resolution_selected, |ui, st, index| {
        if let Some(entry) = st.resolution_entries.get(index as usize).copied() {
            st.edits.set("r_mode", &entry.mode.to_string());
            if entry.mode == -1 {
                st.edits.set("r_customwidth", &entry.width.to_string());
                st.edits.set("r_customheight", &entry.height.to_string());
            }
            show_display(&ui, &mut st);
        }
    });
    on!(on_mode_selected, |ui, st, index| {
        mode_to_cvars(index as usize, &mut st.edits);
        show_display(&ui, &mut st);
    });
    ui.on_open_url(|url| platform::open_url(&url));
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_choose_folder(move || {
            if let Some(ui) = weak.upgrade() {
                choose_folder(&ui, &state);
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_single_player(move || {
            if let Some(ui) = weak.upgrade() {
                start(&ui, &state, false);
            }
        });
    }
    {
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_connect(move || {
            if let Some(ui) = weak.upgrade() {
                start(&ui, &state, true);
            }
        });
    }
    on!(on_dialog_cancelled, |ui, st| {
        let dialog = st.dialog;
        close_dialog(&ui, &mut st);
        if dialog == Dialog::CustomColor {
            show_crosshair(&ui, &st); // the color menu shows the color it had
        }
    });
    {
        // choosing the folder opens a native dialog: not while the state is borrowed
        let weak = ui.as_weak();
        let state = state.clone();
        ui.on_dialog_accepted(move || {
            let Some(ui) = weak.upgrade() else { return };
            if state.borrow().dialog == Dialog::ChooseFolder {
                close_dialog(&ui, &mut state.borrow_mut());
                choose_folder(&ui, &state);
            } else {
                dialog_accepted(&ui, &mut state.borrow_mut());
            }
        });
    }
    on!(on_game_exited, |ui, st| {
        st.running = false;
        // the game saved its config: show what it holds now
        st.reload_config();
        show_status(&ui, &st);
        show_cvars(&ui, &mut st);
    });

    // the config changed (the game saved it, or another program), the game folder's contents changed: show it
    let timer = slint::Timer::default();
    {
        let weak = ui.as_weak();
        let state = state.clone();
        timer.start(slint::TimerMode::Repeated, Duration::from_millis(1500), move || {
            let Some(ui) = weak.upgrade() else { return };
            let Ok(mut st) = state.try_borrow_mut() else { return };
            if !st.running && st.config_changed() {
                st.reload_config();
                show_cvars(&ui, &mut st);
            }
            show_status(&ui, &st);
        });
    }

    ui.show()?;
    dark_title_bar(&ui);
    let result = slint::run_event_loop();
    let _ = ui.hide();
    state.borrow_mut().save();
    result
}

/// The window's title bar and frame dark too, whatever the system's theme (Windows' title bar, the X11 window
/// manager's and Wayland's decorations), also after the system's theme changes
fn dark_title_bar(ui: &LauncherWindow) {
    use slint::winit_030::{winit, WinitWindowAccessor};
    let dark = || Some(winit::window::Theme::Dark);
    ui.window().with_winit_window(|window| window.set_theme(dark()));
    ui.window().on_winit_window_event(move |window, event| {
        if let winit::event::WindowEvent::ThemeChanged(theme) = event {
            if *theme != winit::window::Theme::Dark {
                window.with_winit_window(|w| w.set_theme(dark()));
            }
        }
        slint::winit_030::EventResult::Propagate
    });
}
