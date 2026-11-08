#!/bin/bash
# d2r-metal 自己的验收闸门：只用本机能跑的检查（不靠模拟器真值录制），按层跑，每步经 dmtest.py 出机器可读结果，末行 FORKGATE OK|RED {json}。
#   用法：dm/verify/fork_gate.sh [层…]      层 = quick | unit | core | full | long | device，缺省 quick unit；可写多层（core 含不含 unit 由调用方决定）
#   dm/verify/fork_gate.sh auto        按改动挑层（dm/tools/gate_pick.py：文档与单测 quick unit，宿主 core，图形、两个 App、游戏数据 full）；发版与重装 App 前照旧跑 full
#   quick  源码检查（< 1 分钟）：字面量、中英表、开关登记、场景解析、HUD 派生物不入库
#   unit   单测（约 3 分钟）：宿主、图形、iOS 单测，改写等价回归 regress.sh
#   core   游戏局（≤ 15 分钟）：精选场景无窗口并行 3 局 + 触屏功能用例（持 Mac 游戏锁）
#   full   core 之外：Mac App 测试副本与 iOS 包（Mac 上）场景、smoke_mac、故障注入、Metal 校验一局、回放
#   long   按需（1.5–2.5 小时）：基准（先跑，机器凉）、随机试玩、消毒器宿主、fnfuzz
#   device 真机（手机在时）：smoke.sh、lifecycle.sh
#   结果：$DM_ROOT/run/gate/<时间>_<层>/（每步一个目录：log.txt、result.json、junit.xml），summary.json；latest 指向最近一次
# d2r-metal's own acceptance gate: only checks that run on this machine (no emulator ground-truth recordings), in tiers; every step goes through dmtest.py for a machine-readable result; last line FORKGATE OK|RED {json}.
#   Usage: dm/verify/fork_gate.sh [tier...]      tier = quick | unit | core | full | long | device, default quick unit; several tiers may be given
#   dm/verify/fork_gate.sh auto        the tiers this change needs (dm/tools/gate_pick.py: docs and unit tests quick unit, the host core, graphics, the apps and game data full); full still runs before a release or an app rebuild
#   quick  source checks (< 1 min): literals, string table, switch registry, scenario parsing, no HUD derivatives in git
#   unit   unit tests (~3 min): host, graphics, iOS, rewrite equivalence regress.sh
#   core   game runs (<= 15 min): curated scenarios headless, 3 at a time + the touch function cases (under the Mac game lock)
#   full   beyond core: Mac App test copy and iOS-package-on-Mac scenarios, smoke_mac, fault injection, one run under Metal validation, replays
#   long   on demand (1.5-2.5 h): benchmarks (first, while the machine is cool), random play, sanitizer hosts, fnfuzz
#   device the real phone (when it is there): smoke.sh, lifecycle.sh
#   Results: $DM_ROOT/run/gate/<time>_<tiers>/ (one folder per step: log.txt, result.json, junit.xml), summary.json; latest points at the most recent run
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; DM="$ROOT/dm"
export DM_ROOT="${DM_ROOT:-$HOME/dm-work}"
VENV="$DM_ROOT/build/cache/dm/venv/bin/activate"; [ -f "$VENV" ] && source "$VENV"
# The verdict: summary.json and the last line FORKGATE OK|RED {json}, from steps.tsv; with an argument, the run stopped early for that reason (RED).
# Every exit goes through here, so the last line is always the verdict
gate_summary() {   # gate_summary [abort reason]; exit status 0 = OK, 1 = RED
  python3 - "${STEPS:-}" "${OUT:-}" "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)" "${DIRTY:-0}" "$((SECONDS - ${T0:-$SECONDS}))" "${TIERS[*]}" "${1:-}" <<'PYEOF'
import json, os, sys
steps_f, out, sha, dirty, secs, tiers, abort = sys.argv[1:8]
steps = [dict(zip(('name', 'verdict', 's', 'unavailable'), l.rstrip('\n').split('\t'))) for l in open(steps_f) if l.strip()] if steps_f and os.path.exists(steps_f) else []
for s in steps: s['s'] = int(s['s']); s['unavailable'] = int(s['unavailable'])
bad = [s['name'] for s in steps if s['verdict'] in ('RED', 'ERROR')] + (['abort'] if abort else [])
res = dict(name='fork_gate', verdict='RED' if bad else 'OK', tiers=tiers.split(), sha=sha, uncommitted=int(dirty), s=int(secs),
           red=bad, unavailable=sum(s['unavailable'] for s in steps), steps=steps)
if abort: res['abort'] = abort
if out and os.path.isdir(out): json.dump(res, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
print('FORKGATE ' + res['verdict'] + ' ' + json.dumps({k: res[k] for k in ('tiers', 'sha', 's', 'red', 'unavailable', 'abort') if k in res}))
sys.exit(1 if bad else 0)
PYEOF
}
gate_abort() { gate_summary "$1"; exit 2; }   # gate_abort <reason>: stop before the tiers ran to the end, still with the verdict line
TIERS=("$@"); [ ${#TIERS[@]} -gt 0 ] || TIERS=(quick unit)
if [ "${TIERS[*]}" = auto ]; then   # the tiers the change against origin/main needs (DM_GATE_BASE names another base), with each file's reason
  PICK=$(python3 "$DM/tools/gate_pick.py" explain) || { echo "🔴 gate_pick.py 失败 | failed"; gate_abort "gate_pick.py failed"; }
  read -ra TIERS <<< "$(head -1 <<< "$PICK")"; tail -n +2 <<< "$PICK"
fi
for t in "${TIERS[@]}"; do case "$t" in quick|unit|core|full|long|device) ;; *) echo "🔴 未知层 | unknown tier: $t"; gate_abort "unknown tier: $t";; esac; done
OUT="$DM_ROOT/run/gate/$(date +%Y%m%d_%H%M%S)_$(IFS=_; echo "${TIERS[*]}")"; mkdir -p "$OUT"; ln -sfn "$OUT" "$DM_ROOT/run/gate/latest"
STEPS="$OUT/steps.tsv"; : > "$STEPS"
# Informational steps (measurements with no threshold): a RED verdict is recorded as INFO and doesn't make the gate red; ERROR (a timeout, a command
# that could not start) still does
INFO_STEPS="long_pso_cold"
# This machine's expected unavailable (⚪) items, <step>\t<text the ⚪ line contains>\t<why> (dmtest.py --unavailable-ok). When the file exists, any
# other ⚪ makes its step red: a lost baseline or recording fails the gate instead of passing it quietly. Without it, ⚪ never makes a step red
UNAVAIL_OK="$DM_ROOT/gate/unavailable_ok.tsv"
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
  local name=$1 to=$2; shift 3; local s=$SECONDS d="$OUT/$name" rco=()
  case $name in unit_*) rco=(--rc-only);; *) rco=(--strict-log);; esac   # 测试套：退出码就是结论（套里的负向用例自己会打 RED 行） | test suites: the exit code is the verdict (their negative cases print RED lines); any other step is also red on a red-marker line
  [ -f "$UNAVAIL_OK" ] && rco+=(--unavailable-ok "$UNAVAIL_OK")
  python3 "$DM/tools/dmtest.py" --name "$name" --out "$d" --junit "$d/junit.xml" --timeout "$to" ${rco[@]+"${rco[@]}"} -- "$@" > "$d.console" 2>&1
  local rc=$? v; case $rc in 0) v=OK;; 1) v=RED;; *) v=ERROR;; esac
  if [ "$v" = RED ]; then case " $INFO_STEPS " in *" $name "*) v=INFO;; esac; fi   # an informational step's RED is reported, not counted
  local un ux; read -r un ux <<< "$(python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(len(r.get("unavailable", [])), len(r.get("unexpected_unavailable", [])))' "$d/result.json" 2>/dev/null || echo 0 0)"
  printf '%s\t%s\t%s\t%s\n' "$name" "$v" "$((SECONDS - s))" "$un" >> "$STEPS"
  printf '%-28s %-5s %4ss%s%s\n' "$name" "$v" "$((SECONDS - s))" "$([ "$un" != 0 ] && echo "  ⚪ $un 项不可用 | unavailable")" "$([ "${ux:-0}" != 0 ] && echo ", $ux not expected here ($UNAVAIL_OK)")"
}
lock_on() {   # 开窗的层持 Mac 游戏锁；子进程见 XT_LOCK_HELD=1 就不再取 | tiers that open windows hold the Mac game lock; children see XT_LOCK_HELD=1 and do not take it again
  [ "${XT_LOCK_HELD:-}" = 1 ] && return 0
  source "$DM/ios/tools/mlock.sh" && mlock_acquire "fork_gate ${TIERS[*]} ~$1 min" || { echo "🔴 拿不到 Mac 游戏锁 | could not take the Mac game lock"; gate_abort "could not take the Mac game lock"; }
  trap mlock_release EXIT
}
scenarios() {   # 精选场景：scenarios/*.scn（不含 saves/ 生成器与 faults/） | curated scenarios: scenarios/*.scn (not the saves/ generators or faults/)
  ls "$DM/verify/scenarios/"*.scn
}
# Steps that need a front end (the app command and its macros, front-end test input fe), and the `front` header: a headless host has none
FRONT_RE='^[[:space:]]*(app|background|system_quit|swipe_away|fe|front)([[:space:]]|$)'
headless_scenarios() { for f in "$@"; do grep -qE "$FRONT_RE" "$f" || echo "$f"; done; }
front_scenarios() { for f in "$@"; do grep -qE "$FRONT_RE" "$f" && echo "$f"; done; }
par_scenarios() {   # par_scenarios <backend> <并行数 | jobs> <文件… | files…>：每个场景单独一个 scenario.py 进程 | one scenario.py process per scenario
  local be=$1 jobs=$2; shift 2
  printf '%s\n' "$@" | xargs -P "$jobs" -I{} python3 "$DM/tools/scenario.py" run {} --backend "$be" --headless
}
export -f par_scenarios
scn_run() {   # scn_run <scenario.py run arguments…>: windowed runs are background runs; a front one that stays disturbed is not judged (exit 3, ⚪ line) = unavailable, not red
  python3 "$DM/tools/scenario.py" run "$@"; local rc=$?; [ $rc -eq 3 ] && return 0; return $rc
}
export -f scn_run
export OUT DM DM_ROOT HEAD_SHA DIRTY
touch_one() {   # touch_one <用例 | case>: one touch function case, its verdict line on stdout; exit code 3 = prefix recording missing here (unavailable, not red)
  local c=$1
  bash "$DM/verify/touchchain.sh" "$c" > "$OUT/touch_$c.log" 2>&1; local rc=$?
  case $rc in 0) echo "TOUCHCASE $c OK";; 3) echo "⚪ TOUCHCASE $c 不可用 | unavailable（$(grep -m1 '⚪' "$OUT/touch_$c.log" | cut -c1-120)）";; *) echo "TOUCHCASE $c RED（$OUT/touch_$c.log）"; return 1;; esac
}
export -f touch_one
touch_cases() {   # 触屏功能用例，3 例并行 | the touch function cases, 3 at a time: each runs in its own folder on the deterministic clock, so load doesn't change its verdict (touchchain.sh)
  local cases=() d
  for d in "$DM/verify/touchcases/"*/; do [ -f "$d/script.txt" ] && [ -f "$d/assert.py" ] && cases+=("$(basename "$d")"); done
  # The UI overlay once for every case (touchchain.sh takes DM_ROMFS_OVERLAY; the generator also writes _astc and _variants folders beside it), removed after
  export DM_ROMFS_OVERLAY="$OUT/romfs_overlay"
  python3 "$DM/ios/tools/gen_romfs_overlay.py" "${DM_ROMFS_SRC:-$DM_ROOT/data/switch/3.3.94012/Update/RomFS}" "$DM_ROMFS_OVERLAY" > "$OUT/touch_overlay.log" 2>&1 || { echo "TOUCHCASE overlay RED ($OUT/touch_overlay.log)"; return 1; }
  printf '%s\n' "${cases[@]}" | xargs -P 3 -I{} bash -c 'touch_one "$1"' _ {}; local rc=$?
  rm -rf "$DM_ROMFS_OVERLAY" "$DM_ROMFS_OVERLAY"_astc "$DM_ROMFS_OVERLAY"_variants "$DM_ROMFS_OVERLAY"_astc_variants
  return $(( rc != 0 ))
}
export -f touch_cases
fresh_mac_app() {   # Mac App 测试副本按 HEAD 重出（构建号对不上就重出；测试用 bundle id，玩家的数据不碰） | rebuild the Mac App test copy for HEAD (when its stamp differs; test bundle id, the player's data is untouched)
  # The stamp is the content of the uncommitted changes, not their number: two different trees with five changed files each once shared a stamp, and the Mac tier ran the older build
  local dir="$DM_ROOT/out/macapp_test" stamp="$DM_ROOT/out/macapp_test/.gate_head" want
  want="$HEAD_SHA+$( { git -C "$DM" status --porcelain; git -C "$DM" diff HEAD; } | shasum | cut -c1-16)"
  [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] && return 0
  DM_MAC_BUNDLE_ID=org.d2rmobile.mac.test bash "$DM/tools/mac_app.sh" "$dir" && echo "$want" > "$stamp"
}
export -f fresh_mac_app

for tier in "${TIERS[@]}"; do
  echo "== $tier"
  case $tier in
  quick)
    step quick_literals 120 -- python3 "$DM/i18n/scan_literals.py"
    step quick_i18n 120 -- python3 "$DM/ios/tools/dm_i18n.py" --check
    step quick_switches 120 -- python3 "$DM/tools/switches.py" check
    step quick_enginemap 120 -- python3 "$DM/tools/enginemap.py" check
    step quick_gate 60 -- bash -c 'bash -n "$1" && python3 "$2" lint' _ "$DM/verify/fork_gate.sh" "$DM/tools/gate_pick.py"   # this script parses and every step names files that exist (auto runs only the tiers a change needs, so a typo in another tier would otherwise wait for it)
    step quick_scenarios 120 -- python3 "$DM/tools/scenario.py" check "$DM/verify/scenarios/"*.scn "$DM/verify/scenarios/saves/"*.scn "$DM/verify/scenarios/faults/"*.scn "$DM/verify/scenarios/picture/"*.scn "$DM/verify/scenarios/heap/"*.scn "$DM/verify/scenarios/manual/"*.scn "$DM/verify/golden/"*.scn "$DM/verify/bench/"*.scn
    step quick_hudgen 300 -- bash "$DM/ios/tools/check_hud_gen.sh"
    step quick_readdr 300 -- python3 "$DM/tools/re/addrcheck.py"   # cited main+0x... addresses vs the game program and the private analysis export (unavailable without them)
    ;;
  unit)
    step unit_host 1800 -- make -C "$DM/host/tests" run
    step unit_gfx 1800 -- make -C "$DM/gfx/tests" run
    step unit_ios 1200 -- make -C "$DM/ios/tests" test
    step unit_regress 1800 -- bash "$DM/verify/regress.sh"
    # the Mac packaging tools: mac_app.sh bundles, the launcher's install engine, the launcher's own checks (not the 9-minute end-to-end feed test)
    # the launcher's packaging: the bundle and install engine tests, the launcher's core tests, and its scripts (bash -n; shellcheck when installed)
    step unit_packaging 900 -- bash -c 'python3 "$1/tools/test_mac_bundle.py" && python3 "$1/tools/test_install.py" && swift test --package-path "$1/launcher" --filter CoreTests &&
      for f in release_mac.sh launcher_app.sh extract_game.sh product.sh; do bash -n "$1/tools/$f" || exit 1; done &&
      { ! command -v shellcheck > /dev/null || shellcheck -S warning "$1"/tools/{release_mac,launcher_app,extract_game,product}.sh; }' _ "$DM"
    ;;
  core)
    make -C "$DM/host" -s >/dev/null 2>&1 || { echo "🔴 宿主编译失败 | host build failed"; gate_abort "host build failed"; }
    step core_scenarios 1200 -- bash -c 'par_scenarios cli 3 "$@"' _ $(headless_scenarios $(scenarios))
    lock_on 10
    step core_front_scenarios 900 -- bash -c 'scn_run "$@" --backend cli' _ $(front_scenarios $(scenarios))   # windowed, one at a time, in the background (front-end lifecycle: minimize / quit)
    step core_touchcases 900 -- bash -c touch_cases
    # The picture chain at this Mac's Recommended on the CLI host (scenarios/picture: SMAA, 16x, shadows, 3D 75% with the MetalFX 3D pass, AA switched live)
    step core_picture 900 -- bash -c 'par_scenarios cli 2 "$@"' _ "$DM/verify/scenarios/picture/rec.scn" "$DM/verify/scenarios/picture/rec75.scn" "$DM/verify/scenarios/picture/recfg.scn" "$DM/verify/scenarios/picture/rect.scn" "$DM/verify/scenarios/picture/rec75t.scn" "$DM/verify/scenarios/picture/aa_toggle.scn"
    # Motion: frame generation and the hero's animation on a walk, with a control (motion_gate.py: motion_scan.py on the shown frames and the limbs' trace)
    step core_motion 400 -- python3 "$DM/verify/motion_gate.py"
    # Pipeline warm-up coverage: the town route with Recommended's features forced builds no pipeline that the seed (dm/gfx/nvn/pso_manifest.tsv) or
    # the reviewed allow-list (pso_allow.tsv) lacks (PSO_GATE OK rows=0)
    step core_pso_cli 900 -- python3 "$DM/tools/pso_seed.py" gate --backend cli --headless
    ;;
  full)
    lock_on 15
    step full_macapp_build 900 -- bash -c fresh_mac_app
    # Background runs at this Mac's Recommended picture (DM_TEST_VIDEO_FIRSTBOOT=1: Display Mode = full screen, emulated off screen, the same drawable as real full screen)
    step full_macapp_scenarios 1800 -- bash -c 'DM_TEST_VIDEO_FIRSTBOOT=1 scn_run "$@" --backend mac-app' _ $(scenarios)
    step full_pso_mac 900 -- python3 "$DM/tools/pso_seed.py" gate --backend mac-app   # warm-up coverage on the Mac App test copy at this Mac's Recommended
    step full_smoke_mac 900 -- bash "$DM/ios/tools/smoke_mac.sh" 1   # 也按 HEAD 重出 iOS 包的 Mac 版（ios-mac 场景用它） | also rebuilds the iOS package for the Mac from HEAD (the ios-mac scenarios use it)
    step full_pso_ios 900 -- python3 "$DM/tools/pso_seed.py" gate --backend ios-mac   # and on the iOS package smoke_mac has just rebuilt
    step full_iosmac_scenarios 1800 -- bash -c 'scn_run "$@" --backend ios-mac' _ $(scenarios)   # front runs (the iOS package takes the front): a disturbed one is redone, then ⚪
    step full_metal_validation 600 -- python3 "$DM/tools/scenario.py" run "$DM/verify/scenarios/enter_game.scn" --backend cli --headless --metal-validation api
    step full_metal_validation_rec 600 -- python3 "$DM/tools/scenario.py" run "$DM/verify/scenarios/picture/recfg.scn" --backend cli --headless --metal-validation api   # at the Recommended picture
    step full_metal_validation_rt 600 -- python3 "$DM/tools/scenario.py" run "$DM/verify/scenarios/picture/recfg.scn" --backend cli --headless --metal-validation api --env DM_RT=1   # the same, with ray tracing on
    step full_metal_validation_conv 600 -- python3 "$DM/tools/scenario.py" run "$DM/verify/scenarios/picture/rec.scn" --backend cli --headless --metal-validation api   # at the conventional one
    step full_macapp_picture 900 -- bash -c 'scn_run "$@" --backend mac-app' _ "$DM/verify/scenarios/picture/"mac_*.scn   # first launches: this Mac and an M1 stand-in
    # 我们自己的基线（golden.py；没录过 = ⚪ 不可用，不算红） | our own baselines (golden.py; never recorded = ⚪ unavailable, not red)
    step full_golden_replay 1800 -- bash -c 'python3 "$DM/verify/golden.py" replay check; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc'
    step full_golden_shots 1200 -- bash -c 'python3 "$DM/verify/golden.py" shots check; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc'
    step full_rays_replay 1800 -- bash "$DM/verify/rays_replay.sh" check   # the Mac's ray planes byte for byte against a kept capture (⚪ until recorded)
    step full_golden_picture 2400 -- bash -c 'python3 "$DM/verify/golden.py" shots check --set orig,conv,rect,rec75,rec75t,phone; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc'   # FLIP sets (screenshot_gate.md)
    step full_faults 1200 -- bash -c 'par_scenarios cli 3 "$@"' _ $(headless_scenarios "$DM/verify/scenarios/faults/"*.scn)   # 故障矩阵（scenarios/faults/README.md） | the fault matrix (scenarios/faults/README.md)
    step full_faults_front 600 -- bash -c 'scn_run "$@" --backend cli' _ $(front_scenarios "$DM/verify/scenarios/faults/"*.scn)
    ;;
  long)
    lock_on 120
    # Benchmarks first, while the machine is cool. They run in front by design; a rep still disturbed after its redos is not judged (exit 3 = unavailable, not red)
    for r in town_loop combat ui_heavy rejoin; do step "long_bench_$r" 1800 -- bash -c 'python3 "$1/tools/bench.py" run "$1/verify/bench/$2.scn" --reps 3; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc' _ "$DM" "$r"; done
    step long_profile 900 -- bash -c 'python3 "$1/tools/bench.py" profile "$1/verify/bench/combat.scn"; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc' _ "$DM"   # sampled attribution + hitches
    # GPU heap 0 at the Native cap (scenarios/heap, the readings behind DM_SCR_MAX_PX): measurements, so front runs in real full screen, one at a time
    step long_heap 1800 -- bash -c 'scn_run "$@" --backend cli' _ "$DM/verify/scenarios/heap/"*.scn
    step long_heap_mac 600 -- bash -c 'fresh_mac_app && scn_run "$@" --backend mac-app' _ "$DM/verify/scenarios/heap/"*.scn
    step long_why 400 -- python3 "$DM/verify/why_gate.py"   # known spikes come back named (bench_why.py), and the samples agree with the CPU clock
    step long_regress 2400 -- bash -c 'python3 "$DM/tools/bench.py" regress "$DM/verify/bench/town_loop.scn" --reps 6; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc'   # 对留存基线（bench.py baseline save）；没有基线为 ⚪ | against the kept baseline (bench.py baseline save); none = ⚪
    # One true cold start of the Mac App (a fresh bundle ID, so an empty Metal cache), in front: a row of its warm-up times goes to $DM_ROOT/run/pso/cold.tsv.
    # Informational (INFO_STEPS); a run still disturbed after its redos is ⚪
    step long_pso_cold 1200 -- bash -c 'python3 "$DM/tools/pso_seed.py" cold --backend mac-app; rc=$?; [ $rc -eq 3 ] && exit 0; exit $rc'
    step long_playbot 3600 -- python3 "$DM/tools/playbot.py" run --seed 1 --episodes 4 --intents 15   # 试玩机器人：玩家意图 + 不变量（dm/tools/playbot.py） | the playtest bot: player intents + invariants (dm/tools/playbot.py)
    step long_soak 3600 -- python3 "$DM/tools/playbot.py" soak --minutes 30 --seed 2   # Mac 浸泡：soak_report.py 的四条判据 | Mac soak: soak_report.py's four criteria
    # The sanitizer hosts' builds are steps of their own: a failed build is a red row, not a skipped run
    for san in asan ubsan; do
      step "long_build_$san" 900 -- make -C "$DM/host" "dmhost_$san" -s
      tail -1 "$STEPS" | grep -q $'\tOK\t' && step "long_explore_$san" 3600 -- python3 "$DM/tools/explore.py" run --seed 1 --episodes 4 --dmhost "$DM/host/dmhost_$san"
    done
    step long_build_tsan 900 -- make -C "$DM/host" dmhost_tsan -s
    tail -1 "$STEPS" | grep -q $'\tOK\t' && step long_explore_tsan 3600 -- python3 "$DM/tools/explore.py" run --seed 2 --episodes 2 --dmhost "$DM/host/dmhost_tsan"   # TSan 单独跑：内存 5–10 倍 | TSan alone: 5-10x memory
    step long_fnfuzz 3600 -- python3 "$DM/verify/fnfuzz.py" "$("$DM/tools/product.sh" path rules)" main "$DM_ROOT/data/switch/3.3.94012/Update/ExeFS/main" --trials 3
    ;;
  device)
    # 手机闲着时列为 available (paired)，开着隧道（刚有 devicectl 连过）时列为 connected：两种都在
    # The phone is listed as available (paired) when idle and as connected while a tunnel is open (devicectl talked to it recently): both mean it is there
    if xcrun devicectl list devices 2>/dev/null | grep -qE 'available \(paired\)|connected'; then
      step device_smoke 1800 -- bash "$DM/ios/tools/smoke.sh"
      step device_lifecycle 1800 -- bash "$DM/ios/tools/lifecycle.sh"
    else
      printf 'device\tUNAVAIL\t0\t1\n' >> "$STEPS"; echo "⚪ device 不可用：手机不在 | unavailable: the phone is not reachable"
    fi
    ;;
  esac
done

gate_summary
