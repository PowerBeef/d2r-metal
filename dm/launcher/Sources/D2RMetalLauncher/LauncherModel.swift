import AppKit
import LauncherCore
import Network
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
    /// What the one job slot is running: every action that changes the install takes it, so two never overlap
    enum Job: Equatable { case install, update, repair, revert, relocate, launcherUpdate }

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
    var packs: (runtime: URL, tools: URL)?   // the packs of the installed release (state.json), to run the engine for revert / repair / a move
    var revertedFrom: Int?           // the build the player went back from: offered again as "Update to …", not as the main button
    var hasGameFiles = false         // the root already holds extracted game files (an earlier install stopped): Continue needs nothing chosen
    var gameDamaged: String?         // the engine found the game files damaged: why, shown over the setup form
    var stopping = false             // Stop was pressed: the engine is stopping its tools
    var quitGamePrompt = false       // "Quit D2R Metal to continue": a job waits for the running game
    var online = true

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

    var channel: Channel {
        didSet { guard channel != oldValue else { return }; defaults.set(channel.rawValue, forKey: "Channel"); refresh() }
    }

    let paths: Paths
    let trust = Trust.current
    let defaults = UserDefaults.standard
    let env = ProcessInfo.processInfo.environment
    private(set) var job: Task<Void, Never>?
    private(set) var jobKind: Job?
    private var engineRun: EngineRun?
    private var refreshTask: Task<Void, Never>?
    private var waitingJob: (@MainActor () -> Void)?
    private var monitor: NWPathMonitor?
    private var jobOK = false
    private var installedBundleID = "org.d2rmobile.mac"
    var quitting = false

    /// The game app goes into the launcher's own Game folder (it starts the game, so the game stays out of /Applications and app search)
    var appDir: URL { paths.gameApps }
    var appPath: URL?   // where the last install put the game app (state.json): launcher 0.9.6 and earlier put it in /Applications
    /// A test feed instead of GitHub: DM_LAUNCHER_FEED (environment), or the DMLauncherFeed default in a development build; a manifest.json URL
    /// (file:// works). A release launcher still requires the release key's signature on whatever feed it reads (Feed.checkSignature)
    var feedOverride: URL? {
        if let e = env["DM_LAUNCHER_FEED"] { return URL(string: e) }
        return trust.isRelease ? nil : defaults.string(forKey: "DMLauncherFeed").flatMap(URL.init(string:))
    }
    /// Test installs keep their own data: DM_LAUNCHER_ROOT / DM_LAUNCHER_BUNDLE_ID (never used by players)
    static func makePaths() -> Paths {
        Paths(root: ProcessInfo.processInfo.environment["DM_LAUNCHER_ROOT"].map { URL(fileURLWithPath: $0, isDirectory: true) })
    }
    var bundleID: String? { env["DM_LAUNCHER_BUNDLE_ID"] }
    var ownBuild: Int { Int(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "") ?? 0 }

    init() {
        paths = LauncherModel.makePaths()
        channel = Channel(rawValue: UserDefaults.standard.string(forKey: "Channel") ?? "") ?? .stable
        defaults.removeObject(forKey: "AppFolder")   // an early test build's setting; the game always goes to the Game folder now
        if trust.isRelease { defaults.removeObject(forKey: "DMLauncherFeed") }   // a tester's local feed never sticks to a release launcher
    }

    var busy: Bool { job != nil }
    var updateAvailable: Bool { if let l = latest, let i = installed { return l.installable && l.runsOnThisMac && l.build > i.build } else { return false } }
    /// The update is the main button, unless the player went back from that very build (then it waits in the gear menu)
    var updateIsPrimary: Bool { updateAvailable && !(revertedFrom.map { (latest?.build ?? 0) <= $0 } ?? false) }
    /// A newer launcher in the feed; a development build (build 0, dm/tools/launcher_app.sh's default) never replaces itself
    var launcherUpdate: Manifest.LauncherAsset? { ownBuild > 0 ? latest?.launcher.flatMap { $0.build > ownBuild ? $0 : nil } : nil }
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
    var nothingChosen: Bool { packages.isEmpty && keys == nil && folder == nil }
    /// The welcome screen's Continue: game files are already here and none were chosen
    var canContinue: Bool { hasGameFiles && nothingChosen && gameDamaged == nil }
    /// A newer launcher may install itself now: never while the player fills the setup form or a job runs
    var selfUpdateAllowed: Bool {
        guard job == nil else { return false }
        switch phase { case .loading, .home, .failed: return true; case .welcome: return nothingChosen; case .working: return false }
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
        if let preview = env["DM_LAUNCHER_PREVIEW"] { showPreview(preview); return }
        watchNetwork()
        tidyPrevious()
        readState()
        if appNeedsMove { relocate(); await job?.value }
        let auto = env["DM_LAUNCHER_AUTORUN"]
        // A newer launcher installs itself before the setup form shows (it would interrupt the player choosing files); an offline Mac waits a few
        // seconds at most, and the feed's answer still arrives later
        await refreshFeed(timeout: auto == nil ? 8 : nil)
        if auto == nil || auto == "selfupdate" {   // a test of another job runs the launcher it was given
            if launcherUpdate != nil {
                let ok = await updateLauncher()
                if auto == "selfupdate" {
                    print("LAUNCHER-AUTORUN \(ok ? "OK" : "RED") job=selfupdate from=\(ownBuild) to=\(SelfUpdate.build(of: Bundle.main.bundleURL))\(launcherNotice.map { " error=\($0)" } ?? "")")
                    fflush(stdout)
                }
                if ok { relaunch(); return }
                if auto == "selfupdate" { exit(1) }
            } else if auto == "selfupdate" {
                print("LAUNCHER-AUTORUN RED job=selfupdate no newer launcher in the feed\(feedError.map { " error=\($0)" } ?? "")"); fflush(stdout); exit(1)
            }
        }
        if phase == .loading { phase = installed == nil ? .welcome : .home }
        if let what = auto, what != "selfupdate" { await autorun(what) }
    }

    /// Check for Updates (the gear, the app menu, the game's d2rmetal://update link): the release feed again, and a newer launcher installed
    /// (then a restart into it) when there is one and nothing is in the way
    func checkForUpdates() async {
        launcherNotice = nil
        await refreshFeed()
        if launcherUpdate != nil, selfUpdateAllowed, await updateLauncher() { relaunch() }
    }

    // MARK: the launcher's own update

    /// Installs the newer launcher the release names (SelfUpdate: download, checksum, signature, an atomic swap with this app), with its own
    /// screen; true when the new one is in place (the caller restarts into it). When it cannot (run from its disk image, a folder it cannot write
    /// to, a failed check) it says why at the top of the panel and this launcher carries on, unless the release needs a newer one (min_build)
    func updateLauncher() async -> Bool {
        guard let m = latest, let mu = latestURL, let la = m.launcher, la.build > ownBuild, job == nil else { return false }
        launcherNotice = nil
        let me = Bundle.main.bundleURL
        if let why = SelfUpdate.cannotReplace(me) {
            launcherNotice = "A new launcher (\(la.version)) is out, but this copy cannot update itself: \(why)."
            if la.minBuild > ownBuild { phase = .failed(LauncherError.tooOld(la.minBuild).localizedDescription + " " + launcherNotice!) }
            return false
        }
        let before: Phase = phase == .loading || phase == .working ? (installed == nil ? .welcome : .home) : phase
        jobOK = false
        startJob(.launcherUpdate, "Updating the launcher", steps: [StepRow(id: "ldownload", title: "Downloading launcher \(la.version)", typical: 5),
                                                                   StepRow(id: "linstall", title: "Checking and installing it", typical: 2)],
                 needsGameClosed: false) { [self] in
            do {
                mark("ldownload", .running)
                let su = SelfUpdate(paths: paths, trust: trust)
                let app = try await su.prepare(m, from: mu) { done in Task { @MainActor in self.progress("ldownload", Int(done >> 20), max(1, Int(la.size >> 20))) } }
                try Task.checkCancellation()
                mark("ldownload", .done); mark("linstall", .running)
                try await Task.detached { try su.install(app, over: me) }.value   // a few renames: not cut by Stop
                mark("linstall", .done)
                jobOK = true
            } catch is CancellationError {
                phase = before
            } catch {
                launcherNotice = "Couldn't update the launcher to \(la.version): \(error.localizedDescription)"
                phase = la.minBuild > ownBuild ? .failed(LauncherError.tooOld(la.minBuild).localizedDescription + " " + launcherNotice!) : before
            }
        }
        await job?.value
        return jobOK
    }

    /// Quits and opens the launcher again from its own path, which now holds the new one: a shell waits for this process to end, then opens it.
    /// `open` hands its whole environment to the app it starts, so the test root and feed (DM_LAUNCHER_ROOT, _FEED, _BUNDLE_ID) carry over and
    /// a test launcher keeps its test install; the one-shot switches (DM_LAUNCHER_AUTORUN, _PREVIEW) are taken out
    func relaunch() {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "while /bin/kill -0 \"$1\" 2>/dev/null; do /bin/sleep 0.2; done; exec /usr/bin/open -n \"$2\"", "sh",
                       String(getpid()), Bundle.main.bundlePath]
        p.environment = env.filter { $0.key != "DM_LAUNCHER_AUTORUN" && $0.key != "DM_LAUNCHER_PREVIEW" }
        p.standardInput = FileHandle.nullDevice; p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        do { try p.run() } catch {
            // the new launcher is in place but nothing would start it: say so instead of quitting into nothing
            launcherNotice = "The new launcher is installed. Quit and open D2R Metal Launcher again to use it."
            return
        }
        quitting = true
        NSApp.terminate(nil)
    }

    /// DM_LAUNCHER_AUTORUN=update|repair, for tests (dm/verify/launcher_update.py) on a test root: runs that job as its button does, then prints
    /// `LAUNCHER-AUTORUN OK|RED …` and quits with 0 / 1. OK = back on the home screen with the feed's release installed
    func autorun(_ what: String) async {
        await job?.value   // a move into the Game folder runs first
        switch what {
        case "update": update()
        case "repair": repair()
        default: print("LAUNCHER-AUTORUN RED unknown job \(what)"); fflush(stdout); exit(2)
        }
        await job?.value
        try? await Task.sleep(for: .seconds(2))   // the finished screen stays a moment (screenshots)
        let ok = phase == .home && installed != nil && installed?.build == latest?.build
        var why = ""; if case .failed(let text) = phase { why = " error=\(text)" }
        if quitGamePrompt { why = " error=the game is running" }
        if let fe = feedError { why += " feed=\(fe)" }
        print("LAUNCHER-AUTORUN \(ok ? "OK" : "RED") job=\(what) installed=\(installed.map { "\($0.version) (\($0.build))" } ?? "-") latest=\(latest.map { "\($0.version) (\($0.build))" } ?? "-")\(why)")
        fflush(stdout); exit(ok ? 0 : 1)
    }

    /// What is installed: the game app (state.json's, else a launcher-installed one in the Game folder) and its own version, the installed
    /// release's packs, whether the player went back a version, whether game files are already here
    func readState() {
        struct State: Decodable {
            var packs: P?; var app: String?; var bundle_id: String?; var reverted_from: Int?
            struct P: Decodable { var runtime: String; var tools: String }
        }
        let fm = FileManager.default
        let s = (try? Data(contentsOf: paths.state)).flatMap { try? JSONDecoder().decode(State.self, from: $0) }
        appPath = s?.app.map { URL(fileURLWithPath: $0) }
        if let a = appPath, !fm.fileExists(atPath: a.path), managed(appDir.appendingPathComponent("D2R Metal.app")) { appPath = appDir.appendingPathComponent("D2R Metal.app") }
        let info = NSDictionary(contentsOf: installedApp.appendingPathComponent("Contents/Info.plist"))
        if managedApp, let v = info?["CFBundleShortVersionString"] as? String, let b = Int(info?["CFBundleVersion"] as? String ?? "") { installed = (v, b) } else { installed = nil }
        packs = s?.packs.map { (URL(fileURLWithPath: $0.runtime, isDirectory: true), URL(fileURLWithPath: $0.tools, isDirectory: true)) }
        installedBundleID = s?.bundle_id ?? bundleID ?? "org.d2rmobile.mac"
        revertedFrom = s?.reverted_from
        canRevert = fm.fileExists(atPath: paths.previousApp.path) || fm.fileExists(atPath: paths.legacyPreviousApp.path)
        hasGameFiles = fm.fileExists(atPath: paths.gameFiles.appendingPathComponent("ExeFS/main").path)
            && fm.fileExists(atPath: paths.gameFiles.appendingPathComponent("RomFS/data/build_manifest.psv").path)
    }

    // MARK: the release feed

    /// A new feed request for the current channel, replacing one still under way; its answer is used only while the channel is the same
    func refresh() {
        refreshTask?.cancel()
        let ch = channel
        refreshTask = Task { await fetchFeed(ch) }
    }

    /// refresh, and wait for its answer (at most `timeout` seconds; the answer still lands when it comes)
    func refreshFeed(timeout: Double? = nil) async {
        refresh()
        guard let t = refreshTask else { return }
        guard let timeout else { await t.value; return }
        await withTaskGroup(of: Void.self) { g in
            g.addTask { await t.value }
            g.addTask { try? await Task.sleep(for: .seconds(timeout)) }
            await g.next(); g.cancelAll()
        }
    }

    private func fetchFeed(_ ch: Channel) async {
        do {
            let (m, u) = try await Feed(channel: ch, override: feedOverride, trust: trust).latest()
            guard !Task.isCancelled, ch == channel else { return }
            latest = m; latestURL = u; feedError = nil
        } catch is CancellationError {
        } catch {
            guard !Task.isCancelled, ch == channel else { return }
            feedError = error.localizedDescription   // the last good answer for this channel stays
        }
    }

    /// The network coming back after an offline start fetches the feed again
    private func watchNetwork() {
        let m = NWPathMonitor()
        m.pathUpdateHandler = { [weak self] path in
            Task { @MainActor [weak self] in
                guard let self else { return }
                let up = path.status == .satisfied
                if up && !self.online && (self.latest == nil || self.feedError != nil) { self.refresh() }
                self.online = up
            }
        }
        m.start(queue: .global(qos: .utility)); monitor = m
    }

    // MARK: jobs

    /// Takes the job slot, unless a job runs; a job that changes the game waits for the game to be quit (the "Quit D2R Metal" prompt)
    private func startJob(_ kind: Job, _ title: String, steps: [StepRow], needsGameClosed: Bool = true, _ body: @escaping @MainActor () async -> Void) {
        guard job == nil else { return }
        if needsGameClosed, runningGame() != nil {
            waitingJob = { [weak self] in self?.startJob(kind, title, steps: steps, needsGameClosed: true, body) }
            quitGamePrompt = true
            return
        }
        jobKind = kind; working = title; phase = .working; logPath = nil; stopping = false; self.steps = steps
        job = Task {
            await body()
            job = nil; jobKind = nil; engineRun = nil; stopping = false
        }
    }

    /// The installed game, running (the copy this launcher installed, or one in its folder)
    func runningGame() -> NSRunningApplication? {
        let mine = [installedApp.standardizedFileURL.path, paths.root.standardizedFileURL.path]
        return NSRunningApplication.runningApplications(withBundleIdentifier: installedBundleID).first { app in
            guard !app.isTerminated, let p = app.bundleURL?.standardizedFileURL.path else { return false }
            return p == mine[0] || p.hasPrefix(mine[1] + "/")
        }
    }

    /// The prompt's Quit D2R Metal: asks the game to quit as Command-Q does, waits for it (up to half a minute), then starts the job that waited
    func quitGameAndContinue() async {
        guard let game = runningGame() else { quitGamePrompt = false; runWaitingJob(); return }
        game.terminate()
        for _ in 0..<60 where !game.isTerminated { try? await Task.sleep(for: .milliseconds(500)) }
        guard game.isTerminated else { return }   // it is still running (a dialog of its own?): the prompt stays
        quitGamePrompt = false
        runWaitingJob()
    }
    func cancelWaitingJob() { quitGamePrompt = false; waitingJob = nil }
    private func runWaitingJob() { let j = waitingJob; waitingJob = nil; j?() }

    func install() {
        if canContinue { runEngineJob(.install, "Installing", command: "install", input: .none, from: .feed) }
        else { runEngineJob(.install, "Installing", command: "install", input: gameInput(), from: .feed) }
    }
    func update() { guard updateAvailable else { return }; runEngineJob(.update, "Updating", command: "install", input: .none, from: .feed) }

    /// Every step again, from the installed release's own packs (checked file by file, so a damaged pack is fetched again): no update and no
    /// downgrade, and no network needed when the packs are intact. Packs an older launcher left without a file list are used as they are
    func repair() {
        guard let i = installed else { return }
        if let p = packs, packBuild(p.runtime) == i.build, Packs.contentsIntact(p.runtime) != false, Packs.contentsIntact(p.tools) != false {
            runEngineJob(.repair, "Repairing", command: "repair", input: .none, from: .installed(p))
        } else if let m = latest, m.build == i.build {
            runEngineJob(.repair, "Repairing", command: "repair", input: .none, from: .feed, verify: true)
        } else {
            phase = .failed("Repair needs the files of version \(i.version), and they are no longer on this Mac or on GitHub. Update to the latest version instead, or remove the game and install it again.")
        }
    }

    /// The selector a dropped package goes to (PackageKind.guess), nil when its name says nothing (its check then moves it if needed)
    static func slot(for url: URL) -> Slot? {
        switch PackageKind.guess(url.lastPathComponent) { case .base: .base; case .update: .update; case .dlc: .dlc; case nil: nil }
    }

    func gameInput() -> Engine.Input {
        if let folder { return .folder(folder) }
        if let keys, !packages.isEmpty { return .packages(packages, keys: keys) }
        return .none
    }

    /// Stop: during the download the task is cancelled; once the engine runs, it is asked to stop (it stops its tools, then exits) and the job
    /// waits for that, so nothing keeps running behind the window
    func cancel() {
        guard job != nil, !stopping else { return }
        stopping = true
        if let r = engineRun { r.interrupt() } else { job?.cancel() }
    }

    /// Quitting while a job runs (the app delegate asked first): stop it and wait for the engine to end, at most 20 s, then make sure it has
    func stopForQuit() async {
        cancel()
        guard let j = job else { return }
        await withTaskGroup(of: Void.self) { g in
            g.addTask { await j.value }
            g.addTask { try? await Task.sleep(for: .seconds(20)) }
            await g.next(); g.cancelAll()
        }
        engineRun?.terminate()
    }

    enum PackSource { case feed, installed((runtime: URL, tools: URL)) }

    private func runEngineJob(_ kind: Job, _ what: String, command: String, input: Engine.Input, from source: PackSource, verify: Bool = false) {
        let fromFeed: Bool = { if case .feed = source { return true } else { return false } }()
        startJob(kind, what, steps: (fromFeed ? [StepRow.download] : []) + StepRow.engineSteps) { [self] in
            do {
                let rt: URL, tl: URL
                switch source {
                case .feed:
                    if latest == nil { await refreshFeed() }
                    guard let m = latest, let mu = latestURL else { throw LauncherError.engine(feedError ?? "No release to install.") }
                    if let la = m.launcher, la.minBuild > ownBuild { throw LauncherError.tooOld(la.minBuild) }
                    guard m.installable else { throw LauncherError.newerFormat }
                    guard m.runsOnThisMac else { throw LauncherError.macOS(m.macosMin) }
                    mark("download", .running)
                    (rt, tl) = try await Packs(paths: paths, trust: trust).ensure(m, from: mu, verify: verify) { done, total in
                        Task { @MainActor in self.progress("download", Int(done >> 20), Int(total >> 20)) }
                    }
                    mark("download", .done)
                case .installed(let p):
                    if let team = trust.team {   // a release launcher runs only its team's programs, also from packs it unpacked earlier
                        let packs = Packs(paths: paths, trust: trust)
                        try packs.verifySigned(p.runtime.appendingPathComponent("host/D2RMobile"), team: team)
                        try packs.verifySigned(p.tools.appendingPathComponent("python/bin/python3"), team: team)
                    }
                    (rt, tl) = p
                }
                try Task.checkCancellation()
                try await drive(Engine(paths: paths, runtime: rt, tools: tl, appDir: appDir, bundleID: bundleID), command, input: input)
                readState(); gameDamaged = nil; phase = .home
                if case .packages = input { clearChoices() } else if case .folder = input { clearChoices() }
            } catch is CancellationError {
                readState(); phase = .failed("Stopped. Run it again to continue where it left off.")
            } catch let f as EngineFailure {
                readState(); handle(f)
            } catch {
                readState(); phase = .failed(error.localizedDescription)
            }
        }
    }

    /// Runs the engine to its end; throws CancellationError when it was stopped, EngineFailure when it failed
    private func drive(_ engine: Engine, _ command: String, input: Engine.Input) async throws {
        let run = try engine.start(command, input: input)
        engineRun = run
        if stopping { run.interrupt() }   // Stop came while the engine was starting
        var errors: [EngineFailure.Item] = [], code: Int32 = 0
        for await out in run.output {
            switch out {
            case .event(let ev): if let e = apply(ev) { errors.append(e) }
            case .exit(let c): code = c
            }
        }
        engineRun = nil
        if code == 0 { return }
        if stopping || errors.contains(where: { $0.code == "interrupted" }) { throw CancellationError() }
        throw EngineFailure(items: errors, code: code, stderr: run.stderrTail)
    }

    struct EngineFailure: Error {
        struct Item: Equatable { var code: String?; var msg: String }
        var items: [Item]; var code: Int32; var stderr: [String]
    }

    /// An engine failure, said for the player: damaged game files go back to the setup form; every distinct error is shown, the engine's own
    /// last words when it said nothing
    private func handle(_ f: EngineFailure) {
        if let d = f.items.last(where: { $0.code == "game_damaged" }) {
            gameDamaged = d.msg; phase = .welcome; return
        }
        if f.items.contains(where: { $0.code == "busy" }) {
            phase = .failed("Another install is running for this game (another copy of the launcher?). Wait for it to end, then try again."); return
        }
        var seen = Set<String>()
        let msgs = f.items.map(\.msg).filter { seen.insert($0).inserted }
        if msgs.isEmpty {
            let last = f.stderr.last(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty })
            phase = .failed("The install engine stopped unexpectedly (code \(f.code))" + (last.map { ": \($0)" } ?? "") + ". The log has the details.")
        } else {
            phase = .failed(msgs.map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: "\n\n"))
        }
    }

    private func clearChoices() { for s in [Slot.base, .update, .dlc, .keys, .folder] { set(s, nil) } }

    /// An engine event → the step rows; returns an error event's code and text
    private func apply(_ ev: EngineEvent) -> EngineFailure.Item? {
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
        case "error": return EngineFailure.Item(code: ev.code, msg: ev.msg ?? "unknown error")
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

    /// The build of an unpacked runtime pack (its manifest.json)
    private func packBuild(_ runtime: URL) -> Int? {
        struct B: Decodable { var build: Int }
        return (try? Data(contentsOf: runtime.appendingPathComponent("manifest.json"))).flatMap { try? JSONDecoder().decode(B.self, from: $0) }?.build
    }

    // MARK: other actions

    /// Starts the game with the launcher's mark (DM_FROM_LAUNCHER: an app the launcher installed sends any other start back here, dm/host/app_mac.m),
    /// then quits the launcher; a game already running comes to the front
    func play() {
        if let running = runningGame() { running.activate(); return }
        let cfg = NSWorkspace.OpenConfiguration()
        cfg.environment = ["DM_FROM_LAUNCHER": "1"]; cfg.addsToRecentItems = false
        NSWorkspace.shared.openApplication(at: installedApp, configuration: cfg) { _, error in
            Task { @MainActor in
                if let error { self.phase = .failed("D2R Metal did not open: \(error.localizedDescription)") } else { self.quitting = true; NSApp.terminate(nil) }
            }
        }
    }

    /// Moves the game app from where an older launcher put it into the Game folder: the engine's install with the installed release's own packs
    /// (every step is up to date, so only the app step runs: a few seconds); the old copy leaves its folder (install.py install_app)
    func relocate() {
        guard let packs else { return }
        let old = installedApp
        startJob(.relocate, "Moving D2R Metal into the launcher", steps: StepRow.engineSteps) { [self] in
            do {
                try await drive(Engine(paths: paths, runtime: packs.runtime, tools: packs.tools, appDir: appDir, bundleID: bundleID), "install", input: .none)
                readState(); retire(old); phase = .home
            } catch is CancellationError { readState(); phase = .home }
            catch let f as EngineFailure { readState(); handle(f) }
            catch { readState(); phase = .failed("Could not move D2R Metal into the launcher's folder: \(error.localizedDescription)") }
        }
    }

    /// The copy left at the old place after a move: an older release's engine (the installed packs run the move) knows nothing of it, so the
    /// launcher does what install.py install_app now does itself: a launcher-installed copy becomes the previous version
    private func retire(_ old: URL) {
        let fm = FileManager.default
        guard old.standardizedFileURL != installedApp.standardizedFileURL, fm.fileExists(atPath: old.path), managed(old) else { return }
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
        guard let packs, canRevert else { return }
        startJob(.revert, "Going back to the previous version", steps: []) { [self] in
            do {
                try await drive(Engine(paths: paths, runtime: packs.runtime, tools: packs.tools, appDir: appDir), "revert", input: .none)
                readState(); phase = .home
            } catch is CancellationError { readState(); phase = .home }
            catch let f as EngineFailure { readState(); handle(f) }
            catch { readState(); phase = .failed("Could not go back to the previous version: \(error.localizedDescription)") }
        }
    }

    /// Moves the game files and build outputs to the Trash (recoverable). Saves live elsewhere and are never touched
    func removeGameData() {
        guard job == nil else { return }
        if runningGame() != nil { quitGamePrompt = true; waitingJob = { [weak self] in self?.removeGameData() }; return }
        let fm = FileManager.default
        let items = ["data", "build", "releases", "tools", "downloads", "Game.noindex", "Game"].map { paths.root.appendingPathComponent($0) }.filter { fm.fileExists(atPath: $0.path) }
        let app = managedApp && !installedApp.path.hasPrefix(paths.gameApps.path) ? [installedApp] : []   // an app still outside the Game folder
        NSWorkspace.shared.recycle(items + app) { _, error in
            Task { @MainActor in
                if let error { self.phase = .failed("Could not move the game data to the Trash: \(error.localizedDescription)") }
                else { self.readState(); self.gameDamaged = nil; self.phase = .welcome }
            }
        }
    }
    var managedApp: Bool { managed(installedApp) }
    var hasGameData: Bool { ["data", "build", "Game.noindex"].contains { FileManager.default.fileExists(atPath: paths.root.appendingPathComponent($0).path) } }
    private func managed(_ app: URL) -> Bool {
        (NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist"))?["DMLauncherManaged"] as? Bool) ?? false
    }

    /// The running job's log, else the newest one
    func showLog() {
        let newest = (try? FileManager.default.contentsOfDirectory(at: paths.logs, includingPropertiesForKeys: [.contentModificationDateKey]))?
            .filter { $0.pathExtension == "log" }
            .max { ((try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) < ((try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) }
        if let l = logPath.map({ URL(fileURLWithPath: $0) }) ?? newest { NSWorkspace.shared.activateFileViewerSelecting([l]) }
    }
    var hasLog: Bool { logPath != nil || FileManager.default.fileExists(atPath: paths.logs.path) }
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
    /// DM_LAUNCHER_PREVIEW=welcome|empty|offline|continue|damaged|working|stopping|home|reverted|game|failed: one screen with sample data and
    /// nothing run, to look at the design (screenshots, docs)
    func showPreview(_ which: String) {
        let json = #"""
        {"schema": 1, "version": "1.1.0", "build": 420, "commit": "abc1234", "date": "2026-11-02", "macos_min": "27.0", "chain": "", "product": "",
         "notes": "**Sharper shadows** on characters, and a faster first launch. See the release page for everything in this version.",
         "runtime": {"name": "r", "version": "1.1.0", "sha256": "", "size": 0}, "tools": {"name": "t", "version": "1", "sha256": "", "size": 0}}
        """#
        let sample = try? JSONDecoder().decode(Manifest.self, from: Data(json.utf8))
        NSApp.setActivationPolicy(.regular); NSApp.activate(ignoringOtherApps: true)   // in front, so the screenshot shows the active window
        switch which {
        case "working", "stopping":
            working = "Installing"; phase = .working; stopping = which == "stopping"; job = Task {}
            steps = [StepRow.download] + StepRow.engineSteps
            for (i, seconds) in [14.0, 160, 13, 21].enumerated() { steps[i].state = .done; steps[i].seconds = seconds }   // download … packs
            if let i = steps.firstIndex(where: { $0.id == "shaders" }) { steps[i].state = .running; steps[i].done = 4224; steps[i].total = 6927 }
        case "home": installed = ("1.0.0", 412); latest = sample; canRevert = true; phase = .home
        case "reverted": installed = ("1.0.0", 412); latest = sample; canRevert = true; revertedFrom = 420; phase = .home
        case "game": installed = ("1.0.0", 412); latest = sample; canRevert = true; phase = .home; quitGamePrompt = true
        case "empty": latest = sample; phase = .welcome
        case "offline": feedError = LauncherError.offline.localizedDescription; online = false; phase = .welcome
        case "continue": latest = sample; hasGameFiles = true; phase = .welcome
        case "damaged": latest = sample; hasGameFiles = true; phase = .welcome
                        gameDamaged = "RomFS/data/04-00000002.data is damaged (1,000 bytes instead of 8,915,855)"
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
