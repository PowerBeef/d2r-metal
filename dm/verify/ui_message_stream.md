# UI Message Stream Format

中文：[ui_message_stream.zh-CN.md](ui_message_stream.zh-CN.md)

Every change in the game's UI (opening a panel, closing a panel, switching pages, moving the cursor…) is announced by the game itself with a UI message. Recording the UI messages of a run in order into a file shows the UI flow of a host run (`dmhost` from `dm/host`, which executes the rewritten game program on a Mac): tests assert on it (a panel opened, a page switched), and two runs can be compared message by message. This page defines the format of that file.

## Capture point

- All of the game's UI messages are dispatched through virtual function `+0x28` of the UI bus object `*(main + 0x2f05458)`; in `main` of 3.3.94012, the implementing function is `main + 0xb1dc60` (the vtable read at run time is authoritative; it is not hard-coded).
- At entry: `x0` = the bus object, `x1` → the message header `{+0x00 u64 category hash, +0x08 u64 action hash, +0x10 u64 payload pointer}`.
- The hash is FNV-1a 64 (offset basis `0xcbf29ce484222325`, multiplier `0x100000001b3`), computed byte by byte over the original string. Example: `PanelManager` = `0x7127a406e0b6452c`.
- The same virtual function occasionally has non-message calls (`x1` is not a readable pointer): these are not output.
- The header has one more field: `+0x18 u8` = 1 when the payload is a C string (the third part of a `Category:Action:Argument` message), 0 when it is a binary payload (a selector's value, an input event). Messages are delivered synchronously, with no queue: a handler may send another message before returning, so the stream is in call order (a nested message comes before the rest of its parent's handling).
- Message strings in layout files (`onClickMessage`, `onUpdateMessage`, …) are hashed when the layout loads, through the game's own name lists: the category must be one of its message channels and the action one of that channel's actions. **An unknown category or action becomes hash 0.** A button sends its click message only when both hashes are non-zero, so a click message with an unknown name is silently never sent; a selector still sends its update message, with category hash 0 (this is what the overlay's `TouchVideoMessage:<key>` selectors do).

## One line of JSON per message

UTF-8, terminated by `\n`, with a fixed field order:

```
{"seq":N,"cat_h":"0x7127a406e0b6452c","act_h":"0x...","arg":"MainMenuPanel","cat":"PanelManager","act":"OpenExclusivePanel"}
```

| Field | Required | Meaning |
|---|---|---|
| seq | Yes | Index within this run (starting from 0, counting only messages that are output) |
| cat_h / act_h | Yes | Category / action hash, `0x` + 16 lowercase hex digits. **Comparison uses only these two and arg** |
| arg | Yes | The C string the payload pointer points to (≤ 64 bytes, truncated at 0; must consist entirely of printable ASCII or UTF-8 high bytes), otherwise `null` |
| cat / act | No | Names, for human reading only: looked up in reverse by FNV-1a over all printable strings of ≥ 3 bytes in the read-only region `[0x1b79000, 0x2056000)` of `main`; omitted if not found |

- Time, frame number, thread number and pointer values are not output: these are not comparable between two runs.
- File location: given by the environment variable `DM_UIMSG=<path>`; by default nothing is recorded. The touch cases (`touchcases/*/assert.py`) and the scenario runner read it.
- Save copies must always be writable while recording: a read-only copy makes the save-writing step not happen, and the message flow changes.
