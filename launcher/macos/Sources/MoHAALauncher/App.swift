// MoHAA Launcher for macOS: starts openmohaa.app (the metal branch's game) from the folder it shares with it.
import AppKit
import SwiftUI

@main
struct MoHAALauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = LauncherModel()

    var body: some Scene {
        Window("MoHAA Launcher", id: "main") {
            ContentView().environmentObject(model)
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        // dark, whatever the system's appearance: the window, its sheets and the panels it opens
        NSApp.appearance = NSAppearance(named: .darkAqua)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // `--snapshot <file.png>`: the window's contents as a picture, then quit (CI shows the build this way)
        let args = CommandLine.arguments
        if let k = args.firstIndex(of: "--snapshot"), k + 1 < args.count {
            let url = URL(fileURLWithPath: args[k + 1])
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                let saved = AppDelegate.writeSnapshot(to: url)
                exit(saved ? 0 : 1)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @MainActor
    private static func writeSnapshot(to url: URL) -> Bool {
        guard let view = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil })?.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return false }
        view.cacheDisplay(in: view.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else { return false }
        return (try? png.write(to: url)) != nil
    }
}
