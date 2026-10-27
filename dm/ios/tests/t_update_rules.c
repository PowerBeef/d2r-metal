// 新版本检查规则的单测（dm/ios/Sources/dm_update_rules.h）：启动后第一次一律查、查到之后一段时间内不再联网；安装链接只认版本文件的主机或其子域。
// Unit test for the new-version check rules (dm/ios/Sources/dm_update_rules.h): the first check after launch always runs, and there is no network access for a while after a successful fetch; the install link is accepted only on the version file's host or a subdomain of it.
#include <stdio.h>
#include "dm_update_rules.h"

static int g_fail;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main(void) {
    const double T = DM_UPDATE_TTL_S;
    CHECK(dm_update_due(5, 0) == 1);                    // 这次启动还没查到过 | nothing fetched yet during this launch
    CHECK(dm_update_due(1000 + 60, 1000) == 0);         // 一分钟后又回选角页（刷图一局）：不查 | back on character select a minute later (one farming run): no check
    CHECK(dm_update_due(1000 + T - 1, 1000) == 0);
    CHECK(dm_update_due(1000 + T, 1000) == 1);          // 到点再查 | due again once the interval has passed
    CHECK(dm_update_due(1000 + 10 * T, 1000) == 1);     // 后台放了很久回来 | back after a long time in the background
    CHECK(dm_update_due(500, 1000) == 1);               // 钟往回走：照查 | clock went backwards: check anyway
    // 安装链接的主机 | install link host
    CHECK(dm_update_host_ok("dl.example.com", "dl.example.com") == 1);
    CHECK(dm_update_host_ok("DL.Example.COM", "dl.example.com") == 1);       // 不分大小写 | case-insensitive
    CHECK(dm_update_host_ok("cdn.example.com", "example.com") == 1);         // 版本文件主机的子域 | subdomain of the version file host
    CHECK(dm_update_host_ok("example.com", "updates.example.com") == 0);     // 上一级域不认 | the parent domain is not accepted
    CHECK(dm_update_host_ok("evil.com", "example.com") == 0);
    CHECK(dm_update_host_ok("badexample.com", "example.com") == 0);          // 只是尾巴相同、不隔点 | same tail but no dot boundary
    CHECK(dm_update_host_ok("example.com.evil.com", "example.com") == 0);
    CHECK(dm_update_host_ok(".example.com", "example.com") == 0);            // 点前面是空的：不认 | empty label before the dot: rejected
    CHECK(dm_update_host_ok("", "example.com") == 0);
    CHECK(dm_update_host_ok("example.com", "") == 0);
    CHECK(dm_update_host_ok(0, "example.com") == 0);
    CHECK(dm_update_host_ok("example.com", 0) == 0);
    if (g_fail) { printf("t_update_rules: %d FAIL\n", g_fail); return 1; }
    printf("t_update_rules: OK\n");
    return 0;
}
