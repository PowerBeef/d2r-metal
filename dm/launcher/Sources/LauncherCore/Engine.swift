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
        var a = ["-I", runtime.appendingPathComponent("repo/dm/tools/install.py").path, command, "--root", paths.root.path, "--json"]
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

    public func run(_ command: String, input: Input = .none) -> AsyncThrowingStream<Output, Error> {
        AsyncThrowingStream { cont in
            let p = Process()
            p.executableURL = tools.appendingPathComponent("python/bin/python3")
            p.arguments = arguments(command, input: input)
            let out = Pipe(); p.standardOutput = out; p.standardError = FileHandle.nullDevice
            p.terminationHandler = { _ in }
            do { try p.run() } catch { cont.finish(throwing: LauncherError.engine("The install engine did not start: \(error.localizedDescription)")); return }
            let task = Task {
                do {
                    for try await line in out.fileHandleForReading.bytes.lines {
                        if let ev = EngineEvent.parse(line[...]) { cont.yield(.event(ev)) }
                    }
                } catch {}
                p.waitUntilExit()
                cont.yield(.exit(p.terminationStatus)); cont.finish()
            }
            cont.onTermination = { reason in
                if case .cancelled = reason { p.interrupt(); task.cancel() }   // install.py stops cleanly on SIGINT and resumes later
            }
        }
    }
}
