import LauncherCore
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Environment(LauncherModel.self) private var model

    var body: some View {
        ZStack {
            Backdrop()
            VStack(spacing: 0) {
                Header().padding(.top, 24)
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
                .padding(.horizontal, 28).padding(.top, 12).padding(.bottom, 18)
                .transition(.opacity.combined(with: .scale(scale: 0.98)))
            }
            .animation(.smooth(duration: 0.35), value: model.phase)
        }
        .frame(minWidth: 680, minHeight: 540)
        .preferredColorScheme(.dark)
        .task { await model.start() }
    }
}

struct Header: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        VStack(spacing: 5) {
            GoldTitle(text: "D2R Metal", size: 36)
            Caption(text: "Diablo II: Resurrected  ·  native on Apple silicon", dim: true, size: 10)
            Rule().padding(.top, 2)
            if let i = model.installed { Caption(text: "Installed  \(i.version)", size: 10) }
        }
        .accessibilityElement(children: .combine)
    }
}

// MARK: first install

struct WelcomeView: View {
    @Environment(LauncherModel.self) private var model
    // which selector the file panel is for: kept apart from the panel's own flag, which SwiftUI resets before it hands over the chosen file
    @State private var target: LauncherModel.Slot = .base
    @State private var showPanel = false
    @State private var dropTargeted = false

    var body: some View {
        VStack(spacing: 10) {
            VStack(spacing: 2) {
                Text("Use your own copy: the Switch release with Reign of the Warlock, version 1.0.34.0.")
                Text("Everything is built on this Mac. Nothing is uploaded, and your keys are only read.").foregroundStyle(Palette.parchmentDim)
            }
            .font(.footnote).foregroundStyle(Palette.parchment).multilineTextAlignment(.center)
            GlassEffectContainer(spacing: 16) {
                VStack(spacing: 0) {
                    row(.base, symbol: "shippingbox.fill", title: "Base game", detail: "The game package, .nsp or .xci")
                    Divider().padding(.vertical, 6)
                    row(.update, symbol: "arrow.down.circle.fill", title: "Update 1.0.34.0",
                        detail: model.baseGame?.pathExtension.lowercased() == "xci" ? "Not needed if your .xci already holds it" : "The update package (v2228224), .nsp")
                    Divider().padding(.vertical, 6)
                    row(.dlc, symbol: "sparkles", title: "Reign of the Warlock DLC", detail: "Optional: its data already ships in the base game and the update")
                    Divider().padding(.vertical, 6)
                    row(.keys, symbol: "key.fill", title: "prod.keys", detail: "The keys of your own console")
                }
                .glassCard(padding: 14)
                .overlay { if dropTargeted { RoundedRectangle(cornerRadius: 22).strokeBorder(Palette.goldFill, lineWidth: 1.5).allowsHitTesting(false) } }
                .dropDestination(for: URL.self) { urls, _ in drop(urls); return true } isTargeted: { dropTargeted = $0 }
            }
            HStack(spacing: 10) {
                Caption(text: model.folder == nil ? "Already extracted?" : "Extracted folder", dim: true, size: 10)
                if let f = model.folder {
                    CheckMark(verdict: model.verdicts[.folder], checking: model.checking.contains(.folder))
                    VStack(alignment: .leading, spacing: 1) {
                        Text(f.lastPathComponent).font(.callout).lineLimit(1).truncationMode(.middle).foregroundStyle(Palette.parchment)
                        if let v = model.verdicts[.folder] { VerdictText(verdict: v) }
                    }
                    Button("Clear", systemImage: "xmark") { model.clear(.folder) }.labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small)
                } else {
                    Button("Choose a Folder…") { pick(.folder) }.buttonStyle(.glass).controlSize(.small)
                }
            }
            Spacer(minLength: 0)
            if let e = model.feedError { Label("Can't reach GitHub: \(e)", systemImage: "wifi.exclamationmark").font(.callout).foregroundStyle(Palette.blood) }
            HStack(alignment: .center) {
                Caption(text: "About 30 GB free while installing  ·  10 to 15 minutes", dim: true, size: 10)
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

    private func pick(_ slot: LauncherModel.Slot) { target = slot; showPanel = true }
    private func assign(_ url: URL, to slot: LauncherModel.Slot) { model.choose(url, for: slot) }

    private func row(_ slot: LauncherModel.Slot, symbol: String, title: String, detail: String) -> some View {
        SourceRow(symbol: symbol, title: title, detail: detail, value: model.url(slot), verdict: model.verdicts[slot], checking: model.checking.contains(slot),
                  choose: { pick(slot) }, clear: { model.clear(slot) })
    }

    /// Drops on the card: prod.keys by its extension, packages by the title id or version tag in their names, else the first empty selector
    private func drop(_ urls: [URL]) {
        for u in urls {
            switch u.pathExtension.lowercased() {
            case "keys": assign(u, to: .keys)
            case "nsp", "xci":
                if let slot = LauncherModel.slot(for: u) { assign(u, to: slot) }
                else if model.baseGame == nil { assign(u, to: .base) } else if model.gameUpdate == nil { assign(u, to: .update) } else { assign(u, to: .dlc) }
            default: if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { assign(u, to: .folder) }
            }
        }
    }
}

/// One selector: what it is, the file chosen and what its check found, a glass button to choose and a small one to clear
struct SourceRow: View {
    let symbol: String, title: String, detail: String
    let value: URL?
    let verdict: GameFileCheck.Verdict?
    let checking: Bool
    let choose: () -> Void
    let clear: () -> Void
    var body: some View {
        HStack(spacing: 12) {
            Group {
                if value == nil { Image(systemName: symbol).foregroundStyle(Palette.goldFill) }
                else { CheckMark(verdict: verdict, checking: checking) }
            }
            .font(.title3).frame(width: 26)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.body.weight(.semibold))
                Text(value?.lastPathComponent ?? detail).font(.callout).foregroundStyle(value == nil ? .secondary : .primary)
                    .lineLimit(1).truncationMode(.middle).help(value?.path ?? detail)
                if value != nil, let verdict { VerdictText(verdict: verdict) }
            }
            Spacer(minLength: 12)
            if value != nil { Button("Clear", systemImage: "xmark", action: clear).labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small) }
            Button(value == nil ? "Choose…" : "Change…", action: choose).buttonStyle(.glass)
        }
        .animation(.smooth, value: verdict)
    }
}

/// The result of a file's check as a symbol: a green check when it is the right file, red when it is not, a spinner while checking
struct CheckMark: View {
    let verdict: GameFileCheck.Verdict?
    let checking: Bool
    var body: some View {
        switch verdict {
        case _ where checking: ProgressView().controlSize(.small)
        case .ok?, .note?: Image(systemName: "checkmark.circle.fill").foregroundStyle(.white, .green).symbolEffect(.bounce, value: verdict)
        case .wrong?, .elsewhere?: Image(systemName: "xmark.octagon.fill").foregroundStyle(.white, Palette.blood)
        case nil: Image(systemName: "circle.dotted").foregroundStyle(.secondary)
        }
    }
}

/// The check's one line: green for what was recognized, amber for a note, red for why it is not the right file
struct VerdictText: View {
    let verdict: GameFileCheck.Verdict
    var body: some View {
        switch verdict {
        case .ok(let s): Text(s).font(.caption).foregroundStyle(.green)
        case .note(let s): Text(s).font(.caption).foregroundStyle(.orange)
        case .wrong(let s), .elsewhere(_, let s): Text(s).font(.caption).foregroundStyle(Palette.blood)
        }
    }
}

// MARK: running

struct WorkView: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        let done = model.steps.filter { $0.state == .done || $0.state == .kept }.count
        VStack(spacing: 10) {
            VStack(spacing: 3) {
                Text("\(model.working)…").font(.title3.weight(.semibold)).foregroundStyle(Palette.goldFill)
                Caption(text: model.steps.isEmpty ? "" : "\(done) of \(model.steps.count) steps", dim: true, size: 10)
            }
            GlassEffectContainer {
                let list = VStack(alignment: .leading, spacing: 7) { ForEach(model.steps) { StepLine(step: $0) } }.padding(.vertical, 2)
                ViewThatFits(in: .vertical) { list; ScrollView { list }.scrollIndicators(.never) }   // hugs the rows; scrolls only on a short window
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
        VStack(spacing: 14) {
            Spacer(minLength: 0)
            Button { model.play() } label: {
                Label("Play", systemImage: "play.fill").font(.custom("Optima-Bold", size: 22, relativeTo: .title)).tracking(2)
                    .padding(.horizontal, 40).padding(.vertical, 6)
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
                        Text(notes(l.notes)).font(.callout).lineLimit(4).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
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
