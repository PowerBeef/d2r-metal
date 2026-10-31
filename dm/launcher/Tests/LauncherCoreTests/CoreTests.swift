import CryptoKit
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
    #expect(try m.assetURL(m.runtime.name, url: m.runtime.url, manifestURL: base).absoluteString == "https://github.com/PowerBeef/d2r-metal/releases/latest/download/d2r-metal-runtime-1.0.0.tar.gz")
    #expect(try m.assetURL(m.tools.name, url: m.tools.url, manifestURL: base).absoluteString == "https://example.invalid/tools.tar.gz")
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
    #expect(a.starts(with: ["-I", "-B", "/r/repo/dm/tools/install.py", "install", "--root", "/tmp/root dir", "--json"]))
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

@Test func appleSignedBinaryFailsTheTeamCheck() throws {
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
    for n in ["main", "sdk", "subsdk0"] { try Data("not the game".utf8).write(to: root.appendingPathComponent("Update/ExeFS/\(n)")) }
    try Data().write(to: root.appendingPathComponent("Update/RomFS/data/build_manifest.psv"))
    guard case .wrong(let why) = GameFileCheck.folder(root) else { Issue.record("wrong program accepted"); return }
    #expect(why.contains("1.0.34.0"))
}

func tempDir(_ name: String) throws -> URL {
    let d = FileManager.default.temporaryDirectory.appendingPathComponent("\(name)-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: d, withIntermediateDirectories: true); return d
}

@Test func manifestDecodingIsTolerant() throws {
    // descriptive fields may come and go; installing needs only the release and its packs
    let lean = #"{"schema": 1, "version": "1.1", "build": 500, "runtime": {"name": "r.tgz", "version": "1.1", "sha256": "0", "size": 1}, "tools": {"name": "t.tgz", "version": "3", "sha256": "1", "size": 1}, "extra": true}"#
    let m = try JSONDecoder().decode(Manifest.self, from: Data(lean.utf8))
    #expect(m.installable && m.notes.isEmpty && m.macosMin == "27.0" && m.launcher == nil && m.runsOnThisMac)
    // a newer format: not installable here, but its launcher entry still reads, so this launcher can update itself to one that installs it
    let future = #"{"schema": 2, "version": "2.0", "build": 900, "packs": [], "launcher": {"version": "2.0", "build": 900, "name": "L.zip", "sha256": "2", "size": 3}}"#
    let f = try JSONDecoder().decode(Manifest.self, from: Data(future.utf8))
    #expect(!f.installable && f.launcher?.build == 900)
    let tooNew = #"{"schema": 1, "version": "1", "build": 1, "macos_min": "99.0", "runtime": {"name": "r", "version": "1", "sha256": "0", "size": 1}, "tools": {"name": "t", "version": "1", "sha256": "0", "size": 1}}"#
    #expect(!(try JSONDecoder().decode(Manifest.self, from: Data(tooNew.utf8))).runsOnThisMac)
}

@Test func manifestNamesMustBePlain() throws {
    for bad in ["../x", "a/b", "..", ".hidden", ""] {
        let j = #"{"schema": 1, "version": "1", "build": 1, "runtime": {"name": "r", "version": "\#(bad)", "sha256": "0", "size": 1}, "tools": {"name": "t", "version": "1", "sha256": "0", "size": 1}}"#
        let m = try JSONDecoder().decode(Manifest.self, from: Data(j.utf8))
        #expect(throws: LauncherError.self) { try m.validate() }
    }
    let ok = try JSONDecoder().decode(Manifest.self, from: Data(sample.utf8))
    try ok.validate()
}

@Test func assetsOfANetworkReleaseNeedHttps() throws {
    let m = try JSONDecoder().decode(Manifest.self, from: Data(sample.utf8))
    let gh = URL(string: "https://github.com/PowerBeef/d2r-metal/releases/download/v1.0.0/manifest.json")!
    #expect(try m.assetURL("r.tgz", url: nil, manifestURL: gh).absoluteString == "https://github.com/PowerBeef/d2r-metal/releases/download/v1.0.0/r.tgz")
    #expect(throws: LauncherError.self) { try m.assetURL("r.tgz", url: "http://example.com/r.tgz", manifestURL: gh) }
    #expect(throws: LauncherError.self) { try m.assetURL("r.tgz", url: "file:///etc/r.tgz", manifestURL: gh) }
    let local = URL(fileURLWithPath: "/tmp/rel/manifest.json")
    #expect(try m.assetURL("r.tgz", url: nil, manifestURL: local).path == "/tmp/rel/r.tgz")   // a local test release keeps its files beside it
}

@Test func manifestSignatures() throws {
    let key = Curve25519.Signing.PrivateKey(), pub = key.publicKey.rawRepresentation.base64EncodedString()
    let data = Data(sample.utf8)
    let sig = Data(try key.signature(for: data).base64EncodedString().utf8)
    #expect(ManifestSignature.isValid(sig, for: data, keys: [pub]))
    #expect(!ManifestSignature.isValid(sig, for: data + Data(" ".utf8), keys: [pub]))              // a changed manifest
    #expect(!ManifestSignature.isValid(sig, for: data, keys: ManifestSignature.publicKeys))         // another key
    #expect(!ManifestSignature.isValid(Data("not base64".utf8), for: data, keys: [pub]))
}

@Test func requirementQuotesTheTeam() {
    let r = Trust.requirement(team: #"X" or anchor apple or "Y"#, identifier: "org.d2rmetal.launcher")
    #expect(r.contains(#"leaf[subject.OU] = "X\" or anchor apple or \"Y""#))
    var req: SecRequirement?
    #expect(SecRequirementCreateWithString(Trust.requirement(team: "FK2D8X36G2") as CFString, [], &req) == errSecSuccess)
    #expect(Trust(team: nil).team(for: try! JSONDecoder().decode(Manifest.self, from: Data(sample.utf8))) == "FK2D8X36G2")   // a test launcher: the manifest's
    #expect(Trust(team: "AAAAAAAAAA").team(for: try! JSONDecoder().decode(Manifest.self, from: Data(sample.utf8))) == "AAAAAAAAAA")   // a release: its own
}

@Test func unpackedPacksAreCheckedFileByFile() throws {
    let d = try tempDir("pack"); defer { try? FileManager.default.removeItem(at: d) }
    let src = d.appendingPathComponent("src"); try FileManager.default.createDirectory(at: src.appendingPathComponent("bin"), withIntermediateDirectories: true)
    try Data("tool".utf8).write(to: src.appendingPathComponent("bin/tool")); try Data("lib".utf8).write(to: src.appendingPathComponent("lib.py"))
    let tgz = d.appendingPathComponent("p.tar.gz")
    let tar = Process(); tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar"); tar.arguments = ["-czf", tgz.path, "-C", src.path, "."]
    try tar.run(); tar.waitUntilExit()
    let packs = Packs(paths: Paths(root: d)), dir = d.appendingPathComponent("unpacked")
    try packs.unpack(tgz, into: dir, sha: "abc")
    #expect(packs.isUnpacked(dir, sha: "abc") && Packs.contentsIntact(dir) == true)
    try Data("changed".utf8).write(to: dir.appendingPathComponent("lib.py"))
    #expect(Packs.contentsIntact(dir) == false)
    try FileManager.default.removeItem(at: dir.appendingPathComponent(".files"))
    #expect(Packs.contentsIntact(dir) == nil)   // unpacked by an older launcher: can't tell
}

/// A minimal app bundle (no program), ad-hoc signed
func fakeLauncher(_ dir: URL, build: Int, id: String = SelfUpdate.bundleID) throws -> URL {
    let app = dir.appendingPathComponent("D2R Metal Launcher.app")
    try FileManager.default.createDirectory(at: app.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
    try (["CFBundleIdentifier": id, "CFBundleVersion": "\(build)", "CFBundleExecutable": "x", "CFBundlePackageType": "APPL"] as NSDictionary)
        .write(to: app.appendingPathComponent("Contents/Info.plist"))
    try FileManager.default.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"), to: app.appendingPathComponent("Contents/MacOS/x"))
    let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/codesign"); p.arguments = ["--force", "-s", "-", app.path]
    p.standardError = FileHandle.nullDevice; try p.run(); p.waitUntilExit()
    return app
}

@Test func selfUpdateChecksTheNewLauncher() throws {
    let d = try tempDir("su"); defer { try? FileManager.default.removeItem(at: d) }
    let su = SelfUpdate(paths: Paths(root: d), trust: Trust(team: nil))
    let app = try fakeLauncher(d.appendingPathComponent("a"), build: 7)
    try su.check(app, build: 7, team: nil)                                                         // ad-hoc, no team asked: a test release
    #expect(throws: LauncherError.self) { try su.check(app, build: 8, team: nil) }                 // not the build the release names
    #expect(throws: LauncherError.signature(app.lastPathComponent)) { try su.check(app, build: 7, team: "FK2D8X36G2") }   // not the team's Developer ID
    let other = try fakeLauncher(d.appendingPathComponent("b"), build: 7, id: "org.example.other")
    #expect(throws: LauncherError.self) { try su.check(other, build: 7, team: nil) }
}

@Test func selfUpdateSwapsAndKeepsThePrevious() throws {
    let d = try tempDir("swap"); defer { try? FileManager.default.removeItem(at: d) }
    let su = SelfUpdate(paths: Paths(root: d.appendingPathComponent("root")), trust: Trust(team: nil))
    let current = try fakeLauncher(d.appendingPathComponent("Applications"), build: 1)
    let new = try fakeLauncher(d.appendingPathComponent("staging"), build: 2)
    try su.install(new, over: current, register: false)
    #expect(SelfUpdate.build(of: current) == 2)
    #expect(SelfUpdate.build(of: su.previous.appendingPathComponent(current.lastPathComponent)) == 1)
    #expect(!FileManager.default.fileExists(atPath: new.path))
}

@Test func engineLinesSplitAtNewlineOnly() {
    final class Box: @unchecked Sendable { var lines: [String] = [] }
    let box = Box()
    let s = LineSplitter { box.lines.append(String(decoding: $0, as: UTF8.self)) }
    s.feed(Data(#"{"event": "error", "msg": "a\#u{2028}b"}"#.utf8)); s.feed(Data("\n{\"event\"".utf8)); s.feed(Data(": \"done\"}".utf8)); s.finish()
    #expect(box.lines.count == 2)
    #expect(EngineEvent.parse(box.lines[0][...])?.msg == "a\u{2028}b")
    #expect(EngineEvent.parse(box.lines[1][...])?.event == "done")
}

@Test func checksOfChosenFiles() throws {
    let d = try tempDir("check"); defer { try? FileManager.default.removeItem(at: d) }
    // a big file chosen as the keys is refused without reading it all
    let big = d.appendingPathComponent("big.keys"); FileManager.default.createFile(atPath: big.path, contents: nil)
    let h = try FileHandle(forWritingTo: big); try h.truncate(atOffset: 3 << 20); try h.close()
    guard case .wrong(let why) = GameFileCheck.keys(big) else { Issue.record("a 3 MB key file accepted"); return }
    #expect(why.contains("Too big"))
    // an extracted folder needs all three program files
    try FileManager.default.createDirectory(at: d.appendingPathComponent("x/ExeFS"), withIntermediateDirectories: true)
    try Data("m".utf8).write(to: d.appendingPathComponent("x/ExeFS/main"))
    guard case .wrong(let w) = GameFileCheck.folder(d.appendingPathComponent("x")) else { Issue.record("a folder without sdk accepted"); return }
    #expect(w.contains("sdk"))
    // a package with several tickets fits the selector of any of its titles
    let both = try fakeNSP(["0100726014353001000000000000000c.tik", "0100726014352800000000000000000c.tik"])
    defer { try? FileManager.default.removeItem(at: both) }
    guard case .ok(let s) = GameFileCheck.package(both, expected: .update) else { Issue.record("update + DLC package not taken as the update"); return }
    #expect(s.contains("Update") && s.contains("dlc"))
}

@Test func latestTagFromTheRedirect() {
    #expect(Feed.tag(fromLocation: "https://github.com/PowerBeef/d2r-metal/releases/tag/v1.0.0") == "v1.0.0")
    #expect(Feed.tag(fromLocation: "https://github.com/PowerBeef/d2r-metal/releases") == nil)          // no latest release yet: the list
    #expect(Feed.tag(fromLocation: "https://github.com/PowerBeef/d2r-metal/releases/tag/v1/../x") == nil)
    #expect(Feed.tag(fromLocation: "https://github.com/PowerBeef/d2r-metal/releases/tag/") == nil)
}

/// The latest-release lookup against GitHub itself (another project's releases, the same redirect): off unless DM_LAUNCHER_TEST_NETWORK is set
@Test(.enabled(if: ProcessInfo.processInfo.environment["DM_LAUNCHER_TEST_NETWORK"] != nil)) func latestRedirectIsReadWithoutFollowingIt() async throws {
    var req = URLRequest(url: URL(string: "https://github.com/astral-sh/python-build-standalone/releases/latest")!); req.httpMethod = "HEAD"
    let rsp = try await URLSession.shared.data(for: req, delegate: NoRedirect()).1 as? HTTPURLResponse
    #expect((300..<400).contains(rsp?.statusCode ?? 0))
    #expect(Feed.tag(fromLocation: rsp?.value(forHTTPHeaderField: "Location") ?? "") != nil)
}
