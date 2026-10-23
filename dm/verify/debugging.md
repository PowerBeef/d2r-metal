# Debugging

中文：[debugging.zh-CN.md](debugging.zh-CN.md)

Tools for finding out what went wrong in a run: crash records, symbols for the game program, the debugger, validation and sanitizer builds, and automatic exploration.

## Crashes

When the host is started with `DM_CRASH_JSON=<path>`, its crash handler (`dm/host/crash.c`) writes a structured record there before printing the usual crash lines. The scenario runner sets it to `crash.json` in the result folder. The handler uses only async-signal-safe calls, so the record is written even when the log dump hangs.

| Field | Meaning |
|---|---|
| `sig`, `code`, `addr` | Signal, `si_code`, faulting address |
| `pc`, `lr`, `sp`, `fp`, `x[0..28]` | Registers |
| `pc_where`, `lr_where` | `main+0x…` / `sdk+0x…` when inside a game module, otherwise `null` (host code) |
| `bt` | Frame-pointer backtrace, each `{pc, where}` |
| `modules` | Load range of each game module |
| `host_base` | Load address of the host executable, for `atos -l` |
| `tick`, `stage` | Last tick and stage the watchdog recorded |

After printing, every fatal signal is re-raised with the default action. The process therefore ends by that signal and the system writes its own crash report (`.ips`); a shell still sees exit code 128 + signal. macOS suppresses repeated reports of the same process, so `crash.json` is the record to rely on.

```bash
python3 dm/tools/crashlog.py show crash.json [--bin dm/host/dmhost]     # host frames via atos (file:line), game frames via the symbol table
python3 dm/tools/crashlog.py collect --process dmhost --since <epoch>  # copy and summarize the system .ips reports
```

When a scenario's process crashes, the runner symbolicates the record into `crash.txt`, collects any `.ips`, and the verdict names the crash (`crash SIGSEGV at cmd_crash (dm_ctl_cmds.c:280)`). The control port's `crash kind=segv|abort|trap` command crashes the host on purpose, for testing this path.

## Symbols for the game program

```bash
python3 dm/tools/guestsyms.py build                       # → $DM_ROOT/build/cache/dm/symbols/<product>.tsv (local only, never committed)
python3 dm/tools/guestsyms.py lookup main+0x455aa4 sdk+0x2b0470
```

- **Function boundaries** come from each module's `.eh_frame` (about 57,000 functions in `main`, 29,000 in `sdk`).
- **Names** come from the dynamic symbol table, demangled; `sdk` exports most of them.
- **Notes** come from the source comments that mention `module+0x…`. For example, `main+0x455aa4` is 0x24 into `fn_455a80`, the function the comments describe as building the health-bar info.

`crashlog.py` and anything else that prints `module+offset` can use this table.
