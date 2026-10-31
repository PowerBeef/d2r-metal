import Foundation

/// A release as dm/tools/release_mac.sh describes it in the release's manifest.json (also copied into the runtime pack, where install.py reads it).
/// Decoding is tolerant: only what installing needs is required, so a later release that adds or drops a descriptive field still decodes, and a
/// release of a newer format (schema) still yields its launcher entry, so this launcher can update itself to one that reads it
public struct Manifest: Codable, Equatable, Sendable {
    /// The newest manifest format this launcher installs from
    public static let supportedSchema = 1

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
        static let none = Asset(name: "", url: nil, version: "", sha256: "", size: 0)
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

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        schema = try c.decodeIfPresent(Int.self, forKey: .schema) ?? 1
        launcher = try? c.decodeIfPresent(LauncherAsset.self, forKey: .launcher)   // first, and never fatal: a newer format still names its launcher
        version = try c.decode(String.self, forKey: .version); build = try c.decode(Int.self, forKey: .build)
        commit = (try? c.decodeIfPresent(String.self, forKey: .commit)) ?? ""
        date = (try? c.decodeIfPresent(String.self, forKey: .date)) ?? ""
        notes = (try? c.decodeIfPresent(String.self, forKey: .notes)) ?? ""
        macosMin = (try? c.decodeIfPresent(String.self, forKey: .macosMin)) ?? "27.0"
        updateURL = try? c.decodeIfPresent(String.self, forKey: .updateURL)
        chain = (try? c.decodeIfPresent(String.self, forKey: .chain)) ?? ""
        product = (try? c.decodeIfPresent(String.self, forKey: .product)) ?? ""
        teamID = try? c.decodeIfPresent(String.self, forKey: .teamID)
        if schema > Manifest.supportedSchema {   // a format this launcher does not install from: only its launcher entry is used (installable is false)
            runtime = (try? c.decode(Asset.self, forKey: .runtime)) ?? .none; tools = (try? c.decode(Asset.self, forKey: .tools)) ?? .none
        } else {
            runtime = try c.decode(Asset.self, forKey: .runtime); tools = try c.decode(Asset.self, forKey: .tools)
        }
    }

    /// This launcher can install the release (its format, and its packs named)
    public var installable: Bool { schema <= Manifest.supportedSchema && !runtime.name.isEmpty && !tools.name.isEmpty }

    /// The release runs on this Mac's macOS (its macos_min)
    public var runsOnThisMac: Bool {
        let p = macosMin.split(separator: ".").map { Int($0) ?? 0 }
        return ProcessInfo.processInfo.isOperatingSystemAtLeast(OperatingSystemVersion(majorVersion: p.first ?? 0, minorVersion: p.count > 1 ? p[1] : 0,
                                                                                       patchVersion: p.count > 2 ? p[2] : 0))
    }

    /// The names and versions the launcher turns into paths (downloads/<name>, releases/<version>, tools/<version>) are single, plain path
    /// components: a manifest can never make it write outside its own folders
    public func validate() throws {
        func plain(_ s: String, _ what: String) throws {
            guard !s.isEmpty, s != ".", s != "..", !s.hasPrefix("."), !s.contains("/"), !s.contains("\\"), !s.contains("\0"), !s.contains(":"),
                  s.count <= 200 else { throw LauncherError.badManifest("unsafe \(what) \"\(s)\"") }
        }
        if installable { try plain(runtime.name, "runtime name"); try plain(runtime.version, "runtime version"); try plain(tools.name, "tools name"); try plain(tools.version, "tools version") }
        if let l = launcher { try plain(l.name, "launcher name") }
    }

    /// Where an asset is: its own url, else beside the manifest (the URL the manifest was finally read from, after redirects, so a moving
    /// "latest" link can't mix two releases). An asset of a release read over the network must come over https
    public func assetURL(_ name: String, url: String?, manifestURL: URL) throws -> URL {
        let u: URL
        if let url { guard let x = URL(string: url) else { throw LauncherError.badManifest("bad url \(url)") }; u = x }
        else { u = manifestURL.deletingLastPathComponent().appendingPathComponent(name) }
        let local = manifestURL.isFileURL && u.isFileURL
        guard local || u.scheme?.lowercased() == "https" else { throw LauncherError.badManifest("\(name) is not served over https") }
        return u
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
    public var code: String?             // for error: arch, macos, disk, tools, runtime (preflight); busy, interrupted, stalled, game_damaged, no_game, access, internal
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
