#!/bin/bash
# 场景运行器（dm/tools/scenario.py）拉起 iOS 包：ios-mac = Mac 上跑的 iOS 包（smoke_mac.sh 包好的 .iosdd_mac/DmMacSmoke.app，或 SCN_MAC_APP 指的包），device = 真机（devicectl 拉起，iproxy 转日志端口）。
# 两种都走冒烟的那套守卫：同一把设备锁（与冒烟串行）、iCloud 总闸（cloud_guard.sh：每局不同步，收尾核日志）、测试存档用 DM_SAVE_FRESH 的新副本（不碰玩家存档）。
# 控制走日志端口（DM_LOG_PORT），事件随日志（DM_EVENTS=log）。
# 用法：scenario_launch.sh <ios-mac|device>   一个会话一个进程（设备锁跟着这个进程走）：拿锁、拉起、打印「PORT <本机端口>」，然后从标准输入读命令，每条回一行：
#   alive → yes / no；kill → done（直接杀：kill -9 / SIGKILL）；stop → done（结束实例、核 iCloud、放锁，然后退出）。标准输入关掉也照 stop 收尾
#   环境：SCN_RUN 结果目录（必需）、SCN_SAVE 存档名（缺省 warlock）、SCN_ENV「K=V …」额外环境变量
# Launching the iOS package for the scenario runner (dm/tools/scenario.py): ios-mac = the iOS package running on a Mac (.iosdd_mac/DmMacSmoke.app wrapped by smoke_mac.sh, or the package named by SCN_MAC_APP), device = a real device (launched with devicectl, log port forwarded through iproxy).
# Both use the smoke tests' guards: the same device lock (serialized with the smoke tests), the iCloud master gate (cloud_guard.sh: no sync in any run, log checked at the end), and a fresh copy of the test save via DM_SAVE_FRESH (the player's save is never touched).
# Control goes over the log port (DM_LOG_PORT), and events travel with the log (DM_EVENTS=log).
# Usage: scenario_launch.sh <ios-mac|device>   one process per session (the device lock belongs to this process): take the lock, launch, print "PORT <local port>", then read commands from stdin and answer one line each:
#   alive -> yes / no; kill -> done (kill outright: kill -9 / SIGKILL); stop -> done (end the instance, check iCloud, release the lock, then exit). Closing stdin also wraps up as stop
#   Environment: SCN_RUN result directory (required), SCN_SAVE save name (default warlock), SCN_ENV "K=V ..." extra environment variables
set -uo pipefail
KIND=${1:?ios-mac|device}; RUN=${SCN_RUN:?}
TOOLS=$(cd "$(dirname "$0")" && pwd); TREE=$(git -C "$TOOLS" rev-parse --show-toplevel)
source "$TOOLS/local_env.sh"; B=${DM_BUNDLE_ID:?先在 dm/ios/local.env 里设 DM_BUNDLE_ID | set DM_BUNDLE_ID in dm/ios/local.env first}
freeport() { python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1])"; }

if [ "$KIND" = ios-mac ]; then
  W=${SCN_MAC_APP:-$TREE/.iosdd_mac/DmMacSmoke.app}
  export DM_DEVLOCK_DIR=${DM_ROOT:-$HOME/dm-work}/run/ios/macsmoke.lock
  pids() { local p b; for p in $(ps -axo pid=,comm= | awk '{ pid=$1; $1=""; sub(/^ /,""); if ($0 ~ /\/Wrapper\/DmIOS\.app\/DmIOS$/) print pid }'); do   # 同 smoke_mac.sh：按可执行文件后缀 + bundle id 认 | as smoke_mac.sh: by executable suffix + bundle id
      b=$(lsappinfo info -only bundleID -app "$p" 2>/dev/null | sed -n 's/.*="\(.*\)"$/\1/p'); [ -z "$b" ] || [ "$b" = "$B" ] && echo "$p"; done; }
  container() { local m; for m in "$HOME"/Library/Containers/*/.com.apple.containermanagerd.metadata.plist; do
      [ "$(/usr/libexec/PlistBuddy -c 'Print :MCMMetadataIdentifier' "$m" 2>/dev/null)" = "$B" ] && { echo "$(dirname "$m")/Data"; return 0; }; done; return 1; }
  do_start() {
    [ -x "$W/Wrapper/DmIOS.app/DmIOS" ] || { echo "【红】没有 Mac 上的 iOS 包 $W：先跑一次 dm/ios/tools/smoke_mac.sh 1（它构建并包好），或用 SCN_MAC_APP 指一个 | no iOS package for the Mac: run dm/ios/tools/smoke_mac.sh 1 once, or point SCN_MAC_APP at one"; return 2; }
    source "$TOOLS/devlock.sh"; source "$TOOLS/cloud_guard.sh"; source "$TOOLS/launch_guard.sh"
    devlock_acquire "场景 $(basename "$RUN")" || return 2; LOCKED=1
    [ -z "$(pids)" ] || { echo "【红】本机已有 $B 的实例在跑（pid $(pids | tr '\n' ' ')）：先结束它 | an instance is already running: end it first"; return 2; }
    local C; C=$(container) || C=""
    cloud_guard_env "${C:+$C/Documents}" "${SCN_ENV:-}" || return 2
    [ -n "$C" ] && launch_guard_pre "$C"
    local port; port=$(freeport); local args=(--env "DM_SAVE_FRESH=${SCN_SAVE:-warlock}" --env DM_EVENTS=log --env "DM_LOG_PORT=$port" --env DM_PRESENT_LINK=0)
    [ -n "${CG_ENV:-}" ] && args+=(--env "$CG_ENV")
    for kv in ${SCN_ENV:-}; do args+=(--env "$kv"); done
    caffeinate -u -t 3 >/dev/null 2>&1
    open -n "$W" "${args[@]}" --args -mode boot > "$RUN/open.txt" 2>&1 || { echo "【红】open 失败：$(tail -1 "$RUN/open.txt")"; return 2; }
    echo "PORT $port"
  }
  do_alive() { [ -n "$(pids)" ]; }
  do_kill() { local p; for p in $(pids); do kill -KILL "$p" 2>/dev/null; done; }
  do_stop() {
    local p; for p in $(pids); do kill -TERM "$p" 2>/dev/null; done; sleep 1; for p in $(pids); do kill -KILL "$p" 2>/dev/null; done
    if [ -f "$RUN/port.log" ] && declare -F cloud_guard_check >/dev/null && ! cloud_guard_check "$RUN/port.log"; then echo "【红】iCloud 总闸：本局出现了同步动作 | the iCloud gate saw sync activity" >&2; fi
  }
elif [ "$KIND" = device ]; then
  local_env_need DM_DEVICE && { local_env_udid; local_env_need DM_IOS_UDID; } || exit 2
  D=$DM_DEVICE; UDID=$DM_IOS_UDID; IPX=""
  export DM_DEVLOCK_DIR=${DM_ROOT:-$HOME/dm-work}/run/ios/device.lock
  dev_pids() { local j; j=$(mktemp); xcrun devicectl device info processes --device "$D" --json-output "$j" >/dev/null 2>&1 || { rm -f "$j"; return 1; }
      python3 - "$j" <<'PY'
import json, sys
for p in json.load(open(sys.argv[1])).get('result', {}).get('runningProcesses', []):
    if p.get('executable', '').endswith('/DmIOS'): print(p['processIdentifier'])
PY
      rm -f "$j"; }
  do_start() {
    command -v iproxy >/dev/null || { echo "【红】缺少 iproxy（brew install libimobiledevice）| iproxy missing"; return 2; }
    source "$TOOLS/devlock.sh"; devlock_acquire "场景 $(basename "$RUN")" || return 2; LOCKED=1
    local dport=$(( 40000 + RANDOM % 20000 )) lport; lport=$(freeport)
    local envs=(DEVICECTL_CHILD_DM_SAVE_FRESH="${SCN_SAVE:-warlock}" DEVICECTL_CHILD_DM_EVENTS=log DEVICECTL_CHILD_DM_LOG_PORT="$dport")
    for kv in ${SCN_ENV:-}; do envs+=("DEVICECTL_CHILD_$kv"); done
    env "${envs[@]}" xcrun devicectl device process launch --device "$D" "$B" -mode boot > "$RUN/launch.txt" 2>&1 || { echo "【红】devicectl 拉起失败：$(tail -1 "$RUN/launch.txt")"; return 2; }
    local net; net=$(local_env_iproxy_net "$UDID"); iproxy $net -u "$UDID" "$lport" "$dport" > "$RUN/iproxy.log" 2>&1 & IPX=$!
    sleep 1; echo "PORT $lport"
  }
  do_alive() { [ -n "$(dev_pids)" ]; }
  do_kill() { local p; for p in $(dev_pids); do xcrun devicectl device process signal --device "$D" --pid "$p" --signal SIGKILL >/dev/null 2>&1; done; }
  do_stop() { local p; for p in $(dev_pids); do xcrun devicectl device process terminate --device "$D" --pid "$p" >/dev/null 2>&1; done; [ -n "$IPX" ] && kill "$IPX" 2>/dev/null; true; }
else
  echo "用法 | usage: scenario_launch.sh <ios-mac|device>"; exit 2
fi

LOCKED=0; STOPPED=0
finish() { [ "$STOPPED" = 1 ] && return; STOPPED=1; do_stop; [ "$LOCKED" = 1 ] && devlock_release; }
trap finish EXIT
do_start || exit 2
while IFS= read -r cmd; do
  case "$cmd" in
    alive) if do_alive; then echo yes; else echo no; fi;;
    kill) do_kill; echo "done";;
    stop) finish; echo "done"; exit 0;;
    *) echo "unknown $cmd";;
  esac
done
