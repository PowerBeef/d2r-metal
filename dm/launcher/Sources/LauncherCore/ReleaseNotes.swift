import Foundation

// The release notes for the launcher's "What's new" card. The manifest carries them as Markdown (release_mac.sh --notes); this splits them into
// blocks (headings, paragraphs, list items with their depth, quotes, code, tables) that the card lays out one by one, keeping each block's inline
// styling and links. Parsed inline only, as the card used to, "## What's new" and the "- " of every list item showed as literal text.

public enum NotesBlock: Equatable, Sendable {
    case heading(AttributedString)
    case paragraph(AttributedString)
    /// A list item's paragraph. Depth 0 is the outer list; the marker is "•", "◦" or "3.", or "" for a later paragraph of the same item
    case item(depth: Int, marker: String, AttributedString)
    case quote(AttributedString)
    case code(String)
    case table(header: [AttributedString], rows: [[AttributedString]])
}

public enum ReleaseNotes {
    public static func blocks(_ markdown: String) -> [NotesBlock] {
        let options = AttributedString.MarkdownParsingOptions(interpretedSyntax: .full, failurePolicy: .returnPartiallyParsedIfPossible)
        guard let doc = try? AttributedString(markdown: markdown, options: options) else {
            return markdown.isEmpty ? [] : [.paragraph(AttributedString(markdown))]
        }
        var out: [NotesBlock] = []
        var lastItem: Int?   // the list item the previous block was in, so its next paragraph gets no second marker
        var table: (id: Int, header: [AttributedString], rows: [Int: [AttributedString]])?
        func endTable() {
            if let t = table { out.append(.table(header: t.header, rows: t.rows.keys.sorted().map { t.rows[$0]! })) }
            table = nil
        }
        // every block is one run of the same presentation intent, whose components go from the innermost (the paragraph) outwards
        for (intent, range) in doc.runs[\.presentationIntent] {
            var text = AttributedString(doc[range])
            text.presentationIntent = nil
            guard !text.characters.isEmpty else { continue }
            let parts = intent?.components ?? []
            if let tableID = parts.first(where: { if case .table = $0.kind { true } else { false } })?.identity {
                if table?.id != tableID { endTable(); table = (tableID, [], [:]) }
                if parts.contains(where: { $0.kind == .tableHeaderRow }) { table!.header.append(text); continue }
                for p in parts { if case .tableRow(let row) = p.kind { table!.rows[row, default: []].append(text) } }
                continue
            }
            endTable()
            if let i = parts.firstIndex(where: { if case .listItem = $0.kind { true } else { false } }) {
                guard case .listItem(let ordinal) = parts[i].kind else { continue }
                let depth = parts.filter { if case .listItem = $0.kind { true } else { false } }.count - 1
                let ordered = i + 1 < parts.count && parts[i + 1].kind == .orderedList
                let marker = lastItem == parts[i].identity ? "" : ordered ? "\(ordinal)." : depth == 0 ? "•" : "◦"
                out.append(.item(depth: depth, marker: marker, text))
                lastItem = parts[i].identity
                continue
            }
            lastItem = nil
            switch parts.first?.kind {
            case .header: out.append(.heading(text))
            case .codeBlock: out.append(.code(String(text.characters).trimmingCharacters(in: .newlines)))
            default: out.append(parts.contains { $0.kind == .blockQuote } ? .quote(text) : .paragraph(text))
            }
        }
        endTable()
        return out
    }
}
