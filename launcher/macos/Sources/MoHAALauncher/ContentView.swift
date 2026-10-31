// The launcher's window.
import AppKit
import LauncherCore
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        VStack(spacing: 10) {
            GamePicker()
            GroupBox { ServerSection().padding(4) }
            GroupBox { CrosshairSection().padding(4) }
            GroupBox { DisplaySection().padding(4) }
            Footer()
        }
        .padding(14)
        .frame(width: 640)
        .fixedSize(horizontal: false, vertical: true)
        .preferredColorScheme(.dark)
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didTerminateApplicationNotification)) {
            model.applicationTerminated($0.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.becameActive()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            model.save()
        }
        .alert("MoHAA Launcher", isPresented: Binding(get: { model.alertMessage != nil },
                                                      set: { if !$0 { model.alertMessage = nil } })) {
            Button("OK") { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
        .alert("Rename Server", isPresented: Binding(get: { model.renamingID != nil },
                                                     set: { if !$0 { model.renamingID = nil } })) {
            TextField("Name", text: $model.renameText)
            Button("Rename") { model.commitRename() }
            Button("Cancel", role: .cancel) { model.renamingID = nil }
        }
    }
}

/// Allied Assault, Spearhead, Breakthrough
private struct GamePicker: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        HStack(spacing: 8) {
            ForEach(Game.allCases) { game in
                let selected = model.game == game
                let available = model.available(game)
                Button {
                    model.select(game)
                } label: {
                    HStack(spacing: 8) {
                        Image(nsImage: model.icon(for: game))
                            .resizable()
                            .interpolation(.high)
                            .frame(width: 28, height: 28)
                        Text(game.title)
                            .font(.system(size: 13, weight: selected ? .semibold : .regular))
                    }
                    .frame(maxWidth: .infinity, minHeight: 42)
                    .contentShape(RoundedRectangle(cornerRadius: 8))
                    .background(RoundedRectangle(cornerRadius: 8)
                        .fill(selected ? Color.accentColor.opacity(0.28) : Color.primary.opacity(0.06)))
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .strokeBorder(selected ? Color.accentColor : Color.primary.opacity(0.1), lineWidth: 1))
                }
                .buttonStyle(.plain)
                .disabled(!available)
                .opacity(available ? 1 : 0.4)
                .help(available ? game.title : "\(game.title)'s data (\(game.dataDir)/) isn't in the game folder")
            }
        }
    }
}

/// The player's name, the server and its passwords, and the servers played on
private struct ServerSection: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 8) {
                GridRow {
                    Text("Name").gridColumnAlignment(.trailing)
                    TextField("Name", text: model.name, prompt: Text("UnnamedSoldier"))
                }
                GridRow {
                    Text("Server")
                    TextField("Server", text: $model.address, prompt: Text("address:port"))
                        .onSubmit { model.start(connect: true) }
                }
                GridRow {
                    Text("Password")
                    SecureField("Password", text: $model.password, prompt: Text("If the server has one"))
                        .onSubmit { model.start(connect: true) }
                }
                GridRow {
                    Text("RCON")
                    SecureField("RCON", text: $model.rcon, prompt: Text("For server admins"))
                        .onSubmit { model.start(connect: true) }
                }
            }
            .labelsHidden()
            .textFieldStyle(.roundedBorder)
            .frame(width: 270)

            List(selection: model.selection) {
                ForEach(model.servers) { server in
                    ServerRow(server: server).tag(server.id)
                }
            }
            .listStyle(.bordered(alternatesRowBackgrounds: false))
            .contextMenu(forSelectionType: Server.ID.self) { ids in
                if let id = ids.first {
                    Button("Connect") { model.connect(to: id) }
                    Button("Rename…") { model.beginRename(id) }
                    Divider()
                    Button("Remove", role: .destructive) { model.remove(id) }
                }
            } primaryAction: { ids in
                if let id = ids.first { model.connect(to: id) }
            }
            .overlay {
                if model.servers.isEmpty {
                    Text("The servers you connect to are kept here.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding()
                }
            }
            .frame(height: 128)
        }
    }
}

private struct ServerRow: View {
    @EnvironmentObject private var model: LauncherModel
    let server: Server

    var body: some View {
        HStack(spacing: 6) {
            VStack(alignment: .leading, spacing: 1) {
                Text(server.name).lineLimit(1)
                if server.name != server.address {
                    Text(server.address).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer(minLength: 4)
            Button {
                model.beginRename(server.id)
            } label: {
                Image(systemName: "pencil")
            }
            .help("Rename")
            Button {
                model.remove(server.id)
            } label: {
                Image(systemName: "trash")
            }
            .help("Remove")
            Button {
                model.connect(to: server.id)
            } label: {
                Image(systemName: "play.fill")
            }
            .help("Connect")
            .disabled(!model.canStart)
        }
        .buttonStyle(.borderless)
    }
}

/// cg_crosshair_length, cg_crosshair_gap, cg_crosshair_thickness, cg_crosshair_color
private struct CrosshairSection: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        HStack(spacing: 12) {
            CrosshairPreview(length: Double(model.number("cg_crosshair_length", 1...128).wrappedValue),
                             gap: Double(model.number("cg_crosshair_gap", 1...64).wrappedValue),
                             thickness: Double(model.number("cg_crosshair_thickness", 1...16).wrappedValue),
                             rgb: model.crosshairRGB)
                .frame(width: 60, height: 60)
            NumberField(title: "Length", value: model.number("cg_crosshair_length", 1...128), range: 1...128)
                .help("Each arm's length, in HUD pixels")
            NumberField(title: "Gap", value: model.number("cg_crosshair_gap", 1...64), range: 1...64)
                .help("From the center to each arm")
            NumberField(title: "Thick", value: model.number("cg_crosshair_thickness", 1...16), range: 1...16)
                .help("The arms' thickness")
            Picker("Color", selection: model.colorPreset) {
                ForEach(LauncherModel.colorPresets) { preset in
                    Text(preset.name).tag(Int64(preset.rgb))
                }
                Text("Custom").tag(Int64(-1))
            }
            .labelsHidden()
            .frame(width: 92)
            ColorPicker("Color", selection: model.color, supportsOpacity: false)
                .labelsHidden()
                .help("Any color")
            Spacer(minLength: 0)
            Button("Reset") { model.resetCrosshair() }
                .help("The stock crosshair: length 4, gap 3, thickness 1, white")
        }
    }
}

private struct NumberField: View {
    let title: String
    @Binding var value: Int
    let range: ClosedRange<Int>

    var body: some View {
        HStack(spacing: 3) {
            TextField(title, value: $value, format: .number)
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.trailing)
                .frame(width: 38)
            Stepper(title, value: $value, in: range)
                .labelsHidden()
            Text(title)
                .foregroundStyle(.secondary)
        }
    }
}

/// The crosshair as the game draws it, three screen points to a HUD pixel when it fits
private struct CrosshairPreview: View {
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
            let room = min(size.width, size.height) - 6
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
        .help("The crosshair at three times its size in the game (at 1080p)")
    }
}

/// r_mode with r_customwidth and r_customheight; r_fullscreen, r_noborder and r_borderlessWindow
private struct DisplaySection: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        HStack(spacing: 16) {
            Picker("Resolution", selection: model.resolution) {
                ForEach(model.resolutionEntries) { entry in
                    Text(entry.label).tag(entry)
                }
            }
            .disabled(!model.displayMode.wrappedValue.usesResolution)
            .help(model.displayMode.wrappedValue.usesResolution
                  ? "The game's size"
                  : "\(model.displayMode.wrappedValue.title) takes the desktop's resolution")
            Picker("Mode", selection: model.displayMode) {
                ForEach(DisplayMode.allCases) { mode in
                    Text(mode.title).tag(mode)
                }
            }
            .help("Fullscreen Space: the cleanest mode (r_fullscreen 1, r_noborder 1). Exclusive fullscreen: at the resolution's size.")
        }
    }
}

/// Where the game is, and the buttons
private struct Footer: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        VStack(spacing: 10) {
            Text(model.status)
                .font(.callout)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)
                .help(model.gameFolder.path)
            HStack(spacing: 10) {
                Button("Game Folder…") { model.chooseGameFolder() }
                    .help("The folder with openmohaa.app and the game's data (main/)")
                Spacer()
                Button {
                    model.start(connect: false)
                } label: {
                    Label("Single Player", systemImage: "person.fill")
                        .frame(minWidth: 130)
                }
                .controlSize(.large)
                .disabled(!model.canStart)
                .help("Start the game at its main menu, without joining a server")
                Button {
                    model.start(connect: true)
                } label: {
                    Label("Connect", systemImage: "play.fill")
                        .frame(minWidth: 130)
                }
                .controlSize(.large)
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                .disabled(!model.canConnect)
                .help("Start the game and join the server")
            }
        }
    }
}
