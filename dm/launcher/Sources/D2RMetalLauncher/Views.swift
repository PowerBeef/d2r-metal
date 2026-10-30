import LauncherCore
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Environment(LauncherModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            Header()
            Divider()
            Group {
                switch model.phase {
                case .loading: ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                case .welcome: WelcomeView()
                case .working: WorkView()
                case .home: HomeView()
                case .failed(let text): FailedView(text: text)
                }
            }
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 640, minHeight: 560)
        .task { await model.start() }
    }
}

struct Header: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text("D2R Metal").font(.largeTitle.weight(.semibold))
                Text("Diablo II: Resurrected on your Mac, from your own copy of the game").foregroundStyle(.secondary)
            }
            Spacer()
            if let i = model.installed { Text("Installed: \(i.version)").font(.callout).foregroundStyle(.secondary) }
        }
        .padding(.horizontal, 24).padding(.vertical, 18)
    }
}

// MARK: first install

struct WelcomeView: View {
    @Environment(LauncherModel.self) private var model
    @State private var picking: Picking?
    enum Picking: Identifiable { case packages, keys, folder; var id: Self { self } }

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 18) {
            Text("Install the game").font(.title2.weight(.semibold))
            Text("D2R Metal runs the Nintendo Switch release of Diablo II: Resurrected with the Reign of the Warlock expansion, updated to 1.0.34.0. Use files dumped from your own console and your own copy of the game; nothing is uploaded, and your keys are only read.")
                .fixedSize(horizontal: false, vertical: true)

            GroupBox {
                VStack(alignment: .leading, spacing: 10) {
                    Label("Game packages (.nsp or .xci): the base game and the 1.0.34.0 update", systemImage: "shippingbox")
                    HStack {
                        Text(model.packages.isEmpty ? "None chosen" : model.packages.map(\.lastPathComponent).joined(separator: ", ")).foregroundStyle(.secondary).lineLimit(2)
                        Spacer(); Button("Choose…") { picking = .packages }
                    }
                    Label("Your console's prod.keys", systemImage: "key")
                    HStack {
                        Text(model.keys?.lastPathComponent ?? "None chosen").foregroundStyle(.secondary)
                        Spacer(); Button("Choose…") { picking = .keys }
                    }
                }.padding(6)
            }
            .dropDestination(for: URL.self) { urls, _ in drop(urls); return true }

            HStack {
                Text("Already extracted (a folder with ExeFS and RomFS)?").foregroundStyle(.secondary)
                Button(model.folder?.lastPathComponent ?? "Choose folder…") { picking = .folder }
            }
            Spacer()
            HStack {
                Text("About 30 GB of free space is needed while installing, 13 GB afterwards. The first install takes about 10 minutes.").font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Install") { model.install() }.keyboardShortcut(.defaultAction).disabled(!model.gameInputReady || model.latest == nil)
            }
            if let e = model.feedError { Text("Can't reach GitHub: \(e)").font(.callout).foregroundStyle(.red) }
        }
        .fileImporter(isPresented: Binding(get: { picking != nil }, set: { if !$0 { picking = nil } }),
                      allowedContentTypes: picking == .folder ? [.folder] : [.data], allowsMultipleSelection: picking == .packages) { result in
            guard let urls = try? result.get(), !urls.isEmpty else { return }
            switch picking {
            case .packages: model.packages = urls; model.folder = nil
            case .keys: model.keys = urls[0]; model.folder = nil
            case .folder: model.folder = urls[0]; model.packages = []; model.keys = nil
            case nil: break
            }
        }
    }

    private func drop(_ urls: [URL]) {
        for u in urls {
            switch u.pathExtension.lowercased() {
            case "nsp", "xci": if !model.packages.contains(u) { model.packages.append(u) }; model.folder = nil
            case "keys": model.keys = u; model.folder = nil
            default: if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { model.folder = u; model.packages = []; model.keys = nil }
            }
        }
    }
}

// MARK: running

struct WorkView: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("\(model.working)…").font(.title2.weight(.semibold))
            Text("You can keep using your Mac. If you quit, the next run continues where this one stopped.").foregroundStyle(.secondary)
            ScrollView {
                VStack(alignment: .leading, spacing: 8) { ForEach(model.steps) { StepLine(step: $0) } }
            }
            HStack {
                if model.logPath != nil { Button("Show Log") { model.showLog() } }
                Spacer()
                Button("Stop") { model.cancel() }
            }
        }
    }
}

struct StepLine: View {
    let step: StepRow
    var body: some View {
        HStack(spacing: 10) {
            Group {
                switch step.state {
                case .waiting: Image(systemName: "circle").foregroundStyle(.tertiary)
                case .running: ProgressView().controlSize(.small)
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .kept: Image(systemName: "checkmark.circle").foregroundStyle(.secondary)
                case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
                }
            }.frame(width: 18)
            Text(step.title).foregroundStyle(step.state == .waiting ? .secondary : .primary)
            Spacer()
            if step.state == .running, step.total > 0 {
                ProgressView(value: Double(step.done), total: Double(step.total)).frame(width: 160)
                Text(step.id == "download" ? "\(step.done) / \(step.total) MB" : "\(step.done) / \(step.total)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            } else if step.state == .kept {
                Text("up to date").font(.caption).foregroundStyle(.secondary)
            } else if let s = step.seconds, step.state == .done {
                Text(s < 60 ? "\(Int(s.rounded())) s" : "\(Int((s / 60).rounded())) min").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: installed

struct HomeView: View {
    @Environment(LauncherModel.self) private var model
    @State private var confirmRemove = false
    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Button { model.play() } label: { Label("Play", systemImage: "play.fill").font(.title2.weight(.semibold)).padding(.horizontal, 22).padding(.vertical, 6) }
                    .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                Spacer()
            }
            if let up = model.launcherUpdate {
                GroupBox { Text("A new launcher (\(up.version)) is available. Download it from the project's GitHub releases page.").frame(maxWidth: .infinity, alignment: .leading) }
            }
            if model.updateAvailable, let l = model.latest {
                GroupBox {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Update available: \(l.version)").font(.headline)
                        ScrollView { Text(l.notes).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }.frame(maxHeight: 160)
                        HStack { Spacer(); Button("Update") { model.update() } }
                    }.padding(6)
                }
            } else if let e = model.feedError {
                Text("Couldn't check for updates: \(e)").font(.callout).foregroundStyle(.secondary)
            } else {
                Text("D2R Metal is up to date.").foregroundStyle(.secondary)
            }
            Spacer()
            Form {
                Picker("Updates", selection: $model.channel) {
                    Text("Releases").tag(Channel.stable)
                    Text("Releases and pre-releases").tag(Channel.prerelease)
                }
            }.formStyle(.grouped).frame(maxHeight: 90)
            HStack {
                Button("Repair") { model.repair() }.help("Checks every step again and rebuilds what is missing or damaged")
                if model.canRevert { Button("Go Back to Previous Version") { model.revert() } }
                Spacer()
                Button("Remove Game Data…", role: .destructive) { confirmRemove = true }
            }
        }
        .confirmationDialog("Move the game files and D2R Metal to the Trash?", isPresented: $confirmRemove) {
            Button("Move to Trash", role: .destructive) { model.removeGameData() }
        } message: {
            Text("Your characters and settings are kept: they are stored separately and are not touched.")
        }
    }
}

struct FailedView: View {
    @Environment(LauncherModel.self) private var model
    let text: String
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("Something went wrong", systemImage: "exclamationmark.triangle.fill").font(.title2.weight(.semibold)).foregroundStyle(.orange)
            Text(text).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
            Spacer()
            HStack {
                if model.logPath != nil { Button("Show Log") { model.showLog() } }
                Spacer()
                Button("Back") { model.readState(); model.phase = model.installed == nil ? .welcome : .home }.keyboardShortcut(.defaultAction)
            }
        }
    }
}
