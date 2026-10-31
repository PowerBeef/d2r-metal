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
    /// Where the game app goes: the launcher starts it (as Battle.net starts a game), so it stays out of /Applications; the .noindex suffix keeps it
    /// out of Spotlight and app search, which do index a plain folder under Application Support
    public var gameApps: URL { root.appendingPathComponent("Game.noindex", isDirectory: true) }
    /// The app an install replaced, kept for "previous version" (install.py previous_app); build/previous held it before 0.9.7
    public var previousApp: URL { root.appendingPathComponent("build/previous.noindex/D2R Metal.app") }
    public var legacyPreviousApp: URL { root.appendingPathComponent("build/previous/D2R Metal.app") }
    public var logs: URL { root.appendingPathComponent("logs", isDirectory: true) }

    /// Free space where the root is (what macOS would make available for an important download)
    public func freeBytes() -> Int64 {
        var u = root
        while !FileManager.default.fileExists(atPath: u.path), u.path != "/" { u = u.deletingLastPathComponent() }
        return (try? u.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? Int64.max
    }
}

/// Downloads, checks and unpacks the packs a release names. URLSession downloads carry no quarantine flag, and every file is checked against the
/// manifest's sha256; the packs' main binaries must also carry the expected team's Developer ID signature (Trust.team(for:))
public struct Packs: Sendable {
    public var paths: Paths
    public var trust: Trust
    public init(paths: Paths, trust: Trust = .current) { self.paths = paths; self.trust = trust }

    /// Makes sure both packs of the release are unpacked; progress(bytes done, bytes total) during downloads. verify (Repair): the unpacked files
    /// are checked against the list made when they were unpacked, and a pack that differs is downloaded again. Returns (runtime dir, tools dir)
    public func ensure(_ m: Manifest, from manifestURL: URL, verify: Bool = false, session: URLSession = .shared,
                       progress: @escaping @Sendable (Int64, Int64) -> Void = { _, _ in }) async throws -> (URL, URL) {
        guard m.installable else { throw LauncherError.newerFormat }
        try m.validate()
        let rt = paths.runtime(m.runtime.version), tl = paths.tools(m.tools.version)
        var todo: [(Manifest.Asset, URL)] = []
        for (asset, dir) in [(m.runtime, rt), (m.tools, tl)] {
            if !isUnpacked(dir, sha: asset.sha256) || (verify && Packs.contentsIntact(dir) != true) { todo.append((asset, dir)) }
        }
        let total = todo.reduce(Int64(0)) { $0 + $1.0.size }
        let need = Double(total) * 5 / 1e9 + 0.5   // the archive, its unpacked files (up to four times as big) and some room
        if need > 0, Double(paths.freeBytes()) / 1e9 < need { throw LauncherError.disk(need, Double(paths.freeBytes()) / 1e9) }
        var before: Int64 = 0
        for (asset, dir) in todo {
            let base = before
            let file = try await download(try m.assetURL(asset.name, url: asset.url, manifestURL: manifestURL), name: asset.name, sha256: asset.sha256,
                                          session: session) { done in progress(base + done, total) }
            defer { try? FileManager.default.removeItem(at: file) }
            try Task.checkCancellation()
            try unpack(file, into: dir, sha: asset.sha256)
            before += asset.size
        }
        if let team = trust.team(for: m) {
            try verifySigned(rt.appendingPathComponent("host/D2RMobile"), team: team)
            try verifySigned(tl.appendingPathComponent("python/bin/python3"), team: team)
        }
        return (rt, tl)
    }

    func isUnpacked(_ dir: URL, sha: String) -> Bool {
        (try? String(contentsOf: dir.appendingPathComponent(".unpacked"), encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines) == sha
    }

    /// Downloads a release file into downloads/ (a URLSession download task: no byte-by-byte loop; an interrupted download resumes from where it
    /// stopped when the server allows it) and checks its sha256; a failed or damaged download leaves nothing behind but the resume data
    public func download(_ url: URL, name: String, sha256: String, session: URLSession = .shared,
                         progress: @escaping @Sendable (Int64) -> Void = { _ in }) async throws -> URL {
        let fm = FileManager.default
        try fm.createDirectory(at: paths.downloads, withIntermediateDirectories: true)
        let dest = paths.downloads.appendingPathComponent(name), resume = paths.downloads.appendingPathComponent(name + ".resume")
        try? fm.removeItem(at: dest)
        if url.isFileURL { try fm.copyItem(at: url, to: dest) }
        else {
            let watcher = ProgressWatcher(progress)
            let tmp: URL, rsp: URLResponse
            do {
                if let data = try? Data(contentsOf: resume) {
                    try? fm.removeItem(at: resume)
                    do { (tmp, rsp) = try await session.download(resumeFrom: data, delegate: watcher) }
                    catch let e as URLError where e.code != .cancelled { (tmp, rsp) = try await session.download(from: url, delegate: watcher) }   // stale resume data
                } else { (tmp, rsp) = try await session.download(from: url, delegate: watcher) }
            } catch let e as URLError {
                if let data = e.downloadTaskResumeData { try? data.write(to: resume) }
                if e.code == .cancelled { throw CancellationError() }
                if [.notConnectedToInternet, .networkConnectionLost, .cannotFindHost, .cannotConnectToHost, .timedOut, .dnsLookupFailed].contains(e.code) { throw LauncherError.offline }
                throw e
            }
            guard let http = rsp as? HTTPURLResponse, http.statusCode == 200 || http.statusCode == 206 else {
                try? fm.removeItem(at: tmp); throw LauncherError.http(url.absoluteString, (rsp as? HTTPURLResponse)?.statusCode ?? 0)
            }
            try fm.moveItem(at: tmp, to: dest)
        }
        guard try Packs.sha256(of: dest) == sha256.lowercased() else { try? fm.removeItem(at: dest); throw LauncherError.checksum(name) }
        return dest
    }

    public static func sha256(of file: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: file); defer { try? h.close() }
        var d = SHA256()
        while let chunk = try h.read(upToCount: 1 << 20), !chunk.isEmpty { d.update(data: chunk) }
        return d.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// tar.gz → dir (replacing what was there), with the list of its files' checksums (.files) and a .unpacked stamp holding the archive's
    /// checksum, written last
    func unpack(_ file: URL, into dir: URL, sha: String) throws {
        let fm = FileManager.default, part = dir.appendingPathExtension("part")
        try? fm.removeItem(at: part); try fm.createDirectory(at: part, withIntermediateDirectories: true)
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/tar"); p.arguments = ["-xzf", file.path, "-C", part.path]
        p.standardError = FileHandle.nullDevice
        try p.run(); p.waitUntilExit()
        guard p.terminationStatus == 0 else { try? fm.removeItem(at: part); throw LauncherError.unpack(file.lastPathComponent) }
        try Packs.fileList(part).write(to: part.appendingPathComponent(".files"), atomically: true, encoding: .utf8)
        try sha.write(to: part.appendingPathComponent(".unpacked"), atomically: true, encoding: .utf8)
        try? fm.removeItem(at: dir); try fm.createDirectory(at: dir.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.moveItem(at: part, to: dir)
    }

    /// "<sha256>  <relative path>" for every regular file under dir (sorted; links and the two stamp files left out)
    static func fileList(_ dir: URL) throws -> String {
        let fm = FileManager.default
        guard let e = fm.enumerator(at: dir, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey]) else { return "" }
        var lines: [String] = []
        let base = dir.standardizedFileURL.path + "/"
        for case let u as URL in e {
            let v = try u.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            guard v.isRegularFile == true, v.isSymbolicLink != true else { continue }
            let rel = String(u.standardizedFileURL.path.dropFirst(base.count))
            if rel == ".files" || rel == ".unpacked" { continue }
            lines.append("\(try sha256(of: u))  \(rel)")
        }
        return lines.sorted().joined(separator: "\n") + "\n"
    }

    /// Every file of an unpacked pack is still as it was unpacked; nil for a pack unpacked before the list existed (it can't be checked)
    public static func contentsIntact(_ dir: URL) -> Bool? {
        guard let want = try? String(contentsOf: dir.appendingPathComponent(".files"), encoding: .utf8) else { return nil }
        guard let have = try? fileList(dir) else { return false }
        return want == have
    }

    /// The binary carries a valid Developer ID signature of this team
    public func verifySigned(_ binary: URL, team: String) throws {
        var code: SecStaticCode?, req: SecRequirement?
        guard SecStaticCodeCreateWithPath(binary as CFURL, [], &code) == errSecSuccess, let code,
              SecRequirementCreateWithString(Trust.requirement(team: team) as CFString, [], &req) == errSecSuccess,
              SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), req) == errSecSuccess
        else { throw LauncherError.signature(binary.lastPathComponent) }
    }
}

/// A download task's byte count, reported as it grows
final class ProgressWatcher: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    let report: @Sendable (Int64) -> Void
    private var observation: NSKeyValueObservation?
    init(_ report: @escaping @Sendable (Int64) -> Void) { self.report = report }
    func urlSession(_ session: URLSession, didCreateTask task: URLSessionTask) {
        observation = task.progress.observe(\.completedUnitCount, options: [.new]) { [report] p, _ in report(p.completedUnitCount) }
    }
}
