import LauncherCore
import SwiftUI
import UniformTypeIdentifiers

// The window, laid out as a game launcher's game page: the hero (emblem, wordmark, tagline) on the left over the forge backdrop, a glass panel on
// the right that follows the phase (choosing the game files, the install steps, what's new, an error), and a glass action bar along the bottom
// with the one big action (Install, Play, Update), the gear menu and the status or progress line.

struct ContentView: View {
    @Environment(LauncherModel.self) private var model

    var body: some View {
        ZStack {
            Backdrop()
            VStack(spacing: 0) {
                HStack(alignment: .center, spacing: 30) {
                    Hero().frame(maxWidth: .infinity)
                    Panel().frame(width: 460)
                }
                .padding(.horizontal, 34).padding(.top, 34).padding(.bottom, 14)
                .frame(maxHeight: .infinity)
                ActionBar().padding(.horizontal, 16).padding(.bottom, 16)
            }
        }
        .frame(minWidth: 900, minHeight: 580)
        .preferredColorScheme(.dark)
        .task { await model.start() }
    }
}

// MARK: hero

struct Hero: View {
    var body: some View {
        VStack(spacing: 14) {
            if let emblem = Brand.emblem {
                emblem.resizable().interpolation(.high).scaledToFit().frame(width: 176, height: 176)
                    .shadow(color: Color(red: 0.95, green: 0.32, blue: 0.08).opacity(0.28), radius: 34)
            }
            if let wordmark = Brand.wordmark { wordmark.resizable().interpolation(.high).scaledToFit().frame(width: 320) }
            else { GoldTitle(text: "D2R Metal", size: 38) }
            Rule().frame(width: 300)
            Caption(text: "Native on Apple silicon", size: 11)
            Text("Diablo II: Resurrected and Reign of the Warlock,\nbuilt on this Mac from your own copy of the game.")
                .font(.callout).foregroundStyle(Palette.parchmentDim).multilineTextAlignment(.center).padding(.top, 6)
            HStack(spacing: 22) {
                Feature(symbol: "cpu", text: "Apple silicon")
                Feature(symbol: "sparkles", text: "Metal 4 · MetalFX")
                Feature(symbol: "wifi.slash", text: "Plays offline")
            }
            .padding(.top, 4)
        }
        .accessibilityElement(children: .combine)
    }
}

struct Feature: View {
    let symbol: String, text: String
    var body: some View {
        Label(text, systemImage: symbol).font(.caption.weight(.medium)).foregroundStyle(Palette.parchment.opacity(0.85))
            .labelStyle(.titleAndIcon).symbolRenderingMode(.hierarchical)
    }
}

// MARK: the panel

struct Panel: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        Group {
            switch model.phase {
            case .loading: ProgressView().controlSize(.large).tint(Palette.gold[1]).frame(maxWidth: .infinity, minHeight: 200)
            case .welcome: SetupPanel()
            case .working: StepsPanel()
            case .home: HomePanel()
            case .failed(let text): ErrorPanel(text: text)
            }
        }
        .padding(22)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular, in: .rect(cornerRadius: 26))
        .animation(.smooth(duration: 0.3), value: model.phase)
    }
}

/// A panel's heading: a small gold caption and an optional line under it
struct PanelHeading: View {
    let title: String
    var detail: String?
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title.uppercased()).font(.cinzel(13, .bold)).tracking(2.6).foregroundStyle(Palette.goldFill)
            if let detail { Text(detail).font(.callout).foregroundStyle(Palette.parchmentDim).fixedSize(horizontal: false, vertical: true) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: first install

struct SetupPanel: View {
    @Environment(LauncherModel.self) private var model
    // which selector the file panel is for: kept apart from the panel's own flag, which SwiftUI resets before it hands over the chosen file
    @State private var target: LauncherModel.Slot = .base
    @State private var showPanel = false
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            PanelHeading(title: "Install the game", detail: "Your own copy: the Switch release with Reign of the Warlock, version 1.0.34.0. Your keys are only read; nothing is uploaded.")
            VStack(spacing: 0) {
                row(.base, symbol: "shippingbox.fill", title: "Base game", detail: "The game package, .nsp or .xci")
                Divider().padding(.vertical, 4)
                row(.update, symbol: "arrow.down.circle.fill", title: "Update 1.0.34.0",
                    detail: model.baseGame?.pathExtension.lowercased() == "xci" ? "Not needed if your .xci already holds it" : "The update package (v2228224), .nsp")
                Divider().padding(.vertical, 4)
                row(.dlc, symbol: "sparkles", title: "Reign of the Warlock DLC", detail: "Optional: its data already ships in the update")
                Divider().padding(.vertical, 4)
                row(.keys, symbol: "key.fill", title: "prod.keys", detail: "The keys of your own console")
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            .background(.black.opacity(0.22), in: .rect(cornerRadius: 16))
            .overlay { if dropTargeted { RoundedRectangle(cornerRadius: 16).strokeBorder(Palette.goldFill, lineWidth: 1.5).allowsHitTesting(false) } }
            .dropDestination(for: URL.self) { urls, _ in drop(urls); return true } isTargeted: { dropTargeted = $0 }
            HStack(spacing: 10) {
                if let f = model.folder {
                    CheckMark(verdict: model.verdicts[.folder], checking: model.checking.contains(.folder)).font(.body)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(f.lastPathComponent).font(.callout).lineLimit(1).truncationMode(.middle).foregroundStyle(Palette.parchment)
                        if let v = model.verdicts[.folder] { VerdictText(verdict: v) }
                    }
                    Spacer(minLength: 6)
                    Button("Clear", systemImage: "xmark") { model.clear(.folder) }.labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small)
                } else {
                    Text("Already extracted?").font(.callout).foregroundStyle(Palette.parchmentDim)
                    Button("Choose a Folder…") { pick(.folder) }.buttonStyle(.glass).controlSize(.small)
                }
            }
        }
        .fileImporter(isPresented: $showPanel, allowedContentTypes: target == .folder ? [.folder] : [.item], allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            model.choose(url, for: target)
        }
    }

    private func pick(_ slot: LauncherModel.Slot) { target = slot; showPanel = true }

    private func row(_ slot: LauncherModel.Slot, symbol: String, title: String, detail: String) -> some View {
        SourceRow(symbol: symbol, title: title, detail: detail, value: model.url(slot), verdict: model.verdicts[slot], checking: model.checking.contains(slot),
                  choose: { pick(slot) }, clear: { model.clear(slot) })
    }

    /// Drops on the list: prod.keys by its extension, packages by the title id or version tag in their names, else the first empty selector
    private func drop(_ urls: [URL]) {
        for u in urls {
            switch u.pathExtension.lowercased() {
            case "keys": model.choose(u, for: .keys)
            case "nsp", "xci":
                if let slot = LauncherModel.slot(for: u) { model.choose(u, for: slot) }
                else if model.baseGame == nil { model.choose(u, for: .base) } else if model.gameUpdate == nil { model.choose(u, for: .update) } else { model.choose(u, for: .dlc) }
            default: if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { model.choose(u, for: .folder) }
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
        HStack(spacing: 11) {
            Group {
                if value == nil { Image(systemName: symbol).foregroundStyle(Palette.goldFill) }
                else { CheckMark(verdict: verdict, checking: checking) }
            }
            .font(.title3).frame(width: 24)
            ZStack(alignment: .leading) {
                // Room for the tallest state (title, file name, check line), so the row keeps its height when a file is chosen or cleared
                VStack(alignment: .leading, spacing: 1) {
                    Text(verbatim: " ").font(.body.weight(.semibold)); Text(verbatim: " ").font(.callout); Text(verbatim: " ").font(.caption)
                }
                .hidden()
                VStack(alignment: .leading, spacing: 1) {
                    Text(title).font(.body.weight(.semibold))
                    Text(value?.lastPathComponent ?? detail).font(.callout).foregroundStyle(value == nil ? .secondary : .primary)
                        .lineLimit(1).truncationMode(.middle).help(value?.path ?? detail)
                    if value != nil, let verdict { VerdictText(verdict: verdict) }
                }
            }
            Spacer(minLength: 8)
            if value != nil { Button("Clear", systemImage: "xmark", action: clear).labelStyle(.iconOnly).buttonStyle(.glass).controlSize(.small) }
            Button(value == nil ? "Choose…" : "Change…", action: choose).buttonStyle(.glass).controlSize(.small)
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

/// The check's one line (never wrapped, so the row's height holds; the whole text is in the tooltip): green for what was recognized,
/// amber for a note, red for why it is not the right file
struct VerdictText: View {
    let verdict: GameFileCheck.Verdict
    var body: some View {
        let (text, color): (String, Color) = switch verdict {
        case .ok(let s): (s, .green)
        case .note(let s): (s, .orange)
        case .wrong(let s), .elsewhere(_, let s): (s, Palette.blood)
        }
        Text(text).font(.caption).foregroundStyle(color).lineLimit(1).minimumScaleFactor(0.85).truncationMode(.tail).help(text)
    }
}

// MARK: running

struct StepsPanel: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        let done = model.steps.filter { $0.state == .done || $0.state == .kept }.count
        VStack(alignment: .leading, spacing: 14) {
            PanelHeading(title: model.working, detail: model.steps.isEmpty ? nil : "\(done) of \(model.steps.count) steps · you can keep using your Mac")
            let list = VStack(alignment: .leading, spacing: 8) { ForEach(model.steps) { StepLine(step: $0) } }.padding(.vertical, 2)
            ViewThatFits(in: .vertical) { list; ScrollView { list }.scrollIndicators(.never) }   // hugs the rows; scrolls only on a short window
            Text("If you quit, the next run continues where this one stopped.").font(.footnote).foregroundStyle(Palette.parchmentDim)
        }
    }
}

struct StepLine: View {
    let step: StepRow
    var body: some View {
        HStack(spacing: 11) {
            Group {
                switch step.state {
                case .waiting: Image(systemName: "circle.dotted").foregroundStyle(.tertiary)
                case .running: ProgressView().controlSize(.small).tint(Palette.gold[1])
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(Palette.goldFill)
                case .kept: Image(systemName: "checkmark.circle").foregroundStyle(.secondary)
                case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(Palette.blood)
                }
            }
            .font(.callout).frame(width: 18)
            Text(step.title).font(.callout).foregroundStyle(step.state == .waiting ? .secondary : .primary)
                .fontWeight(step.state == .running ? .semibold : .regular)
            Spacer()
            trailing.font(.caption.monospacedDigit()).foregroundStyle(.secondary)
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

struct HomePanel: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 14) {
            if let l = model.latest {
                PanelHeading(title: model.updateAvailable ? "Version \(l.version) is ready" : "What's new", detail: model.updateAvailable ? nil : "Version \(l.version) · \(l.date)")
                CappedScroll(cap: 170) {
                    Text(notes(l.notes)).font(.callout).foregroundStyle(Palette.parchment).lineSpacing(3)
                        .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                }
                Link(destination: URL(string: "https://github.com/PowerBeef/d2r-metal/releases")!) {
                    Label("Release notes", systemImage: "arrow.up.right.square").font(.callout)
                }
                .foregroundStyle(Palette.gold[1])
            } else {
                PanelHeading(title: "What's new", detail: model.feedError.map { "Couldn't check for updates: \($0)" } ?? "Checking for updates…")
            }
            Divider()
            VStack(alignment: .leading, spacing: 9) {
                Text("YOUR GAME").font(.cinzel(11, .bold)).tracking(2.2).foregroundStyle(Palette.parchmentDim)
                if let i = model.installed { InfoRow(label: "Version", value: "\(i.version) (\(i.build))") }
                InfoRow(label: "Location", value: model.installedApp.deletingLastPathComponent().path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
                HStack {
                    Text("Updates").font(.callout).foregroundStyle(.secondary).frame(width: 74, alignment: .leading)
                    Picker("Updates", selection: $model.channel) {
                        Text("Releases").tag(Channel.stable)
                        Text("With pre-releases").tag(Channel.prerelease)
                    }
                    .pickerStyle(.menu).labelsHidden().fixedSize().controlSize(.small)
                }
                if let up = model.launcherUpdate {
                    Label("A new launcher (\(up.version)) is on the releases page", systemImage: "sparkles").font(.callout).foregroundStyle(Palette.gold[1])
                }
            }
        }
    }

    private func notes(_ s: String) -> AttributedString {   // the release notes are Markdown; show their inline styling and links
        (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(s)
    }
}

/// Content at its own height up to a cap, scrolling beyond it (release notes, an error's text)
struct CappedScroll<Content: View>: View {
    var cap: CGFloat
    @ViewBuilder var content: Content
    @State private var height: CGFloat = 0
    var body: some View {
        ScrollView { content.onGeometryChange(for: CGFloat.self) { $0.size.height } action: { height = $0 } }
            .frame(height: min(max(height, 1), cap)).scrollBounceBehavior(.basedOnSize)
    }
}

struct InfoRow: View {
    let label: String, value: String
    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).font(.callout).foregroundStyle(.secondary).frame(width: 74, alignment: .leading)
            Text(value).font(.callout).foregroundStyle(Palette.parchment).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
        }
    }
}

struct ErrorPanel: View {
    @Environment(LauncherModel.self) private var model
    let text: String
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("Something went wrong", systemImage: "exclamationmark.triangle.fill").font(.title3.weight(.semibold)).foregroundStyle(Palette.blood)
            CappedScroll(cap: 240) { Text(text).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
            if model.logPath != nil { Button("Show Log", systemImage: "doc.text.magnifyingglass") { model.showLog() }.buttonStyle(.glass) }
        }
    }
}

// MARK: the action bar

struct ActionBar: View {
    @Environment(LauncherModel.self) private var model
    @State private var confirmRemove = false

    var body: some View {
        GlassEffectContainer(spacing: 14) {
            HStack(spacing: 14) {
                primary
                if case .home = model.phase { gear }
                status.frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 6)
                trailing
            }
            .padding(.horizontal, 18).padding(.vertical, 14)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
        }
        .confirmationDialog("Move the game files and D2R Metal to the Trash?", isPresented: $confirmRemove) {
            Button("Move to Trash", role: .destructive) { model.removeGameData() }
        } message: {
            Text("Your characters and settings are kept: they are stored separately and are not touched.")
        }
    }

    /// The one big action, ember-tinted: Install, Play or Update; Stop while a job runs; Back after a failure
    @ViewBuilder private var primary: some View {
        switch model.phase {
        case .welcome:
            big("Install", symbol: "flame.fill") { model.install() }.disabled(!model.gameInputReady || model.latest == nil)
        case .home:
            if model.updateAvailable { big("Update", symbol: "arrow.down.circle.fill") { model.update() } }
            else { big("Play", symbol: "play.fill") { model.play() } }
        case .working:
            Button { model.cancel() } label: { Label("Stop", systemImage: "stop.fill").font(.cinzel(17, .bold)).tracking(1.5).frame(width: 150).padding(.vertical, 6) }
                .buttonStyle(.glass).controlSize(.extraLarge)
        case .failed:
            big("Back", symbol: "arrow.uturn.backward") { model.readState(); model.phase = model.installed == nil ? .welcome : .home }
        case .loading:
            big("Play", symbol: "play.fill") {}.disabled(true)
        }
    }

    private func big(_ title: String, symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: symbol).font(.cinzel(19, .bold, relativeTo: .title2)).tracking(2.2).frame(width: 150).padding(.vertical, 6)
        }
        .primaryGlass().controlSize(.extraLarge).keyboardShortcut(.defaultAction)
    }

    private var gear: some View {
        @Bindable var model = model
        return Menu {
            Button("Check for Updates", systemImage: "arrow.clockwise") { Task { await model.refreshFeed() } }
            if model.updateAvailable { Button("Play Without Updating", systemImage: "play") { model.play() } }
            Button("Repair", systemImage: "wrench.and.screwdriver") { model.repair() }
            if model.canRevert { Button("Go Back to the Previous Version", systemImage: "clock.arrow.circlepath") { model.revert() } }
            Divider()
            Button("Show Game in Finder", systemImage: "folder") { model.showGame() }
            if model.logPath != nil { Button("Show Log", systemImage: "doc.text.magnifyingglass") { model.showLog() } }
            Divider()
            Button("Remove the Game…", systemImage: "trash", role: .destructive) { confirmRemove = true }
        } label: {
            Image(systemName: "gearshape.fill").font(.title3).frame(width: 30, height: 30)
        }
        .menuIndicator(.hidden).buttonStyle(.glass).controlSize(.extraLarge).fixedSize()
        .help("Updates, repair, the game's folder")
    }

    @ViewBuilder private var status: some View {
        switch model.phase {
        case .loading: EmptyView()
        case .welcome:
            StatusText(title: model.gameInputReady ? "Ready to install" : "Choose your game files",
                       detail: "About 30 GB free while installing · 10 to 15 minutes the first time")
        case .working:
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(model.runningStep?.title ?? model.working).font(.headline).foregroundStyle(Palette.parchment).lineLimit(1)
                    Spacer()
                    if let eta = model.etaText { Text(eta).font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
                }
                ProgressView(value: model.overallProgress).tint(Palette.ember)
            }
        case .home:
            if let i = model.installed {
                StatusText(title: "D2R Metal \(i.version)",
                           detail: model.updateAvailable ? "Version \(model.latest?.version ?? "") is ready to install"
                                   : model.feedError != nil ? "Couldn't check for updates" : model.latest == nil ? "Checking for updates…" : "Up to date")
            }
        case .failed:
            StatusText(title: "Something went wrong", detail: "Nothing is lost: fix what the panel says, then try again", tint: Color(red: 1, green: 0.66, blue: 0.55))   // light coral: readable on the ember glass
        }
    }

    @ViewBuilder private var trailing: some View {
        if case .working = model.phase, model.logPath != nil {
            Button("Show Log", systemImage: "doc.text.magnifyingglass") { model.showLog() }.labelStyle(.iconOnly).buttonStyle(.glass).help("Show the log")
        }
    }
}

struct StatusText: View {
    let title: String, detail: String
    var tint: Color = Palette.parchment
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.headline).foregroundStyle(tint).lineLimit(1)
            Text(detail).font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
        }
    }
}
