#!/bin/bash
# The Mac's ray tracing, byte for byte: a town frame captured with rays at Full is kept with the five ray planes a known-good build made from it. check replays the kept
# capture through this tree's replay layer (dm/gfx/tests/nvn_replay --rays --frames N --planes-ref) and compares every plane byte for byte. A control replay with a
# larger sun (DM_RT_SUN=0.5, softer key shadows) must change the key plane and no other, or a match would prove nothing.
#   dm/verify/rays_replay.sh record --reason "<why>"   capture (town_loop, headless, this Mac's Recommended with rays), replay twice: the two must agree; keep both in
#                                                      $DM_ROOT/data/switch/golden/rays with a line in its LOG.tsv
#   dm/verify/rays_replay.sh check                     last line RAYS_REPLAY OK | RED (exit 0 / 1); ⚪ when nothing is recorded or this build can't replay the
#                                                      capture (its record layout or shader chain changed): record again, with the reason
# RAYS_FRAMES=<n> replays the frame n times (default 600: the rays bring in at most 24 new structures a frame). A change meant to alter what the rays draw records
# again; a match proves nothing about the forward shaders' use of the planes, only what the kernels wrote. Captures hold game memory: never in git.
set -u
DM="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="${DM_ROOT:-$HOME/dm-work}"; G="$ROOT/data/switch/golden/rays"; FRAMES="${RAYS_FRAMES:-600}"
R="$ROOT/run/rays_replay/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$R"
op="${1:-}"; reason=""
[ "$op" = record ] && { [ "${2:-}" = --reason ] && [ -n "${3:-}" ] && reason=$3 || { echo "usage: rays_replay.sh record --reason \"<why>\""; exit 2; }; }
case "$op" in record|check) ;; *) echo "usage: rays_replay.sh record --reason \"<why>\" | check"; exit 2;; esac

make -C "$DM/gfx/tests" nvn_replay >"$R/build.log" 2>&1 || { echo "🔴 nvn_replay did not build: $R/build.log"; echo "RAYS_REPLAY RED build"; exit 1; }
replay() {   # replay <capture> <name> [--planes-ref file] [-- env...]: planes to $R/<name>.bin, log to $R/<name>.log; returns nvn_replay's exit status
  local cap=$1 name=$2; shift 2; local ref=() envs=()
  if [ "${1:-}" = --planes-ref ]; then ref=(--planes-ref "$2"); shift 2; fi
  [ "${1:-}" = -- ] && { shift; envs=("$@"); }
  ( cd "$DM/gfx/tests" && env ${envs[@]+"${envs[@]}"} ./nvn_replay "$cap" --rays --frames "$FRAMES" --max-pixels 999999999 --png "$R/$name.png" --planes "$R/$name.bin" ${ref[@]+"${ref[@]}"} ) >"$R/$name.log" 2>&1
}
differ() { grep -E "^planes: $2 [0-9]+ bytes differ" "$R/$1.log" | awk '{print $3}'; }   # differ <name> <plane>: bytes of that plane that differ from the reference

if [ "$op" = record ]; then
  mkdir -p "$G"
  E="$(cd "$DM/.." && python3 dm/tools/picture.py rec) DM_RT=1 DM_WATCH_STALL=300 DM_NVN_SYNC=1"
  python3 "$DM/tools/scenario.py" run "$DM/verify/bench/town_loop.scn" --backend cli --headless --out "$R/capture" --env "$E DM_NVN_CAPTURE=700:1:$R/town.nvncap" 2>&1 | grep -E '^SCENARIO' | cut -c1-120
  [ -s "$R/town.nvncap" ] || { echo "🔴 no capture written: $R/capture/host.log"; echo "RAYS_REPLAY RED record"; exit 1; }
  replay "$R/town.nvncap" first || { echo "🔴 the first replay failed: $R/first.log"; echo "RAYS_REPLAY RED record"; exit 1; }
  replay "$R/town.nvncap" second --planes-ref "$R/first.bin"; rc=$?
  grep -E "^planes: " "$R/second.log"
  [ $rc -eq 0 ] || { echo "🔴 two replays of one capture don't agree: nothing kept ($R/second.log)"; echo "RAYS_REPLAY RED record"; exit 1; }
  mv "$R/town.nvncap" "$G/town.nvncap"; cp "$R/first.bin" "$G/town.planes"
  printf '%s\t%s\t%s\t%s\n' "$(date +%F)" "$(git -C "$DM/.." rev-parse --short HEAD)" "$FRAMES" "$reason" >> "$G/LOG.tsv"
  echo "RAYS_REPLAY OK recorded $G (frames $FRAMES)"; exit 0
fi

[ -s "$G/town.nvncap" ] && [ -s "$G/town.planes" ] || { echo "⚪ RAYS_REPLAY nothing recorded: dm/verify/rays_replay.sh record --reason \"…\""; exit 0; }
replay "$G/town.nvncap" now --planes-ref "$G/town.planes"; rc=$?
grep -E "^planes: |MATCH|DIFFER" "$R/now.log" | grep -v "not zero"
[ $rc -eq 2 ] && { grep -m1 "【红】" "$R/now.log" | sed 's/【红】//'; echo "⚪ RAYS_REPLAY this build can't replay the kept capture: record again with the reason ($R/now.log)"; exit 0; }
replay "$G/town.nvncap" control --planes-ref "$G/town.planes" -- DM_RT_SUN=0.5
ctl_key=$(differ control key); ctl_rest=0
for p in occlusion local character reflection; do d=$(differ control $p); ctl_rest=$((ctl_rest + ${d:-1})); done
echo "control (a larger sun): key plane ${ctl_key:-?} bytes differ, the other planes ${ctl_rest}"
if [ "${ctl_key:-0}" -eq 0 ] || [ "$ctl_rest" -ne 0 ]; then echo "🔴 the control didn't change the key plane alone: the check can't see a change ($R/control.log)"; echo "RAYS_REPLAY RED control"; exit 1; fi
[ $rc -eq 0 ] && { echo "RAYS_REPLAY OK every ray plane equals the recorded one (frames $FRAMES, $R)"; exit 0; }
echo "RAYS_REPLAY RED the ray planes differ from the recorded ones ($R/now.log); a change meant to alter the rays records again with its reason"; exit 1
