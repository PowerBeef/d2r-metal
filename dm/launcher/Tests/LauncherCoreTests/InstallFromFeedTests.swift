import Foundation
import Testing
@testable import LauncherCore

/// End to end, what the launcher's Install button does, without its window: a release feed → the packs downloaded, checked and unpacked →
/// install.py from those packs on an extracted game folder → D2R Metal.app. Off unless DM_LAUNCHER_TEST_FEED (a manifest.json URL, e.g. a release
/// built with dm/tools/release_mac.sh --unsigned) and DM_LAUNCHER_TEST_GAME (a folder with ExeFS and RomFS) are set; it takes several minutes.
/// DM_LAUNCHER_TEST_ROOT (default: a temporary folder) holds the install, the app goes to <root>/Applications with bundle id
/// org.d2rmobile.mac.launchertest4, so nothing of a player's install or saves is touched.
@Test(.enabled(if: ProcessInfo.processInfo.environment["DM_LAUNCHER_TEST_FEED"] != nil && ProcessInfo.processInfo.environment["DM_LAUNCHER_TEST_GAME"] != nil),
      .timeLimit(.minutes(30)))
func installFromFeed() async throws {
    let env = ProcessInfo.processInfo.environment
    let root = URL(fileURLWithPath: env["DM_LAUNCHER_TEST_ROOT"] ?? NSTemporaryDirectory() + "launcher test \(getpid())", isDirectory: true)
    let paths = Paths(root: root), apps = root.appendingPathComponent("Applications", isDirectory: true)
    let (m, u) = try await Feed(channel: .stable, override: URL(string: env["DM_LAUNCHER_TEST_FEED"]!)).latest()
    let (rt, tl) = try await Packs(paths: paths).ensure(m, from: u)
    #expect(FileManager.default.isExecutableFile(atPath: tl.appendingPathComponent("python/bin/python3").path))
    let engine = Engine(paths: paths, runtime: rt, tools: tl, appDir: apps, bundleID: "org.d2rmobile.mac.launchertest4")
    var code: Int32 = -1, steps: [String: String] = [:], errors: [String] = []
    for try await out in engine.run("install", input: .folder(URL(fileURLWithPath: env["DM_LAUNCHER_TEST_GAME"]!, isDirectory: true))) {
        switch out {
        case .event(let ev):
            if ev.event == "step", let s = ev.step, let st = ev.state { steps[s] = st; print("step \(s) \(st) \(ev.seconds.map { "\($0) s" } ?? "")") }
            if ev.event == "error", let msg = ev.msg { errors.append(msg) }
        case .exit(let c): code = c
        }
    }
    #expect(code == 0, "engine exit \(code): \(errors)")
    #expect(steps.values.allSatisfy { $0 == "done" || $0 == "kept" })
    #expect(FileManager.default.fileExists(atPath: apps.appendingPathComponent("D2R Metal.app/Contents/MacOS/D2RMobile").path))
}
