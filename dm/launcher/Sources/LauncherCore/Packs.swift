import CryptoKit
import Foundation
import Security

/// The install root and the packs in it. The root is also the workspace (DM_ROOT) of install.py: game files under data/, build outputs under build/
public struct Paths: Sendable {
    public var root: URL
    public init(root: URL? = nil) {
        self.root = root ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("D2R Metal", isDirectory: true)
    }
    public var downloads: URL { root.appendingPathComponent("downloads", isDirectory: true) }
    public func runtime(_ version: String) -> URL { root.appendingPathComponent("releases/\(version)/runtime", isDirectory: true) }
    public func tools(_ version: String) -> URL { root.appendingPathComponent("tools/\(version)", isDirectory: true) }
    public var state: URL { root.appendingPathComponent("build/state.json") }
    public var gameFiles: URL { root.appendingPathComponent("data/switch/3.3.94012/Update", isDirectory: true) }
    /// Where the game app goes: the launcher starts it (as Battle.net starts a game), so it stays out of /Applications and app search
    public var gameApps: URL { root.appendingPathComponent("Game", isDirectory: true) }
}

/// Downloads, checks and unpacks the packs a release names. URLSession downloads carry no quarantine flag, and every file is checked against the
/// manifest's sha256; when the manifest names a team, the packs' main binaries must also carry that team's Developer ID signature
public struct Packs: Sendable {
    public var paths: Paths
    public init(paths: Paths) { self.paths = paths }

    /// Makes sure both packs of the release are unpacked; progress(bytes done, bytes total) during downloads. Returns (runtime dir, tools dir)
    public func ensure(_ m: Manifest, from manifestURL: URL, session: URLSession = .shared,
                       progress: @escaping @Sendable (Int64, Int64) -> Void = { _, _ in }) async throws -> (URL, URL) {
        let rt = paths.runtime(m.runtime.version), tl = paths.tools(m.tools.version)
        let todo = [(m.runtime, rt), (m.tools, tl)].filter { !isUnpacked($0.1, sha: $0.0.sha256) }
        let total = todo.reduce(Int64(0)) { $0 + $1.0.size }; var before: Int64 = 0
        for (asset, dir) in todo {
            let base = before
            let file = try await download(m.url(of: asset.name, url: asset.url, manifestURL: manifestURL), name: asset.name, sha256: asset.sha256, session: session) { done in
                progress(base + done, total)
            }
            try unpack(file, into: dir, sha: asset.sha256)
            try? FileManager.default.removeItem(at: file)
            before += asset.size
        }
        if let team = m.teamID {
            try verifySigned(rt.appendingPathComponent("host/D2RMobile"), team: team)
            try verifySigned(tl.appendingPathComponent("python/bin/python3"), team: team)
        }
        return (rt, tl)
    }

    func isUnpacked(_ dir: URL, sha: String) -> Bool {
        (try? String(contentsOf: dir.appendingPathComponent(".unpacked"), encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines) == sha
    }

    public func download(_ url: URL, name: String, sha256: String, session: URLSession = .shared,
                         progress: @escaping @Sendable (Int64) -> Void = { _ in }) async throws -> URL {
        try FileManager.default.createDirectory(at: paths.downloads, withIntermediateDirectories: true)
        let dest = paths.downloads.appendingPathComponent(name)
        try? FileManager.default.removeItem(at: dest)
        if url.isFileURL { try FileManager.default.copyItem(at: url, to: dest) }
        else {
            let (bytes, rsp) = try await session.bytes(from: url)
            guard let http = rsp as? HTTPURLResponse, http.statusCode == 200 else { throw LauncherError.http(url.absoluteString, (rsp as? HTTPURLResponse)?.statusCode ?? 0) }
            FileManager.default.createFile(atPath: dest.path, contents: nil)
            let out = try FileHandle(forWritingTo: dest); defer { try? out.close() }
            var buf = Data(); buf.reserveCapacity(1 << 20); var n: Int64 = 0
            for try await b in bytes {
                buf.append(b)
                if buf.count >= 1 << 20 { try out.write(contentsOf: buf); n += Int64(buf.count); buf.removeAll(keepingCapacity: true); progress(n) }
            }
            try out.write(contentsOf: buf); n += Int64(buf.count); progress(n)
        }
        guard try Packs.sha256(of: dest) == sha256.lowercased() else { try? FileManager.default.removeItem(at: dest); throw LauncherError.checksum(name) }
        return dest
    }

    public static func sha256(of file: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: file); defer { try? h.close() }
        var d = SHA256()
        while let chunk = try h.read(upToCount: 1 << 20), !chunk.isEmpty { d.update(data: chunk) }
        return d.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// tar.gz → dir (replacing what was there), with a .unpacked stamp holding the archive's checksum written last
    func unpack(_ file: URL, into dir: URL, sha: String) throws {
        let fm = FileManager.default, part = dir.appendingPathExtension("part")
        try? fm.removeItem(at: part); try fm.createDirectory(at: part, withIntermediateDirectories: true)
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/tar"); p.arguments = ["-xzf", file.path, "-C", part.path]
        try p.run(); p.waitUntilExit()
        guard p.terminationStatus == 0 else { try? fm.removeItem(at: part); throw LauncherError.unpack(file.lastPathComponent) }
        try sha.write(to: part.appendingPathComponent(".unpacked"), atomically: true, encoding: .utf8)
        try? fm.removeItem(at: dir); try fm.createDirectory(at: dir.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.moveItem(at: part, to: dir)
    }

    /// The binary carries a valid Developer ID signature of this team
    public func verifySigned(_ binary: URL, team: String) throws {
        var code: SecStaticCode?, req: SecRequirement?
        let text = "anchor apple generic and certificate leaf[subject.OU] = \"\(team)\""
        guard SecStaticCodeCreateWithPath(binary as CFURL, [], &code) == errSecSuccess, let code,
              SecRequirementCreateWithString(text as CFString, [], &req) == errSecSuccess,
              SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), req) == errSecSuccess
        else { throw LauncherError.signature(binary.lastPathComponent) }
    }
}
