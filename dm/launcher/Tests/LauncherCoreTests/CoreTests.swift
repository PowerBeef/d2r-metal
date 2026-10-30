import Foundation
import Testing
@testable import LauncherCore

let sample = """
{"schema": 1, "version": "1.0.0", "build": 412, "commit": "abc1234", "date": "2026-10-30", "notes": "First release",
 "macos_min": "27.0", "update_url": "https://github.com/PowerBeef/d2r-metal/releases/latest/download/latest_mac.json",
 "chain": "ad2699b98ae0", "product": "3.3.94012-r49", "team_id": "FK2D8X36G2",
 "runtime": {"name": "d2r-metal-runtime-1.0.0.tar.gz", "version": "1.0.0", "sha256": "00", "size": 10},
 "tools": {"name": "d2r-metal-tools-1.tar.gz", "url": "https://example.invalid/tools.tar.gz", "version": "1", "sha256": "11", "size": 20},
 "launcher": {"version": "1.0.0", "build": 412, "name": "D2R-Metal-Launcher-1.0.0.zip", "sha256": "22", "size": 5}}
"""

@Test func manifestDecodes() throws {
    let m = try JSONDecoder().decode(Manifest.self, from: Data(sample.utf8))
    #expect(m.build == 412 && m.teamID == "FK2D8X36G2" && m.macosMin == "27.0")
    #expect(m.launcher?.minBuild == 0)   // absent = any launcher
    let base = URL(string: "https://github.com/PowerBeef/d2r-metal/releases/latest/download/manifest.json")!
    #expect(m.url(of: m.runtime.name, url: m.runtime.url, manifestURL: base).absoluteString == "https://github.com/PowerBeef/d2r-metal/releases/latest/download/d2r-metal-runtime-1.0.0.tar.gz")
    #expect(m.url(of: m.tools.name, url: m.tools.url, manifestURL: base).absoluteString == "https://example.invalid/tools.tar.gz")
}

@Test func engineEventsParse() {
    let p = EngineEvent.parse(#"{"event": "progress", "step": "warm", "done": 64, "total": 7212, "t": 1.0}"#)
    #expect(p?.event == "progress" && p?.done == 64 && p?.total == 7212)
    let s = EngineEvent.parse(#"{"event": "step", "step": "shaders", "state": "start", "typical_s": 120}"#)
    #expect(s?.typicalS == 120)
    let start = EngineEvent.parse(#"{"event": "start", "typical_s": {"unpack": 300}, "log": "/x.log"}"#)   // a table here, not a number
    #expect(start?.log == "/x.log" && start?.typicalS == nil)
    #expect(EngineEvent.parse("[12:00:00] not json") == nil)
}

@Test func engineArguments() {
    let paths = Paths(root: URL(fileURLWithPath: "/tmp/root dir"))
    let e = Engine(paths: paths, runtime: URL(fileURLWithPath: "/r"), tools: URL(fileURLWithPath: "/t"), appDir: URL(fileURLWithPath: "/Applications"))
    let a = e.arguments("install", input: .packages([URL(fileURLWithPath: "/g/base.nsp"), URL(fileURLWithPath: "/g/upd.nsp")], keys: URL(fileURLWithPath: "/k/prod.keys")))
    #expect(a.starts(with: ["-I", "/r/repo/dm/tools/install.py", "install", "--root", "/tmp/root dir", "--json"]))
    #expect(a.suffix(5) == ["--game", "/g/base.nsp", "/g/upd.nsp", "--keys", "/k/prod.keys"])
    let r = e.arguments("revert")
    #expect(!r.contains("--runtime") && r.contains("--app-dir"))
    let s = e.arguments("status")
    #expect(!s.contains("--runtime") && !s.contains("--app-dir"))
}

@Test func checksumOfAFile() throws {
    let f = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-sha-\(getpid())")
    try Data("abc".utf8).write(to: f); defer { try? FileManager.default.removeItem(at: f) }
    #expect(try Packs.sha256(of: f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
}

@Test func unsignedBinaryFailsTheTeamCheck() throws {
    // /bin/ls is signed by Apple, not by the project's team: refused
    #expect(throws: LauncherError.signature("ls")) { try Packs(paths: Paths(root: URL(fileURLWithPath: "/tmp"))).verifySigned(URL(fileURLWithPath: "/bin/ls"), team: "FK2D8X36G2") }
}

@Test func packageKindsFromNames() {
    #expect(PackageKind.guess("Diablo II Resurrected [0100726014352000][v0].nsp") == .base)
    #expect(PackageKind.guess("Diablo II Resurrected [0100726014352800][v2228224].nsp") == .update)
    #expect(PackageKind.guess("Diablo II Resurrected Reign of the Warlock [01007260143530a1][v0].nsp") == .dlc)
    #expect(PackageKind.guess("D2R UPDATE 1.0.34.nsp") == .update)
    #expect(PackageKind.guess("game.xci") == nil)
}

/// A synthetic PFS0 (.nsp) holding only a table of contents with these names (no content)
func fakeNSP(_ names: [String]) throws -> URL {
    var strings = Data(); var offsets: [UInt32] = []
    for n in names { offsets.append(UInt32(strings.count)); strings.append(contentsOf: Array(n.utf8) + [0]) }
    while strings.count % 16 != 0 { strings.append(0) }
    func le32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8(truncatingIfNeeded: v >> ($0 * 8)) } }
    var d = Data("PFS0".utf8)
    d.append(contentsOf: le32(UInt32(names.count)) + le32(UInt32(strings.count)) + le32(0))
    for o in offsets { d.append(contentsOf: [UInt8](repeating: 0, count: 16) + le32(o) + le32(0)) }
    d.append(strings)
    let f = FileManager.default.temporaryDirectory.appendingPathComponent("fake-\(UUID().uuidString).nsp")
    try d.write(to: f); return f
}

@Test func packagesAreRecognizedByTheirTicket() throws {
    let upd = try fakeNSP(["a1b2.cnmt.nca", "c3d4.nca", "0100726014352800000000000000000c.tik", "0100726014352800000000000000000c.cert"])
    let base = try fakeNSP(["e5f6.nca", "0100726014352000000000000000000c.tik"])
    let dlc = try fakeNSP(["0100726014353001000000000000000c.tik"])
    let other = try fakeNSP(["0100000000010000000000000000000c.tik"])
    defer { for f in [upd, base, dlc, other] { try? FileManager.default.removeItem(at: f) } }
    guard case .ok = GameFileCheck.package(upd, expected: .update) else { Issue.record("update not recognized"); return }
    guard case .ok = GameFileCheck.package(base, expected: .base) else { Issue.record("base not recognized"); return }
    guard case .ok = GameFileCheck.package(dlc, expected: .dlc) else { Issue.record("dlc not recognized"); return }
    guard case .elsewhere(.update, _) = GameFileCheck.package(upd, expected: .base) else { Issue.record("update in the base selector not redirected"); return }
    guard case .wrong = GameFileCheck.package(other, expected: .base) else { Issue.record("another game accepted"); return }
}

@Test func keyFilesAreCheckedWithoutShowingValues() throws {
    let dir = FileManager.default.temporaryDirectory
    let good = dir.appendingPathComponent("keys-\(getpid()).keys"), odd = dir.appendingPathComponent("odd-\(getpid()).keys"), junk = dir.appendingPathComponent("junk-\(getpid()).keys")
    defer { for f in [good, odd, junk] { try? FileManager.default.removeItem(at: f) } }
    let zeros = { (n: Int) in String(repeating: "0", count: n) }
    try "header_key = \(zeros(64))\nmaster_key_00 = \(zeros(32))\n".write(to: good, atomically: true, encoding: .utf8)
    try "header_key = \(zeros(64))\nmaster_key_00 = \(zeros(32))\nmaster_kek_source_17 = \(zeros(34))\n".write(to: odd, atomically: true, encoding: .utf8)
    try "hello world\n".write(to: junk, atomically: true, encoding: .utf8)
    guard case .ok(let s) = GameFileCheck.keys(good) else { Issue.record("good keys refused"); return }
    #expect(!s.contains(zeros(32)))
    guard case .note(let n) = GameFileCheck.keys(odd) else { Issue.record("odd line not noted"); return }
    #expect(n.contains("1 line"))
    guard case .wrong = GameFileCheck.keys(junk) else { Issue.record("junk accepted"); return }
}

@Test func extractedFolderNeedsTheRightProgram() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("extracted-\(getpid())")
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root.appendingPathComponent("Update/ExeFS"), withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: root.appendingPathComponent("Update/RomFS/data"), withIntermediateDirectories: true)
    try Data("not the game".utf8).write(to: root.appendingPathComponent("Update/ExeFS/main"))
    try Data().write(to: root.appendingPathComponent("Update/RomFS/data/build_manifest.psv"))
    guard case .wrong(let why) = GameFileCheck.folder(root) else { Issue.record("wrong program accepted"); return }
    #expect(why.contains("1.0.34.0"))
}
