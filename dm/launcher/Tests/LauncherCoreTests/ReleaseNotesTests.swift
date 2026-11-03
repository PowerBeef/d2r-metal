import Foundation
import Testing
@testable import LauncherCore

private func plain(_ b: NotesBlock) -> String {
    switch b {
    case .heading(let t): "H " + String(t.characters)
    case .paragraph(let t): "P " + String(t.characters)
    case .item(let depth, let marker, let t): "I\(depth) \(marker) " + String(t.characters)
    case .quote(let t): "Q " + String(t.characters)
    case .code(let s): "C " + s
    case .table(let header, let rows): "T " + ([header] + rows).map { $0.map { String($0.characters) }.joined(separator: "|") }.joined(separator: " / ")
    }
}

@Test func notesBlocksFromReleaseMarkdown() {
    let md = """
    D2R Metal 1.1 plays with a **mouse and keyboard**.

    ## What's new

    - **PC controls.** The keys are D2R's PC defaults:
      - left click walks;
      - I opens the inventory.
    - A controller brings the console interface back.

    1. Open the launcher.
    2. Press **Update**.

    > Saves stay as they are.

    | File | What it is |
    |---|---|
    | `a.dmg` | The launcher |
    | `b.zip` | The same, for updates |

    **Not yet:** rebinding. See [the guide](https://example.invalid/mac).
    """
    let blocks = ReleaseNotes.blocks(md)
    #expect(blocks.map(plain) == [
        "P D2R Metal 1.1 plays with a mouse and keyboard.",
        "H What's new",
        "I0 • PC controls. The keys are D2R's PC defaults:",
        "I1 ◦ left click walks;",
        "I1 ◦ I opens the inventory.",
        "I0 • A controller brings the console interface back.",
        "I0 1. Open the launcher.",
        "I0 2. Press Update.",
        "Q Saves stay as they are.",
        "T File|What it is / a.dmg|The launcher / b.zip|The same, for updates",
        "P Not yet: rebinding. See the guide.",
    ])
    // inline styling and links survive in each block
    guard case .paragraph(let last) = blocks.last else { Issue.record("no closing paragraph"); return }
    #expect(last.runs.contains { $0.link?.absoluteString == "https://example.invalid/mac" })
    #expect(last.runs.contains { $0.inlinePresentationIntent == .stronglyEmphasized })
    #expect(!last.runs.contains { $0.presentationIntent != nil })
}

@Test func notesBlocksPlainText() {
    #expect(ReleaseNotes.blocks("").isEmpty)
    #expect(ReleaseNotes.blocks("First release").map(plain) == ["P First release"])
}
