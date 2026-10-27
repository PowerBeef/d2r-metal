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
dm/tools/re/re.sh reflect     # the engine's own class / field / enum registrations → export/reflect.json (reflect.py)
dm/tools/re/re.sh tables      # the data tables' columns and where each table hangs → export/tables.json (tables.py; runs the game: server stopped)
dm/tools/re/re.sh autoname    # bulk naming candidates → $DM_ROOT/re/names_auto.tsv (autoname.py; prints coverage)
dm/tools/re/re.sh seed        # what the repo already knows, plus the reflection and data tables and the bulk names (below)
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

`DM_RE_PORT` moves the server and the client off port 8000.

In a main-checkout Claude Code session the same server is an MCP server: `claude mcp add --transport http -s local ghidra http://127.0.0.1:8000/mcp`. Worktree sessions and subagents use `re.sh q`.

## What `seed` writes

All of it is re-runnable; hand-given names (Ghidra's `USER_DEFINED`) are never overwritten.

- Every NVN function-pointer slot from `dm/gfx/nvn/nvn_slots.tsv` as `pfn_<name>`, typed from `nvn.h` when it parses.
- Every source comment that mentions `main+0x…`, as a `[dm]` pre-comment at that address.
- The rewrite rules (`main.cuts/obs/params.tsv` of the build output) as `dm-rewrite` bookmarks.
- The widget vtables of `dm/host/dm_widget.h` as `vt_<Class>`, and structures under `/dm`: the widget base and per-class fields, the unit → path → room → level chain (`dm_unit.h`, `dm/verify/tick_state_format.md`), and the game object with its five unit hash tables, the inventory and the item data (the reading notes in `dm/host/diag.c`).
- The curated name table `$DM_ROOT/re/names.tsv`: `offset<TAB>name<TAB>confidence<TAB>source`, one row per name, `#` for comments. Confidence is `high`, `medium` or `low`. It wins over every other source.
- The engine's reflection tables from `reflect.py`. The game registers its UI and data classes with their parent class and a table of fields (the names layout JSON uses, with byte offsets); `reflect.py` runs each registration in a CPU emulator (Unicorn, project venv) and reads the tables back. `seed` turns them into one structure per class under `/dm/reflect` (parent fields included), `type_<Class>` descriptors, `vt_Type_<Class>`, `<Class>_Create` factories and `<Class>_GetType` getters, and makes the message channels and each channel's actions into enums under `/dm/enums` (values = the FNV-1a 64 hashes the game compares against). Enums whose values the game builds in code go in `$DM_ROOT/re/enums.tsv` (`enum<TAB>name<TAB>value<TAB>source`).
- The data tables from `tables.py`. At startup the game reads each excel table into an array of fixed-size records, driven by a column array on the reader's caller's stack: 0x20-byte entries of name, kind, length or bit number, offset in the record, and a link or callback. Those arrays exist only during the call, so `tables.py` takes them from the running game: lldb starts the headless CLI host, breaks at the table reader (`main+0x4d9040`) to copy the whole column array, at its allocation point (`main+0x4d96b0`) for the record array and row count, and when the master loader returns (`main+0x2410e0`) looks for each record array in the three table root objects (`[main+0x2d64a28/a38/a48]`). The game reads every table twice, once per data version (loader flags 2 and 3). `seed` turns this into one record structure per table under `/dm/tables` (`txt_<table>`, fields named after the columns, flag bits merged into one `u32` whose comment lists the bits, lookups commented with the table they look up), a `DataTables` structure for the root objects (each table's record pointer and row count, plus the merged item table at `+0x15c0`) and the three root pointers (`[main+0x2d64a18 + version × 16]`, data versions 1–3 as in `G+0x106` and unit `+0x1bd`; version 1 shares version 2's root) as typed `g_tables_v1..v3`. About a minute; it refuses while the query server is up.

- The bulk naming candidates `$DM_ROOT/re/names_auto.tsv` from `autoname.py`, rows marked `high` or `medium` only, after `names.tsv` (its addresses and hand names are never touched). `autoname.py` derives names mechanically for functions still unnamed: a `Class::Method` string only that function references (`Class__Method`), a library source-file path in its asserts (`<library>_<file>_<offset>`), a whole function that is one branch to a named one (`j_<name>`), a slot only one known vtable references (`<Class>_vf<slot>`, from `vt_<Class>` labels, the C++ type info that library code keeps, and the vtable each reflected class's factory stores; game code has no type info, and a factory that takes several vtables is only listed as `low`), a data-table callback (`TxtCb_<table>_<column>`), and the client command packet handlers read one by one (`$DM_ROOT/re/notes/c2s_labels.tsv`). Short wrappers around one named function are listed as `low` for review and not written. Review the file before `seed`; it prints naming coverage before and after.

## Where the names go

`export` writes `$DM_ROOT/re/export/main.names.tsv` (offset, name, kind, confidence, source) and `types.h`. `dm/tools/guestsyms.py` reads the names file when it exists, so `crashlog.py`, `dm where` / `dm bt` in lldb and `sampagg.py` show engine function names; a name whose confidence is not `high` carries it in brackets.

## Files

| File | What it does |
|---|---|
| `re.sh` | Entry point: environment, project lock, server start / stop |
| `nso2elf.py`, `test_nso2elf.py` | NSO → ELF; unit test on a hand-built NSO (no game files) |
| `ghidra_db.py` | `import`, `analyze`, `baseline`, `check` |
| `reflect.py` | Extracts the engine's reflection registrations (classes, fields, enums) by emulation |
| `tables.py` | Records the data tables' column arrays and record arrays from the running game (lldb) |
| `autoname.py` | Bulk naming candidates from strings, thunks, vtables, table callbacks and packet handlers |
| `seed.py` | Writes the repo's knowledge, the reflection and data tables and the accepted bulk names into the project |
| `export.py` | Names and types as text, local history commit |
| `serve.py` | Starts pyghidra-mcp without the startup index |
| `q.py` | Command-line client of the server |
