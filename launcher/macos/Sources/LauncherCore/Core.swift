// The launcher's logic, without UI, as launcher/desktop/src/core.cpp has it for Windows and Linux: the game's
// config file, the settings the launcher edits, the game's command line and the crosshair's geometry.
import Foundation

public enum Game: Int, CaseIterable, Identifiable, Codable {
    case alliedAssault = 0, spearhead, breakthrough

    public var id: Int { rawValue }
    public var title: String { ["Allied Assault", "Spearhead", "Breakthrough"][rawValue] }
    /// The game's data folder, which is also its folder for configs
    public var dataDir: String { ["main", "mainta", "maintt"][rawValue] }
}

public typealias Cvars = [String: String] // lowercase name -> value

/// The values a config's `seta`/`set` lines leave, as the game reads them: quotes, `;` and `//` as the game's own
/// command parser handles them. Names are lowercased (cvar names don't depend on case).
public func parseConfig(_ text: String) -> Cvars {
    var out = Cvars()
    var tokens: [String] = []
    let b = Array(text.utf8)
    let n = b.count
    var i = 0
    var starComment = false

    func takeCommand() {
        defer { tokens.removeAll() }
        guard tokens.count >= 3 else { return }
        let cmd = tokens[0].lowercased()
        guard ["set", "seta", "sets", "setu"].contains(cmd) else { return }
        out[tokens[1].lowercased()] = tokens[2...].joined(separator: " ")
    }
    func at(_ k: Int) -> UInt8 { k < n ? b[k] : 0 }
    let quote = UInt8(ascii: "\""), slash = UInt8(ascii: "/"), star = UInt8(ascii: "*")
    let newline = UInt8(ascii: "\n"), cr = UInt8(ascii: "\r"), semicolon = UInt8(ascii: ";"), space = UInt8(ascii: " ")

    while i < n {
        let c = b[i]
        if starComment {
            if c == star && at(i + 1) == slash {
                starComment = false
                i += 2
            } else {
                i += 1
            }
            continue
        }
        if c == newline || c == cr || c == semicolon {
            // the end of a command (the game's command buffer splits there outside quotes)
            takeCommand()
            i += 1
            continue
        }
        if c <= space {
            i += 1
            continue
        }
        if c == slash && at(i + 1) == slash {
            while i < n && b[i] != newline && b[i] != cr { i += 1 }
            continue
        }
        if c == slash && at(i + 1) == star {
            starComment = true
            i += 2
            continue
        }
        var token: [UInt8] = []
        if c == quote {
            // a quoted token runs to the closing quote or the end of the line
            i += 1
            while i < n && b[i] != quote && b[i] != newline && b[i] != cr {
                token.append(b[i])
                i += 1
            }
            if i < n && b[i] == quote { i += 1 }
        } else {
            while i < n && b[i] > space && b[i] != quote && b[i] != semicolon
                && !(b[i] == slash && (at(i + 1) == slash || at(i + 1) == star)) {
                token.append(b[i])
                i += 1
            }
        }
        tokens.append(String(decoding: token, as: UTF8.self))
    }
    takeCommand()
    return out
}

public func readConfig(_ url: URL) -> Cvars? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return parseConfig(String(decoding: data, as: UTF8.self))
}

/// The first config that exists: for the game's own data folder, then for Allied Assault's (an expansion's first
/// start inherits it), in each home folder in turn, `omconfig.cfg` before the original game's `unnamedsoldier.cfg`.
public func findConfig(homes: [URL], game: Game) -> URL? {
    var dirs = [game.dataDir]
    if game != .alliedAssault { dirs.append(Game.alliedAssault.dataDir) }
    for name in ["omconfig.cfg", "unnamedsoldier.cfg"] {
        for dir in dirs {
            for home in homes {
                let url = home.appendingPathComponent(dir).appendingPathComponent("configs").appendingPathComponent(name)
                var isDir: ObjCBool = false
                if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && !isDir.boolValue {
                    return url
                }
            }
        }
    }
    return nil
}

/// The settings the launcher edits. Each holds the value the game's config had when it was last read and the one
/// shown in the launcher; only the ones the player changed in the launcher go to the game, so a change made in the
/// game (its menus, the console) is never undone by starting it from here.
public struct CvarEdits: Equatable {
    struct Entry: Equatable {
        let name: String
        let def: String
        var value: String
        var loaded: String
    }
    private var entries: [Entry]

    public init(defaults: [(String, String)]) {
        entries = defaults.map { Entry(name: $0.0.lowercased(), def: $0.1, value: $0.1, loaded: $0.1) }
    }

    public func get(_ name: String) -> String {
        entries.first { $0.name == name.lowercased() }?.value ?? ""
    }

    public mutating func set(_ name: String, _ value: String) {
        if let k = entries.firstIndex(where: { $0.name == name.lowercased() }) {
            entries[k].value = value
        }
    }

    public func changed(_ name: String) -> Bool {
        guard let e = entries.first(where: { $0.name == name.lowercased() }) else { return false }
        return e.value != e.loaded
    }

    /// Takes the config's values (the defaults for the ones it doesn't have); a value changed in the launcher stays
    public mutating func load(_ config: Cvars) {
        for k in entries.indices {
            let changed = entries[k].value != entries[k].loaded
            entries[k].loaded = config[entries[k].name] ?? entries[k].def
            if !changed { entries[k].value = entries[k].loaded }
        }
    }

    /// The changed values, in the order of the defaults
    public var changes: [(String, String)] {
        entries.filter { $0.value != $0.loaded }.map { ($0.name, $0.value) }
    }

    /// The game was started with the changes: they are the config's values now
    public mutating func applied() {
        for k in entries.indices { entries[k].loaded = entries[k].value }
    }
}

/// One value on the game's command line. The game joins its arguments into one line, quoting the ones with a space,
/// and splits that line into commands at each `+` and `;` outside quotes: a value is cleaned of quotes and control
/// characters, and quoted when it has none of its own (no space) but could be split or cut (`+`, `;`, `//`).
public func engineArg(_ value: String) -> String {
    var v = ""
    for scalar in value.unicodeScalars {
        if scalar == "\t" {
            v.append(" ")
        } else if scalar != "\"" && scalar.value >= 0x20 && scalar.value != 0x7f {
            v.unicodeScalars.append(scalar)
        }
    }
    if v.contains(" ") { return v } // the game puts it in quotes itself
    if v.isEmpty || v.contains("+") || v.contains(";") || v.contains("//") || v.contains("/*") {
        return "\"" + v + "\""
    }
    return v
}

public struct Launch {
    /// fs_homepath: where the game writes everything (the game folder), if not empty
    public var homepath = ""
    /// fs_basepath: the game folder when the game isn't in it, else empty
    public var basepath = ""
    public var game: Game = .alliedAssault
    public var cvars: [(String, String)] = []
    public var address = ""
    public var password = ""
    public var rcon = ""
    public init() {}
}

/// The game's arguments: +set fs_homepath, +set fs_basepath, +set com_target_game N, the cvars, the passwords,
/// +connect
public func buildArgs(_ launch: Launch) -> [String] {
    var args: [String] = []
    func set(_ name: String, _ value: String) { args += ["+set", name, engineArg(value)] }
    if !launch.homepath.isEmpty { set("fs_homepath", launch.homepath) }
    if !launch.basepath.isEmpty { set("fs_basepath", launch.basepath) }
    set("com_target_game", String(launch.game.rawValue))
    for (name, value) in launch.cvars { set(name, value) }
    if !launch.password.isEmpty { set("password", launch.password) }
    if !launch.rcon.isEmpty { set("rconPassword", launch.rcon) }
    if !launch.address.isEmpty { args += ["+connect", engineArg(launch.address)] }
    return args
}

public func trimmed(_ s: String) -> String {
    s.trimmingCharacters(in: .whitespacesAndNewlines)
}

public func sameAddress(_ a: String, _ b: String) -> Bool {
    func key(_ s: String) -> String {
        let k = trimmed(s).lowercased()
        return k.contains(":") ? k : k + ":12203" // the game's default port
    }
    return key(a) == key(b)
}

/// "FFFFFF", "#ffffff" -> 0xFFFFFF; nil if it isn't 6 hex digits (the game then draws white)
public func parseColor(_ text: String) -> UInt32? {
    var hex = trimmed(text)
    if hex.hasPrefix("#") { hex.removeFirst() }
    guard hex.count == 6, hex.allSatisfy({ $0.isHexDigit }) else { return nil }
    return UInt32(hex, radix: 16)
}

public func colorHex(_ rgb: UInt32) -> String {
    String(format: "%06X", rgb & 0xFFFFFF)
}

public struct Box: Equatable {
    public var x, y, w, h: Int
}

/// The crosshair as cg_drawtools.cpp lays it out, in HUD pixels: the four arms around a center square of the
/// thickness, whose top left corner is (0, 0); `extent` is the width (and height) the arms span.
public func crosshairBoxes(length: Double, gap: Double, thickness: Double) -> (arms: [Box], extent: Int) {
    func pixels(_ value: Double, _ maximum: Double) -> Int {
        let v = !(value >= 1) ? 1 : min(value, maximum)
        return Int(v + 0.5)
    }
    let l = pixels(length, 128), t = pixels(thickness, 16)
    let s = max(pixels(gap, 64) - t / 2, 1)
    return ([Box(x: -s - l, y: 0, w: l, h: t), Box(x: t + s, y: 0, w: l, h: t),
             Box(x: 0, y: -s - l, w: t, h: l), Box(x: 0, y: t + s, w: t, h: l)], 2 * (s + l) + t)
}

/// The game's window modes on the Metal port, as r_fullscreen, r_noborder and r_borderlessWindow pick them
public enum DisplayMode: Int, CaseIterable, Identifiable {
    case space, exclusive, borderlessFullscreen, window, borderlessWindow

    public var id: Int { rawValue }
    public var title: String {
        ["Fullscreen Space", "Exclusive fullscreen", "Borderless fullscreen", "Window", "Borderless window"][rawValue]
    }
    /// The fullscreen Space and a borderless window over the screen take the desktop's size, whatever r_mode says
    public var usesResolution: Bool { self == .exclusive || self == .window || self == .borderlessWindow }

    public init(fullscreen: String, noborder: String, borderlessWindow: String) {
        let fs = (Int(fullscreen) ?? 0) != 0, nb = (Int(noborder) ?? 0) != 0, bw = (Int(borderlessWindow) ?? 0) != 0
        if fs {
            self = nb ? (bw ? .borderlessFullscreen : .space) : .exclusive
        } else {
            self = nb ? .borderlessWindow : .window
        }
    }

    public func apply(to edits: inout CvarEdits) {
        switch self {
        case .space:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "1"); edits.set("r_borderlessWindow", "0")
        case .exclusive:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "0")
        case .borderlessFullscreen:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "1"); edits.set("r_borderlessWindow", "1")
        case .window:
            edits.set("r_fullscreen", "0"); edits.set("r_noborder", "0")
        case .borderlessWindow:
            edits.set("r_fullscreen", "0"); edits.set("r_noborder", "1")
        }
    }
}

public struct Resolution: Hashable {
    public var width: Int
    public var height: Int
    public init(_ width: Int, _ height: Int) {
        self.width = width
        self.height = height
    }
}

/// The sizes to offer: the display's, the common ones that fit it, largest first, without duplicates
public func resolutionList(displayModes: [Resolution], desktop: Resolution) -> [Resolution] {
    let common = [
        (3840, 2160), (3440, 1440), (2560, 1600), (2560, 1440), (2560, 1080), (1920, 1200), (1920, 1080),
        (1680, 1050), (1600, 900), (1440, 900), (1366, 768), (1280, 1024), (1280, 960), (1280, 800), (1280, 720),
        (1024, 768), (800, 600), (640, 480),
    ].map { Resolution($0.0, $0.1) }
    var list: [Resolution] = []
    func add(_ r: Resolution) {
        if r.width >= 640 && r.height >= 480 && !list.contains(r) { list.append(r) }
    }
    add(desktop)
    displayModes.forEach(add)
    for r in common where desktop.width <= 0 || (r.width <= desktop.width && r.height <= desktop.height) {
        add(r)
    }
    return list.sorted { $0.width != $1.width ? $0.width > $1.width : $0.height > $1.height }
}

/// A data folder (main, mainta, maintt), in any case: retail folders sometimes come as MAIN or MainTA
public func dataFolder(_ folder: URL, _ name: String) -> URL? {
    let fm = FileManager.default
    var isDir: ObjCBool = false
    let exact = folder.appendingPathComponent(name)
    if fm.fileExists(atPath: exact.path, isDirectory: &isDir) && isDir.boolValue { return exact }
    let names = (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
    guard let match = names.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) else { return nil }
    let url = folder.appendingPathComponent(match)
    return fm.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue ? url : nil
}

/// The retail game's data: main/Pak0.pk3, in any case
public func hasGameData(_ folder: URL) -> Bool {
    guard let main = dataFolder(folder, "main") else { return false }
    let names = (try? FileManager.default.contentsOfDirectory(atPath: main.path)) ?? []
    return names.contains { $0.caseInsensitiveCompare("Pak0.pk3") == .orderedSame }
}

/// A server in the list: what it's called there (its address until it's renamed), its address and the passwords
/// used on it. The player's own name isn't part of it: that's the game's.
public struct Server: Codable, Identifiable, Hashable {
    public var id = UUID()
    public var label: String
    public var address: String
    public var password = ""
    public var rcon = ""

    enum CodingKeys: String, CodingKey { case label, address, password, rcon }

    public init(label: String, address: String, password: String = "", rcon: String = "") {
        self.label = label
        self.address = address
        self.password = password
        self.rcon = rcon
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? ""
        if trimmed(label).isEmpty { label = address }
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
    }
}

/// Per game: the server fields and the servers played on
public struct GameSlot: Codable {
    public var address = ""
    public var password = ""
    public var rcon = ""
    public var servers: [Server] = []

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
        servers = (try c.decodeIfPresent([Server].self, forKey: .servers) ?? []).filter { !trimmed($0.address).isEmpty }
    }
}

/// The launcher's own settings, as the game folder's mohlauncher.json holds them (the Windows and Linux launcher's
/// are the same)
public struct LauncherSettings: Codable {
    public var game = Game.alliedAssault
    public var slots = [GameSlot](repeating: GameSlot(), count: Game.allCases.count)

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        game = (try? c.decodeIfPresent(Game.self, forKey: .game)) ?? .alliedAssault
        slots = (try? c.decodeIfPresent([GameSlot].self, forKey: .slots)) ?? []
        while slots.count < Game.allCases.count { slots.append(GameSlot()) }
    }

    public static func from(json data: Data?) -> LauncherSettings {
        data.flatMap { try? JSONDecoder().decode(LauncherSettings.self, from: $0) } ?? LauncherSettings()
    }

    /// Keeps the server in the game's list (added at the end), with the passwords used
    public mutating func remember(_ game: Game, address: String, password: String, rcon: String) {
        if let k = slots[game.rawValue].servers.firstIndex(where: { sameAddress($0.address, address) }) {
            slots[game.rawValue].servers[k].address = address
            slots[game.rawValue].servers[k].password = password
            slots[game.rawValue].servers[k].rcon = rcon
        } else {
            slots[game.rawValue].servers.append(Server(label: address, address: address, password: password, rcon: rcon))
        }
    }
}

/// The servers the launcher comes with, first in their game's list: they can't be renamed or removed
public let builtinServers: [(game: Game, label: String, address: String)] = [
    (.alliedAssault, "Forte FFA", "151.80.234.90:12203"),
]

/// A server as the list shows it
public struct Listed: Identifiable {
    /// with the passwords last used on it
    public var server: Server
    /// its place in the game's saved servers, if it's there
    public var saved: Int?
    public var builtin: Bool
    public var id: String { builtin ? "builtin:" + server.address : server.id.uuidString }
}

/// The game's list: its built-in servers, then the ones the player joined (a built-in one's entry, which keeps the
/// passwords used, isn't listed twice)
public func listedServers(_ settings: LauncherSettings, _ game: Game) -> [Listed] {
    let saved = settings.slots[game.rawValue].servers
    var list: [Listed] = []
    for b in builtinServers where b.game == game {
        let k = saved.firstIndex { sameAddress($0.address, b.address) }
        let server = Server(label: b.label, address: b.address, password: k.map { saved[$0].password } ?? "",
                            rcon: k.map { saved[$0].rcon } ?? "")
        list.append(Listed(server: server, saved: k, builtin: true))
    }
    for (k, s) in saved.enumerated()
    where !builtinServers.contains(where: { $0.game == game && sameAddress($0.address, s.address) }) {
        list.append(Listed(server: s, saved: k, builtin: false))
    }
    return list
}
