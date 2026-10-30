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
