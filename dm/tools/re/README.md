# dm/tools/re: the engine-analysis database

中文: [README.zh-CN.md](README.zh-CN.md)

A private, persistent Ghidra database of the game's `main` program, so that fixes can rest on what the engine actually does. Sessions and people query it through a local server; the names it collects flow back into the repo's debugging tools.

**Everything it produces stays on your machine, in `$DM_ROOT/re` (default `~/dm-work/re`).** Decompiled code, disassembly, string dumps and Ghidra files never go into this repository, an issue, a pull request or a web page (CONTRIBUTING rule 1, [docs/DERIVED.md](../../../docs/DERIVED.md)). What may enter the repo is what already does: addresses with descriptions in our own words, and original tool code like this folder. The local `pre-commit` guard refuses disassembler databases and staged lines that look like decompiler output. `$DM_ROOT/re` keeps its own local git history for the exports and notes, with no remote and a hook that refuses every push.

## Requirements

- Ghidra 12.2 (`brew install ghidra`, then `brew pin ghidra`: an upgrade converts projects one way) and Java 25 (`brew install openjdk@25`).
- Python 3.13 for PyGhidra (its JPype wheels stop at 3.13) and `uv`. `re.sh setup` builds `$DM_ROOT/re/venv`; the project venv is not used.
- The extracted game files (`./setup.sh`), about 3 GB of disk for the project, and memory: run one Ghidra process at a time, not next to a game run or an Xcode build.

## Building the database (once)

```bash
dm/tools/re/re.sh setup       # $DM_ROOT/re, the 3.13 venv (pyghidra, pyghidra-mcp), local history
dm/tools/re/re.sh convert     # ExeFS main → re/in/main.elf (nso2elf.py)
dm/tools/re/re.sh import      # into the project; a function at each .eh_frame start; non-returning imports
dm/tools/re/re.sh check       # relocations, relocated bytes, text bytes, eh_frame functions → CHECK OK
dm/tools/re/re.sh analyze     # Ghidra auto-analysis incl. Decompiler Parameter ID (long: run in the background)
dm/tools/re/re.sh baseline    # decompile every function once → log/decomp_baseline.json
dm/tools/re/re.sh seed        # what the repo already knows (below)
dm/tools/re/re.sh export      # names and types as text, committed to the local history
dm/tools/re/re.sh snap        # a .gzf snapshot (the last two are kept)
```

- `nso2elf.py` turns the NSO into a standard ELF shared object, so Ghidra's own ELF loader applies all 63,284 relocations and names the 656 imports. It adds section headers (Ghidra only analyses unwind tables in blocks named `.eh_frame*` and only treats `.got*` as read-only). No byte is changed.
- **Addresses.** The program sits at base `0x7100000000`, so the low 32 bits of a project address are the `main+0x…` offset: a function Ghidra calls `FUN_71…` followed by `005d3bc0` is `main+0x5d3bc0`. Tools take and print `main+0x…`.

## Querying it

Only one process can open the project at a time. The query server is that process while it runs; every other command refuses to start until `re.sh stop`.

```bash
dm/tools/re/re.sh serve       # pyghidra-mcp on 127.0.0.1:8000 (serve.py: no full index at startup; --index for one)
dm/tools/re/re.sh q decompile main+0x5d3bc0 [--callees --xrefs --strings]
dm/tools/re/re.sh q xrefs main+0x2f05458
dm/tools/re/re.sh q callgraph main+0xb1dc60 [--called]
dm/tools/re/re.sh q strings PanelManager      # also: find <name>, bytes <addr> [n]
dm/tools/re/re.sh q name main+0x… NewName     # rename a function (saved)
dm/tools/re/re.sh q comment main+0x… "what it does"
dm/tools/re/re.sh stop        # SIGINT: the server saves, then closes the project
```

In a main-checkout Claude Code session the same server is an MCP server: `claude mcp add --transport http -s local ghidra http://127.0.0.1:8000/mcp`. Worktree sessions and subagents use `re.sh q`.

## What `seed` writes

All of it is re-runnable; hand-given names (Ghidra's `USER_DEFINED`) are never overwritten.

- Every NVN function-pointer slot from `dm/gfx/nvn/nvn_slots.tsv` as `pfn_<name>`, typed from `nvn.h` when it parses.
- Every source comment that mentions `main+0x…`, as a `[dm]` pre-comment at that address.
- The rewrite rules (`main.cuts/obs/params.tsv` of the build output) as `dm-rewrite` bookmarks.
- The widget vtables of `dm/host/dm_widget.h` as `vt_<Class>`, and structures under `/dm`: the widget base and per-class fields, and the unit → path → room → level chain (`dm_unit.h`, `dm/verify/tick_state_format.md`).
- The curated name table `$DM_ROOT/re/names.tsv`: `offset<TAB>name<TAB>confidence<TAB>source`, one row per name, `#` for comments. Confidence is `high`, `medium` or `low`.

## Where the names go

`export` writes `$DM_ROOT/re/export/main.names.tsv` (offset, name, kind, confidence, source) and `types.h`. `dm/tools/guestsyms.py` reads the names file when it exists, so `crashlog.py`, `dm where` / `dm bt` in lldb and `sampagg.py` show engine function names; a name whose confidence is not `high` carries it in brackets.

## Files

| File | What it does |
|---|---|
| `re.sh` | Entry point: environment, project lock, server start / stop |
| `nso2elf.py`, `test_nso2elf.py` | NSO → ELF; unit test on a hand-built NSO (no game files) |
| `ghidra_db.py` | `import`, `analyze`, `baseline`, `check` |
| `seed.py` | Writes the repo's knowledge into the project |
| `export.py` | Names and types as text, local history commit |
| `serve.py` | Starts pyghidra-mcp without the startup index |
| `q.py` | Command-line client of the server |
