// swift-tools-version:5.9
// MoHAA Launcher for macOS: SwiftUI and AppKit. `./build.sh` makes the app; `swift test` checks LauncherCore.
import PackageDescription

let package = Package(
    name: "mohlauncher",
    platforms: [.macOS(.v14)],
    targets: [
        // the logic, without UI: the game's config, the edited settings, the command line (as launcher/desktop's core)
        .target(name: "LauncherCore"),
        .executableTarget(name: "mohlauncher", dependencies: ["LauncherCore"]),
        .testTarget(name: "LauncherCoreTests", dependencies: ["LauncherCore"]),
    ]
)
