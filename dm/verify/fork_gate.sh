#!/bin/bash
# d2r-metal 自己的验收闸门：只用本机能跑的检查（不靠模拟器真值录制），按层跑，每步经 dmtest.py 出机器可读结果，末行 FORKGATE OK|RED {json}。
#   用法：dm/verify/fork_gate.sh [层…]      层 = quick | unit | core | full | long | device，缺省 quick unit；可写多层（core 含不含 unit 由调用方决定）
#   quick  源码检查（< 1 分钟）：字面量、中英表、开关登记、场景解析、HUD 派生物不入库
#   unit   单测（约 6 分钟）：宿主、图形（Metal 4 与旧接口）、iOS 单测，改写等价回归 regress.sh
#   core   游戏局（≤ 15 分钟）：精选场景无窗口并行 3 局 + 触屏功能用例（持 Mac 游戏锁）
#   full   core 之外：Mac App 测试副本与 iOS 包（Mac 上）场景、smoke_mac、故障注入、Metal 校验一局、回放
#   long   按需（1.5–2.5 小时）：基准（先跑，机器凉）、随机试玩、消毒器宿主、fnfuzz
#   device 真机（手机在时）：smoke.sh、lifecycle.sh
#   结果：$DM_ROOT/run/gate/<时间>_<层>/（每步一个目录：log.txt、result.json、junit.xml），summary.json；latest 指向最近一次
# d2r-metal's own acceptance gate: only checks that run on this machine (no emulator ground-truth recordings), in tiers; every step goes through dmtest.py for a machine-readable result; last line FORKGATE OK|RED {json}.
#   Usage: dm/verify/fork_gate.sh [tier...]      tier = quick | unit | core | full | long | device, default quick unit; several tiers may be given
#   quick  source checks (< 1 min): literals, string table, switch registry, scenario parsing, no HUD derivatives in git
#   unit   unit tests (~6 min): host, graphics (Metal 4 and older API), iOS, rewrite equivalence regress.sh
#   core   game runs (<= 15 min): curated scenarios headless, 3 at a time + the touch function cases (under the Mac game lock)
#   full   beyond core: Mac App test copy and iOS-package-on-Mac scenarios, smoke_mac, fault injection, one run under Metal validation, replays
#   long   on demand (1.5-2.5 h): benchmarks (first, while the machine is cool), random play, sanitizer hosts, fnfuzz
#   device the real phone (when it is there): smoke.sh, lifecycle.sh
#   Results: $DM_ROOT/run/gate/<time>_<tiers>/ (one folder per step: log.txt, result.json, junit.xml), summary.json; latest points at the most recent run
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; DM="$ROOT/dm"
export DM_ROOT="${DM_ROOT:-$HOME/dm-work}"
VENV="$DM_ROOT/build/cache/dm/venv/bin/activate"; [ -f "$VENV" ] && source "$VENV"
TIERS=("$@"); [ ${#TIERS[@]} -gt 0 ] || TIERS=(quick unit)
for t in "${TIERS[@]}"; do case "$t" in quick|unit|core|full|long|device) ;; *) echo "🔴 未知层 | unknown tier: $t"; exit 2;; esac; done
OUT="$DM_ROOT/run/gate/$(date +%Y%m%d_%H%M%S)_$(IFS=_; echo "${TIERS[*]}")"; mkdir -p "$OUT"; ln -sfn "$OUT" "$DM_ROOT/run/gate/latest"
STEPS="$OUT/steps.tsv"; : > "$STEPS"
HEAD_SHA=$(git -C "$ROOT" rev-parse --short HEAD); DIRTY=$(git -C "$ROOT" status --porcelain -- . ':!dm/gfx/tools/dmsh_chain.lock' ':!dm/gfx/nvn/dmsh_chain.h' ':!dm/gfx/nvn/c1_full.tsv' | wc -l | tr -d ' ')
T0=$SECONDS
echo "FORKGATE 开始 | start: 层 | tiers ${TIERS[*]} @ $HEAD_SHA（未提交改动 | uncommitted changes: $DIRTY）→ $OUT"

# 本机缺 GNU timeout（regress.sh 要）：给一个直通替身，只放在本次结果目录里 | GNU timeout missing on this machine (regress.sh needs it): provide a pass-through stand-in, only inside this run's folder
if ! command -v timeout >/dev/null; then
  mkdir -p "$OUT/bin"; printf '#!/bin/bash\nwhile [[ "$1" == -* ]]; do shift; done; shift; exec "$@"\n' > "$OUT/bin/timeout"; chmod +x "$OUT/bin/timeout"; export PATH="$OUT/bin:$PATH"
fi
[ -z "${LLVM_BIN:-}" ] && [ -x /opt/homebrew/opt/llvm/bin/llvm-objdump ] && export LLVM_BIN=/opt/homebrew/opt/llvm/bin

# 跑一步：dmtest.py 包住命令，记 名字 / 结论 / 秒数 / 不可用条数 | run one step: dmtest.py wraps the command; record name / verdict / seconds / unavailable count
step() {   # step <名字 | name> <超时秒 | timeout s> -- <命令 | command…>
  local name=$1 to=$2; shift 3; local s=$SECONDS d="$OUT/$name"
  python3 "$DM/tools/dmtest.py" --name "$name" --out "$d" --junit "$d/junit.xml" --timeout "$to" -- "$@" > "$d.console" 2>&1
  local rc=$? v; case $rc in 0) v=OK;; 1) v=RED;; *) v=ERROR;; esac
  local un; un=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1])).get("unavailable", [])))' "$d/result.json" 2>/dev/null || echo 0)
  printf '%s\t%s\t%s\t%s\n' "$name" "$v" "$((SECONDS - s))" "$un" >> "$STEPS"
  printf '%-28s %-5s %4ss%s\n' "$name" "$v" "$((SECONDS - s))" "$([ "$un" != 0 ] && echo "  ⚪ $un 项不可用 | unavailable")"
}
lock_on() {   # 开窗的层持 Mac 游戏锁；子进程见 XT_LOCK_HELD=1 就不再取 | tiers that open windows hold the Mac game lock; children see XT_LOCK_HELD=1 and do not take it again
  [ "${XT_LOCK_HELD:-}" = 1 ] && return 0
  source "$DM/ios/tools/mlock.sh" && mlock_acquire "fork_gate ${TIERS[*]} ~$1 min" || { echo "🔴 拿不到 Mac 游戏锁 | could not take the Mac game lock"; exit 2; }
  trap mlock_release EXIT
}
scenarios() {   # 精选场景：scenarios/*.scn（不含 saves/ 生成器与 faults/） | curated scenarios: scenarios/*.scn (not the saves/ generators or faults/)
  ls "$DM/verify/scenarios/"*.scn
}
FRONT_RE='^[[:space:]]*(app|background|system_quit|swipe_away)([[:space:]]|$)'   # 要前端的步（app 命令与它的宏）：无窗宿主没有前端 | steps that need a front end (the app command and its macros): a headless host has none
headless_scenarios() { for f in "$@"; do grep -qE "$FRONT_RE" "$f" || echo "$f"; done; }
front_scenarios() { for f in "$@"; do grep -qE "$FRONT_RE" "$f" && echo "$f"; done; }
par_scenarios() {   # par_scenarios <backend> <并行数 | jobs> <文件… | files…>：每个场景单独一个 scenario.py 进程 | one scenario.py process per scenario
  local be=$1 jobs=$2; shift 2
  printf '%s\n' "$@" | xargs -P "$jobs" -I{} python3 "$DM/tools/scenario.py" run {} --backend "$be" --headless
}
export -f par_scenarios
export OUT DM DM_ROOT HEAD_SHA DIRTY
touch_cases() {   # 触屏功能用例：退出码 3 = 本机缺前缀录制（不可用，不算红） | touch function cases: exit code 3 = prefix recording missing here (unavailable, not red)
  local bad=0
  for d in "$DM/verify/touchcases/"*/; do
    local c; c=$(basename "$d"); [ -f "$d/script.txt" ] && [ -f "$d/assert.py" ] || continue
    bash "$DM/verify/touchchain.sh" "$c" > "$OUT/touch_$c.log" 2>&1; local rc=$?
    case $rc in 0) echo "TOUCHCASE $c OK";; 3) echo "⚪ TOUCHCASE $c 不可用 | unavailable（$(grep -m1 '⚪' "$OUT/touch_$c.log" | cut -c1-120)）";; *) echo "TOUCHCASE $c RED（$OUT/touch_$c.log）"; bad=1;; esac
  done
  return $bad
}
export -f touch_cases
fresh_mac_app() {   # Mac App 测试副本按 HEAD 重出（构建号对不上就重出；测试用 bundle id，玩家的数据不碰） | rebuild the Mac App test copy for HEAD (when its stamp differs; test bundle id, the player's data is untouched)
  local dir="$DM_ROOT/out/macapp_test" stamp="$DM_ROOT/out/macapp_test/.gate_head"
  [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$HEAD_SHA+$DIRTY" ] && return 0
  DM_MAC_BUNDLE_ID=org.d2rmobile.mac.test bash "$DM/tools/mac_app.sh" "$dir" && echo "$HEAD_SHA+$DIRTY" > "$stamp"
}
export -f fresh_mac_app

for tier in "${TIERS[@]}"; do
  echo "== $tier"
  case $tier in
  quick)
    step quick_literals 120 -- python3 "$DM/i18n/scan_literals.py"
    step quick_i18n 120 -- python3 "$DM/ios/tools/dm_i18n.py" --check
    step quick_switches 120 -- python3 "$DM/tools/switches.py" check
    step quick_scenarios 120 -- python3 "$DM/tools/scenario.py" check "$DM/verify/scenarios/"*.scn "$DM/verify/scenarios/saves/"*.scn "$DM/verify/bench/"*.scn
    step quick_hudgen 300 -- bash "$DM/ios/tools/check_hud_gen.sh"
    ;;
  unit)
    step unit_host 1800 -- make -C "$DM/host/tests" run
    step unit_gfx 1800 -- make -C "$DM/gfx/tests" run
    step unit_gfx_m3 1800 -- make -C "$DM/gfx/tests" run-m3
    step unit_ios 1200 -- make -C "$DM/ios/tests" test
    step unit_regress 1800 -- bash "$DM/verify/regress.sh"
    ;;
  core)
    make -C "$DM/host" -s >/dev/null 2>&1 || { echo "🔴 宿主编译失败 | host build failed"; exit 2; }
    step core_scenarios 1200 -- bash -c 'par_scenarios cli 3 "$@"' _ $(headless_scenarios $(scenarios))
    lock_on 10
    step core_front_scenarios 900 -- python3 "$DM/tools/scenario.py" run $(front_scenarios $(scenarios)) --backend cli   # 开窗、一个一个跑（前端生命周期：最小化 / 退出） | windowed, one at a time (front-end lifecycle: minimize / quit)
    step core_touchcases 900 -- bash -c touch_cases
    ;;
  full)
    lock_on 15
    step full_macapp_build 900 -- bash -c fresh_mac_app
    step full_macapp_scenarios 1800 -- python3 "$DM/tools/scenario.py" run $(scenarios) --backend mac-app
    step full_smoke_mac 900 -- bash "$DM/ios/tools/smoke_mac.sh" 1   # 也按 HEAD 重出 iOS 包的 Mac 版（ios-mac 场景用它） | also rebuilds the iOS package for the Mac from HEAD (the ios-mac scenarios use it)
    step full_iosmac_scenarios 1800 -- python3 "$DM/tools/scenario.py" run $(scenarios) --backend ios-mac
    step full_metal_validation 600 -- python3 "$DM/tools/scenario.py" run "$DM/verify/scenarios/enter_game.scn" --backend cli --headless --metal-validation api
    [ -d "$DM/verify/scenarios/faults" ] && step full_faults 1800 -- bash -c 'par_scenarios cli 3 "$@"' _ "$DM/verify/scenarios/faults/"*.scn
    ;;
  long)
    lock_on 120
    step long_bench 3600 -- python3 "$DM/tools/bench.py" run "$DM/verify/bench/town_loop.scn" --reps 3
    for san in asan ubsan; do
      make -C "$DM/host" "dmhost_$san" -s >/dev/null 2>&1 || { echo "🔴 dmhost_$san 编译失败 | build failed"; continue; }
      step "long_explore_$san" 3600 -- python3 "$DM/tools/explore.py" run --seed 1 --episodes 4 --dmhost "$DM/host/dmhost_$san"
    done
    make -C "$DM/host" dmhost_tsan -s >/dev/null 2>&1 && step long_explore_tsan 3600 -- python3 "$DM/tools/explore.py" run --seed 2 --episodes 2 --dmhost "$DM/host/dmhost_tsan"   # TSan 单独跑：内存 5–10 倍 | TSan alone: 5-10x memory
    step long_fnfuzz 3600 -- python3 "$DM/verify/fnfuzz.py" "$("$DM/tools/product.sh" path rules)" main "$DM_ROOT/data/switch/3.3.94012/Update/ExeFS/main" --trials 3
    ;;
  device)
    if xcrun devicectl list devices 2>/dev/null | grep -q 'available (paired)'; then
      step device_smoke 1800 -- bash "$DM/ios/tools/smoke.sh"
      step device_lifecycle 1800 -- bash "$DM/ios/tools/lifecycle.sh"
    else
      printf 'device\tUNAVAIL\t0\t1\n' >> "$STEPS"; echo "⚪ device 不可用：手机不在 | unavailable: the phone is not reachable"
    fi
    ;;
  esac
done

red=$(awk -F'\t' '$2 == "RED" || $2 == "ERROR"' "$STEPS" | wc -l | tr -d ' ')
python3 - "$STEPS" "$OUT/summary.json" "$HEAD_SHA" "$DIRTY" "$((SECONDS - T0))" "${TIERS[*]}" <<'EOF'
import json, sys
steps = [dict(zip(('name', 'verdict', 's', 'unavailable'), l.rstrip('\n').split('\t'))) for l in open(sys.argv[1]) if l.strip()]
for s in steps: s['s'] = int(s['s']); s['unavailable'] = int(s['unavailable'])
bad = [s['name'] for s in steps if s['verdict'] in ('RED', 'ERROR')]
res = dict(name='fork_gate', verdict='RED' if bad else 'OK', tiers=sys.argv[6].split(), sha=sys.argv[3], uncommitted=int(sys.argv[4]), s=int(sys.argv[5]),
           red=bad, unavailable=sum(s['unavailable'] for s in steps), steps=steps)
json.dump(res, open(sys.argv[2], 'w'), indent=1)
print('FORKGATE ' + res['verdict'] + ' ' + json.dumps({k: res[k] for k in ('tiers', 'sha', 's', 'red', 'unavailable')}))
EOF
[ "$red" = 0 ]
