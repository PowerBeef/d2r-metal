import AppKit
import LauncherCore
import Observation

/// One install / update / repair run and what the window shows of it
struct StepRow: Identifiable, Equatable {
    let id: String
    var title: String
    var state: State = .waiting
    var done = 0, total = 0
    var seconds: Double?
    var typical: Double = 0
    enum State: Equatable { case waiting, running, done, kept, failed }
}

@MainActor @Observable
final class LauncherModel {
    enum Phase: Equatable { case loading, welcome, working, home, failed(String) }

    var phase: Phase = .loading
    var installed: (version: String, build: Int)?
    var latest: Manifest?
    var latestURL: URL?
    var feedError: String?
    var steps: [StepRow] = []
    var working = ""                 // what the running job is, for the header ("Installing", "Updating"…)
    var logPath: String?
    var canRevert = false
    var packs: (runtime: URL, tools: URL)?   // the packs of the installed release (state.json), to run the engine for revert

    // the game files the player picked
    var baseGame: URL?   // the base game package (.nsp, or an .xci that may hold the update too)
    var gameUpdate: URL? // the 1.0.34.0 update package
    var dlc: URL?        // optional: the expansion's data already ships in the base game and the update; unpacking skips it
    var keys: URL?
    var folder: URL?
    var packages: [URL] { [baseGame, gameUpdate, dlc].compactMap { $0 } }
    enum Slot: Hashable { case base, update, dlc, keys, folder }
    var verdicts: [Slot: GameFileCheck.Verdict] = [:]   // what each chosen file was found to be (GameFileCheck), shown beside it
    var checking: Set<Slot> = []

    let paths: Paths
    let defaults = UserDefaults.standard
    private var job: Task<Void, Never>?

    var channel: Channel {
        get { Channel(rawValue: defaults.string(forKey: "Channel") ?? "") ?? .stable }
        set { defaults.set(newValue.rawValue, forKey: "Channel"); Task { await refreshFeed() } }
    }
    var appDir: URL {
        get { URL(fileURLWithPath: defaults.string(forKey: "AppFolder") ?? "/Applications", isDirectory: true) }
        set { defaults.set(newValue.path, forKey: "AppFolder") }
    }
    /// A test feed instead of GitHub: DM_LAUNCHER_FEED (environment) or the DMLauncherFeed default, a manifest.json URL (file:// works)
    var feedOverride: URL? {
        (ProcessInfo.processInfo.environment["DM_LAUNCHER_FEED"] ?? defaults.string(forKey: "DMLauncherFeed")).flatMap(URL.init(string:))
    }
    /// Test installs keep their own data: DM_LAUNCHER_ROOT / DM_LAUNCHER_BUNDLE_ID (never used by players)
    static func makePaths() -> Paths {
        Paths(root: ProcessInfo.processInfo.environment["DM_LAUNCHER_ROOT"].map { URL(fileURLWithPath: $0, isDirectory: true) })
    }
    var bundleID: String? { ProcessInfo.processInfo.environment["DM_LAUNCHER_BUNDLE_ID"] }
    var ownBuild: Int { Int(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "") ?? 0 }

    init() { paths = LauncherModel.makePaths() }

    var updateAvailable: Bool { if let l = latest, let i = installed { return l.build > i.build } else { return false } }
    var launcherUpdate: Manifest.LauncherAsset? { latest?.launcher.flatMap { $0.build > ownBuild ? $0 : nil } }
    var installedApp: URL { appDir.appendingPathComponent("D2R Metal.app") }
    /// Ready when the chosen files passed their checks: an extracted folder, or the base game, the update (unless the base is an .xci) and the keys
    var gameInputReady: Bool {
        guard checking.isEmpty else { return false }
        func good(_ s: Slot) -> Bool { switch verdicts[s] { case .ok?, .note?: true; default: false } }
        if folder != nil { return good(.folder) }
        let xci = baseGame?.pathExtension.lowercased() == "xci"
        return baseGame != nil && good(.base) && keys != nil && good(.keys) && (gameUpdate != nil ? good(.update) : xci) && (dlc == nil || good(.dlc))
    }

    func url(_ slot: Slot) -> URL? {
        switch slot { case .base: baseGame; case .update: gameUpdate; case .dlc: dlc; case .keys: keys; case .folder: folder }
    }
    private func set(_ slot: Slot, _ url: URL?) {
        switch slot { case .base: baseGame = url; case .update: gameUpdate = url; case .dlc: dlc = url; case .keys: keys = url; case .folder: folder = url }
        if url == nil { verdicts[slot] = nil; checking.remove(slot) }
    }

    /// A file for a selector: kept there and checked off the main thread. A package of this game chosen in the wrong selector moves to its own
    /// (when that one is free); packages and an extracted folder are two ways, so the latest choice clears the other
    func choose(_ url: URL, for slot: Slot) {
        set(slot, url); verdicts[slot] = nil; checking.insert(slot)
        if slot == .folder { for s in [Slot.base, .update, .dlc, .keys] { set(s, nil) } } else { set(.folder, nil) }
        Task {
            let v: GameFileCheck.Verdict = await Task.detached {
                switch slot {
                case .keys: GameFileCheck.keys(url)
                case .folder: GameFileCheck.folder(url)
                case .base: GameFileCheck.package(url, expected: .base)
                case .update: GameFileCheck.package(url, expected: .update)
                case .dlc: GameFileCheck.package(url, expected: .dlc)
                }
            }.value
            guard self.url(slot) == url else { return }   // replaced meanwhile
            checking.remove(slot)
            if case .elsewhere(let kind, let what) = v {
                let target: Slot = kind == .base ? .base : kind == .update ? .update : .dlc
                if self.url(target) == nil { set(slot, nil); choose(url, for: target); return }
                verdicts[slot] = .wrong(what + ", and that selector already has a file")
                return
            }
            verdicts[slot] = v
        }
    }
    func clear(_ slot: Slot) { set(slot, nil) }

    func start() async {
        if let preview = ProcessInfo.processInfo.environment["DM_LAUNCHER_PREVIEW"] { showPreview(preview); return }
        readState()
        phase = installed == nil ? .welcome : .home
        await refreshFeed()
    }

    func readState() {
        struct State: Decodable { var release: Rel?; var packs: P?; struct Rel: Decodable { var version: String?; var build: Int? }; struct P: Decodable { var runtime: String; var tools: String } }
        let s = (try? Data(contentsOf: paths.state)).flatMap { try? JSONDecoder().decode(State.self, from: $0) }
        if let v = s?.release?.version, let b = s?.release?.build, FileManager.default.fileExists(atPath: installedApp.path) { installed = (v, b) } else { installed = nil }
        packs = s?.packs.map { (URL(fileURLWithPath: $0.runtime, isDirectory: true), URL(fileURLWithPath: $0.tools, isDirectory: true)) }
        canRevert = FileManager.default.fileExists(atPath: paths.root.appendingPathComponent("build/previous/D2R Metal.app").path)
    }

    func refreshFeed() async {
        do { let (m, u) = try await Feed(channel: channel, override: feedOverride).latest(); latest = m; latestURL = u; feedError = nil }
        catch { feedError = error.localizedDescription }
    }

    // MARK: jobs

    func install() { runJob("Installing", command: "install", input: gameInput()) }
    func update() { runJob("Updating", command: "install", input: .none) }
    func repair() { runJob("Repairing", command: "repair", input: .none) }

    /// The selector a dropped package goes to (PackageKind.guess), nil when its name says nothing (its check then moves it if needed)
    static func slot(for url: URL) -> Slot? {
        switch PackageKind.guess(url.lastPathComponent) { case .base: .base; case .update: .update; case .dlc: .dlc; case nil: nil }
    }

    func gameInput() -> Engine.Input {
        if let folder { return .folder(folder) }
        if let keys, !packages.isEmpty { return .packages(packages, keys: keys) }
        return .none
    }

    func cancel() { job?.cancel() }

    private func runJob(_ what: String, command: String, input: Engine.Input) {
        guard job == nil else { return }
        working = what; phase = .working; logPath = nil
        steps = [StepRow(id: "download", title: "Downloading D2R Metal")] + StepRow.engineSteps
        job = Task {
            defer { job = nil }
            do {
                if latest == nil { await refreshFeed() }
                guard let m = latest, let mu = latestURL else { throw LauncherError.engine(feedError ?? "No release to install.") }
                if let la = m.launcher, la.minBuild > ownBuild { throw LauncherError.tooOld(la.minBuild) }
                mark("download", .running)
                let (rt, tl) = try await Packs(paths: paths).ensure(m, from: mu) { done, total in
                    Task { @MainActor in self.progress("download", Int(done >> 20), Int(total >> 20)) }
                }
                mark("download", .done)
                let engine = Engine(paths: paths, runtime: rt, tools: tl, appDir: appDir, bundleID: bundleID)
                var failure: String?, code: Int32 = 0
                for try await out in engine.run(command, input: input) {
                    switch out {
                    case .event(let ev): failure = apply(ev) ?? failure
                    case .exit(let c): code = c
                    }
                }
                if Task.isCancelled { throw CancellationError() }
                if code != 0 { throw LauncherError.engine(failure ?? "The install stopped (code \(code)). The log has the details.") }
                readState(); phase = .home
            } catch is CancellationError {
                readState(); phase = .failed("Stopped. Run it again to continue where it left off.")
            } catch {
                readState(); phase = .failed(error.localizedDescription)
            }
        }
    }

    /// An engine event → the step rows; returns the error text of an error event
    private func apply(_ ev: EngineEvent) -> String? {
        if let l = ev.log { logPath = l }
        switch ev.event {
        case "step":
            guard let s = ev.step else { break }
            switch ev.state {
            case "start": mark(s, .running); if let t = ev.typicalS, let i = steps.firstIndex(where: { $0.id == s }) { steps[i].typical = t }
            case "done": mark(s, .done, seconds: ev.seconds)
            case "kept": mark(s, .kept, seconds: ev.seconds)
            case "fail": mark(s, .failed)
            default: break
            }
        case "progress": if let s = ev.step, let d = ev.done, let t = ev.total { progress(s, d, t) }
        case "error": return ev.msg
        default: break
        }
        return nil
    }

    private func mark(_ id: String, _ st: StepRow.State, seconds: Double? = nil) {
        guard let i = steps.firstIndex(where: { $0.id == id }) else { return }
        steps[i].state = st; if let seconds { steps[i].seconds = seconds }
    }
    private func progress(_ id: String, _ done: Int, _ total: Int) {
        guard let i = steps.firstIndex(where: { $0.id == id }) else { return }
        steps[i].done = done; steps[i].total = total
    }

    // MARK: other actions

    func play() {
        NSWorkspace.shared.openApplication(at: installedApp, configuration: NSWorkspace.OpenConfiguration()) { _, error in
            Task { @MainActor in
                if let error { self.phase = .failed("D2R Metal did not open: \(error.localizedDescription)") } else { NSApp.terminate(nil) }
            }
        }
    }

    func revert() {
        guard let packs else { return }
        let engine = Engine(paths: paths, runtime: packs.runtime, tools: packs.tools, appDir: appDir)
        working = "Reverting"; phase = .working; steps = []
        job = Task {
            defer { job = nil }
            var code: Int32 = 0, msg: String?
            do {
                for try await out in engine.run("revert") {
                    switch out { case .exit(let c): code = c; case .event(let ev): if ev.event == "error" { msg = ev.msg } }
                }
            } catch { msg = error.localizedDescription; code = 1 }
            readState(); phase = code == 0 ? .home : .failed(msg ?? "Could not go back to the previous version.")
        }
    }

    /// Moves the game files and build outputs to the Trash (recoverable). Saves live elsewhere and are never touched
    func removeGameData() {
        let fm = FileManager.default
        let items = ["data", "build", "releases", "tools", "downloads"].map { paths.root.appendingPathComponent($0) }.filter { fm.fileExists(atPath: $0.path) }
        NSWorkspace.shared.recycle(items + (managedApp ? [installedApp] : [])) { _, error in
            Task { @MainActor in
                if let error { self.phase = .failed("Could not move the game data to the Trash: \(error.localizedDescription)") }
                else { self.readState(); self.phase = .welcome }
            }
        }
    }
    var managedApp: Bool {
        (NSDictionary(contentsOf: installedApp.appendingPathComponent("Contents/Info.plist"))?["DMLauncherManaged"] as? Bool) ?? false
    }

    func showLog() { if let l = logPath { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: l)]) } }
}

extension LauncherModel {
    /// DM_LAUNCHER_PREVIEW=welcome|empty|working|home|failed: one screen with sample data and nothing run, to look at the design (screenshots, docs)
    func showPreview(_ which: String) {
        let json = #"""
        {"schema": 1, "version": "1.1.0", "build": 420, "commit": "abc1234", "date": "2026-11-02", "macos_min": "27.0", "chain": "", "product": "",
         "notes": "**Sharper shadows** on characters, and a faster first launch. See the release page for everything in this version.",
         "runtime": {"name": "", "version": "1.1.0", "sha256": "", "size": 0}, "tools": {"name": "", "version": "1", "sha256": "", "size": 0}}
        """#
        let sample = try? JSONDecoder().decode(Manifest.self, from: Data(json.utf8))
        switch which {
        case "working":
            working = "Installing"; phase = .working
            steps = [StepRow(id: "download", title: "Downloading D2R Metal")] + StepRow.engineSteps
            for (i, seconds) in [14.0, 160, 13, 21].enumerated() { steps[i].state = .done; steps[i].seconds = seconds }   // download … packs
            if let i = steps.firstIndex(where: { $0.id == "shaders" }) { steps[i].state = .running; steps[i].done = 4224; steps[i].total = 6927 }
        case "home": installed = ("1.0.0", 412); latest = sample; canRevert = true; phase = .home
        case "empty": latest = sample; phase = .welcome
        case "failed": phase = .failed("The game files in the folder you chose are not from version 1.0.34.0 (ExeFS/main does not match). Choose the files of the 1.0.34.0 update.")
        default: baseGame = URL(fileURLWithPath: "/Volumes/Games/Diablo II Resurrected [0100726014352000][v0].nsp")
                 gameUpdate = URL(fileURLWithPath: "/Volumes/Games/Diablo II Resurrected [0100726014352800][v2228224].nsp")
                 keys = URL(fileURLWithPath: "/Users/player/.switch/prod.keys")
                 verdicts = [.base: .ok("Base game, title 0100726014352000"), .update: .ok("Update, title 0100726014352800 · version checked at install"),
                             .keys: .note("Console keys, 412 entries; 2 lines of the wrong length will be skipped")]
                 latest = sample; phase = .welcome
        }
    }
}

extension StepRow {
    static let engineSteps: [StepRow] = [
        StepRow(id: "unpack", title: "Unpacking your game files"),
        StepRow(id: "product", title: "Preparing the game program"),
        StepRow(id: "packs", title: "Extracting the shaders"),
        StepRow(id: "shaders", title: "Translating the shaders"),
        StepRow(id: "c1", title: "Checking the shaders"),
        StepRow(id: "overlay", title: "Building the interface"),
        StepRow(id: "tables", title: "Reading the game tables"),
        StepRow(id: "app", title: "Assembling D2R Metal"),
        StepRow(id: "warm", title: "Compiling the shaders for this Mac"),
    ]
}
