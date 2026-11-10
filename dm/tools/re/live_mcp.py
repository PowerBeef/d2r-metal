#!/usr/bin/env python3
# MCP server `dm-live`: the live analysis harness as tools for a Claude Code session (stdio; run with $DM_ROOT/re/venv/bin/python, which has
# the mcp package). It keeps one headless game alive between calls, so a session can ask a question of the running game step by step:
#   game_start / game_step / game_stop   a headless CLI game with the live hooks on; each step is a scenario line (the control port's commands,
#                                        `live do=add …`, `stick 0 1 frames=60`, `state`, `expect …`); its outputs land in a run of the run store
#   run_scenario                         a whole scenario file into the run store
#   lookup / struct                      the snapshot: what an address is, a structure's fields
#   report / counts / cover / cover_diff / edges / view / ingest   the run store's reports (live.py) and the write-back (re.sh ingest)
# Register (main checkout): claude mcp add dm-live -s local -- "$DM_ROOT/re/venv/bin/python" -I "$PWD/dm/tools/re/live_mcp.py"
# What the tools return names game functions and carries runtime values: for the session only, never for a commit, an issue or a web page.
# One game run at a time: game_start refuses while a session is up.
import contextlib
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
import live  # noqa: E402
import live_index  # noqa: E402
import scenario  # noqa: E402


class Session:
    """One headless game with the hooks on, driven a scenario line at a time (the scenario runner's own steps and macros)."""
    def __init__(self):
        self.r = None; self.dir = None; self.n = 0; self.head = ''

    def start(self, save='warlock', level=None, env='', enter=True, name='session'):
        if self.r is not None: raise RuntimeError(f'a game session is already up (run {os.path.basename(self.dir)}): game_stop first')
        d = live.new_run(name); goto = f' DM_TEST_GOTO_LEVEL=150:{int(level)}' if level is not None else ''
        self.head = f'name {name}\nrequires cli\nsave {save}\nenv DM_LIVE=1 DM_LIVE_OUT={d}/live.bin{goto} {env}'.rstrip() + '\ntimeout 7200\n\n'
        r = scenario.Runner(self._scn('state'), 'cli', d, True)
        r.c = r.backend.start(); self.r, self.dir, self.n = r, d, 0
        return self.step('enter_game') if enter else {'run': os.path.basename(d)}

    def _scn(self, line):
        p = os.path.join(live.LIVE, 'session.scn'); os.makedirs(live.LIVE, exist_ok=True)
        with open(p, 'w', encoding='utf-8') as f: f.write(self.head + line + '\n')
        return p

    def step(self, line):
        if self.r is None: raise RuntimeError('no game session: game_start first')
        if not self.r.backend.alive(): self.r = None; raise RuntimeError(f'the game has exited (log: {self.dir}/host.log)')
        _, steps, _ = scenario.parse(self._scn(line)); st = steps[0]
        mark = self.r.cc.mark(); res = self.r.do(st); self.r.prev_mark = mark; self.n += 1
        with open(os.path.join(self.dir, 'session_steps.txt'), 'a', encoding='utf-8') as f: f.write(line + '\n')
        return res

    def stop(self):
        if self.r is None: return 'no game session'
        r, d = self.r, self.dir; self.r = None
        with contextlib.suppress(Exception): r.cc.cmd('live', do='flush')
        with contextlib.suppress(Exception): r.cc.cmd('quit')
        r.backend.wait_exit(10); r.backend.stop()
        steps = open(os.path.join(d, 'session_steps.txt'), encoding='utf-8').read() if os.path.exists(os.path.join(d, 'session_steps.txt')) else ''
        live.run_meta(d, scenario='(interactive session)', scenario_text=self.head + steps, verdict=f'session, {self.n} steps')
        return os.path.basename(d)


S = Session()


def text_of(fn, *a, **k):
    """A report function's printed text (the reports print; an MCP tool returns)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try: fn(*a, **k)
        except SystemExit as e: print(e)
    return buf.getvalue()[-20000:]


def quiet(fn, *a, **k):
    """Run with stdout captured (the protocol owns stdout); returns (result, printed)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): res = fn(*a, **k)
    return res, buf.getvalue()


def build():
    from mcp.server.fastmcp import FastMCP
    mcp = FastMCP('dm-live')

    @mcp.tool()
    def game_start(save: str = 'warlock', level: int | None = None, env: str = '', name: str = 'session') -> str:
        """Start a headless game with the live hooks on and enter a game with the test save. level = a levels.txt area to warp to (8 = Den of
        Evil); env = extra DM_… settings as K=V words. One session at a time; outputs go to a new run of the run store."""
        res, _ = quiet(S.start, save, level, env, True, name)
        return f'run {os.path.basename(S.dir)}: {res}'

    @mcp.tool()
    def game_step(line: str) -> str:
        """One scenario line on the running game, e.g. `live do=add at=main+0x5b9290 kind=ret cap=[x0+0x4].w type=x0:Unit max=500`,
        `live do=load file=/path/trace.txt`, `live do=counts file=counts.tsv`, `live do=cover file=/path/blocks.txt`, `live do=watch
        at=main+0x31e0848 chain=66c6 len=2`, `stick 0 1 frames=60`, `sleep 3`, `state`, `mem main+0x… 16`. Returns the step's reply."""
        try: res, _ = quiet(S.step, line)
        except Exception as e: return f'RED: {e}'   # noqa: BLE001 -- the tool reports every failure as text
        return str(res)[:6000]

    @mcp.tool()
    def game_stop() -> str:
        """Flush the hooks' records, quit the game and close the run. Returns the run's name for the report tools."""
        res, _ = quiet(S.stop); return str(res)

    @mcp.tool()
    def run_scenario(path: str, name: str = '') -> str:
        """Run a whole scenario file (headless CLI host, hooks on) into the run store and report it."""
        if S.r is not None: return 'a game session is up: game_stop first (one game run at a time)'
        return text_of(live.cmd_run, path, name or None)

    @mcp.tool()
    def lookup(where: str) -> str:
        """What the snapshot knows about main+0x…: the function, its register arguments, whether the rewriter changed that instruction (never
        hook those), and for a call site its target or that it is computed."""
        off = int(where.split('+')[-1], 16); ix = live_index.Index(); f = ix.function(off)
        if not f: return f'{where}: no function there (snapshot: re.sh live-index)'
        args = ', '.join(f'x{r} {n}: {t}' for r, (n, t) in sorted(ix.args(f[0]).items())); c = ix.call(off)
        return (f'main+{f[0]:#x} {f[2]} (+{off - f[0]:#x} of {f[1]:#x}); args: {args or "none known"}; blocks: {len(ix.leaders(f[0]))}'
                + (f'; rewritten: {ix.never_hook(off)}' if ix.never_hook(off) else '') + (f'; call site → {"computed" if c[1] is None else f"main+{c[1]:#x}"}' if c else ''))

    @mcp.tool()
    def struct(name: str) -> str:
        """A /dm structure's fields from the snapshot (offset, name, type, size), for typed captures: cap=[x0+0x<offset>].w with type=x0:<name>."""
        ix = live_index.Index(); rows = ix.db.execute('select off, name, type, size from fields where struct = ? order by off', (name,)).fetchall() if ix.db else []
        return '\n'.join(f'+{o:#x}\t{n}\t{t}\t{s_}' for o, n, t, s_ in rows) or f'no structure {name}'

    @mcp.tool()
    def report(run: str = 'latest') -> str:
        """Everything a run holds: hooks with typed arguments and callers, watchpoint writers, the census, coverage files, resolved call sites."""
        return text_of(lambda: live.report(live.resolve_run(run)))

    @mcp.tool()
    def counts(run: str = 'latest', file: str = 'counts.tsv', top: int = 40) -> str:
        """The call-trace census of a run (`live do=counts file=…`): functions that ran, by calls."""
        return text_of(lambda: live.counts(os.path.join(live.resolve_run(run), file), top))

    @mcp.tool()
    def cover(run: str = 'latest', file: str = 'cov.tsv', top: int = 40) -> str:
        """A coverage file of a run (`live do=cover_dump file=…`): functions by blocks that ran."""
        return text_of(lambda: live.cover(os.path.join(live.resolve_run(run), file), top))

    @mcp.tool()
    def cover_diff(a: str, b: str, run: str = 'latest', top: int = 60) -> str:
        """Blocks that ran in coverage file b and not in a (same run): what only the second action runs."""
        d = live.resolve_run(run); return text_of(lambda: live.cover_diff(os.path.join(d, a), os.path.join(d, b), top))

    @mcp.tool()
    def edges(run: str = 'latest') -> str:
        """Computed call sites the run resolved: (site, function reached, count)."""
        return text_of(lambda: live.edges(live.resolve_run(run)))

    @mcp.tool()
    def view(where: str, run: str = 'latest', cov: str = '') -> str:
        """The annotated view of one function for a run: decompiled lines with coverage, hits and values, as a private local HTML file. Returns
        its path (needs the function's decompile cache: built on demand when the query server is stopped)."""
        return text_of(lambda: live.view(live.resolve_run(run), where, cov or None))

    @mcp.tool()
    def ingest(run: str = 'latest', dry_run: bool = True) -> str:
        """Write a run's findings into the analysis project (comments, computed-call references, coverage bookmarks); dry_run shows what
        would change. The query server must be stopped."""
        p = subprocess.run(['bash', os.path.join(REPO, 'dm', 'tools', 're', 're.sh'), 'ingest', live.resolve_run(run)] + (['--dry-run'] if dry_run else []), capture_output=True, text=True, check=False)
        return (p.stdout + p.stderr)[-6000:]

    return mcp


if __name__ == '__main__':
    build().run()
