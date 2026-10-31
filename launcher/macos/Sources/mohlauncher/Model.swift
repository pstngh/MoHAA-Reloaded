// The launcher's state: the game folder, the player's servers, the game's config and starting the game. It behaves
// as the Windows and Linux launcher (launcher/desktop/src/main.rs) does; LauncherCore is the logic they share.
import AppKit
import LauncherCore
import SwiftUI

struct ColorPreset: Identifiable {
    let name: String
    let rgb: UInt32
    var id: UInt32 { rgb }
}

/// An entry of the resolution menu: the desktop's (r_mode -2), a size (r_mode -1 with r_customwidth and
/// r_customheight), or a numbered mode a config holds
struct ResolutionEntry: Hashable, Identifiable {
    var mode: Int
    var width = 0
    var height = 0
    var id: Self { self }

    var label: String {
        switch mode {
        case -2: return "Desktop resolution"
        case -1: return "\(width) × \(height)"
        default: return "r_mode \(mode)"
        }
    }
}

@MainActor
final class LauncherModel: ObservableObject {
    nonisolated static let cvarDefaults: [(String, String)] = [
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
        ("r_borderlessWindow", "0"),
    ]

    nonisolated static let colorPresets = [
        ColorPreset(name: "White", rgb: 0xFFFFFF), ColorPreset(name: "Green", rgb: 0x00FF00),
        ColorPreset(name: "Yellow", rgb: 0xFFFF00), ColorPreset(name: "Cyan", rgb: 0x00FFFF),
        ColorPreset(name: "Red", rgb: 0xFF0000), ColorPreset(name: "Magenta", rgb: 0xFF00FF),
        ColorPreset(name: "Black", rgb: 0x000000),
    ]

    /// The launcher's settings, in the game folder
    nonisolated static let settingsFile = "mohlauncher.json"
    /// Where the Metal port kept its configs when the game folder couldn't be written: only read, to carry them over
    nonisolated static let supportFolder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("openmohaa", isDirectory: true)
    /// The folder the launcher is in: the game folder, if the game's data is beside it
    nonisolated static let ownFolder = Bundle.main.bundleURL.deletingLastPathComponent()
    nonisolated static let githubURL = URL(string: "https://github.com/pstngh/MoHAA-Reloaded")!
    nonisolated static let discordURL = URL(string: "https://dsc.gg/ForteFFA")!

    @Published private(set) var settings = LauncherSettings()
    @Published private(set) var game = Game.alliedAssault
    @Published var address = ""
    @Published var password = ""
    @Published var rcon = ""
    @Published var edits = CvarEdits(defaults: LauncherModel.cvarDefaults)
    /// The game folder (see findGame) and the game, as last found
    @Published private(set) var folder: URL?
    @Published private(set) var gameAppURL: URL?
    @Published private(set) var gameApp: NSRunningApplication?
    @Published private(set) var starting = false
    /// The last save failed: the game folder can't be written
    @Published private(set) var unwritable = false
    @Published var askForFolder = false
    @Published var alertMessage: String?
    @Published var renaming: Int?
    @Published var renameText = ""
    @Published var removing: Int?

    /// The game folder chosen with the folder dialog, for this session: it's kept nowhere outside the game folder
    private var chosen: URL?
    /// The config last read, and when it was written: read again when it changes
    private var configSeen: (URL?, Date?) = (nil, nil)
    private var ticker: Timer?
    let displaySizes: [Resolution]

    init() {
        displaySizes = LauncherModel.findDisplaySizes()
        findGame()
        loadSettings()
        askForFolder = folder == nil
        // the game folder or its contents changed, or its config (the game saved it): show it
        ticker = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
    }

    // MARK: The game folder

    /// Looks for the game folder again: the first with the game's data (main/Pak0.pk3) of the one chosen in this
    /// session and the launcher's own; everything the launcher and the game write goes there. The game is beside
    /// the launcher (the download's folder), else in the game folder. True if the folder changed.
    @discardableResult
    func findGame() -> Bool {
        let found = [chosen, LauncherModel.ownFolder].compactMap { $0 }.first(where: hasGameData)
        gameAppURL = [LauncherModel.ownFolder, found].compactMap { $0 }
            .map { $0.appendingPathComponent("openmohaa.app", isDirectory: true) }
            .first { FileManager.default.fileExists(atPath: $0.path) }
        let changed = found?.standardizedFileURL != folder?.standardizedFileURL
        folder = found
        return changed
    }

    /// The game folder's mohlauncher.json (none yet: the defaults), and its config
    func loadSettings() {
        unwritable = false
        let data = folder.flatMap { try? Data(contentsOf: $0.appendingPathComponent(LauncherModel.settingsFile)) }
        settings = LauncherSettings.from(json: data)
        game = available(settings.game) ? settings.game : .alliedAssault
        loadFields()
        reloadConfig()
    }

    /// An expansion without its data can't start
    func available(_ g: Game) -> Bool {
        g == .alliedAssault || folder.map { dataFolder($0, g.dataDir) != nil } ?? true
    }

    var running: Bool { gameApp.map { !$0.isTerminated } ?? false }
    var canStart: Bool { folder != nil && gameAppURL != nil && !running && !starting }
    var canConnect: Bool { canStart && !trimmed(address).isEmpty }
    /// Something stands in the way: no game, or a game folder that can't be written
    var problem: Bool { folder == nil || gameAppURL == nil || unwritable }

    /// The problem first: the line may be cut off at the end
    var status: String {
        guard let folder else { return "No game found: choose your MOHAA folder." }
        let path = (folder.path as NSString).abbreviatingWithTildeInPath
        if gameAppURL == nil { return "No openmohaa.app beside the launcher or in \(path)" }
        if unwritable { return "Can't write to the game folder: \(path)" }
        if running { return "\(game.title) is running." }
        return path
    }

    func chooseGameFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = folder ?? LauncherModel.ownFolder
        panel.message = "Choose your MOHAA folder: the one with main/ (Pak0.pk3 to Pak6.pk3)."
        panel.prompt = "Choose"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard hasGameData(url) else {
            alertMessage = "\(url.path) has no main/ with Pak0.pk3."
            return
        }
        chosen = url.standardizedFileURL == LauncherModel.ownFolder.standardizedFileURL ? nil : url
        findGame()
        loadSettings()
    }

    func icon(for game: Game) -> NSImage {
        NSImage(named: ["game-aa", "game-sh", "game-bt"][game.rawValue]) ?? NSImage()
    }

    /// The GitHub and Discord logos, drawn in the text's color
    func logo(_ name: String) -> NSImage {
        let image = Bundle.main.url(forResource: name, withExtension: "svg").flatMap { NSImage(contentsOf: $0) } ?? NSImage()
        image.isTemplate = true
        return image
    }

    // MARK: Games and servers

    var listed: [Listed] { listedServers(settings, game) }

    var bookmarked: Bool {
        !trimmed(address).isEmpty && listed.contains { sameAddress($0.server.address, address) }
    }

    func isSelected(_ item: Listed) -> Bool {
        !trimmed(address).isEmpty && sameAddress(item.server.address, address)
    }

    func select(_ newGame: Game) {
        guard newGame != game, available(newGame) else { return }
        storeFields()
        game = newGame
        loadFields()
        reloadConfig()
        save()
    }

    private func storeFields() {
        settings.slots[game.rawValue].address = trimmed(address)
        settings.slots[game.rawValue].password = password
        settings.slots[game.rawValue].rcon = rcon
        settings.game = game
    }

    private func loadFields() {
        let slot = settings.slots[game.rawValue]
        address = slot.address
        password = slot.password
        rcon = slot.rcon
    }

    func fill(_ item: Listed) {
        address = item.server.address
        password = item.server.password
        rcon = item.server.rcon
    }

    func connect(_ item: Listed) {
        fill(item)
        start(connect: true)
    }

    /// The server in the fields, kept in the list without joining it (or its passwords updated)
    func bookmark() {
        let a = trimmed(address)
        guard !a.isEmpty else { return }
        settings.remember(game, address: a, password: password, rcon: rcon)
        save()
    }

    // a built-in server has neither
    func beginRename(_ item: Listed) {
        guard !item.builtin, let k = item.saved else { return }
        renameText = item.server.label
        renaming = k
    }

    func commitRename() {
        defer { renaming = nil }
        guard let k = renaming, settings.slots[game.rawValue].servers.indices.contains(k) else { return }
        let label = trimmed(renameText)
        let server = settings.slots[game.rawValue].servers[k]
        settings.slots[game.rawValue].servers[k].label = label.isEmpty ? server.address : label
        save()
    }

    func beginRemove(_ item: Listed) {
        guard !item.builtin, let k = item.saved else { return }
        removing = k
    }

    var removingLabel: String {
        guard let k = removing, settings.slots[game.rawValue].servers.indices.contains(k) else { return "" }
        return settings.slots[game.rawValue].servers[k].label
    }

    func commitRemove() {
        defer { removing = nil }
        guard let k = removing, settings.slots[game.rawValue].servers.indices.contains(k) else { return }
        settings.slots[game.rawValue].servers.remove(at: k)
        save()
    }

    // MARK: The game's settings

    /// The game's config: the game folder's (the game is started with its home there), else the one the Metal port
    /// kept in Application Support when the game folder couldn't be written, which the next start copies over
    private var configPath: URL? {
        findConfig(homes: (folder.map { [$0] } ?? []) + [LauncherModel.supportFolder], game: game)
    }

    private func modified(_ url: URL?) -> Date? {
        url.flatMap { try? FileManager.default.attributesOfItem(atPath: $0.path)[.modificationDate] as? Date }
    }

    func reloadConfig() {
        let path = configPath
        edits.load(path.flatMap(readConfig) ?? [:])
        configSeen = (path, modified(path))
    }

    private var configChanged: Bool {
        let path = configPath
        return path != configSeen.0 || modified(path) != configSeen.1
    }

    /// The configs kept outside the game folder before, copied into it once (for the data folders it has), so that
    /// the settings carry over; nothing is written outside the game folder
    private func importConfigs(into folder: URL) {
        let fm = FileManager.default
        for g in Game.allCases {
            guard let dir = dataFolder(folder, g.dataDir) else { continue }
            let target = dir.appendingPathComponent("configs/omconfig.cfg")
            let old = LauncherModel.supportFolder.appendingPathComponent("\(g.dataDir)/configs/omconfig.cfg")
            guard !fm.fileExists(atPath: target.path), fm.fileExists(atPath: old.path) else { continue }
            try? fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.copyItem(at: old, to: target)
        }
    }

    var name: Binding<String> {
        Binding(get: { self.edits.get("name") }, set: { self.edits.set("name", $0) })
    }

    func number(_ cvar: String, _ range: ClosedRange<Int>) -> Binding<Int> {
        Binding(get: {
            let value = Int((Double(self.edits.get(cvar)) ?? Double(range.lowerBound)).rounded())
            return min(max(value, range.lowerBound), range.upperBound)
        }, set: {
            self.edits.set(cvar, String(min(max($0, range.lowerBound), range.upperBound)))
        })
    }

    var crosshairRGB: UInt32 { parseColor(edits.get("cg_crosshair_color")) ?? 0xFFFFFF }

    /// The color menu: a preset's color, or -1 for another
    var colorPreset: Binding<Int64> {
        Binding(get: {
            LauncherModel.colorPresets.contains { $0.rgb == self.crosshairRGB } ? Int64(self.crosshairRGB) : -1
        }, set: {
            if $0 >= 0 { self.edits.set("cg_crosshair_color", colorHex(UInt32($0))) }
        })
    }

    var color: Binding<Color> {
        Binding(get: {
            let rgb = self.crosshairRGB
            return Color(.sRGB, red: Double((rgb >> 16) & 0xFF) / 255, green: Double((rgb >> 8) & 0xFF) / 255,
                         blue: Double(rgb & 0xFF) / 255)
        }, set: { color in
            guard let c = NSColor(color).usingColorSpace(.sRGB) else { return }
            func byte(_ v: CGFloat) -> UInt32 { UInt32(min(max((v * 255).rounded(), 0), 255)) }
            self.edits.set("cg_crosshair_color", colorHex(byte(c.redComponent) << 16 | byte(c.greenComponent) << 8
                                                          | byte(c.blueComponent)))
        })
    }

    func resetCrosshair() {
        for (name, value) in LauncherModel.cvarDefaults where name.hasPrefix("cg_crosshair_") {
            edits.set(name, value)
        }
    }

    var displayMode: Binding<DisplayMode> {
        Binding(get: {
            DisplayMode(fullscreen: self.edits.get("r_fullscreen"), noborder: self.edits.get("r_noborder"),
                        borderlessWindow: self.edits.get("r_borderlessWindow"))
        }, set: { $0.apply(to: &self.edits) })
    }

    var resolutionEntries: [ResolutionEntry] {
        var entries = [ResolutionEntry(mode: -2)]
            + displaySizes.map { ResolutionEntry(mode: -1, width: $0.width, height: $0.height) }
        let current = currentResolution
        if !entries.contains(current) {
            // the config's own size where it belongs in the list, or its numbered mode at the end
            if current.mode == -1, let k = entries.dropFirst().firstIndex(where: {
                $0.width < current.width || ($0.width == current.width && $0.height < current.height)
            }) {
                entries.insert(current, at: k)
            } else {
                entries.append(current)
            }
        }
        return entries
    }

    private var currentResolution: ResolutionEntry {
        let mode = Int(edits.get("r_mode")) ?? -2
        if mode == -1 {
            return ResolutionEntry(mode: -1, width: Int(edits.get("r_customwidth")) ?? 0,
                                   height: Int(edits.get("r_customheight")) ?? 0)
        }
        return ResolutionEntry(mode: mode < -1 ? -2 : mode)
    }

    var resolution: Binding<ResolutionEntry> {
        Binding(get: { self.currentResolution }, set: { entry in
            self.edits.set("r_mode", String(entry.mode))
            if entry.mode == -1 {
                self.edits.set("r_customwidth", String(entry.width))
                self.edits.set("r_customheight", String(entry.height))
            }
        })
    }

    private static func findDisplaySizes() -> [Resolution] {
        let display = CGMainDisplayID()
        var desktop = Resolution(0, 0)
        if let mode = CGDisplayCopyDisplayMode(display) {
            desktop = Resolution(mode.width, mode.height)
        }
        let options = [kCGDisplayShowDuplicateLowResolutionModes as String: true] as CFDictionary
        let modes = (CGDisplayCopyAllDisplayModes(display, options) as? [CGDisplayMode]) ?? []
        return resolutionList(displayModes: modes.map { Resolution($0.width, $0.height) }, desktop: desktop)
    }

    // MARK: Starting the game

    func start(connect: Bool) {
        guard !running, !starting else { return }
        findGame()
        guard let folder else {
            askForFolder = true
            return
        }
        guard let appURL = gameAppURL else {
            alertMessage = "openmohaa.app isn't beside the launcher or in \(folder.path): keep MoHAA Launcher with the files of the download it came in."
            return
        }
        storeFields()
        let slot = settings.slots[game.rawValue]
        if connect && slot.address.isEmpty { return }

        // one game at a time: one started some other way can't take these settings
        let bundleID = Bundle(url: appURL)?.bundleIdentifier ?? "io.github.pstngh.openmohaa-metal"
        if let other = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).first {
            other.activate()
            alertMessage = "The game is already running. Quit it to start it from here with these settings."
            return
        }

        // the settings kept outside the game folder before, carried over once; then what the config holds now, so
        // that only the changes made here are passed
        importConfigs(into: folder)
        reloadConfig()
        var launch = Launch()
        launch.game = game
        // everything the game writes (configs, saves, screenshots, demos, logs) goes into the game folder
        launch.homepath = folder.path
        // the game finds its data beside itself; elsewhere it's told where
        if appURL.deletingLastPathComponent().standardizedFileURL != folder.standardizedFileURL {
            launch.basepath = folder.path
        }
        let changes = edits.changes
        for change in changes {
            if change.0 == "name" {
                let name = trimmed(change.1)
                if !name.isEmpty { launch.cvars.append(("name", name)) }
            } else {
                launch.cvars.append(change)
            }
        }
        if connect {
            launch.address = slot.address
            launch.password = slot.password
            launch.rcon = slot.rcon
            settings.remember(game, address: slot.address, password: slot.password, rcon: slot.rcon)
        }
        save()

        let configuration = NSWorkspace.OpenConfiguration()
        configuration.arguments = buildArgs(launch)
        configuration.activates = true
        starting = true
        // through Launch Services: the game is its own app, as when it's opened in the Finder
        NSWorkspace.shared.openApplication(at: appURL, configuration: configuration) { app, error in
            let message = error?.localizedDescription
            Task { @MainActor in
                self.starting = false
                if let message {
                    self.alertMessage = "The game didn't start: \(message)"
                } else {
                    self.gameApp = app
                    self.edits.applied()
                }
            }
        }
    }

    /// An app quit: if it was the game, read the config it saved
    func applicationTerminated(_ app: NSRunningApplication?) {
        guard let app, let gameApp, app.processIdentifier == gameApp.processIdentifier else { return }
        self.gameApp = nil
        reloadConfig()
    }

    /// The game folder found again (its own settings if it changed), or its config read again if it changed
    func tick() {
        if running { return }
        gameApp = nil
        if findGame() {
            loadSettings()
            if folder != nil { askForFolder = false } // the question it asked is answered
        } else if configChanged {
            reloadConfig()
        }
    }

    /// mohlauncher.json in the game folder, and nowhere else (with no game folder, nowhere)
    func save() {
        storeFields()
        guard let folder else { return }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(settings) else { return }
        let url = folder.appendingPathComponent(LauncherModel.settingsFile)
        do {
            try data.write(to: url, options: .atomic)
            // it holds passwords
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            unwritable = false
        } catch {
            unwritable = true
        }
    }
}
