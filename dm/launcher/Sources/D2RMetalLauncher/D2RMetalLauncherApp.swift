import AppKit
import SwiftUI

@main
struct D2RMetalLauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = LauncherModel()

    var body: some Scene {
        Window("D2R Metal Launcher", id: "main") {
            ContentView()
                .environment(model)
                .onAppear { delegate.model = model }
                // d2rmetal://update: the installed game's main-menu notice opens the launcher for the update (dm/host/app_mac.m upd_click)
                .onOpenURL { url in if url.host == "update" { Task { await model.checkForUpdates() } } }
        }
        .windowStyle(.hiddenTitleBar)          // the dark window reaches the top edge; the window's buttons sit on its top bar
        .windowBackgroundDragBehavior(.enabled)
        .windowResizability(.contentMinSize)
        // the first window's size follows the display it opens on (later launches keep the size the player gave it): about two thirds of the
        // visible area, at least 980 x 640 when the display has room for it, at most 1440 x 900 (beyond that the page is capped and centered anyway)
        .defaultWindowPlacement { _, context in
            let r = context.defaultDisplay.visibleRect
            let w = min(r.width, max(980, min(1440, r.width * 0.66))), h = min(r.height, max(640, min(900, r.height * 0.72)))
            return WindowPlacement(size: CGSize(width: w, height: h))
        }
        .commands {
            CommandGroup(after: .appInfo) {
                Button("Check for Updates…") { Task { await model.checkForUpdates() } }.disabled(model.busy)
            }
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Game") {
                Button("Play") { model.play() }.keyboardShortcut("p").disabled(model.installed == nil || model.busy)
                Button("Show Game in Finder") { model.showGame() }.disabled(model.installed == nil)
                Button("Show Log") { model.showLog() }.keyboardShortcut("l").disabled(!model.hasLog)
                Divider()
                Button("Stop") { model.cancel() }.keyboardShortcut(".", modifiers: .command).disabled(!model.busy || model.stopping)
            }
        }
    }
}

/// Quitting (Command-Q, or closing the window, which quits) while a job runs asks first; on yes the job is stopped and its engine waited for, so
/// no tool keeps running after the launcher is gone and the next start resumes cleanly
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var model: LauncherModel?

    /// Dark only: the window, its menus, sheets and alerts
    func applicationWillFinishLaunching(_ notification: Notification) { NSApp.appearance = NSAppearance(named: .darkAqua) }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, model.busy, !model.quitting else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "Stop and quit?"
        alert.informativeText = "\(model.working) is still running. If you quit, it stops now and continues where it left off the next time you open the launcher."
        alert.addButton(withTitle: "Stop and Quit"); alert.addButton(withTitle: "Keep Running")
        guard alert.runModal() == .alertFirstButtonReturn else { return .terminateCancel }
        model.quitting = true
        Task { await model.stopForQuit(); NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}
