"""验证用的公共断言库（原 touchcases/lib.py；touchcases 那边留了同名的转接，用例照旧 import lib）。

一、触屏全链用例的局目录（touchchain.sh）：load(目录) → Result，读 c2s / ticks / uimsg / state 四份记录。
    面板开关按 uimsg.jsonl 每行的 ``tick``（宿主 DM_UIMSG 在局内写服务端当前拍号）对齐；
    局外（无 tick）的消息归入此前最近的拍号，首个拍号之前归入拍 0。
二、事件流（DM_EVENTS，event_stream.md）：events(路径) → Events，按事件问面板、存档、红标。
三、日志红标守卫：logred(日志, allow=[正则…]) → 没被放行的红标行（【红】、🔴、FATAL、CRASH、看门狗）；命令行 assertlib.py logred <日志> [--allow 正则]… 打 LOGRED OK|RED。
Shared assertion library for verification (formerly touchcases/lib.py; touchcases keeps a shim of the same name, so cases still import lib).

1. Run directories of touch full-chain cases (touchchain.sh): load(dir) -> Result, reading the four records c2s / ticks / uimsg / state.
   Panel open/close is aligned by the ``tick`` of each line in uimsg.jsonl (in-run, the host's DM_UIMSG writes the server's current tick number);
   out-of-run messages (no tick) are attributed to the most recent preceding tick number, and those before the first tick number to tick 0.
2. The event stream (DM_EVENTS, event_stream.md): events(path) -> Events, asking about panels, saves and red markers by event.
3. Log red-marker guard: logred(log, allow=[regex...]) -> the red-marker lines not allowed (【红】, 🔴, FATAL, CRASH, watchdog); from the shell, assertlib.py logred <log> [--allow regex]... prints LOGRED OK|RED.
"""

import json
import re
import sys
from pathlib import Path


class AssertFail(AssertionError):
    pass


_FAILURES = []
_CHECKS = 0
_TICK_RE = re.compile(r"^(?:DM:Tick:)?(\d+)(?:#)?$")


def check(cond, msg):
    global _CHECKS
    _CHECKS += 1
    if not cond:
        _FAILURES.append(str(msg))
    return bool(cond)


def _read_jsonl(path):
    try:
        with path.open(encoding="utf-8") as stream:
            return [json.loads(line) for line in stream if line.strip()]
    except (OSError, ValueError) as exc:
        raise AssertFail(f"无法读取 {path}: {exc}") from exc


def _required(directory, name):
    path = directory / name
    if not path.is_file():
        raise AssertFail(f"缺少必需文件：{path}")
    return path


def _tick_from_message(row):
    if isinstance(row.get("tick"), int):
        return row["tick"]
    if row.get("cat") == "DM" and row.get("act") == "Tick":
        match = _TICK_RE.match(str(row.get("arg", "")))
        if match:
            return int(match.group(1))
    if row.get("act") == "DM:Tick":
        match = _TICK_RE.match(str(row.get("arg", "")))
        if match:
            return int(match.group(1))
    return None


class Result:
    def __init__(self, directory, c2s, ticks, uimsg, state):
        self.directory = directory
        self._c2s = c2s
        self._ticks = ticks
        self._uimsg = uimsg
        self._state = state
        self._state_by_tick = {}
        for row in state:
            tick = row.get("tick")
            if isinstance(tick, int):
                self._state_by_tick[tick] = row
        self._panel_events = self._build_panel_events(uimsg)

    @staticmethod
    def _build_panel_events(rows):
        current_tick = 0
        events = []
        for row in rows:
            if isinstance(row.get("tick"), int):      # 宿主 DM_UIMSG 局内每行带服务端拍号 | In-run, each line of the host's DM_UIMSG carries the server tick number
                current_tick = row["tick"]
            else:
                marker_tick = _tick_from_message(row)
                if marker_tick is not None:
                    current_tick = marker_tick
                    continue
            action = row.get("act")
            panel = row.get("arg")
            if not panel or action not in {
                "OpenPanel", "OpenExclusivePanel", "UnloadPanel", "ClosePanel"
            }:
                continue
            events.append((current_tick, action, panel))
        return events

    def _state_at(self, tick):
        rows = [item for item in self._state_by_tick if item <= tick]
        if not rows:
            return None
        return self._state_by_tick[max(rows)]

    def items_at(self, tick):
        row = self._state_at(tick)
        if row is None:
            return {}
        return {item["id"]: dict(item) for item in row.get("items", [])}

    def cursor_at(self, tick):
        row = self._state_at(tick)
        return None if row is None else row.get("cursor")

    def players_at(self, tick):
        row = self._state_at(tick)
        return None if row is None else row.get("players")

    def panels_at(self, tick):
        panels = set()
        for event_tick, action, panel in self._panel_events:
            if event_tick > tick:
                break
            if action == "OpenExclusivePanel":
                panels.clear()
                panels.add(panel)
            elif action == "OpenPanel":
                panels.add(panel)
            else:
                panels.discard(panel)
        return panels

    def c2s_ops(self, t0=None, t1=None):
        result = []
        for row in self._c2s:
            if row.get("q") == "sys":
                continue
            tick = row.get("tick")
            if not isinstance(tick, int) or (t0 is not None and tick < t0) or (
                t1 is not None and tick > t1
            ):
                continue
            packet = str(row.get("pkt", ""))
            if len(packet) < 2:
                raise AssertFail(f"命令包缺少 opcode：tick={tick} pkt={packet!r}")
            try:
                opcode = int(packet[:2], 16)
            except ValueError as exc:
                raise AssertFail(f"命令包不是十六进制：tick={tick} pkt={packet!r}") from exc
            result.append((tick, opcode, packet))
        return result

    def state_rows(self):
        return list(self._state)

    def tick_units(self):
        return [row["u"] for row in self._ticks if isinstance(row.get("u"), dict)]


def load(directory):
    directory = Path(directory)
    paths = {name: _required(directory, name) for name in (
        "c2s.jsonl", "ticks.jsonl", "uimsg.jsonl", "state.jsonl"
    )}
    return Result(
        directory,
        _read_jsonl(paths["c2s.jsonl"]),
        _read_jsonl(paths["ticks.jsonl"]),
        _read_jsonl(paths["uimsg.jsonl"]),
        _read_jsonl(paths["state.jsonl"]),
    )


# ---------- 事件流 | Event stream ----------
class Events:
    """一局的事件（dm/tools/dmevents.py 读的那种：NDJSON 文件或夹着「[dm] EV」行的日志）。 | One run's events (what dm/tools/dmevents.py reads: an NDJSON file or a log with "[dm] EV" lines)."""
    BAD = ("red", "fatal", "crash", "watchdog")

    def __init__(self, rows):
        self.rows = list(rows)

    def of(self, ev, **match):
        return [e for e in self.rows if e.get("ev") == ev and all(e.get(k) == v for k, v in match.items())]

    def first(self, ev, **match):
        hit = self.of(ev, **match)
        return hit[0] if hit else None

    def panels_open(self, upto_seq=None):
        """到 upto_seq 为止开着的面板（按 panel 事件的开 / 关推）。 | Panels open up to upto_seq (from the open / close panel events)."""
        open_ = set()
        for e in self.rows:
            if upto_seq is not None and e.get("seq", 0) > upto_seq: break
            if e.get("ev") != "panel": continue
            if e.get("act") == "OpenExclusivePanel": open_ = {e.get("name")}
            elif e.get("act") == "OpenPanel": open_.add(e.get("name"))
            elif e.get("act") in ("ClosePanel", "UnloadPanel"): open_.discard(e.get("name"))
        return open_

    def saves(self):
        return self.of("save", result="commit")

    def bad(self, allow_codes=()):
        return [e for e in self.rows if e.get("ev") in self.BAD and e.get("code") not in set(allow_codes)]


def events(path):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    import dmevents   # noqa: PLC0415  # pyright: ignore[reportMissingImports]
    return Events(dmevents.load(str(path)))


# ---------- 日志红标守卫 | Log red-marker guard ----------
RED_MARKERS = re.compile(r"【红】|🔴|\bFATAL\b|CRASH sig=|看门狗")


def logred(log_path, allow=()):
    """日志里没被放行的红标行（行号, 文本）。allow = 正则，命中即放行（已知、可容忍的红）。 | Red-marker lines in the log that are not allowed (line number, text). allow = regexes; a hit is let through (known, tolerated reds)."""
    pats = [re.compile(a) for a in allow]
    out = []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if RED_MARKERS.search(line) and not any(p.search(line) for p in pats):
                out.append((n, line.rstrip("\n")[:300]))
    return out


def logred_main(argv):
    import argparse   # noqa: PLC0415
    ap = argparse.ArgumentParser(prog="assertlib.py logred", description="Fail when a log has red-marker lines that are not allowed.")
    ap.add_argument("log"); ap.add_argument("--allow", action="append", default=[], help="regex of a tolerated red line (repeatable)")
    a = ap.parse_args(argv)
    bad = logred(a.log, a.allow)
    for n, line in bad[:20]: print(f"  {n}: {line}")
    print(f"LOGRED {'RED' if bad else 'OK'} {len(bad)} 行红标 | red-marker lines" + (f"（放行 {len(a.allow)} 条规则 | {len(a.allow)} allow rules）" if a.allow else ""))
    return 1 if bad else 0


def main(argv=None):
    if argv and argv[0] == "logred": return logred_main(argv[1:])
    del argv
    if _FAILURES:
        print(f"红 {len(_FAILURES)} 条")
        for failure in _FAILURES:
            print(f"- {failure}")
        return 1
    print(f"绿 {_CHECKS} 条")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
