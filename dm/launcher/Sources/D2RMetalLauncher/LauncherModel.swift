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
    var launcherNotice: String?      // why a newer launcher could not be installed (shown at the top of the panel)
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
    /// Where installs put the game app: the launcher's own Game folder (it starts the game, so the game stays out of /Applications and app search)
    var appDir: URL {
        get { defaults.string(forKey: "AppFolder").map { URL(fileURLWithPath: $0, isDirectory: true) } ?? paths.gameApps }
        set { defaults.set(newValue.path, forKey: "AppFolder") }
    }
    var appPath: URL?   // where the last install put the game app (state.json): launcher 0.9.6 and earlier put it in /Applications
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
    var installedApp: URL { appPath ?? appDir.appendingPathComponent("D2R Metal.app") }
    /// An install whose app is still where an older launcher put it: the next start moves it into the Game folder (relocate)
    var appNeedsMove: Bool { installed != nil && managedApp && installedApp.deletingLastPathComponent().standardizedFileURL != appDir.standardizedFileURL }
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
        tidyPrevious()
        readState()
        phase = installed == nil ? .welcome : .home
        if appNeedsMove { relocate() }
        await refreshFeed()
        let auto = ProcessInfo.processInfo.environment["DM_LAUNCHER_AUTORUN"]
        if auto == nil || auto == "selfupdate" {   // a test of another job runs the launcher it was given
            while job != nil { try? await Task.sleep(for: .milliseconds(200)) }
            if launcherUpdate != nil {
                let ok = await updateLauncher()
                if auto == "selfupdate" {
                    print("LAUNCHER-AUTORUN \(ok ? "OK" : "RED") job=selfupdate from=\(ownBuild) to=\(SelfUpdate.build(of: Bundle.main.bundleURL))\(launcherNotice.map { " error=\($0)" } ?? "")")
                    fflush(stdout)
                }
                if ok { relaunch(); return }
                if auto == "selfupdate" { exit(1) }
            } else if auto == "selfupdate" { print("LAUNCHER-AUTORUN RED job=selfupdate no newer launcher in the feed"); fflush(stdout); exit(1) }
        }
        if let what = auto, what != "selfupdate" { await autorun(what) }
    }

    /// The gear menu's Check for Updates: the release feed again, and a newer launcher installed (then a restart into it) when there is one
    func checkForUpdates() async {
        await refreshFeed()
        if launcherUpdate != nil, await updateLauncher() { relaunch() }
    }

    // MARK: the launcher's own update

    /// Installs the newer launcher the release names (SelfUpdate: download, checksum, signature, an atomic swap with this app), with its own
    /// screen; true when the new one is in place (the caller restarts into it). When it cannot (run from its disk image, a folder it cannot write
    /// to, a failed check) it says why at the top of the panel and this launcher carries on, unless the release needs a newer one (min_build)
    func updateLauncher() async -> Bool {
        guard let m = latest, let mu = latestURL, let la = m.launcher, la.build > ownBuild, job == nil else { return false }
        let me = Bundle.main.bundleURL
        if let why = SelfUpdate.cannotReplace(me) {
            launcherNotice = "A new launcher (\(la.version)) is out, but this copy cannot update itself: \(why)."
            if la.minBuild > ownBuild { phase = .failed(LauncherError.tooOld(la.minBuild).localizedDescription + " " + launcherNotice!) }
            return false
        }
        let before = phase
        working = "Updating the launcher"; phase = .working; logPath = nil
        steps = [StepRow(id: "ldownload", title: "Downloading launcher \(la.version)", typical: 5),
                 StepRow(id: "linstall", title: "Checking and installing it", typical: 2)]
        do {
            mark("ldownload", .running)
            let su = SelfUpdate(paths: paths)
            let app = try await su.prepare(m, from: mu) { done in Task { @MainActor in self.progress("ldownload", Int(done >> 20), max(1, Int(la.size >> 20))) } }
            mark("ldownload", .done); mark("linstall", .running)
            try su.install(app, over: me)
            mark("linstall", .done)
            return true
        } catch {
            launcherNotice = "Couldn't update the launcher to \(la.version): \(error.localizedDescription)"
            phase = la.minBuild > ownBuild ? .failed(LauncherError.tooOld(la.minBuild).localizedDescription + " " + launcherNotice!) : before
            return false
        }
    }

    /// Quits and opens the launcher again from its own path, which now holds the new one: a shell waits for this process to end, then opens it.
    /// `open` hands its whole environment to the app it starts, so the test root and feed (DM_LAUNCHER_ROOT, _FEED, _BUNDLE_ID) carry over and
    /// a test launcher keeps its test install; the one-shot switches (DM_LAUNCHER_AUTORUN, _PREVIEW) are taken out
    func relaunch() {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "while /bin/kill -0 \"$1\" 2>/dev/null; do /bin/sleep 0.2; done; exec /usr/bin/open -n \"$2\"", "sh",
                       String(getpid()), Bundle.main.bundlePath]
        p.environment = ProcessInfo.processInfo.environment.filter { $0.key != "DM_LAUNCHER_AUTORUN" && $0.key != "DM_LAUNCHER_PREVIEW" }
        p.standardInput = FileHandle.nullDevice; p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        try? p.run()
        NSApp.terminate(nil)
    }

    /// DM_LAUNCHER_AUTORUN=update|repair, for tests (dm/verify/launcher_update.py) on a test root: runs that job as its button does, then prints
    /// `LAUNCHER-AUTORUN OK|RED …` and quits with 0 / 1. OK = back on the home screen with the feed's release installed
    func autorun(_ what: String) async {
        while job != nil { try? await Task.sleep(for: .milliseconds(200)) }   // a move into the Game folder runs first
        switch what {
        case "update": update()
        case "repair": repair()
        default: print("LAUNCHER-AUTORUN RED unknown job \(what)"); fflush(stdout); exit(2)
        }
        while job != nil { try? await Task.sleep(for: .milliseconds(300)) }
        try? await Task.sleep(for: .seconds(2))   // the finished screen stays a moment (screenshots)
        let ok = phase == .home && installed != nil && installed?.build == latest?.build
        var why = ""; if case .failed(let text) = phase { why = " error=\(text)" }
        print("LAUNCHER-AUTORUN \(ok ? "OK" : "RED") job=\(what) installed=\(installed.map { "\($0.version) (\($0.build))" } ?? "-") latest=\(latest.map { "\($0.version) (\($0.build))" } ?? "-")\(why)")
        fflush(stdout); exit(ok ? 0 : 1)
    }

    func readState() {
        struct State: Decodable { var release: Rel?; var packs: P?; var app: String?; struct Rel: Decodable { var version: String?; var build: Int? }; struct P: Decodable { var runtime: String; var tools: String } }
        let s = (try? Data(contentsOf: paths.state)).flatMap { try? JSONDecoder().decode(State.self, from: $0) }
        appPath = s?.app.map { URL(fileURLWithPath: $0) }
        if let v = s?.release?.version, let b = s?.release?.build, FileManager.default.fileExists(atPath: installedApp.path) { installed = (v, b) } else { installed = nil }
        packs = s?.packs.map { (URL(fileURLWithPath: $0.runtime, isDirectory: true), URL(fileURLWithPath: $0.tools, isDirectory: true)) }
        canRevert = FileManager.default.fileExists(atPath: paths.previousApp.path) || FileManager.default.fileExists(atPath: paths.legacyPreviousApp.path)
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
        steps = [StepRow.download] + StepRow.engineSteps
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

    /// Starts the game with the launcher's mark (DM_FROM_LAUNCHER: an app the launcher installed sends any other start back here, dm/host/app_mac.m),
    /// then quits the launcher
    func play() {
        let cfg = NSWorkspace.OpenConfiguration()
        cfg.environment = ["DM_FROM_LAUNCHER": "1"]; cfg.addsToRecentItems = false
        NSWorkspace.shared.openApplication(at: installedApp, configuration: cfg) { _, error in
            Task { @MainActor in
                if let error { self.phase = .failed("D2R Metal did not open: \(error.localizedDescription)") } else { NSApp.terminate(nil) }
            }
        }
    }

    /// Moves the game app from where an older launcher put it into the Game folder: the engine's install with the installed release's own packs
    /// (every step is up to date, so only the app step runs: a few seconds); the old copy becomes the previous version (install.py install_app)
    func relocate() {
        guard let packs, job == nil else { return }
        let engine = Engine(paths: paths, runtime: packs.runtime, tools: packs.tools, appDir: appDir, bundleID: bundleID)
        working = "Moving D2R Metal into the launcher"; phase = .working; steps = StepRow.engineSteps; logPath = nil
        let old = installedApp
        job = Task {
            defer { job = nil }
            var code: Int32 = 0, msg: String?
            do {
                for try await out in engine.run("install") {
                    switch out { case .exit(let c): code = c; case .event(let ev): msg = apply(ev) ?? msg }
                }
            } catch { msg = error.localizedDescription; code = 1 }
            readState()
            if code == 0 { retire(old) }
            phase = code == 0 ? .home : .failed(msg ?? "Could not move D2R Metal into the launcher's folder.")
        }
    }

    /// The copy left at the old place after a move: an older release's engine (the installed packs run the move) knows nothing of it, so the
    /// launcher does what install.py install_app now does itself: a launcher-installed copy becomes the previous version
    private func retire(_ old: URL) {
        let fm = FileManager.default
        guard old.standardizedFileURL != installedApp.standardizedFileURL, fm.fileExists(atPath: old.path),
              (NSDictionary(contentsOf: old.appendingPathComponent("Contents/Info.plist"))?["DMLauncherManaged"] as? Bool) == true else { return }
        let prev = paths.previousApp
        try? fm.createDirectory(at: prev.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? fm.removeItem(at: prev)
        try? fm.moveItem(at: old, to: prev)
        let folder = old.deletingLastPathComponent()   // the Game folder of a 0.9.7 test build, now empty
        if folder.path.hasPrefix(paths.root.path), (try? fm.contentsOfDirectory(atPath: folder.path))?.isEmpty == true { try? fm.removeItem(at: folder) }
        canRevert = fm.fileExists(atPath: prev.path)
    }

    /// A previous version an older engine kept in build/previous (indexed by Spotlight, so it showed in app search) moves to build/previous.noindex,
    /// as install.py previous_app does; when both exist the older one goes
    private func tidyPrevious() {
        let fm = FileManager.default, legacy = paths.legacyPreviousApp
        guard fm.fileExists(atPath: legacy.path) else { return }
        if fm.fileExists(atPath: paths.previousApp.path) { try? fm.removeItem(at: legacy) }
        else { try? fm.createDirectory(at: paths.previousApp.deletingLastPathComponent(), withIntermediateDirectories: true); try? fm.moveItem(at: legacy, to: paths.previousApp) }
        if (try? fm.contentsOfDirectory(atPath: legacy.deletingLastPathComponent().path))?.isEmpty == true { try? fm.removeItem(at: legacy.deletingLastPathComponent()) }
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
        let items = ["data", "build", "releases", "tools", "downloads", "Game.noindex", "Game"].map { paths.root.appendingPathComponent($0) }.filter { fm.fileExists(atPath: $0.path) }
        let app = managedApp && !installedApp.path.hasPrefix(paths.gameApps.path) ? [installedApp] : []   // an app still outside the Game folder
        NSWorkspace.shared.recycle(items + app) { _, error in
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
    func showGame() { NSWorkspace.shared.activateFileViewerSelecting([installedApp]) }

    // MARK: progress of the running job

    var runningStep: StepRow? { steps.first { $0.state == .running } }
    /// The whole job's progress, each step weighted by its typical time on an M6 (TYPICAL_S in install.py); a step found up to date counts as done
    var overallProgress: Double {
        let total = steps.reduce(0) { $0 + $1.typical }
        guard total > 0 else { return 0 }
        let done = steps.reduce(0.0) { acc, s in
            switch s.state {
            case .done, .kept, .failed: acc + s.typical
            case .running: acc + s.typical * (s.total > 0 ? Double(s.done) / Double(s.total) : 0.3)
            case .waiting: acc
            }
        }
        return min(done / total, 1)
    }
    /// "About 4 min left" from the typical times of the steps still to run (nil when under a minute or nothing is known)
    var etaText: String? {
        let left = steps.reduce(0.0) { acc, s in
            switch s.state {
            case .waiting: acc + s.typical
            case .running: acc + s.typical * (s.total > 0 ? 1 - Double(s.done) / Double(s.total) : 0.7)
            default: acc
            }
        }
        return left >= 60 ? "About \(Int((left / 60).rounded())) min left" : nil
    }
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
        NSApp.setActivationPolicy(.regular); NSApp.activate(ignoringOtherApps: true)   // in front, so the screenshot shows the active window
        switch which {
        case "working":
            working = "Installing"; phase = .working
            steps = [StepRow.download] + StepRow.engineSteps
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
    // typical seconds on an M6, as install.py's TYPICAL_S (the engine's start events replace them); the download's depends on the connection
    static let engineSteps: [StepRow] = [
        StepRow(id: "unpack", title: "Unpacking your game files", typical: 300),
        StepRow(id: "product", title: "Preparing the game program", typical: 15),
        StepRow(id: "packs", title: "Extracting the shaders", typical: 20),
        StepRow(id: "shaders", title: "Translating the shaders", typical: 120),
        StepRow(id: "c1", title: "Checking the shaders", typical: 10),
        StepRow(id: "overlay", title: "Building the interface", typical: 20),
        StepRow(id: "tables", title: "Reading the game tables", typical: 5),
        StepRow(id: "app", title: "Assembling D2R Metal", typical: 5),
        StepRow(id: "warm", title: "Compiling the shaders for this Mac", typical: 240),
    ]
    static let download = StepRow(id: "download", title: "Downloading D2R Metal", typical: 20)
}
