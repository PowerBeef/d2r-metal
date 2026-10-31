import Foundation

/// Runs dm/tools/install.py from the packs with the tools pack's Python, and streams its JSON events
public struct Engine: Sendable {
    public var paths: Paths
    public var runtime: URL
    public var tools: URL
    public var appDir: URL
    public var bundleID: String?

    public init(paths: Paths, runtime: URL, tools: URL, appDir: URL, bundleID: String? = nil) {
        self.paths = paths; self.runtime = runtime; self.tools = tools; self.appDir = appDir; self.bundleID = bundleID
    }

    public enum Input: Sendable, Equatable {
        case none                                   // update / resume: the root already holds the game files
        case packages([URL], keys: URL)             // the player's .nsp / .xci and prod.keys, read in place
        case folder(URL)                            // already extracted ExeFS + RomFS
    }

    public func arguments(_ command: String, input: Input = .none) -> [String] {
        // -I: none of the player's Python settings; -B: no bytecode written into the packs
        var a = ["-I", "-B", runtime.appendingPathComponent("repo/dm/tools/install.py").path, command, "--root", paths.root.path, "--json"]
        let builds = command == "install" || command == "repair"
        if builds || command == "check" { a += ["--runtime", runtime.path, "--tools", tools.path] }
        if builds || command == "revert" { a += ["--app-dir", appDir.path] }
        if let bundleID, builds { a += ["--bundle-id", bundleID] }
        switch input {
        case .none: break
        case .packages(let pkgs, let keys): a += ["--game"] + pkgs.map(\.path) + ["--keys", keys.path]
        case .folder(let f): a += ["--from", f.path]
        }
        return a
    }

    /// The events of one engine run; the stream ends with the process (its exit code in the last .exit)
    public enum Output: Sendable { case event(EngineEvent), exit(Int32) }

    /// Starts the engine. Its stdout is read as JSON events, one per line (split at "\n" only: a message may hold other line separators); its
    /// stderr (Python's own errors, before the engine's log is open) is appended to logs/engine-stderr.log and kept for the error message
    public func start(_ command: String, input: Input = .none) throws -> EngineRun {
        let p = Process()
        p.executableURL = tools.appendingPathComponent("python/bin/python3")
        p.arguments = arguments(command, input: input)
        p.standardInput = FileHandle.nullDevice
        try? FileManager.default.createDirectory(at: paths.logs, withIntermediateDirectories: true)
        let run = EngineRun(process: p, stderrLog: paths.logs.appendingPathComponent("engine-stderr.log"), command: command)
        do { try p.run() } catch { throw LauncherError.engine("The install engine did not start: \(error.localizedDescription)") }
        return run
    }

    /// start + the events as a stream; cancelling the consumer stops the engine (SIGINT), which then ends on its own
    public func run(_ command: String, input: Input = .none) -> AsyncThrowingStream<Output, Error> {
        AsyncThrowingStream { cont in
            let r: EngineRun
            do { r = try start(command, input: input) } catch { cont.finish(throwing: error); return }
            let task = Task { for await o in r.output { cont.yield(o) }; cont.finish() }
            cont.onTermination = { reason in if case .cancelled = reason { r.interrupt(); task.cancel() } }
        }
    }
}

/// One running engine: its events, and the way to stop it. interrupt() sends SIGINT: install.py then stops the running tool's whole process group,
/// reports "interrupted" and exits; the output stream ends with its exit code, so a caller can wait for it
public final class EngineRun: @unchecked Sendable {
    public let output: AsyncStream<Engine.Output>
    let process: Process
    private let lock = NSLock()
    private var tail: [String] = []
    private var outDone = false, exitCode: Int32?
    private let cont: AsyncStream<Engine.Output>.Continuation

    init(process p: Process, stderrLog: URL, command: String) {
        process = p
        (output, cont) = AsyncStream.makeStream(of: Engine.Output.self)
        let out = Pipe(), err = Pipe()
        p.standardOutput = out; p.standardError = err
        let log: FileHandle? = {
            if !FileManager.default.fileExists(atPath: stderrLog.path) { FileManager.default.createFile(atPath: stderrLog.path, contents: nil) }
            let h = try? FileHandle(forWritingTo: stderrLog); _ = try? h?.seekToEnd()
            try? h?.write(contentsOf: Data("--- \(Date()) install.py \(command)\n".utf8))
            return h
        }()
        let events = LineSplitter { [cont] line in if let ev = EngineEvent.parse(String(decoding: line, as: UTF8.self)[...]) { cont.yield(.event(ev)) } }
        let errors = LineSplitter { [weak self] line in
            try? log?.write(contentsOf: line + Data([0x0A]))
            self?.keep(String(decoding: line, as: UTF8.self))
        }
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; events.finish(); self?.finished(out: true, code: nil) } else { events.feed(d) }
        }
        err.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; errors.finish(); try? log?.close() } else { errors.feed(d) }
        }
        p.terminationHandler = { [weak self] p in self?.finished(out: false, code: p.terminationStatus) }
    }

    /// The exit comes last, after every event: the process has ended and its output is read to the end
    private func finished(out: Bool, code: Int32?) {
        lock.lock()
        if out { outDone = true } else { exitCode = code }
        let done = outDone && exitCode != nil, c = exitCode ?? 0
        lock.unlock()
        if done { cont.yield(.exit(c)); cont.finish() }
    }

    private func keep(_ line: String) { lock.lock(); tail = (tail + [line]).suffix(20); lock.unlock() }

    /// The engine's last stderr lines (a Python error that happened before its log was open)
    public var stderrTail: [String] { lock.lock(); defer { lock.unlock() }; return tail }

    public func interrupt() { if process.isRunning { process.interrupt() } }
    public func terminate() { if process.isRunning { process.terminate() } }
}

/// Bytes → lines at "\n" (only), whatever the chunks
final class LineSplitter: @unchecked Sendable {
    private var buf = Data()
    private let lock = NSLock()
    private let onLine: (Data) -> Void
    init(_ onLine: @escaping (Data) -> Void) { self.onLine = onLine }
    func feed(_ d: Data) {
        lock.lock(); buf.append(d)
        var lines: [Data] = []
        while let i = buf.firstIndex(of: 0x0A) { lines.append(Data(buf[buf.startIndex..<i])); buf.removeSubrange(buf.startIndex...i) }
        lock.unlock()
        lines.forEach(onLine)
    }
    func finish() { lock.lock(); let rest = buf; buf = Data(); lock.unlock(); if !rest.isEmpty { onLine(rest) } }
}
