# 本机设备配置（不进仓）。被各脚本 source：按序找 $DM_LOCAL_ENV → dm/ios/local.env → ~/.config/dm-ios/local.env，读第一个存在的
# （后一个位置供同一台机器上的多个工作树共用）。样例与各项含义见 dm/ios/local.env.example。
# local_env_need <变量…>：缺哪项就报哪项并返回非零。
# Local device configuration (not committed to the repo). Sourced by the scripts: looks in order at $DM_LOCAL_ENV → dm/ios/local.env → ~/.config/dm-ios/local.env and reads the first that exists
# (the last location lets multiple worktrees on the same machine share it). For a sample and the meaning of each item see dm/ios/local.env.example.
# local_env_need <variables...>: reports whichever items are missing and returns nonzero.
_le_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
for _le_f in "${DM_LOCAL_ENV:-}" "$_le_dir/local.env" "$HOME/.config/dm-ios/local.env"; do
  [ -n "$_le_f" ] && [ -f "$_le_f" ] && { set -a; source "$_le_f"; set +a; DM_LOCAL_ENV_FILE=$_le_f; break; }
done
local_env_need() {
  local v miss=
  for v in "$@"; do [ -n "${!v:-}" ] || miss="$miss $v"; done
  [ -z "$miss" ] || { echo "🔴 缺本机配置：${miss}（把 dm/ios/local.env.example 复制成 dm/ios/local.env 填好；本次读的是：${DM_LOCAL_ENV_FILE:-没找到配置文件}）" >&2; return 1; }
}
# local_env_udid：DM_IOS_UDID 没填时，按 DM_DEVICE 向 devicectl 查出这台设备的硬件 UDID 填上并导出（同 smoke.sh 的查法）；查不到返回非零。
#   界面测试（xcodebuild -destination）、Instruments（xctrace --device）、端口转发（iproxy -u）都要这个号；付费账号的 local.env 通常不填它
# local_env_iproxy_net <UDID>：这台设备只连着网络（USB 上没有、usbmuxd 的网络列表里有）时输出 -n 给 iproxy 用；连着 USB 或查不了时什么都不输出
# local_env_udid: when DM_IOS_UDID is not filled in, look up this device's hardware UDID from devicectl by DM_DEVICE, fill it in and export it (same lookup as smoke.sh); returns nonzero when it cannot be found.
#   UI tests (xcodebuild -destination), Instruments (xctrace --device) and port forwarding (iproxy -u) all need this ID; the local.env of a paid account usually leaves it empty
# local_env_iproxy_net <UDID>: prints -n for iproxy when this device is connected only over the network (not on USB, but in usbmuxd's network list); prints nothing when it is on USB or cannot be checked
local_env_udid() {
  [ -n "${DM_IOS_UDID:-}" ] && return 0; [ -n "${DM_DEVICE:-}" ] || return 1
  local j tb; j=$(mktemp) || return 1; tb=$(command -v timeout || command -v gtimeout || true)
  ${tb:+"$tb" 20} xcrun devicectl list devices --json-output "$j" > /dev/null 2>&1
  DM_IOS_UDID=$(python3 -c "import json,sys;print(next((d.get('hardwareProperties',{}).get('udid','') for d in json.load(open(sys.argv[1]))['result']['devices'] if sys.argv[2] in (d.get('identifier'), d.get('hardwareProperties',{}).get('udid'))), ''))" "$j" "$DM_DEVICE" 2> /dev/null)
  rm -f "$j"; [ -n "$DM_IOS_UDID" ] && export DM_IOS_UDID
}
local_env_iproxy_net() {
  command -v idevice_id > /dev/null || return 0
  idevice_id -l 2> /dev/null | grep -qx "$1" && return 0
  if idevice_id -n 2> /dev/null | grep -qx "$1"; then printf '%s\n' -n; fi
}
