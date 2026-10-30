import SwiftUI

@main
struct D2RMetalLauncherApp: App {
    @State private var model = LauncherModel()

    var body: some Scene {
        Window("D2R Metal Launcher", id: "main") {
            ContentView()
                .environment(model)
                // d2rmetal://update: the installed game's main-menu notice opens the launcher for the update (dm/host/app_mac.m upd_click)
                .onOpenURL { url in if url.host == "update" { Task { await model.refreshFeed() } } }
        }
        .windowStyle(.hiddenTitleBar)          // the backdrop reaches the top edge; the traffic lights sit on it
        .windowBackgroundDragBehavior(.enabled)
        .windowResizability(.contentMinSize)
        .defaultSize(width: 740, height: 600)   // fits a 1280 x 720 screen (690 points under the menu bar)
    }
}
