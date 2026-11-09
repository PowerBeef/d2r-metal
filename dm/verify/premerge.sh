#!/bin/bash
# Before a branch or worktree lands on main: build the host as the game tiers will, then the gate's quick and unit tiers, in this checkout
# (about 3 minutes). It catches what only shows when everything is built together (a symbol one test binary lacks, a file the Makefile
# doesn't list, a stale switch registry) before the main checkout's 15 to 45 minute game tiers start. No game run, no game lock.
#   dm/verify/premerge.sh        last line PREMERGE OK|RED
# The full gate still runs on main after the merge (fork_gate.sh auto).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
export DM_ROOT="${DM_ROOT:-$HOME/dm-work}"
VENV="$DM_ROOT/build/cache/dm/venv/bin/activate"; [ -f "$VENV" ] && source "$VENV"   # the host Makefile's generated tables need the project's Python
LOG="$(mktemp -t premerge)"
echo "PREMERGE: building the host ($ROOT/dm/host)"
if ! make -C "$ROOT/dm/host" -j8 > "$LOG" 2>&1; then
  grep -E "error|Error|Undefined|ld:" "$LOG" | head -20; echo "PREMERGE RED: the host does not build (log $LOG)"; exit 1
fi
rm -f "$LOG"
bash "$HERE/fork_gate.sh" quick unit; rc=$?
[ $rc -eq 0 ] && { echo "PREMERGE OK"; exit 0; }
echo "PREMERGE RED"; exit 1
