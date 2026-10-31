// The launcher's state: the game folder, the player's servers, the game's config and starting the game.
import AppKit
import LauncherCore
import SwiftUI

struct Server: Codable, Identifiable, Hashable {
    var id = UUID()
    /// what it's called in the list (its address until it's renamed); the player's own name is the game's
    var label: String
    var address: String
    var password = ""
    var rcon = ""

    enum CodingKeys: String, CodingKey { case label, address, password, rcon }

    init(label: String, address: String, password: String = "", rcon: String = "") {
        self.label = label
        self.address = address
        self.password = password
        self.rcon = rcon
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? address
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
    }
}

/// What the launcher keeps of its own for each game: the server fields and the list
struct GameSlot: Codable {
    var address = ""
    var password = ""
    var rcon = ""
    var servers: [Server] = []

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
        servers = try c.decodeIfPresent([Server].self, forKey: .servers) ?? []
    }
}

struct LauncherSettings: Codable {
    var game = Game.alliedAssault
    /// Chosen with "Game Folder…"; nil for the folder the launcher is in
    var gameFolder: String?
    var slots = [GameSlot](repeating: GameSlot(), count: Game.allCases.count)

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        game = (try? c.decodeIfPresent(Game.self, forKey: .game)) ?? .alliedAssault
        gameFolder = try c.decodeIfPresent(String.self, forKey: .gameFolder)
        slots = try c.decodeIfPresent([GameSlot].self, forKey: .slots) ?? []
        while slots.count < Game.allCases.count { slots.append(GameSlot()) }
    }
}

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

    nonisolated static let supportFolder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("openmohaa", isDirectory: true)
    /// mohlauncher.json beside the app (portable: it goes where the launcher goes), else, where that folder can't be
    /// written (a translocated or read-only copy), in Application Support
    nonisolated static let settingsURLs = [
        ownFolder.appendingPathComponent("mohlauncher.json"),
        supportFolder.appendingPathComponent("mohlauncher.json"),
    ]
    /// The folder the launcher is in: the game folder, if openmohaa.app is beside it
    nonisolated static let ownFolder = Bundle.main.bundleURL.deletingLastPathComponent()

    @Published private(set) var settings = LauncherSettings()
    @Published private(set) var game = Game.alliedAssault
    @Published var address = ""
    @Published var password = ""
    @Published var rcon = ""
    @Published var edits = CvarEdits(defaults: LauncherModel.cvarDefaults)
    @Published private(set) var gameApp: NSRunningApplication?
    @Published private(set) var starting = false
    @Published var alertMessage: String?
    @Published var renamingID: Server.ID?
    @Published var renameText = ""

    let displaySizes: [Resolution]

    init() {
        displaySizes = LauncherModel.findDisplaySizes()
        if let data = LauncherModel.settingsURLs.lazy.compactMap({ try? Data(contentsOf: $0) }).first,
           let saved = try? JSONDecoder().decode(LauncherSettings.self, from: data) {
            settings = saved
        }
        var first = settings.game
        if !available(first) { first = .alliedAssault } // an expansion picked last time whose data has gone
        game = first
        loadFields()
        reloadConfig()
    }

    // MARK: The game folder

    var gameFolder: URL {
        settings.gameFolder.map { URL(fileURLWithPath: $0, isDirectory: true) } ?? LauncherModel.ownFolder
    }

    var gameAppURL: URL { gameFolder.appendingPathComponent("openmohaa.app", isDirectory: true) }

    var gameFound: Bool { FileManager.default.fileExists(atPath: gameAppURL.path) }

    func hasData(_ game: Game) -> Bool {
        let fm = FileManager.default
        if fm.fileExists(atPath: gameFolder.appendingPathComponent(game.dataDir).path) { return true }
        // a case-sensitive volume, and a retail folder in another case (MAIN, MainTA)
        let names = (try? fm.contentsOfDirectory(atPath: gameFolder.path)) ?? []
        return names.contains { $0.caseInsensitiveCompare(game.dataDir) == .orderedSame }
    }

    /// An expansion without its data can't start; without main/ nobody knows, so all stay open
    func available(_ game: Game) -> Bool {
        game == .alliedAssault || !gameFound || !hasData(.alliedAssault) || hasData(game)
    }

    /// Where the game reads its config: the game folder when it can write there (else Application Support), the
    /// other one behind it
    var configHomes: [URL] {
        let support = LauncherModel.supportFolder
        return FileManager.default.isWritableFile(atPath: gameFolder.path) ? [gameFolder, support] : [support, gameFolder]
    }

    var running: Bool { gameApp.map { !$0.isTerminated } ?? false }
    var canStart: Bool { gameFound && !running && !starting }
    var canConnect: Bool { canStart && !trimmed(address).isEmpty }

    var status: String {
        let folder = (gameFolder.path as NSString).abbreviatingWithTildeInPath
        if !gameFound { return "openmohaa.app isn't in \(folder): choose the game folder." }
        if !hasData(.alliedAssault) { return "No game data (main/ with Pak0.pk3 to Pak6.pk3) in \(folder)." }
        if running { return "\(game.title) is running." }
        return "Game folder: \(folder)"
    }

    func icon(for game: Game) -> NSImage {
        NSImage(named: ["game-aa", "game-sh", "game-bt"][game.rawValue]) ?? NSImage()
    }

    func chooseGameFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = gameFolder
        panel.message = "Choose your MOHAA folder: the one with openmohaa.app and main/."
        panel.prompt = "Choose"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let own = LauncherModel.ownFolder.standardizedFileURL.path
        settings.gameFolder = url.standardizedFileURL.path == own ? nil : url.path
        if !gameFound { alertMessage = "openmohaa.app isn't in \(url.path)." }
        if !available(game) {
            select(.alliedAssault)
        } else {
            reloadConfig()
        }
        save()
    }

    // MARK: Games and servers

    var servers: [Server] { settings.slots[game.rawValue].servers }

    var matchingServer: Server? {
        guard !trimmed(address).isEmpty else { return nil }
        return servers.first { sameAddress($0.address, address) }
    }

    /// The list's selection is the server whose address is in the field; picking one fills the fields
    var selection: Binding<Server.ID?> {
        Binding(get: { self.matchingServer?.id }, set: { id in
            if let server = self.servers.first(where: { $0.id == id }) { self.fill(server) }
        })
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

    func fill(_ server: Server) {
        address = server.address
        password = server.password
        rcon = server.rcon
    }

    func connect(to id: Server.ID) {
        guard let server = servers.first(where: { $0.id == id }) else { return }
        fill(server)
        start(connect: true)
    }

    func beginRename(_ id: Server.ID) {
        guard let server = servers.first(where: { $0.id == id }) else { return }
        renameText = server.label
        renamingID = id
    }

    func commitRename() {
        defer { renamingID = nil }
        guard let id = renamingID, let k = servers.firstIndex(where: { $0.id == id }) else { return }
        let name = trimmed(renameText)
        settings.slots[game.rawValue].servers[k].label = name.isEmpty ? servers[k].address : name
        save()
    }

    func remove(_ id: Server.ID) {
        settings.slots[game.rawValue].servers.removeAll { $0.id == id }
        save()
    }

    // MARK: The game's settings

    func reloadConfig() {
        let url = findConfig(homes: configHomes, game: game)
        edits.load(url.flatMap(readConfig) ?? [:])
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
        var entries = [ResolutionEntry(mode: -2)] + displaySizes.map { ResolutionEntry(mode: -1, width: $0.width, height: $0.height) }
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
        guard gameFound else {
            alertMessage = "openmohaa.app isn't in \(gameFolder.path).\n\nCopy mohlauncher.app next to openmohaa.app, or choose the game folder."
            return
        }
        storeFields()
        if connect && trimmed(address).isEmpty { return }

        // one game at a time: one started some other way can't take these settings
        let bundleID = Bundle(url: gameAppURL)?.bundleIdentifier ?? "io.github.pstngh.openmohaa-metal"
        if let other = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).first {
            other.activate()
            alertMessage = "The game is already running. Quit it to start it from here with these settings."
            return
        }

        // what the config holds now, so that only the changes made here are passed
        reloadConfig()
        var launch = Launch()
        launch.game = game
        launch.cvars = edits.changes.compactMap { (change: (String, String)) -> (String, String)? in
            guard change.0 == "name" else { return change }
            let name = trimmed(change.1)
            return name.isEmpty ? nil : ("name", name)
        }
        if connect {
            let slot = settings.slots[game.rawValue]
            launch.address = slot.address
            launch.password = slot.password
            launch.rcon = slot.rcon
            // keep the server in the list, with the passwords used
            if let k = servers.firstIndex(where: { sameAddress($0.address, slot.address) }) {
                settings.slots[game.rawValue].servers[k].address = slot.address
                settings.slots[game.rawValue].servers[k].password = slot.password
                settings.slots[game.rawValue].servers[k].rcon = slot.rcon
            } else {
                settings.slots[game.rawValue].servers.append(
                    Server(label: slot.address, address: slot.address, password: slot.password, rcon: slot.rcon))
            }
        }
        save()

        let configuration = NSWorkspace.OpenConfiguration()
        configuration.arguments = buildArgs(launch)
        configuration.activates = true
        starting = true
        // through Launch Services: the game is its own app, as when it's opened in the Finder
        NSWorkspace.shared.openApplication(at: gameAppURL, configuration: configuration) { app, error in
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

    /// Back from the game or another app: the config or the game folder may have changed
    func becameActive() {
        if !running {
            gameApp = nil
            reloadConfig()
        }
        objectWillChange.send()
    }

    func save() {
        storeFields()
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(settings) else { return }
        let fm = FileManager.default
        for (k, url) in LauncherModel.settingsURLs.enumerated() {
            if k > 0 { try? fm.createDirectory(at: LauncherModel.supportFolder, withIntermediateDirectories: true) }
            if (try? data.write(to: url, options: .atomic)) != nil {
                // it holds passwords
                try? fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
                return
            }
        }
    }
}
