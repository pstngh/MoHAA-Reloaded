// The launcher's window, as the Windows and Linux launcher's (launcher/desktop/ui/app.slint) with macOS's own
// controls: the games along the top, the player and the servers, the crosshair and the display, and the buttons.
//
// One spacing system throughout (Metrics): every field is a block of a label over its control (labelGap apart),
// and blocks, cards and sections are `gap` apart, with `gap` of padding inside each card and around the window.
import AppKit
import LauncherCore
import SwiftUI

enum Metrics {
    static let gap: CGFloat = 12
    static let labelGap: CGFloat = 4
    static let labelHeight: CGFloat = 16
    static let control: CGFloat = 32 // a control's height, where rows must line up (as on Windows and Linux)
    static let row: CGFloat = 34
    // the crosshair preview spans two fields' controls: from the first's top to the second's bottom
    static let preview: CGFloat = 2 * control + gap + labelHeight + labelGap
}

extension Color {
    static let base = Color(red: 14 / 255, green: 14 / 255, blue: 14 / 255) // near black
    static let card = Color.white.opacity(0.035)
    static let stroke = Color.white.opacity(0.07)
    static let hoverFill = Color.white.opacity(0.06)
    static let selectedFill = Color.white.opacity(0.09)
    static let warning = Color(red: 252 / 255, green: 195 / 255, blue: 61 / 255)
    static let discord = Color(red: 88 / 255, green: 101 / 255, blue: 242 / 255) // Discord's blurple
}

struct ContentView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        VStack(spacing: Metrics.gap) {
            GameTabs()
            Card {
                PlayerAndServers()
            }
            HStack(alignment: .top, spacing: Metrics.gap) {
                Card {
                    CrosshairSection()
                }
                Card {
                    DisplaySection()
                }
                .frame(width: 214)
            }
            .fixedSize(horizontal: false, vertical: true)
            Footer()
        }
        .padding(Metrics.gap)
        .frame(width: 660)
        .fixedSize(horizontal: false, vertical: true)
        .background(Color.base)
        .background(WindowStyler())
        .preferredColorScheme(.dark)
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didTerminateApplicationNotification)) {
            model.applicationTerminated($0.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.tick()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            model.save()
        }
        .alert("Where is MOHAA?", isPresented: $model.askForFolder) {
            Button("Choose Folder…") {
                Task { @MainActor in model.chooseGameFolder() }
            }
            Button("Not Now", role: .cancel) {}
        } message: {
            Text("Choose the folder Medal of Honor: Allied Assault is installed in: the one with main/ (Pak0.pk3 to Pak6.pk3), and mainta/ and maintt/ for the expansions.")
        }
    }
}

/// The window's title bar in the window's own near black
struct WindowStyler: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        Task { @MainActor in
            guard let window = view.window else { return }
            window.titlebarAppearsTransparent = true
            window.backgroundColor = NSColor(srgbRed: 14 / 255, green: 14 / 255, blue: 14 / 255, alpha: 1)
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

struct Card<Content: View>: View {
    let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Metrics.gap) {
            content
        }
        .padding(Metrics.gap)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.card))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.stroke))
    }
}

struct FieldLabel: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(.secondary)
            .frame(height: Metrics.labelHeight)
    }
}

/// A field: its label (and an action on the right of it) over its control
struct Field<Accessory: View, Control: View>: View {
    let label: String
    let accessory: Accessory
    let control: Control

    init(_ label: String, @ViewBuilder accessory: () -> Accessory, @ViewBuilder control: () -> Control) {
        self.label = label
        self.accessory = accessory()
        self.control = control()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Metrics.labelGap) {
            HStack(spacing: 0) {
                FieldLabel(text: label)
                Spacer(minLength: 8)
                accessory
            }
            .frame(height: Metrics.labelHeight)
            control
        }
    }
}

extension Field where Accessory == EmptyView {
    init(_ label: String, @ViewBuilder control: () -> Control) {
        self.init(label, accessory: { EmptyView() }, control: control)
    }
}

/// A small action in a field's label row, on the right: an optional symbol and a word
struct LabelAction: View {
    let systemImage: String?
    let title: String
    var enabled = true
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 4) {
                if let systemImage {
                    Image(systemName: systemImage).font(.system(size: 10, weight: .semibold))
                }
                Text(title).font(.system(size: 12))
            }
            .foregroundStyle(!enabled ? Color.secondary.opacity(0.5)
                : hover ? Color.primary : Color.accentColor)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onHover { hover = $0 }
    }
}

struct GameTabs: View {
    var body: some View {
        HStack(spacing: Metrics.gap / 2) {
            ForEach(Game.allCases) { game in
                GameTab(game: game)
            }
        }
    }
}

struct GameTab: View {
    @EnvironmentObject var model: LauncherModel
    let game: Game
    @State var hover = false

    var body: some View {
        let selected = model.game == game
        let available = model.available(game)
        Button {
            model.select(game)
        } label: {
            HStack(spacing: 0) {
                // the game's icon in a box of its own, lifted off the tab
                Image(nsImage: model.icon(for: game))
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 24, height: 24)
                    .frame(width: 36, height: 36)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(selected ? 0.12 : 0.07)))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.white.opacity(0.1)))
                    .shadow(color: .black.opacity(0.5), radius: 3, y: 1)
                // the name, centered in the rest, and the selection bar as wide as it
                Text(game.title)
                    .font(.system(size: 13, weight: selected ? .semibold : .regular))
                    .padding(.vertical, 6)
                    .overlay(alignment: .bottom) {
                        if selected {
                            Capsule().fill(Color.accentColor).frame(height: 3)
                        }
                    }
                    .frame(maxWidth: .infinity)
                Color.clear.frame(width: 12)
            }
            .padding(5)
            .frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(selected ? Color.selectedFill : hover ? Color.hoverFill : Color.card))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.stroke))
            .contentShape(RoundedRectangle(cornerRadius: 8))
        }
        .buttonStyle(.plain)
        .disabled(!available)
        .opacity(available ? 1 : 0.4)
        .onHover { hover = $0 }
        .help(available ? game.title : "\(game.title)'s data (\(game.dataDir)/) isn't in the game folder")
    }
}

/// The player, the server and the servers played on
struct PlayerAndServers: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        VStack(spacing: Metrics.gap) {
            Grid(horizontalSpacing: Metrics.gap, verticalSpacing: Metrics.gap) {
                GridRow {
                    Field("Player name") {
                        TextField("Player name", text: model.name, prompt: Text("UnnamedSoldier"))
                    }
                    Field("Server", accessory: {
                        LabelAction(systemImage: model.bookmarked ? "bookmark.fill" : "bookmark",
                                    title: model.bookmarked ? "Bookmarked" : "Bookmark",
                                    enabled: !trimmed(model.address).isEmpty) {
                            model.bookmark()
                        }
                        .help(model.bookmarked ? "In the list: click to keep its passwords too"
                              : "Add it to the list without joining")
                    }) {
                        TextField("Server", text: $model.address, prompt: Text("host:port"))
                            .onSubmit { model.start(connect: true) }
                    }
                }
                GridRow {
                    Field("Server password") {
                        SecureField("Server password", text: $model.password, prompt: Text("If the server has one"))
                            .onSubmit { model.start(connect: true) }
                    }
                    Field("RCON password") {
                        SecureField("RCON password", text: $model.rcon, prompt: Text("For server admins"))
                            .onSubmit { model.start(connect: true) }
                    }
                }
            }
            .labelsHidden()
            .textFieldStyle(.roundedBorder)
            .controlSize(.large)
            ServerList()
        }
        .alert("Rename Server", isPresented: Binding(get: { model.renaming != nil },
                                                     set: { if !$0 { model.renaming = nil } })) {
            TextField("Name", text: $model.renameText)
            Button("Rename") { model.commitRename() }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Remove Server?", isPresented: Binding(get: { model.removing != nil },
                                                      set: { if !$0 { model.removing = nil } })) {
            Button("Remove", role: .destructive) { model.commitRemove() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("\(model.removingLabel) and its passwords leave the list.")
        }
    }
}

struct ServerList: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        let listed = model.listed
        ScrollView {
            VStack(spacing: 0) {
                ForEach(listed) { item in
                    ServerRow(item: item)
                }
            }
            .padding(2)
        }
        .frame(height: 3 * Metrics.row + 4)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.black.opacity(0.19)))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.white.opacity(0.04)))
        .overlay(alignment: .bottom) {
            // the room the list doesn't fill yet
            if listed.count < 3 {
                Text(listed.isEmpty ? "The servers you join are kept here: double-click one to join it again."
                     : "The servers you join are kept here.")
                    .font(.system(size: 12))
                    .foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity)
                    .frame(height: CGFloat(3 - listed.count) * Metrics.row)
                    .allowsHitTesting(false)
            }
        }
    }
}

struct ServerRow: View {
    @EnvironmentObject var model: LauncherModel
    let item: Listed
    @State var hover = false

    var body: some View {
        let selected = model.isSelected(item)
        HStack(spacing: 2) {
            // two even columns, whatever the texts' lengths
            Text(item.server.label)
                .fontWeight(item.builtin ? .semibold : .regular)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(item.server.label == item.server.address ? "" : item.server.address)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading)
            if item.builtin {
                // the same columns as the other rows, and a pin for a server that stays
                Color.clear.frame(width: 28, height: 28)
                Image(systemName: "pin.fill")
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
                    .frame(width: 28, height: 28)
                    .help("Always in the list")
            } else {
                RowButton(systemImage: "pencil", help: "Rename") { model.beginRename(item) }
                RowButton(systemImage: "trash", help: "Remove") { model.beginRemove(item) }
            }
            RowButton(systemImage: "play.fill", help: "Join", accent: true, enabled: model.canStart) {
                model.connect(item)
            }
        }
        .padding(.leading, Metrics.gap)
        .padding(.trailing, 3)
        .frame(height: Metrics.row)
        .background(RoundedRectangle(cornerRadius: 4)
            .fill(selected ? Color.selectedFill : hover ? Color.hoverFill : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { model.connect(item) }
        .onTapGesture { model.fill(item) }
        .onHover { hover = $0 }
        .contextMenu {
            Button("Join") { model.connect(item) }
            if !item.builtin {
                Button("Rename…") { model.beginRename(item) }
                Button("Remove", role: .destructive) { model.beginRemove(item) }
            }
        }
    }
}

struct RowButton: View {
    let systemImage: String
    let help: String
    var accent = false
    var enabled = true
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 12))
                .foregroundStyle(!enabled ? Color.secondary.opacity(0.4)
                    : accent ? Color.accentColor : Color.primary.opacity(0.85))
                .frame(width: 28, height: 28)
                .background(RoundedRectangle(cornerRadius: 4).fill(hover && enabled ? Color.selectedFill : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onHover { hover = $0 }
        .help(help)
    }
}

/// cg_crosshair_length, cg_crosshair_gap, cg_crosshair_thickness, cg_crosshair_color: the preview spans the two
/// rows of fields beside it, from the first's control to the last's
struct CrosshairSection: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        HStack(alignment: .top, spacing: Metrics.gap) {
            VStack(alignment: .leading, spacing: Metrics.labelGap) {
                HStack(spacing: 0) {
                    FieldLabel(text: "Crosshair")
                    Spacer(minLength: 4)
                    LabelAction(systemImage: nil, title: "Reset") { model.resetCrosshair() }
                        .help("The stock crosshair: length 4, gap 3, thickness 1, white")
                }
                .frame(height: Metrics.labelHeight)
                CrosshairPreview(length: Double(model.number("cg_crosshair_length", 1...128).wrappedValue),
                                 gap: Double(model.number("cg_crosshair_gap", 1...64).wrappedValue),
                                 thickness: Double(model.number("cg_crosshair_thickness", 1...16).wrappedValue),
                                 rgb: model.crosshairRGB)
                    .frame(width: Metrics.preview, height: Metrics.preview)
            }
            .frame(width: Metrics.preview)
            Grid(horizontalSpacing: Metrics.gap, verticalSpacing: Metrics.gap) {
                GridRow {
                    Field("Length") {
                        NumberField(value: model.number("cg_crosshair_length", 1...128), range: 1...128)
                            .frame(height: Metrics.control)
                    }
                    Field("Gap") {
                        NumberField(value: model.number("cg_crosshair_gap", 1...64), range: 1...64)
                            .frame(height: Metrics.control)
                    }
                }
                GridRow {
                    Field("Thickness") {
                        NumberField(value: model.number("cg_crosshair_thickness", 1...16), range: 1...16)
                            .frame(height: Metrics.control)
                    }
                    Field("Color") {
                        HStack(spacing: 6) {
                            Picker("Color", selection: model.colorPreset) {
                                ForEach(LauncherModel.colorPresets) { preset in
                                    Text(preset.name).tag(Int64(preset.rgb))
                                }
                                Text("Custom").tag(Int64(-1))
                            }
                            .fixedSize() // the names whole: the swatch gives way
                            ColorPicker("Any color", selection: model.color, supportsOpacity: false)
                                .frame(width: 36)
                        }
                        .frame(height: Metrics.control)
                    }
                }
            }
            .labelsHidden()
            .controlSize(.large)
        }
        .fixedSize(horizontal: false, vertical: true)
    }
}

struct NumberField: View {
    @Binding var value: Int
    let range: ClosedRange<Int>

    var body: some View {
        HStack(spacing: 4) {
            TextField("", value: $value, format: .number)
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.trailing)
                .frame(width: 56)
            Stepper("", value: $value, in: range)
        }
    }
}

/// The crosshair as the game draws it, three screen points to a HUD pixel when it fits
struct CrosshairPreview: View {
    let length: Double
    let gap: Double
    let thickness: Double
    let rgb: UInt32

    var body: some View {
        Canvas { context, size in
            let r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF
            let luma = (r * 3 + g * 6 + b) / 10
            // the game's sky or a wall behind it: black, grey for a dark crosshair
            context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(luma < 64 ? Color(white: 0.5) : .black))

            let (arms, extent) = crosshairBoxes(length: length, gap: gap, thickness: thickness)
            let room = min(size.width, size.height) - 8
            var zoom: CGFloat = 3
            if CGFloat(extent) * zoom > room {
                zoom = room / CGFloat(extent)
                if zoom >= 1 { zoom = zoom.rounded(.down) }
            }
            let center = CGFloat(arms[2].w) * zoom // the center square is as wide as the arms are thick
            let x0 = ((size.width - center) / 2).rounded(.down)
            let y0 = ((size.height - center) / 2).rounded(.down)
            let color = Color(.sRGB, red: Double(r) / 255, green: Double(g) / 255, blue: Double(b) / 255)
            for box in arms {
                let rect = CGRect(x: x0 + CGFloat(box.x) * zoom, y: y0 + CGFloat(box.y) * zoom,
                                  width: max(1, CGFloat(box.w) * zoom), height: max(1, CGFloat(box.h) * zoom))
                context.fill(Path(rect), with: .color(color))
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.stroke))
        .help("The crosshair at three times its size in the game (at 1080p)")
    }
}

/// r_mode with r_customwidth and r_customheight; r_fullscreen, r_noborder and r_borderlessWindow
struct DisplaySection: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        let mode = model.displayMode.wrappedValue
        VStack(alignment: .leading, spacing: Metrics.gap) {
            Field("Resolution") {
                Picker("Resolution", selection: model.resolution) {
                    ForEach(model.resolutionEntries) { entry in
                        Text(entry.label).tag(entry)
                    }
                }
                .disabled(!mode.usesResolution)
                .help(mode.usesResolution ? "The game's size" : "\(mode.title) takes the desktop's resolution")
            }
            Field("Display mode") {
                Picker("Display mode", selection: model.displayMode) {
                    ForEach(DisplayMode.allCases) { mode in
                        Text(mode.title).tag(mode)
                    }
                }
            }
        }
        .labelsHidden()
        .controlSize(.large)
    }
}

/// The project's links, where the game is (a click changes it), and the buttons
struct Footer: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        HStack(spacing: Metrics.gap / 2) {
            LinkButton(image: model.logo("github"), help: "MoHAA Reloaded on GitHub", hoverColor: .primary) {
                NSWorkspace.shared.open(LauncherModel.githubURL)
            }
            LinkButton(image: model.logo("discord"), help: "Forte's Discord", hoverColor: .discord) {
                NSWorkspace.shared.open(LauncherModel.discordURL)
            }
            Button {
                model.chooseGameFolder()
            } label: {
                HStack(spacing: 6) {
                    if model.running {
                        Circle().fill(Color.green).frame(width: 8, height: 8).frame(width: 14)
                    } else {
                        Image(systemName: "folder.fill").font(.system(size: 11))
                    }
                    Text(model.status)
                        .font(.system(size: 12))
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                .foregroundStyle(model.problem ? Color.warning : Color.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("The game folder: click to choose another")
            Button {
                model.start(connect: false)
            } label: {
                Label("Single Player", systemImage: "person.fill")
            }
            .controlSize(.large)
            .disabled(!model.canStart)
            .help("Start the game at its main menu, without joining a server")
            Button {
                model.start(connect: true)
            } label: {
                Label("Connect", systemImage: "play.fill").frame(minWidth: 90)
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.canConnect)
            .help("Start the game and join the server")
        }
        .alert("MoHAA Launcher", isPresented: Binding(get: { model.alertMessage != nil },
                                                      set: { if !$0 { model.alertMessage = nil } })) {
            Button("OK") {}
        } message: {
            Text(model.alertMessage ?? "")
        }
    }
}

/// An icon that opens a web page
struct LinkButton: View {
    let image: NSImage
    let help: String
    let hoverColor: Color
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(nsImage: image)
                .renderingMode(.template)
                .resizable()
                .frame(width: 17, height: 17)
                .foregroundStyle(hover ? hoverColor : Color.secondary)
                .frame(width: 28, height: 28)
                .background(RoundedRectangle(cornerRadius: 4).fill(hover ? Color.hoverFill : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
        .help(help)
    }
}
