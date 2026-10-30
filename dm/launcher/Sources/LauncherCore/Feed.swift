import Foundation

/// Where releases come from: the project's GitHub releases (stable = the latest release, pre-release = the newest of any kind), or a test feed
/// (DMLauncherFeed: a manifest.json URL, file:// included, whose assets sit beside it)
public enum Channel: String, CaseIterable, Sendable { case stable, prerelease }

public struct Feed: Sendable {
    public static let repo = "PowerBeef/d2r-metal"
    public var channel: Channel
    public var override: URL?

    public init(channel: Channel, override: URL? = nil) { self.channel = channel; self.override = override }

    /// The newest release of the channel: its manifest and the URL it was read from (assets resolve against it)
    public func latest(session: URLSession = .shared) async throws -> (Manifest, URL) {
        let url: URL
        if let override { url = override }
        else if channel == .stable { url = URL(string: "https://github.com/\(Feed.repo)/releases/latest/download/manifest.json")! }
        else { url = try await newestManifestURL(session: session) }
        let data = try await fetch(url, session: session)
        do { return (try JSONDecoder().decode(Manifest.self, from: data), url) }
        catch { throw LauncherError.badManifest(String(describing: error)) }
    }

    func newestManifestURL(session: URLSession) async throws -> URL {
        struct Release: Decodable { var draft: Bool; var assets: [A]; struct A: Decodable { var name: String; var browser_download_url: String } }
        let api = URL(string: "https://api.github.com/repos/\(Feed.repo)/releases?per_page=10")!
        let rel = try JSONDecoder().decode([Release].self, from: try await fetch(api, session: session))   // newest first
        for r in rel where !r.draft {
            if let a = r.assets.first(where: { $0.name == "manifest.json" }), let u = URL(string: a.browser_download_url) { return u }
        }
        throw LauncherError.noRelease
    }

    func fetch(_ url: URL, session: URLSession) async throws -> Data {
        if url.isFileURL { return try Data(contentsOf: url) }
        var req = URLRequest(url: url); req.cachePolicy = .reloadIgnoringLocalCacheData; req.timeoutInterval = 20
        let (data, rsp) = try await session.data(for: req)
        guard let http = rsp as? HTTPURLResponse, http.statusCode == 200 else { throw LauncherError.http(url.absoluteString, (rsp as? HTTPURLResponse)?.statusCode ?? 0) }
        return data
    }
}

public enum LauncherError: LocalizedError, Equatable {
    case http(String, Int), badManifest(String), noRelease, checksum(String), signature(String), unpack(String), engine(String), tooOld(Int)

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
        }
    }
}
