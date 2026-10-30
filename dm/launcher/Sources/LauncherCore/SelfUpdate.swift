import CoreServices
import Darwin
import Foundation
import Security

/// The launcher updating itself, as Battle.net does at start. The release's launcher zip (the manifest's "launcher" entry, made by
/// dm/tools/release_mac.sh) is downloaded and checked like the packs: its checksum, then the app inside: the launcher's bundle id, the build the
/// manifest names and a valid signature (the project's Developer ID when the manifest names a team). It is then swapped with the running app in
/// one atomic exchange (renamex_np RENAME_SWAP: there is never a moment with no launcher or a half-copied one), and the replaced launcher is kept
/// in the root's build/launcher-previous.noindex (out of app search) until the next update.
public struct SelfUpdate: Sendable {
    public static let bundleID = "org.d2rmetal.launcher"
    public var paths: Paths
    public init(paths: Paths) { self.paths = paths }

    var staging: URL { paths.downloads.appendingPathComponent("launcher.noindex", isDirectory: true) }
    public var previous: URL { paths.root.appendingPathComponent("build/launcher-previous.noindex", isDirectory: true) }

    /// Why this copy of the launcher cannot replace itself, or nil when it can: a launcher run from its disk image, from the read-only copy macOS
    /// makes of a quarantined download (App Translocation) or from a folder it cannot write to updates by hand instead
    public static func cannotReplace(_ app: URL) -> String? {
        let p = app.path
        if app.pathExtension != "app" { return "not an app bundle" }
        if p.contains("/AppTranslocation/") { return "macOS runs this copy from a temporary place; move D2R Metal Launcher to Applications" }
        if p.hasPrefix("/Volumes/") && !FileManager.default.isWritableFile(atPath: app.deletingLastPathComponent().path) {
            return "it runs from its disk image; drag it to Applications"
        }
        if !FileManager.default.isWritableFile(atPath: app.deletingLastPathComponent().path) { return "it cannot write to \(app.deletingLastPathComponent().path)" }
        return nil
    }

    /// Downloads and checks the new launcher; returns the unpacked app, ready to swap in
    public func prepare(_ m: Manifest, from manifestURL: URL, session: URLSession = .shared,
                        progress: @escaping @Sendable (Int64) -> Void = { _ in }) async throws -> URL {
        guard let la = m.launcher else { throw LauncherError.badLauncher("the release names no launcher") }
        let zip = try await Packs(paths: paths).download(m.url(of: la.name, url: la.url, manifestURL: manifestURL), name: la.name, sha256: la.sha256,
                                                         session: session, progress: progress)
        defer { try? FileManager.default.removeItem(at: zip) }
        let fm = FileManager.default
        try? fm.removeItem(at: staging); try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto"); p.arguments = ["-x", "-k", zip.path, staging.path]
        try p.run(); p.waitUntilExit()
        guard p.terminationStatus == 0,
              let app = try fm.contentsOfDirectory(at: staging, includingPropertiesForKeys: nil).first(where: { $0.pathExtension == "app" })
        else { throw LauncherError.unpack(la.name) }
        try check(app, build: la.build, team: m.teamID)
        return app
    }

    /// The app is the launcher, of the build the release names, with a valid signature: the team's Developer ID when the manifest names one
    public func check(_ app: URL, build: Int, team: String?) throws {
        let info = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")) ?? [:]
        guard info["CFBundleIdentifier"] as? String == Self.bundleID else { throw LauncherError.badLauncher("it is not D2R Metal Launcher") }
        guard Int(info["CFBundleVersion"] as? String ?? "") == build else {
            throw LauncherError.badLauncher("it is build \(info["CFBundleVersion"] as? String ?? "?"), the release names \(build)")
        }
        var code: SecStaticCode?, req: SecRequirement?
        if let team {
            let text = "anchor apple generic and identifier \"\(Self.bundleID)\" and certificate leaf[subject.OU] = \"\(team)\""
            guard SecRequirementCreateWithString(text as CFString, [], &req) == errSecSuccess else { throw LauncherError.signature(app.lastPathComponent) }
        }
        let flags = SecCSFlags(rawValue: kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate)
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &code) == errSecSuccess, let code,
              SecStaticCodeCheckValidity(code, flags, req) == errSecSuccess
        else { throw LauncherError.signature(app.lastPathComponent) }
    }

    /// Swaps the checked new launcher into the running one's place and keeps the replaced one as the previous launcher. The exchange is one
    /// rename on the running app's volume; a launcher on another volume gets the new one copied beside it first
    public func install(_ new: URL, over current: URL) throws {
        let fm = FileManager.default
        var incoming = new
        if renamex_np(incoming.path, current.path, UInt32(RENAME_SWAP)) != 0 {
            guard errno == EXDEV else { throw LauncherError.cannotReplace(current.deletingLastPathComponent().path, String(cString: strerror(errno))) }
            incoming = current.deletingLastPathComponent().appendingPathComponent(".D2R Metal Launcher.update.app")   // hidden, for a moment
            try? fm.removeItem(at: incoming)
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto"); p.arguments = [new.path, incoming.path]
            try p.run(); p.waitUntilExit()
            guard p.terminationStatus == 0, renamex_np(incoming.path, current.path, UInt32(RENAME_SWAP)) == 0 else {
                let why = String(cString: strerror(errno)); try? fm.removeItem(at: incoming)
                throw LauncherError.cannotReplace(current.deletingLastPathComponent().path, why)
            }
        }
        // incoming now holds the replaced launcher
        try? fm.createDirectory(at: previous, withIntermediateDirectories: true)
        let kept = previous.appendingPathComponent(current.lastPathComponent)
        try? fm.removeItem(at: kept)
        if (try? fm.moveItem(at: incoming, to: kept)) == nil { try? fm.removeItem(at: incoming) }
        try? fm.removeItem(at: staging)
        LSRegisterURL(current as CFURL, true)   // LaunchServices learns the new version at once (its version, icon, URL scheme)
    }

    /// The launcher's own build number, from its Info.plist
    public static func build(of app: URL) -> Int {
        Int((NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist"))?["CFBundleVersion"] as? String) ?? "") ?? 0
    }
}
