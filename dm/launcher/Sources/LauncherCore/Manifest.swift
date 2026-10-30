import Foundation

/// A release as dm/tools/release_mac.sh describes it in the release's manifest.json (also copied into the runtime pack, where install.py reads it).
public struct Manifest: Codable, Equatable, Sendable {
    public var schema: Int
    public var version: String           // shown to the player: "1.2.0"
    public var build: Int                // monotonic (git rev-list --count): what update checks compare
    public var commit: String
    public var date: String
    public var notes: String             // release notes, plain text / light Markdown
    public var macosMin: String          // the host's minimum system, e.g. "27.0"
    public var updateURL: String?        // the in-game notice's version file (SWUpdateURL of the installed app)
    public var chain: String             // shader chain version of the runtime pack (b-<chain>)
    public var product: String           // rewrite product, e.g. 3.3.94012-r49
    public var teamID: String?           // Developer ID team that signed the packs' binaries; nil = unsigned (a local test feed)
    public var runtime: Asset
    public var tools: Asset
    public var launcher: LauncherAsset?

    public struct Asset: Codable, Equatable, Sendable {
        public var name: String          // file name; resolved next to the manifest unless url is given
        public var url: String?
        public var version: String       // tools: the tools pack version (kept across releases); runtime: the release version
        public var sha256: String
        public var size: Int64
    }
    public struct LauncherAsset: Codable, Equatable, Sendable {
        public var version: String
        public var build: Int
        public var minBuild: Int         // launchers older than this cannot drive this release (install format change): they must update first
        public var name: String
        public var url: String?
        public var sha256: String
        public var size: Int64
    }

    enum CodingKeys: String, CodingKey {
        case schema, version, build, commit, date, notes, chain, product, runtime, tools, launcher
        case macosMin = "macos_min", updateURL = "update_url", teamID = "team_id"
    }
    enum LauncherKeys: String, CodingKey { case version, build, minBuild = "min_build", name, url, sha256, size }

    /// Where an asset is: its own url, else beside the manifest
    public func url(of name: String, url: String?, manifestURL: URL) -> URL {
        if let url, let u = URL(string: url) { return u }
        return manifestURL.deletingLastPathComponent().appendingPathComponent(name)
    }
}

extension Manifest.LauncherAsset {
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: Manifest.LauncherKeys.self)
        version = try c.decode(String.self, forKey: .version); build = try c.decode(Int.self, forKey: .build)
        minBuild = try c.decodeIfPresent(Int.self, forKey: .minBuild) ?? 0; name = try c.decode(String.self, forKey: .name)
        url = try c.decodeIfPresent(String.self, forKey: .url); sha256 = try c.decode(String.self, forKey: .sha256); size = try c.decode(Int64.self, forKey: .size)
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: Manifest.LauncherKeys.self)
        try c.encode(version, forKey: .version); try c.encode(build, forKey: .build); try c.encode(minBuild, forKey: .minBuild); try c.encode(name, forKey: .name)
        try c.encodeIfPresent(url, forKey: .url); try c.encode(sha256, forKey: .sha256); try c.encode(size, forKey: .size)
    }
}

/// What install.py prints with --json, one object per line
public struct EngineEvent: Decodable, Equatable, Sendable {
    public var event: String             // start, step, progress, error, note, done
    public var step: String?
    public var state: String?            // for step: start, done, kept, fail
    public var msg: String?
    public var code: String?             // for error: arch, macos, disk, tools, runtime (preflight)
    public var done: Int?
    public var total: Int?
    public var seconds: Double?
    public var typicalS: Double?
    public var log: String?
    public var app: String?

    enum CodingKeys: String, CodingKey { case event, step, state, msg, code, done, total, seconds, log, app, typicalS = "typical_s" }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        event = try c.decode(String.self, forKey: .event)
        step = try c.decodeIfPresent(String.self, forKey: .step); state = try c.decodeIfPresent(String.self, forKey: .state)
        msg = try c.decodeIfPresent(String.self, forKey: .msg); code = try c.decodeIfPresent(String.self, forKey: .code)
        done = try c.decodeIfPresent(Int.self, forKey: .done); total = try c.decodeIfPresent(Int.self, forKey: .total)
        seconds = try c.decodeIfPresent(Double.self, forKey: .seconds); log = try c.decodeIfPresent(String.self, forKey: .log)
        app = try c.decodeIfPresent(String.self, forKey: .app)
        typicalS = try? c.decodeIfPresent(Double.self, forKey: .typicalS)   // a number on step events, a table on start
    }

    public static func parse(_ line: Substring) -> EngineEvent? {
        guard line.first == "{" else { return nil }
        return try? JSONDecoder().decode(EngineEvent.self, from: Data(line.utf8))
    }
}
