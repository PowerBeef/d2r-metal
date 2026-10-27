#!/bin/bash
# 引擎分析库的总入口：环境（Java 25、Ghidra 12.2、$DM_ROOT/re/venv）、工程锁、查询服务的起停。产物全在 $DM_ROOT/re，不入库。
#   setup     建 $DM_ROOT/re 与 Python 3.13 venv（pyghidra 取 Ghidra 自带的、pyghidra-mcp 定版）
#   convert   游戏 ExeFS 的 main → re/in/main.elf（nso2elf.py）
#   reflect   引擎反射表（类、字段、枚举）→ re/export/reflect.json（reflect.py，用项目 venv 的 unicorn，不碰工程）
#   tables    数据表的列描述与表根位置 → re/export/tables.json（tables.py：lldb 下跑一次无窗口游戏到读完表；查询服务开着时拒跑）
#   import | analyze | baseline | check | seed | export   → ghidra_db.py / seed.py / export.py（要求查询服务没在跑：工程同一时刻只能有一个进程打开）
#   serve [--index] | stop | status   查询服务 pyghidra-mcp（serve.py；127.0.0.1:${DM_RE_PORT:-8000}，唯一持有工程的进程）
#   q …       查询服务的命令行客户端（q.py：反编译 / 交叉引用 / 改名 / 注释，地址一律写 main+0x…）
#   snap      把工程打包成 re/snap/main-<时间>.gzf，只留最近两份
# Entry point for the engine-analysis database: environment (Java 25, Ghidra 12.2, $DM_ROOT/re/venv), the project lock, starting and stopping the query server. All output stays in $DM_ROOT/re, never committed.
#   setup     creates $DM_ROOT/re and the Python 3.13 venv (pyghidra from Ghidra's own bundle, pyghidra-mcp at a pinned version)
#   convert   the game's ExeFS main -> re/in/main.elf (nso2elf.py)
#   reflect   the engine's reflection tables (classes, fields, enums) -> re/export/reflect.json (reflect.py, with unicorn from the project venv; does not touch the project)
#   tables    the data tables' column descriptors and where each table hangs -> re/export/tables.json (tables.py: runs the headless game under lldb until the tables are read; refuses while the query server is up)
#   import | analyze | baseline | check | seed | export   -> ghidra_db.py / seed.py / export.py (the query server must not be running: only one process can open the project at a time)
#   serve [--index] | stop | status   the pyghidra-mcp query server (serve.py; 127.0.0.1:${DM_RE_PORT:-8000}, the only process that holds the project)
#   q ...     command-line client for the query server (q.py: decompile / xrefs / rename / comment, addresses always written main+0x...)
#   snap      packs the project into re/snap/main-<time>.gzf, keeping the latest two
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DM_ROOT="${DM_ROOT:-$HOME/dm-work}"
RE="$DM_ROOT/re"
GHIDRA="${GHIDRA_INSTALL_DIR:-/opt/homebrew/Cellar/ghidra/12.2/libexec}"
export GHIDRA_INSTALL_DIR="$GHIDRA"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25/libexec/openjdk.jdk/Contents/Home}"
export DM_ROOT
PY="$RE/venv/bin/python"
PORT="${DM_RE_PORT:-8000}"
LOCK="$RE/ghidra/d2r.lock~"           # Ghidra 持有通道锁的是这个文件 | the file Ghidra holds the channel lock on
PIDF="$RE/log/serve.pid"
PYGHIDRA_MCP_VERSION=0.2.7

die() { echo "🔴 $*" >&2; exit 1; }
need_free() {   # 工程没被别的进程打开：锁文件在、且有进程开着它（Ghidra 用通道锁，进程退出后留下的锁文件不算） | the project is not open in another process: the lock file exists and a process has it open (Ghidra uses channel locks; a lock file left by an exited process does not count)
  [ -e "$LOCK" ] && lsof "$LOCK" >/dev/null 2>&1 && die "the Ghidra project is open (lock $LOCK): run 're.sh stop' first, or close the Ghidra window"
  return 0
}
serving() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

cmd="${1:-status}"; shift || true
mkdir -p "$RE/log"; cd "$RE/log"       # JVM 崩溃文件（hs_err_pid*）落在工作目录 | JVM crash files (hs_err_pid*) land in the working directory
case "$cmd" in
  setup)
    [ -x "$JAVA_HOME/bin/java" ] || die "Java 25 not found at $JAVA_HOME (brew install openjdk@25)"
    [ -d "$GHIDRA/Ghidra" ] || die "Ghidra not found at $GHIDRA (brew install ghidra; then brew pin ghidra)"
    mkdir -p "$RE"/{ghidra,export,notes,snap,in,ref}
    [ -x "$PY" ] || uv venv --python python3.13 "$RE/venv"
    VIRTUAL_ENV="$RE/venv" uv pip install --find-links "$GHIDRA/Ghidra/Features/PyGhidra/pypkg/dist" \
      "pyghidra==$(ls "$GHIDRA/Ghidra/Features/PyGhidra/pypkg/dist" | sed -n 's/^pyghidra-\(.*\)\.tar\.gz$/\1/p')" \
      "pyghidra-mcp==$PYGHIDRA_MCP_VERSION" 'lz4==4.4.5'
    [ -d "$RE/.git" ] || git -C "$RE" init -q
    printf '#!/bin/sh\necho "✗ %s holds private engine analysis: it is never pushed anywhere." >&2\nexit 1\n' "$RE" > "$RE/.git/hooks/pre-push"
    chmod +x "$RE/.git/hooks/pre-push"
    printf '/ghidra/\n/snap/\n/log/\n/ref/\n/in/\n/venv/\n' > "$RE/.gitignore"
    echo "RE-SETUP OK"
    ;;
  convert)
    "$PY" -I "$HERE/nso2elf.py" "$DM_ROOT/data/switch/3.3.94012/Update/ExeFS/main" "$RE/in/main.elf"
    ;;
  reflect)
    "$DM_ROOT/build/cache/dm/venv/bin/python" -I "$HERE/reflect.py" "$@"
    ;;
  tables)
    python3 -I "$HERE/tables.py" "$@"
    ;;
  import|analyze|baseline|check)
    need_free; "$PY" -I "$HERE/ghidra_db.py" "$cmd" "$@"
    ;;
  seed|export)
    need_free; "$PY" -I "$HERE/$cmd.py" "$@"
    ;;
  serve)
    serving && { echo "already serving on 127.0.0.1:$PORT (pid $(cat "$PIDF"))"; exit 0; }
    need_free
    JAVA_TOOL_OPTIONS="-Xmx${GHIDRA_SERVE_MAXMEM:-6g}" nohup "$PY" -I "$HERE/serve.py" --project-path "$RE/ghidra/d2r.gpr" \
      -t streamable-http -o 127.0.0.1 -p "$PORT" --no-force-analysis "$@" > "$RE/log/serve.log" 2>&1 &
    echo $! > "$PIDF"
    for _ in $(seq 1 120); do
      curl -s -o /dev/null "http://127.0.0.1:$PORT/mcp" && { echo "SERVE OK 127.0.0.1:$PORT pid $(cat "$PIDF")"; exit 0; }
      kill -0 "$(cat "$PIDF")" 2>/dev/null || { tail -20 "$RE/log/serve.log"; die "the server exited"; }
      sleep 1
    done
    die "the server did not answer within 120 s (see $RE/log/serve.log)"
    ;;
  q)
    "$PY" -I "$HERE/q.py" "$@"
    ;;
  stop)
    if serving; then
      "$PY" -I "$HERE/q.py" save >/dev/null || die "the server did not save; it is left running (pid $(cat "$PIDF"))"
      # SIGINT：服务存盘后关工程；它还在等手头的索引活时，第二次 SIGINT 直接退出（上一步已存盘，不丢改动）
      # SIGINT: the server saves, then closes the project; while it still waits for in-flight indexing, a second SIGINT exits at once (saved in the step above, nothing lost)
      kill -INT "$(cat "$PIDF")"
      for _ in $(seq 1 60); do kill -0 "$(cat "$PIDF")" 2>/dev/null || break; sleep 1; done
      kill -0 "$(cat "$PIDF")" 2>/dev/null && kill -INT "$(cat "$PIDF")"
      for _ in $(seq 1 30); do kill -0 "$(cat "$PIDF")" 2>/dev/null || break; sleep 1; done
      kill -0 "$(cat "$PIDF")" 2>/dev/null && die "the server did not stop (pid $(cat "$PIDF"))"
    fi
    rm -f "$PIDF"; echo "STOP OK"
    ;;
  status)
    serving && echo "serving: 127.0.0.1:$PORT pid $(cat "$PIDF")" || echo "server: stopped"
    [ -e "$LOCK" ] && lsof "$LOCK" >/dev/null 2>&1 && echo "project: open" || echo "project: free"
    du -sh "$RE/ghidra" 2>/dev/null | sed 's/^/project size: /'
    ;;
  snap)
    need_free
    out="$RE/snap/main-$(date +%Y%m%d-%H%M).gzf"
    "$PY" -I - "$out" <<'EOF'
import sys, pyghidra
from pyghidra.launcher import HeadlessPyGhidraLauncher
HeadlessPyGhidraLauncher().start()
from java.io import File
from ghidra.util.task import TaskMonitor
import os
project = pyghidra.open_project(os.path.join(os.environ['DM_ROOT'], 're', 'ghidra'), 'd2r')
project.getProjectData().getFile('/main.elf').packFile(File(sys.argv[1]), TaskMonitor.DUMMY)
project.close()
EOF
    ls -1t "$RE"/snap/main-*.gzf | tail -n +3 | while read -r old; do rm -f "$old"; done
    echo "SNAP OK $out ($(du -h "$out" | cut -f1))"
    ;;
  *) die "usage: re.sh setup|convert|reflect|tables|import|analyze|baseline|check|seed|export|serve|stop|status|snap|q" ;;
esac
