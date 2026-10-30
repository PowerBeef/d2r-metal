// swift-tools-version: 6.2
// D2R Metal Launcher (docs/LAUNCHER.md): installs the game on this Mac from the player's own game files and updates it from the project's GitHub
// releases. Built by dm/tools/launcher_app.sh (swift build + the app bundle); the install work itself is dm/tools/install.py, run from the packs.
import PackageDescription

let package = Package(
    name: "D2RMetalLauncher",
    platforms: [.macOS("27.0")],
    targets: [
        .target(name: "LauncherCore", path: "Sources/LauncherCore"),
        .executableTarget(name: "D2RMetalLauncher", dependencies: ["LauncherCore"], path: "Sources/D2RMetalLauncher"),
        .testTarget(name: "LauncherCoreTests", dependencies: ["LauncherCore"], path: "Tests/LauncherCoreTests"),
    ]
)
