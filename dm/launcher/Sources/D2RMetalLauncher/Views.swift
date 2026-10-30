import LauncherCore
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Environment(LauncherModel.self) private var model

    var body: some View {
        ZStack {
            Backdrop()
            VStack(spacing: 0) {
                Header().padding(.top, 34)
                Group {
                    switch model.phase {
                    case .loading: ProgressView().controlSize(.large).tint(Palette.gold[1]).frame(maxHeight: .infinity)
                    case .welcome: WelcomeView()
                    case .working: WorkView()
                    case .home: HomeView()
                    case .failed(let text): FailedView(text: text)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding(.horizontal, 32).padding(.top, 22).padding(.bottom, 26)
                .transition(.opacity.combined(with: .scale(scale: 0.98)))
            }
            .animation(.smooth(duration: 0.35), value: model.phase)
        }
        .frame(minWidth: 720, minHeight: 680)
        .preferredColorScheme(.dark)
        .task { await model.start() }
    }
}

struct Header: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        VStack(spacing: 8) {
            GoldTitle(text: "D2R Metal")
            Caption(text: "Diablo II: Resurrected  ·  native on Apple silicon", dim: true, size: 11)
            Rule().padding(.top, 4)
            if let i = model.installed { Caption(text: "Installed  \(i.version)", size: 10).padding(.top, 2) }
        }
        .accessibilityElement(children: .combine)
    }
}

// MARK: first install

struct WelcomeView: View {
    @Environment(LauncherModel.self) private var model
    // which selector the file panel is for: kept apart from the panel's own flag, which SwiftUI resets before it hands over the chosen file
    @State private var target: Slot = .base
    @State private var showPanel = false
    @State private var dropTargeted = false
    enum Slot { case base, update, dlc, keys, folder }

    var body: some View {
        VStack(spacing: 16) {
            VStack(spacing: 4) {
                Text("Use your own copy: the Switch release with Reign of the Warlock, version 1.0.34.0.")
                Text("Everything is built on this Mac. Nothing is uploaded, and your keys are only read.").foregroundStyle(Palette.parchmentDim)
            }
            .font(.callout).foregroundStyle(Palette.parchment).multilineTextAlignment(.center)
            GlassEffectContainer(spacing: 16) {
                VStack(spacing: 0) {
                    SourceRow(symbol: "shippingbox.fill", title: "Base game", detail: "The game package, .nsp or .xci", value: model.baseGame,
                              choose: { pick(.base) }, clear: { model.baseGame = nil })
                    Divider().padding(.vertical, 10)
                    SourceRow(symbol: "arrow.down.circle.fill", title: "Update 1.0.34.0",
                              detail: model.baseGame?.pathExtension.lowercased() == "xci" ? "Not needed if your .xci already holds it" : "The update package (v2228224), .nsp",
                              value: model.gameUpdate, choose: { pick(.update) }, clear: { model.gameUpdate = nil })
                    Divider().padding(.vertical, 10)
                    SourceRow(symbol: "sparkles", title: "Reign of the Warlock DLC", detail: "Optional: its data already ships in the base game and the update",
                              value: model.dlc, choose: { pick(.dlc) }, clear: { model.dlc = nil })
                    Divider().padding(.vertical, 10)
                    SourceRow(symbol: "key.fill", title: "prod.keys", detail: "The keys of your own console", value: model.keys,
                              choose: { pick(.keys) }, clear: { model.keys = nil })
                }
                .glassCard()
                .overlay { if dropTargeted { RoundedRectangle(cornerRadius: 24).strokeBorder(Palette.goldFill, lineWidth: 1.5).allowsHitTesting(false) } }
                .dropDestination(for: URL.self) { urls, _ in drop(urls); return true } isTargeted: { dropTargeted = $0 }
            }
            HStack(spacing: 10) {
                Caption(text: model.folder == nil ? "Already extracted?" : "Extracted folder", dim: true, size: 10)
                if let f = model.folder {
                    Text(f.lastPathComponent).font(.callout).lineLimit(1).truncationMode(.middle).foregroundStyle(Palette.parchment)
                    Button("Clear", systemImage: "xmark") { model.folder = nil }.labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small)
                } else {
                    Button("Choose a Folder…") { pick(.folder) }.buttonStyle(.glass).controlSize(.small)
                }
            }
            Spacer(minLength: 0)
            if let e = model.feedError { Label("Can't reach GitHub: \(e)", systemImage: "wifi.exclamationmark").font(.callout).foregroundStyle(Palette.blood) }
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 3) {
                    Caption(text: "About 30 GB free while installing", dim: true, size: 10)
                    Caption(text: "First install: 10 to 15 minutes", dim: true, size: 10)
                }
                Spacer()
                Button { model.install() } label: { Label("Install", systemImage: "flame.fill").font(.title3.weight(.semibold)).padding(.horizontal, 18).padding(.vertical, 4) }
                    .primaryGlass().controlSize(.large).keyboardShortcut(.defaultAction)
                    .disabled(!model.gameInputReady || model.latest == nil)
            }
            .frame(maxWidth: 600)
        }
        .fileImporter(isPresented: $showPanel, allowedContentTypes: target == .folder ? [.folder] : [.item], allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            assign(url, to: target)
        }
    }

    private func pick(_ slot: Slot) { target = slot; showPanel = true }

    private func assign(_ url: URL, to slot: Slot) {
        switch slot {
        case .base: model.baseGame = url
        case .update: model.gameUpdate = url
        case .dlc: model.dlc = url
        case .keys: model.keys = url
        case .folder: model.folder = url; return
        }
        model.folder = nil   // packages and an extracted folder are two ways; the latest choice wins
    }

    /// Drops on the card: prod.keys by its extension, packages by the title id or version tag in their names, else the first empty selector
    private func drop(_ urls: [URL]) {
        for u in urls {
            switch u.pathExtension.lowercased() {
            case "keys": assign(u, to: .keys)
            case "nsp", "xci":
                if let path = LauncherModel.slot(for: u) { model[keyPath: path] = u; model.folder = nil }
                else if model.baseGame == nil { assign(u, to: .base) } else if model.gameUpdate == nil { assign(u, to: .update) } else { assign(u, to: .dlc) }
            default: if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { assign(u, to: .folder) }
            }
        }
    }
}

/// One selector: what it is, the file chosen, a glass button to choose and a small one to clear
struct SourceRow: View {
    let symbol: String, title: String, detail: String
    let value: URL?
    let choose: () -> Void
    let clear: () -> Void
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: value == nil ? symbol : "checkmark.seal.fill")
                .font(.title2).foregroundStyle(Palette.goldFill).frame(width: 30)
                .contentTransition(.symbolEffect(.replace))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(value?.lastPathComponent ?? detail).font(.callout).foregroundStyle(value == nil ? .secondary : .primary)
                    .lineLimit(1).truncationMode(.middle).help(value?.path ?? detail)
            }
            Spacer(minLength: 12)
            if value != nil { Button("Clear", systemImage: "xmark", action: clear).labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small) }
            Button(value == nil ? "Choose…" : "Change…", action: choose).buttonStyle(.glass)
        }
    }
}

// MARK: running

struct WorkView: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        let done = model.steps.filter { $0.state == .done || $0.state == .kept }.count
        VStack(spacing: 16) {
            VStack(spacing: 6) {
                Text("\(model.working)…").font(.title2.weight(.semibold)).foregroundStyle(Palette.goldFill)
                Caption(text: model.steps.isEmpty ? "" : "\(done) of \(model.steps.count) steps", dim: true, size: 10)
            }
            GlassEffectContainer {
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) { ForEach(model.steps) { StepLine(step: $0) } }.padding(.vertical, 2)
                }
                .scrollIndicators(.never)
                .glassCard()
            }
            Text("You can keep using your Mac. If you quit, the next run continues where this one stopped.")
                .font(.footnote).foregroundStyle(Palette.parchmentDim).multilineTextAlignment(.center)
            HStack(spacing: 12) {
                if model.logPath != nil { Button("Show Log", systemImage: "doc.text.magnifyingglass") { model.showLog() }.buttonStyle(.glass) }
                Button("Stop", systemImage: "stop.fill") { model.cancel() }.buttonStyle(.glass)
            }
        }
    }
}

struct StepLine: View {
    let step: StepRow
    var body: some View {
        HStack(spacing: 12) {
            Group {
                switch step.state {
                case .waiting: Image(systemName: "circle.dotted").foregroundStyle(.tertiary)
                case .running: ProgressView().controlSize(.small).tint(Palette.gold[1])
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(Palette.goldFill)
                case .kept: Image(systemName: "checkmark.circle").foregroundStyle(.secondary)
                case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(Palette.blood)
                }
            }
            .font(.body).frame(width: 20)
            VStack(alignment: .leading, spacing: 5) {
                HStack {
                    Text(step.title).foregroundStyle(step.state == .waiting ? .secondary : .primary)
                        .fontWeight(step.state == .running ? .semibold : .regular)
                    Spacer()
                    trailing.font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
                if step.state == .running, step.total > 0 {
                    ProgressView(value: Double(step.done), total: Double(step.total)).tint(Palette.ember).controlSize(.small)
                }
            }
        }
        .animation(.smooth, value: step.state)
    }

    @ViewBuilder private var trailing: some View {
        if step.state == .running, step.total > 0 {
            Text(step.id == "download" ? "\(step.done) / \(step.total) MB" : "\(step.done) / \(step.total)")
        } else if step.state == .kept {
            Text("up to date")
        } else if let s = step.seconds, step.state == .done {
            Text(s < 60 ? "\(Int(s.rounded())) s" : "\(Int((s / 60).rounded())) min")
        }
    }
}

// MARK: installed

struct HomeView: View {
    @Environment(LauncherModel.self) private var model
    @State private var confirmRemove = false
    var body: some View {
        @Bindable var model = model
        VStack(spacing: 20) {
            Spacer(minLength: 0)
            Button { model.play() } label: {
                Label("Play", systemImage: "play.fill").font(.custom("Optima-Bold", size: 26, relativeTo: .title)).tracking(2)
                    .padding(.horizontal, 44).padding(.vertical, 10)
            }
            .primaryGlass().controlSize(.extraLarge).keyboardShortcut(.defaultAction)
            if let up = model.launcherUpdate {
                Label("A new launcher (\(up.version)) is on the releases page", systemImage: "sparkles").font(.callout).foregroundStyle(Palette.parchment)
            }
            Group {
                if model.updateAvailable, let l = model.latest {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Label("Version \(l.version) is ready", systemImage: "arrow.down.circle.fill").font(.headline).foregroundStyle(Palette.goldFill)
                            Spacer()
                            Button("Update", systemImage: "arrow.down") { model.update() }.buttonStyle(.glass)
                        }
                        Text(notes(l.notes)).font(.callout).lineLimit(6).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                    }
                    .glassCard()
                } else {
                    Label(model.feedError.map { "Couldn't check for updates: \($0)" } ?? "Up to date", systemImage: model.feedError == nil ? "checkmark.seal" : "wifi.exclamationmark")
                        .font(.callout).foregroundStyle(Palette.parchmentDim)
                }
            }
            Spacer(minLength: 0)
            GlassEffectContainer(spacing: 12) {
                HStack(spacing: 12) {
                    Picker("Updates", selection: $model.channel) {
                        Text("Releases").tag(Channel.stable)
                        Text("With pre-releases").tag(Channel.prerelease)
                    }
                    .pickerStyle(.menu).labelsHidden().fixedSize().buttonStyle(.glass)
                    Spacer()
                    Button("Repair", systemImage: "wrench.and.screwdriver") { model.repair() }.buttonStyle(.glass)
                        .help("Checks every step again and rebuilds what is missing or damaged")
                    if model.canRevert { Button("Previous Version", systemImage: "clock.arrow.circlepath") { model.revert() }.buttonStyle(.glass) }
                    Button("Remove…", systemImage: "trash", role: .destructive) { confirmRemove = true }.buttonStyle(.glass)
                        .help("Moves the game files and D2R Metal to the Trash; your characters stay")
                }
            }
            .frame(maxWidth: 600)
        }
        .confirmationDialog("Move the game files and D2R Metal to the Trash?", isPresented: $confirmRemove) {
            Button("Move to Trash", role: .destructive) { model.removeGameData() }
        } message: {
            Text("Your characters and settings are kept: they are stored separately and are not touched.")
        }
    }

    private func notes(_ s: String) -> AttributedString {   // the release notes are Markdown; show their inline styling and links
        (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(s)
    }
}

struct FailedView: View {
    @Environment(LauncherModel.self) private var model
    let text: String
    var body: some View {
        VStack(spacing: 18) {
            Spacer(minLength: 0)
            VStack(alignment: .leading, spacing: 12) {
                Label("Something went wrong", systemImage: "exclamationmark.triangle.fill").font(.title3.weight(.semibold)).foregroundStyle(Palette.blood)
                Text(text).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassCard()
            Spacer(minLength: 0)
            GlassEffectContainer(spacing: 12) {
                HStack(spacing: 12) {
                    if model.logPath != nil { Button("Show Log", systemImage: "doc.text.magnifyingglass") { model.showLog() }.buttonStyle(.glass) }
                    Spacer()
                    Button("Back") { model.readState(); model.phase = model.installed == nil ? .welcome : .home }
                        .primaryGlass().controlSize(.large).keyboardShortcut(.defaultAction)
                }
            }
            .frame(maxWidth: 600)
        }
    }
}
