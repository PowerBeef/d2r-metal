// 新版本检查的规则（纯函数，dm_update.m 用；单测 tests/t_update_rules.c）。
// 查的间隔：选角页每出现一次（启动、每局退回、列表重开、回前台）都可能要查；上次查到之后 DM_UPDATE_TTL_S 秒内不再联网。
// 只算查到的那次：取不到 / 格式不对不记，下次照查（失败提示要能在网络恢复后撤掉）。启动后第一次一律查。
// 安装链接的主机：要与版本文件同一主机或是它的子域。版本文件地址是出包时写进包里的，安装链接跟着它走；版本文件被改 / 镜像出错时不把玩家送去别的站装包。
// Rules of the new-version check (pure functions, used by dm_update.m; unit test tests/t_update_rules.c).
// Check interval: every appearance of the character select page (launch, each return from a run, the list reopened, returning to the foreground) may want a check; within DM_UPDATE_TTL_S seconds of the last successful fetch there is no new network access.
// Only a successful fetch counts: a failed fetch / bad format is not recorded and the next visit checks again (the failure notice must be able to clear once the network is back). The first check after launch always runs.
// Host of the install link: it must be the version file's host or a subdomain of it. The version file address is baked into the package at packaging time and the install link follows it; a tampered version file / a broken mirror cannot send the player to install from another site.
#pragma once

#define DM_UPDATE_TTL_S 3600.0

// now / last_ok：单调钟秒数（含睡眠）；last_ok = 0 表示这次启动还没查到过。返回 1 = 该联网查了
// now / last_ok: monotonic seconds (counting sleep); last_ok = 0 means nothing has been fetched yet during this launch. Returns 1 = time to fetch
static inline int dm_update_due(double now, double last_ok) {
    if (last_ok <= 0) return 1;
    if (now < last_ok) return 1;   // 钟往回走（不该发生）：照查 | the clock went backwards (should not happen): check anyway
    return now - last_ok >= DM_UPDATE_TTL_S;
}

// 主机名比较只认 ASCII、不分大小写（主机名本来就是 ASCII，国际化域名是 xn-- 形式）
// Host names are compared as ASCII, case-insensitively (host names are ASCII anyway; internationalized names come as xn--)
static inline int dm_update_ieq(const char *a, const char *b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) { char x = a[i], y = b[i]; if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a'); if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a'); if (x != y) return 0; }
    return 1;
}
// link_host：安装链接实际要去的主机（https 链接的主机；itms-services 链接取 url= 里那个 https 清单地址的主机）；file_host：版本文件的主机。返回 1 = 认这个链接
// link_host: the host the install link really goes to (the host of an https link; for an itms-services link, the host of the https manifest address in url=); file_host: the version file's host. Returns 1 = accept the link
static inline int dm_update_host_ok(const char *link_host, const char *file_host) {
    if (!link_host || !file_host || !*link_host || !*file_host) return 0;
    unsigned long l = 0, f = 0; while (link_host[l]) l++; while (file_host[f]) f++;
    if (l == f) return dm_update_ieq(link_host, file_host, l);
    return l > f + 1 && link_host[l - f - 1] == '.' && dm_update_ieq(link_host + l - f, file_host, f);   // 子域：前面隔一个点 | subdomain: separated by a dot
}
