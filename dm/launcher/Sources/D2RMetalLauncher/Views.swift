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
        .frame(minWidth: 720, minHeight: 620)
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
    @State private var picking: Picking?
    @State private var dropTargeted = false
    enum Picking: Identifiable { case packages, keys, folder; var id: Self { self } }

    var body: some View {
        @Bindable var model = model
        VStack(spacing: 18) {
            VStack(spacing: 4) {
                Text("Use your own copy: the Switch release with Reign of the Warlock, version 1.0.34.0.")
                Text("Everything is built on this Mac. Nothing is uploaded, and your keys are only read.").foregroundStyle(Palette.parchmentDim)
            }
            .font(.callout).foregroundStyle(Palette.parchment).multilineTextAlignment(.center)
            GlassEffectContainer(spacing: 16) {
                VStack(spacing: 0) {
                    SourceRow(symbol: "shippingbox.fill", title: "Game packages", detail: "The base game and the 1.0.34.0 update, .nsp or .xci",
                              values: model.packages.map(\.lastPathComponent)) { picking = .packages }
                    Divider().padding(.vertical, 12)
                    SourceRow(symbol: "key.fill", title: "prod.keys", detail: "The keys of your own console", values: model.keys.map { [$0.lastPathComponent] } ?? []) { picking = .keys }
                    Divider().padding(.vertical, 12)
                    SourceRow(symbol: "folder.fill", title: "Or an extracted folder", detail: "A folder holding ExeFS and RomFS", values: model.folder.map { [$0.lastPathComponent] } ?? []) { picking = .folder }
                }
                .glassCard()
                .overlay { if dropTargeted { RoundedRectangle(cornerRadius: 24).strokeBorder(Palette.goldFill, lineWidth: 1.5).allowsHitTesting(false) } }
                .dropDestination(for: URL.self) { urls, _ in drop(urls); return true } isTargeted: { dropTargeted = $0 }
            }
            Caption(text: "Or drop your files on the card", dim: true, size: 10)
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

/// One way to give the game files: what it is, what was chosen, and a glass button to choose
struct SourceRow: View {
    let symbol: String, title: String, detail: String
    let values: [String]   // what was chosen, one file per line; empty = nothing yet
    let choose: () -> Void
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: values.isEmpty ? symbol : "checkmark.seal.fill")
                .font(.title2).foregroundStyle(Palette.goldFill).frame(width: 30)
                .contentTransition(.symbolEffect(.replace))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                if values.isEmpty { Text(detail).font(.callout).foregroundStyle(.secondary) }
                ForEach(values, id: \.self) { Text($0).font(.callout).lineLimit(1).truncationMode(.middle) }
            }
            Spacer(minLength: 12)
            Button(values.isEmpty ? "Choose…" : "Change…", action: choose).buttonStyle(.glass)
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
