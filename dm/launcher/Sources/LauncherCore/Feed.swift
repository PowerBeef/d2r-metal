import Foundation

/// Where releases come from: the project's GitHub release that GitHub calls latest (there is one channel: pre-releases are never offered), or a
/// test feed (DM_LAUNCHER_FEED / DMLauncherFeed: a manifest.json URL, file:// included, whose assets sit beside it)
public struct Feed: Sendable {
    public static let repo = "PowerBeef/d2r-metal"
    public var override: URL?
    public var trust: Trust

    public init(override: URL? = nil, trust: Trust = .current) { self.override = override; self.trust = trust }

    /// The latest release: its manifest, checked (signature, names), and its URL in that release's own folder
    /// (github.com/<repo>/releases/download/<tag>/manifest.json), which the signature and the assets resolve against: one release's files,
    /// even if a newer release is published meanwhile. (Not the URL the bytes finally came from: GitHub serves them from its download host
    /// under an opaque name.)
    public func latest(session: URLSession = .shared) async throws -> (Manifest, URL) {
        let url: URL
        if let override { url = override } else { url = try await latestManifestURL(session: session) }
        let data = try await fetch(url, session: session).0
        try await checkSignature(of: data, at: url, session: session)
        let m: Manifest
        do { m = try JSONDecoder().decode(Manifest.self, from: data) } catch { throw LauncherError.badManifest(String(describing: error)) }
        try m.validate()
        return (m, url)
    }

    /// The release GitHub calls latest, read from the redirect of github.com/<repo>/releases/latest (to .../releases/tag/<tag>)
    /// without following it. No API call, so GitHub's API rate limit does not apply
    func latestManifestURL(session: URLSession) async throws -> URL {
        var req = URLRequest(url: URL(string: "https://github.com/\(Feed.repo)/releases/latest")!)
        req.httpMethod = "HEAD"; req.cachePolicy = .reloadIgnoringLocalCacheData; req.timeoutInterval = 20
        let rsp: URLResponse
        do { rsp = try await session.data(for: req, delegate: NoRedirect()).1 }
        catch let e as URLError where e.code == .cancelled { throw CancellationError() }
        catch let e as URLError where Feed.offlineCodes.contains(e.code) { throw LauncherError.offline }
        guard let http = rsp as? HTTPURLResponse else { throw LauncherError.http(req.url!.absoluteString, 0) }
        if http.statusCode == 429 { throw LauncherError.rateLimited }
        guard (300..<400).contains(http.statusCode) else { throw LauncherError.http(req.url!.absoluteString, http.statusCode) }
        guard let tag = Feed.tag(fromLocation: http.value(forHTTPHeaderField: "Location") ?? "") else { throw LauncherError.noRelease }   // → the release list: none yet
        return URL(string: "https://github.com/\(Feed.repo)/releases/download/\(tag)/manifest.json")!
    }

    /// The tag in a ".../releases/tag/<tag>" redirect; nil for any other place, or a tag that is not a plain name
    static func tag(fromLocation loc: String) -> String? {
        guard let r = loc.range(of: "/releases/tag/") else { return nil }
        let tag = String(loc[r.upperBound...]).split(separator: "?").first.map(String.init) ?? ""
        return !tag.isEmpty && tag.allSatisfy({ $0.isLetter || $0.isNumber || "._-".contains($0) }) && tag != "." && tag != ".." ? tag : nil
    }

    static let offlineCodes: [URLError.Code] = [.notConnectedToInternet, .networkConnectionLost, .cannotFindHost, .cannotConnectToHost, .timedOut, .dnsLookupFailed]

    /// manifest.json.sig beside the manifest, checked against the release keys before any field is read. A release launcher requires it; a
    /// development or test launcher checks it when it is there (a test release made without the key has none)
    func checkSignature(of data: Data, at url: URL, session: URLSession) async throws {
        let sigURL = url.deletingLastPathComponent().appendingPathComponent(url.lastPathComponent + ".sig")
        let sig: Data?
        do { sig = try await fetch(sigURL, session: session).0 }
        catch LauncherError.http(_, 404) { sig = nil }
        catch let e as CocoaError where e.code == .fileReadNoSuchFile { sig = nil }
        guard let sig else {
            if trust.isRelease { throw LauncherError.unsignedManifest }
            return
        }
        guard ManifestSignature.isValid(sig, for: data) else { throw LauncherError.unsignedManifest }
    }

    /// The bytes at url and the URL they finally came from (after redirects)
    func fetch(_ url: URL, session: URLSession) async throws -> (Data, URL) {
        if url.isFileURL { return (try Data(contentsOf: url), url) }
        var req = URLRequest(url: url); req.cachePolicy = .reloadIgnoringLocalCacheData; req.timeoutInterval = 20
        let data: Data, rsp: URLResponse
        do { (data, rsp) = try await session.data(for: req) }
        catch let e as URLError where e.code == .cancelled { throw CancellationError() }
        catch let e as URLError where Feed.offlineCodes.contains(e.code) { throw LauncherError.offline }
        guard let http = rsp as? HTTPURLResponse else { throw LauncherError.http(url.absoluteString, 0) }
        if (http.statusCode == 403 || http.statusCode == 429) && http.value(forHTTPHeaderField: "x-ratelimit-remaining") == "0" || http.statusCode == 429 {
            throw LauncherError.rateLimited
        }
        guard http.statusCode == 200 else { throw LauncherError.http(url.absoluteString, http.statusCode) }
        return (data, http.url ?? url)
    }
}

/// A task delegate that stops at the first redirect (its response is returned as is)
final class NoRedirect: NSObject, URLSessionTaskDelegate, Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest) async -> URLRequest? { nil }
}

public enum LauncherError: LocalizedError, Equatable {
    case http(String, Int), badManifest(String), noRelease, checksum(String), signature(String), unpack(String), engine(String), tooOld(Int)
    case badLauncher(String), cannotReplace(String, String), offline, rateLimited, unsignedManifest, newerFormat, macOS(String), disk(Double, Double)

    public var errorDescription: String? {
        switch self {
        case .http(let u, let c): return "Could not download \(u) (HTTP \(c))."
        case .badManifest(let e): return "The release description could not be read (\(e))."
        case .noRelease: return "No release with a manifest was found on GitHub."
        case .checksum(let n): return "\(n) was damaged on the way (checksum mismatch). Try again."
        case .signature(let n): return "\(n) is not signed by the project; it was not used."
        case .unpack(let n): return "\(n) could not be unpacked."
        case .engine(let m): return m
        case .tooOld(let b): return "This release needs a newer launcher (build \(b) or later). Update the launcher first."
        case .badLauncher(let why): return "The downloaded launcher was not used: \(why)."
        case .cannotReplace(let dir, let why): return "The launcher could not replace itself in \(dir) (\(why)). Download the new one from the releases page."
        case .offline: return "GitHub can't be reached. Check your internet connection."
        case .rateLimited: return "GitHub is limiting how often it can be asked. Try again in a few minutes."
        case .unsignedManifest: return "The release description is not signed by the project, so it was not used."
        case .newerFormat: return "This release needs a newer launcher. Update the launcher first."
        case .macOS(let v): return "This release needs macOS \(v) or later."
        case .disk(let need, let free): return String(format: "Not enough free space: %.1f GB is needed, %.1f GB is free.", need, free)
        }
    }
}
