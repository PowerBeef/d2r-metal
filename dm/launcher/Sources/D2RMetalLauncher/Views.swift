import LauncherCore
import SwiftUI
import UniformTypeIdentifiers

// The window, dark only (Theme.swift): a thin top bar with the icon and the name; the game's page in the main area (what to do now: choosing the
// game files, the install steps, what's new, an error), in cards; a side column with the installed game's details and every action; and a bottom
// bar with the one main button and the status or the progress of the running job.

struct ContentView: View {
    @Environment(LauncherModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(spacing: 0) {
            TopBar()
            Theme.line.frame(height: 1)
            HStack(spacing: 0) {
                ScrollView { MainPage().padding(28).frame(maxWidth: .infinity, alignment: .leading) }.scrollIndicators(.automatic)
                Theme.line.frame(width: 1)
                ScrollView { SideColumn().padding(.horizontal, 20).padding(.vertical, 24) }.frame(width: 284).scrollIndicators(.never)
            }
            Theme.line.frame(height: 1)
            ActionBar().padding(.horizontal, 24).frame(height: 76)
        }
        .ignoresSafeArea(.container, edges: .top)   // the top bar shares its line with the window's buttons
        .background(Theme.bg)
        .foregroundStyle(Theme.text)
        .tint(Theme.accent)
        .frame(minWidth: 900, minHeight: 580)
        .preferredColorScheme(.dark)
        .task { await model.start() }
        .alert("Quit D2R Metal to continue?", isPresented: $model.quitGamePrompt) {
            Button("Quit D2R Metal") { Task { await model.quitGameAndContinue() } }
            Button("Cancel", role: .cancel) { model.cancelWaitingJob() }
        } message: {
            Text("D2R Metal is running, and its files are about to change. Leave your game first so your progress is saved; the launcher then quits it and goes on.")
        }
    }
}

// MARK: top bar

struct TopBar: View {
    var body: some View {
        HStack(spacing: 9) {
            Brand.icon.resizable().interpolation(.high).frame(width: 22, height: 22).accessibilityHidden(true)
            Text("D2R Metal").font(.system(size: 13, weight: .semibold))
            Text("Launcher").font(.system(size: 13)).foregroundStyle(Theme.text3)
            Spacer()
        }
        .padding(.leading, 86)   // the window's buttons sit at the left of this bar
        .frame(height: 52)
    }
}

// MARK: the main page

struct MainPage: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            VStack(alignment: .leading, spacing: 5) {
                Text("Diablo II: Resurrected").font(.system(size: 26, weight: .semibold))
                Text("With Reign of the Warlock · built on this Mac from your own copy of the game").font(.system(size: 13)).foregroundStyle(Theme.text2)
            }
            if let notice = model.launcherNotice { Notice(kind: .warn, text: notice, link: ("Releases page", "https://github.com/PowerBeef/d2r-metal/releases")) }
            switch model.phase {
            case .loading: ProgressView().controlSize(.small).frame(maxWidth: .infinity, minHeight: 120)
            case .welcome: SetupCards()
            case .working: StepsCard()
            case .home: WhatsNewCard()
            case .failed(let text): ErrorCard(text: text)
            }
        }
    }
}

/// A line of warning, error or information above a page's cards
struct Notice: View {
    enum Kind { case info, warn, error }
    let kind: Kind, text: String
    var link: (String, String)?
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: kind == .info ? "info.circle.fill" : "exclamationmark.triangle.fill")
                .foregroundStyle(kind == .info ? Theme.text2 : kind == .warn ? Theme.warn : Theme.bad).accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 6) {
                Text(text).font(.system(size: 13)).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                if let link, let u = URL(string: link.1) { Link(destination: u) { Label(link.0, systemImage: "arrow.up.right") }.font(.system(size: 13)).foregroundStyle(Theme.text2) }
            }
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.surface))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(kind == .info ? Theme.line : (kind == .warn ? Theme.warn : Theme.bad).opacity(0.45)))
        .accessibilityElement(children: .combine)
    }
}

// MARK: first install

struct SetupCards: View {
    @Environment(LauncherModel.self) private var model
    // which selector the file panel is for: kept apart from the panel's own flag, which SwiftUI resets before it hands over the chosen file
    @State private var target: LauncherModel.Slot = .base
    @State private var showPanel = false
    @State private var otherFiles = false
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if model.latest == nil, let e = model.feedError {
                Notice(kind: .info, text: "Can't check for releases. " + (model.online ? e : "This Mac is offline; the launcher tries again when it's back."))
            }
            if let why = model.gameDamaged {
                Notice(kind: .warn, text: "Your game files are damaged or incomplete (\(why)). Choose your game packages or an extracted folder again; the damaged files are replaced.")
            }
            if model.hasGameFiles && model.gameDamaged == nil && model.nothingChosen && !otherFiles {
                VStack(alignment: .leading, spacing: 10) {
                    SectionTitle(text: "Finish installing")
                    VStack(alignment: .leading, spacing: 14) {
                        HStack(alignment: .firstTextBaseline, spacing: 10) {
                            Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.ok).accessibilityHidden(true)
                            VStack(alignment: .leading, spacing: 3) {
                                Text("Your game files are on this Mac").font(.system(size: 14, weight: .medium))
                                Text("From the install that stopped. Continue picks up where it left off; the files are checked again first.")
                                    .font(.system(size: 13)).foregroundStyle(Theme.text2).fixedSize(horizontal: false, vertical: true)
                            }
                        }
                        Button("Use Other Game Files…") { otherFiles = true }.buttonStyle(QuietButtonStyle())
                    }
                    .card()
                }
            } else {
                VStack(alignment: .leading, spacing: 10) {
                    SectionTitle(text: "Your game files")
                    VStack(spacing: 0) {
                        row(.base, title: "Base game", detail: "The game package, .nsp or .xci")
                        Theme.line.frame(height: 1)
                        row(.update, title: "Update 1.0.34.0",
                            detail: model.baseGame?.pathExtension.lowercased() == "xci" ? "Not needed if your .xci already holds it" : "The update package (v2228224), .nsp")
                        Theme.line.frame(height: 1)
                        row(.dlc, title: "Reign of the Warlock", detail: "Optional: its data already ships in the update")
                        Theme.line.frame(height: 1)
                        row(.keys, title: "prod.keys", detail: "The keys of your own console")
                    }
                    .padding(.horizontal, 16).padding(.vertical, 4)
                    .background(RoundedRectangle(cornerRadius: 12).fill(Theme.surface))
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(dropTargeted ? Theme.accent : Theme.line))
                    .dropDestination(for: URL.self) { urls, _ in drop(urls); return true } isTargeted: { dropTargeted = $0 }
                    HStack(spacing: 10) {
                        if model.folder != nil {
                            row(.folder, title: "Extracted folder", detail: "").padding(.horizontal, 16).card(padding: 0)
                        } else {
                            Text("Already extracted (ExeFS and RomFS)?").font(.system(size: 13)).foregroundStyle(Theme.text2)
                            Button("Choose a Folder…") { pick(.folder) }.buttonStyle(QuietButtonStyle())
                        }
                    }
                    Text("Your own copy of the Switch release with Reign of the Warlock, version 1.0.34.0; files can be dropped on the list. Your keys are only read, and nothing is uploaded. About 30 GB of free space is needed while installing; the first install takes 10 to 15 minutes.")
                        .font(.system(size: 12)).foregroundStyle(Theme.text3).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .fileImporter(isPresented: $showPanel, allowedContentTypes: target == .folder ? [.folder] : [.item], allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            model.choose(url, for: target)
        }
    }

    private func pick(_ slot: LauncherModel.Slot) { target = slot; showPanel = true }

    private func row(_ slot: LauncherModel.Slot, title: String, detail: String) -> some View {
        FileRow(title: title, detail: detail, value: model.url(slot), verdict: model.verdicts[slot], checking: model.checking.contains(slot),
                choose: { pick(slot) }, clear: { model.clear(slot) })
    }

    /// Drops: prod.keys by its extension, packages by the title id or version tag in their names, else the first empty selector
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

/// One selector: what it is, the file chosen and what its check found, and the buttons to choose and clear. Three lines are always reserved,
/// so a row keeps its height when a file is chosen or its check line appears
struct FileRow: View {
    let title: String, detail: String
    let value: URL?
    let verdict: GameFileCheck.Verdict?
    let checking: Bool
    let choose: () -> Void
    let clear: () -> Void
    var body: some View {
        HStack(spacing: 12) {
            CheckMark(verdict: verdict, checking: checking).frame(width: 18)
            ZStack(alignment: .leading) {
                VStack(alignment: .leading, spacing: 2) { Text(verbatim: " ").font(.system(size: 14)); Text(verbatim: " ").font(.system(size: 12)); Text(verbatim: " ").font(.system(size: 11)) }.hidden()
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.system(size: 14, weight: .medium))
                    Text(value?.lastPathComponent ?? detail).font(.system(size: 12)).foregroundStyle(Theme.text2)
                        .lineLimit(1).truncationMode(.middle).help(value?.path ?? detail)
                    if value != nil, let verdict { VerdictText(verdict: verdict) }
                }
            }
            Spacer(minLength: 8)
            if value != nil {
                Button("Clear", systemImage: "xmark", action: clear).labelStyle(.iconOnly).buttonStyle(.plain).foregroundStyle(Theme.text3)
                    .accessibilityLabel("Clear \(title)").help("Clear")
            }
            Button(value == nil ? "Choose…" : "Change…", action: choose).buttonStyle(QuietButtonStyle())
                .accessibilityLabel(value == nil ? "Choose \(title)" : "Change \(title)")
        }
        .padding(.vertical, 10)
    }
}

/// The result of a file's check: green when it is the right file, red when it is not, a spinner while checking, an empty circle before
struct CheckMark: View {
    let verdict: GameFileCheck.Verdict?
    let checking: Bool
    var body: some View {
        switch verdict {
        case _ where checking: ProgressView().controlSize(.mini).accessibilityLabel("Checking")
        case .ok?, .note?: Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.ok).accessibilityLabel("Checked")
        case .wrong?, .elsewhere?: Image(systemName: "xmark.circle.fill").foregroundStyle(Theme.bad).accessibilityLabel("Not usable")
        case nil: Image(systemName: "circle").foregroundStyle(Theme.text3).accessibilityHidden(true)
        }
    }
}

/// The check's one line (never wrapped, so the row keeps its height; the whole text is in the tooltip)
struct VerdictText: View {
    let verdict: GameFileCheck.Verdict
    var body: some View {
        let (text, color): (String, Color) = switch verdict {
        case .ok(let s): (s, Theme.ok)
        case .note(let s): (s, Theme.warn)
        case .wrong(let s), .elsewhere(_, let s): (s, Theme.bad)
        }
        Text(text).font(.system(size: 11)).foregroundStyle(color).lineLimit(1).truncationMode(.tail).help(text)
    }
}

// MARK: running

struct StepsCard: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        let done = model.steps.filter { $0.state == .done || $0.state == .kept }.count
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle(text: model.stopping ? "Stopping…" : model.steps.isEmpty ? model.working : "\(model.working) · \(done) of \(model.steps.count) steps")
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(model.steps.enumerated()), id: \.element.id) { i, step in
                    if i > 0 { Theme.line.frame(height: 1) }
                    StepLine(step: step)
                }
            }
            .padding(.horizontal, 16).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 12).fill(Theme.surface))
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Theme.line))
            Text(model.stopping ? "Waiting for the running step to stop cleanly." : "You can keep using your Mac. If you stop or quit, the next run continues where this one stopped.")
                .font(.system(size: 12)).foregroundStyle(Theme.text3)
        }
    }
}

struct StepLine: View {
    let step: StepRow
    var body: some View {
        HStack(spacing: 12) {
            Group {
                switch step.state {
                case .waiting: Circle().strokeBorder(Theme.text3, lineWidth: 1.2).frame(width: 13, height: 13)
                case .running: ProgressView().controlSize(.mini).tint(Theme.accent)
                case .done: Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(Theme.text2)
                case .kept: Image(systemName: "checkmark").font(.system(size: 12, weight: .regular)).foregroundStyle(Theme.text3)
                case .failed: Image(systemName: "xmark").font(.system(size: 12, weight: .bold)).foregroundStyle(Theme.bad)
                }
            }
            .frame(width: 18)
            Text(step.title).font(.system(size: 13, weight: step.state == .running ? .semibold : .regular))
                .foregroundStyle(step.state == .waiting ? Theme.text3 : step.state == .running ? Theme.text : Theme.text2)
            Spacer()
            trailing.font(.system(size: 12)).foregroundStyle(Theme.text3).monospacedDigit()
        }
        .padding(.vertical, 9)
        .accessibilityElement(children: .combine)
        .accessibilityValue(stateText)
    }

    @ViewBuilder private var trailing: some View {
        if step.state == .running, step.total > 0 {
            Text(step.id == "download" || step.id == "ldownload" ? "\(step.done) of \(step.total) MB" : "\(step.done) of \(step.total)")
        } else if step.state == .kept {
            Text("Up to date")
        } else if let s = step.seconds, step.state == .done {
            Text(s < 60 ? "\(Int(s.rounded())) s" : "\(Int((s / 60).rounded())) min")
        }
    }

    private var stateText: String {
        switch step.state {
        case .waiting: "waiting"
        case .running: step.total > 0 ? "running, \(step.done) of \(step.total)" : "running"
        case .done: "done"
        case .kept: "up to date"
        case .failed: "failed"
        }
    }
}

// MARK: installed

struct WhatsNewCard: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let l = model.latest {
                SectionTitle(text: model.updateAvailable ? "Version \(l.version) is available" : "What's new in version \(l.version)")
                VStack(alignment: .leading, spacing: 12) {
                    Text(notes(l.notes)).font(.system(size: 13)).lineSpacing(3).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Link(destination: URL(string: "https://github.com/PowerBeef/d2r-metal/releases")!) { Label("Release notes on GitHub", systemImage: "arrow.up.right") }
                            .font(.system(size: 13)).foregroundStyle(Theme.text2)
                        Spacer()
                        if !l.date.isEmpty { Text("Released \(l.date)").font(.system(size: 12)).foregroundStyle(Theme.text3) }
                    }
                }
                .card()
                if model.updateAvailable && !model.updateIsPrimary {
                    Text("You went back to the previous version. Update from the side column when you want.").font(.system(size: 12)).foregroundStyle(Theme.text3)
                }
            } else if let e = model.feedError {
                Notice(kind: .info, text: "Couldn't check for updates: \(e)")
            } else {
                Text("Checking for updates…").font(.system(size: 13)).foregroundStyle(Theme.text2)
            }
        }
    }

    private func notes(_ s: String) -> AttributedString {   // the release notes are Markdown; show their inline styling and links
        (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(s)
    }
}

struct ErrorCard: View {
    @Environment(LauncherModel.self) private var model
    let text: String
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle(text: "Something went wrong")
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.bad).accessibilityHidden(true)
                    Text(text).font(.system(size: 13)).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                }
                if model.hasLog { Button("Show Log") { model.showLog() }.buttonStyle(QuietButtonStyle()) }
            }
            .card()
            Text("Nothing is lost: fix what is described above, then try again. The log has the details; please attach it to an issue.")
                .font(.system(size: 12)).foregroundStyle(Theme.text3)
        }
    }
}

// MARK: side column

struct SideColumn: View {
    @Environment(LauncherModel.self) private var model
    @State private var confirmRemove = false

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            VStack(alignment: .leading, spacing: 10) {
                SectionTitle(text: "Installed")
                if let i = model.installed {
                    info("Version", "\(i.version) (\(i.build))")
                    info("Location", model.installedApp.deletingLastPathComponent().path.replacingOccurrences(of: NSHomeDirectory(), with: "~"), path: true)
                } else {
                    Text("Not installed yet").font(.system(size: 13)).foregroundStyle(Theme.text2)
                }
            }
            Theme.line.frame(height: 1)
            VStack(alignment: .leading, spacing: 12) {
                SectionTitle(text: "Manage")
                let idle = !model.busy
                Button("Check for Updates") { Task { await model.checkForUpdates() } }.buttonStyle(LinkButtonStyle()).disabled(!idle)
                if model.installed != nil {
                    if model.updateAvailable && !model.updateIsPrimary, let l = model.latest {
                        Button("Update to \(l.version)") { model.update() }.buttonStyle(LinkButtonStyle()).disabled(!idle)
                    } else if model.updateAvailable {
                        Button("Play Without Updating") { model.play() }.buttonStyle(LinkButtonStyle()).disabled(!idle)
                    }
                    Button("Repair") { model.repair() }.buttonStyle(LinkButtonStyle()).disabled(!idle)
                    if model.canRevert { Button("Go Back to the Previous Version") { model.revert() }.buttonStyle(LinkButtonStyle()).disabled(!idle) }
                    Button("Show Game in Finder") { model.showGame() }.buttonStyle(LinkButtonStyle())
                }
                if model.hasLog { Button("Show Log") { model.showLog() }.buttonStyle(LinkButtonStyle()) }
                if model.hasGameData {
                    Button("Remove the Game…") { confirmRemove = true }.buttonStyle(LinkButtonStyle(destructive: true)).disabled(!idle)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .confirmationDialog("Move the game files and D2R Metal to the Trash?", isPresented: $confirmRemove) {
            Button("Move to Trash", role: .destructive) { model.removeGameData() }
        } message: {
            Text("Your characters and settings are kept: they are stored separately and are not touched.")
        }
    }

    private func info(_ label: String, _ value: String, path: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.system(size: 12)).foregroundStyle(Theme.text3)
            Text(value).font(.system(size: 13)).lineLimit(path ? 2 : 1).truncationMode(.middle).textSelection(.enabled).help(value)
        }
        .accessibilityElement(children: .combine)
    }
}

// MARK: the action bar

struct ActionBar: View {
    @Environment(LauncherModel.self) private var model
    var body: some View {
        HStack(spacing: 20) {
            primary
            status.frame(maxWidth: .infinity, alignment: .leading)
            Text("Launcher \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development build")")
                .font(.system(size: 11)).foregroundStyle(Theme.text3)
        }
    }

    /// The one main action: Install / Continue / Try Again, Play / Update, Stop while a job runs, Back after a failure
    @ViewBuilder private var primary: some View {
        switch model.phase {
        case .loading: Button("Play") {}.buttonStyle(PrimaryButtonStyle()).disabled(true)
        case .welcome:
            if model.latest == nil && model.feedError != nil { main("Try Again") { model.refresh() } }
            else if model.canContinue { main("Continue") { model.install() }.disabled(model.latest == nil) }
            else { main("Install") { model.install() }.disabled(!model.gameInputReady || model.latest == nil) }
        case .home:
            if model.updateIsPrimary { main("Update") { model.update() } } else { main("Play") { model.play() } }
        case .working:
            Button(model.stopping ? "Stopping…" : "Stop") { model.cancel() }
                .buttonStyle(QuietButtonStyle(large: true))
                .keyboardShortcut(.cancelAction).disabled(model.stopping)
                .help("Stop (Esc, or Command-period); the next run continues where this one stopped")
        case .failed:
            main("Back") { model.readState(); model.phase = model.installed == nil ? .welcome : .home }
        }
    }

    private func main(_ title: String, action: @escaping () -> Void) -> some View {
        Button(title, action: action).buttonStyle(PrimaryButtonStyle()).keyboardShortcut(.defaultAction)
    }

    @ViewBuilder private var status: some View {
        switch model.phase {
        case .loading: Text("Checking…").font(.system(size: 13)).foregroundStyle(Theme.text2)
        case .working:
            VStack(alignment: .leading, spacing: 7) {
                HStack(alignment: .firstTextBaseline) {
                    Text(model.stopping ? "Stopping…" : model.runningStep?.title ?? model.working).font(.system(size: 13, weight: .medium)).lineLimit(1)
                    Spacer()
                    if !model.stopping, let eta = model.etaText { Text(eta).font(.system(size: 12)).foregroundStyle(Theme.text3).monospacedDigit() }
                }
                ProgressBar(value: model.overallProgress)
            }
        default:
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 13, weight: .medium)).lineLimit(1)
                Text(detail).font(.system(size: 12)).foregroundStyle(Theme.text2).lineLimit(1).truncationMode(.tail)
            }
        }
    }

    private var title: String {
        switch model.phase {
        case .welcome: model.latest == nil && model.feedError != nil ? "Can't check for releases" : model.canContinue ? "Ready to continue" : model.gameInputReady ? "Ready to install" : "Choose your game files"
        case .home: model.installed.map { "D2R Metal \($0.version)" } ?? ""
        case .failed: "Something went wrong"
        default: ""
        }
    }
    private var detail: String {
        switch model.phase {
        case .welcome:
            if model.latest == nil, model.feedError != nil { return model.online ? "GitHub can't be reached" : "This Mac is offline" }
            return model.latest.map { "Version \($0.version)" + (model.canContinue ? " · about 3 GB free needed" : " · about 30 GB free needed while installing") } ?? "Checking for releases…"
        case .home:
            return model.updateIsPrimary ? "Version \(model.latest?.version ?? "") is ready to install"
                : model.updateAvailable ? "Version \(model.latest?.version ?? "") is available"
                : model.feedError != nil ? "Couldn't check for updates" : model.latest == nil ? "Checking for updates…" : "Up to date"
        case .failed: return "Nothing is lost; the page above says what happened"
        default: return ""
        }
    }
}

/// A thin progress bar in the accent red
struct ProgressBar: View {
    let value: Double
    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Theme.raised)
                Capsule().fill(Theme.accent).frame(width: max(4, g.size.width * min(max(value, 0), 1)))
            }
        }
        .frame(height: 4)
        .accessibilityElement().accessibilityLabel("Progress").accessibilityValue("\(Int((value * 100).rounded())) percent")
    }
}
