import Foundation

/// Where releases come from: the project's GitHub releases (stable = the latest release, pre-release = the newest of any kind), or a test feed
/// (DM_LAUNCHER_FEED / DMLauncherFeed: a manifest.json URL, file:// included, whose assets sit beside it)
public enum Channel: String, CaseIterable, Sendable { case stable, prerelease }

public struct Feed: Sendable {
    public static let repo = "PowerBeef/d2r-metal"
    public var channel: Channel
    public var override: URL?
    public var trust: Trust

    public init(channel: Channel, override: URL? = nil, trust: Trust = .current) { self.channel = channel; self.override = override; self.trust = trust }

    /// The newest release of the channel: its manifest, checked (signature, names), and the URL it was finally read from (assets resolve against it)
    public func latest(session: URLSession = .shared) async throws -> (Manifest, URL) {
        let url: URL
        if let override { url = override }
        else if channel == .stable { url = URL(string: "https://github.com/\(Feed.repo)/releases/latest/download/manifest.json")! }
        else { url = try await newestManifestURL(session: session) }
        let (data, final) = try await fetch(url, session: session)
        try await checkSignature(of: data, at: final, session: session)
        let m: Manifest
        do { m = try JSONDecoder().decode(Manifest.self, from: data) } catch { throw LauncherError.badManifest(String(describing: error)) }
        try m.validate()
        return (m, final)
    }

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

    func newestManifestURL(session: URLSession) async throws -> URL {
        struct Release: Decodable { var draft: Bool; var assets: [A]; struct A: Decodable { var name: String; var browser_download_url: String } }
        let api = URL(string: "https://api.github.com/repos/\(Feed.repo)/releases?per_page=10")!
        let rel = try JSONDecoder().decode([Release].self, from: try await fetch(api, session: session).0)   // newest first
        for r in rel where !r.draft {
            if let a = r.assets.first(where: { $0.name == "manifest.json" }), let u = URL(string: a.browser_download_url) { return u }
        }
        throw LauncherError.noRelease
    }

    /// The bytes at url and the URL they finally came from (after redirects)
    func fetch(_ url: URL, session: URLSession) async throws -> (Data, URL) {
        if url.isFileURL { return (try Data(contentsOf: url), url) }
        var req = URLRequest(url: url); req.cachePolicy = .reloadIgnoringLocalCacheData; req.timeoutInterval = 20
        let data: Data, rsp: URLResponse
        do { (data, rsp) = try await session.data(for: req) }
        catch let e as URLError where e.code == .cancelled { throw CancellationError() }
        catch let e as URLError where [.notConnectedToInternet, .networkConnectionLost, .cannotFindHost, .cannotConnectToHost, .timedOut, .dnsLookupFailed].contains(e.code) {
            throw LauncherError.offline
        }
        guard let http = rsp as? HTTPURLResponse else { throw LauncherError.http(url.absoluteString, 0) }
        if (http.statusCode == 403 || http.statusCode == 429) && http.value(forHTTPHeaderField: "x-ratelimit-remaining") == "0" || http.statusCode == 429 {
            throw LauncherError.rateLimited
        }
        guard http.statusCode == 200 else { throw LauncherError.http(url.absoluteString, http.statusCode) }
        return (data, http.url ?? url)
    }
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
