// 新版本检查的规则（纯函数，dm_update.m 用；单测 tests/t_update_rules.c）。
// 查的间隔：选角页每出现一次（启动、每局退回、列表重开、回前台）都可能要查；上次查到之后 DM_UPDATE_TTL_S 秒内不再联网。
// 只算查到的那次：取不到 / 格式不对不记，下次照查（失败提示要能在网络恢复后撤掉）。启动后第一次一律查。
// Rules of the new-version check (pure functions, used by dm_update.m; unit test tests/t_update_rules.c).
// Check interval: every appearance of the character select page (launch, each return from a run, the list reopened, returning to the foreground) may want a check; within DM_UPDATE_TTL_S seconds of the last successful fetch there is no new network access.
// Only a successful fetch counts: a failed fetch / bad format is not recorded and the next visit checks again (the failure notice must be able to clear once the network is back). The first check after launch always runs.
#pragma once

#define DM_UPDATE_TTL_S 3600.0

// now / last_ok：单调钟秒数（含睡眠）；last_ok = 0 表示这次启动还没查到过。返回 1 = 该联网查了
// now / last_ok: monotonic seconds (counting sleep); last_ok = 0 means nothing has been fetched yet during this launch. Returns 1 = time to fetch
static inline int dm_update_due(double now, double last_ok) {
    if (last_ok <= 0) return 1;
    if (now < last_ok) return 1;   // 钟往回走（不该发生）：照查 | the clock went backwards (should not happen): check anyway
    return now - last_ok >= DM_UPDATE_TTL_S;
}
