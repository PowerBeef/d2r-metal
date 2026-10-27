// 分享日志脱敏单测（秒级）：dm_scrub.c。造一个存档目录（角色在存档、工作目录、iCloud 备份、同步状态里各一个），按 Mac / iOS 外壳的登记法加规则，
// 期望都是对着规则手写的整行，不跑被测实现得出。
// Share-log scrubbing unit test (takes seconds): dm_scrub.c. Builds a save directory (one character each in the save, the working directory, the iCloud backup and the sync state), adds rules the way the Mac / iOS shells register them;
// every expectation is a whole line written by hand from the rules, not obtained by running the implementation under test.
#include "../dm_scrub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char R[512];
static void put(const char *rel, const char *txt) {   // R 下写一个文件，中间目录按需建 | write a file under R, creating directories as needed
    char p[1024]; snprintf(p, sizeof p, "%s/%s", R, rel); for (char *s = p + strlen(R) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "w"); if (f) { fputs(txt, f); fclose(f); }
}
static void expect(const dm_scrub *s, const char *in, const char *want) {
    size_t n = 0; char *o = dm_scrub_run(s, in, strlen(in), &n);
    CHECK(o && n == strlen(want) && !memcmp(o, want, n), "输入「%s」\n      应为「%s」\n      实为「%.*s」", in, want, o ? (int)n : 4, o ? o : "NULL");
    free(o);
}

int main(void) {
    snprintf(R, sizeof R, "%s/t_scrub.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(R, 0755);
    put("save/Alice/Alice.d2s", "a"); put("save/Settings/Settings.json", "{}"); put("save/_cloudinbox/1-0/mine.pack", "p");
    put("save.work/Bob/Bob.d2s", "b"); put("save.cloudbak/Carol-1700000000000/Carol/Carol.d2s", "c");
    put("save.cloudstate", "Dave\t1a2b\t123\t1\n_stash\tff\t1\t0\n");
    char save[600]; snprintf(save, sizeof save, "%s/save", R);

    dm_scrub *s = dm_scrub_new();
    // ① 角色名：存档 / 工作目录 / 备份 / 同步状态各一个，按名字排序编号；下划线开头的单位与 Settings 不算；再收一次不重复
    // (1) Character names: one each from the save / working directory / backup / sync state, numbered in name order; units starting with an underscore and Settings do not count; collecting again adds nothing
    CHECK(dm_scrub_add_chars(s, save) == 4, "① 收到 4 个角色名（Alice Bob Carol Dave）");
    CHECK(dm_scrub_add_chars(s, save) == 0, "① 再收一次不重复");
    dm_scrub_add(s, "/Users/tester", "~", 1);
    dm_scrub_add(s, "com.tester.d2rmobile", "<bundle-id>", 0); dm_scrub_add(s, "com~tester~d2rmobile", "<bundle-id>", 0);
    dm_scrub_add(s, "LG UltraFine", "<display>", 0);
    dm_scrub_add(s, "/Volumes/My Passport", "/Volumes/<volume>", 0);
    dm_scrub_add(s, "tester", "<user>", 1);
    dm_scrub_add(s, "x", "<too-short>", 0);   // 短于 2 字节不加 | shorter than 2 bytes: not added

    // ② 登记的字面替换 | (2) registered literal replacements
    expect(s, "存档 /Users/tester/Library/Application Support/D2R Mobile/save\n", "存档 ~/Library/Application Support/D2R Mobile/save\n");
    expect(s, "/Users/tester2/x\n", "/Users/tester2/x\n");   // 别的用户（名字更长）不动 | another user (longer name) is left alone
    expect(s, "user tester, testers\n", "user <user>, testers\n");
    expect(s, "屏幕：LG UltraFine，刷新率 60.00 Hz\n", "屏幕：<display>，刷新率 60.00 Hz\n");
    expect(s, "iCloud~com~tester~d2rmobile/Documents\n", "iCloud~<bundle-id>/Documents\n");
    // ③ 路径里的 UUID 换掉，尖括号里的映像 UUID 照留 | (3) UUIDs in paths are replaced, image UUIDs in angle brackets stay
    expect(s, "/var/mobile/Containers/Data/Application/0A1B2C3D-4E5F-6789-ABCD-EF0123456789/Library/Caches/com.tester.d2rmobile/x\n",
              "/var/mobile/Containers/Data/Application/<uuid>/Library/Caches/<bundle-id>/x\n");
    expect(s, "/usr/lib/libobjc-trampolines.dylib (arm64e) <EBDF3E6C-EAE3-3890-A857-1D567371FEF3>\n", "/usr/lib/libobjc-trampolines.dylib (arm64e) <EBDF3E6C-EAE3-3890-A857-1D567371FEF3>\n");
    expect(s, "/x/0A1B2C3D-4E5F-6789-ABCD-EF0123456789AB/y\n", "/x/0A1B2C3D-4E5F-6789-ABCD-EF0123456789AB/y\n");   // 比 UUID 长的十六进制串不是 UUID | a hex run longer than a UUID is not one
    // ④ 每用户临时目录 | (4) per-user temporary directory
    expect(s, "written to /var/folders/yq/w17qs6dn4pq1rwx2qsk96kh40000gn/T/org.d2rmobile.mac.test.savedState\n", "written to /var/folders/<tmp>/T/org.d2rmobile.mac.test.savedState\n");
    // ⑤ /Volumes：挂着的（登记了）与没挂着的（按形状）| (5) /Volumes: mounted (registered) and not mounted (by shape)
    expect(s, "fs 挂载 $: → /Volumes/My Passport/RomFS（只读）\n", "fs 挂载 $: → /Volumes/<volume>/RomFS（只读）\n");
    expect(s, "资源 /Volumes/Old Disk 2（只读）\n", "资源 /Volumes/<volume>（只读）\n");
    expect(s, "/Volumes/Ext 不可读\n", "/Volumes/<volume> 不可读\n");
    expect(s, "/Volumes/Données/x\n", "/Volumes/<volume>/x\n");
    // ⑥ 角色名：前后是中文标点、空格、斜杠、连字符时换，嵌在别的词里不换；存档里没有的角色按 <名>.d2s 换
    // (6) Character names: replaced next to Chinese punctuation, spaces, slashes, hyphens; not inside other words; characters not in the save are caught by <name>.d2s
    expect(s, "百宝囊：Bob 还没有侧文件\n", "百宝囊：<char2> 还没有侧文件\n");
    expect(s, "刷怪积分牌：「Alice」还没有记录（/Users/tester/x/save.work/Alice/Alice.sw_kills）\n", "刷怪积分牌：「<char1>」还没有记录（~/x/save.work/<char1>/<char1>.sw_kills）\n");
    expect(s, "iCloud 同步：Carol 的本机那份挪进备份 /b/Carol-1700000000000\n", "iCloud 同步：<char3> 的本机那份挪进备份 /b/<char3>-1700000000000\n");
    expect(s, "iCloud 同步：Dave 另一台删了\n", "iCloud 同步：<char4> 另一台删了\n");
    expect(s, "/save/Alice/Alice.d2s\n", "/save/<char1>/<char1>.d2s\n");
    expect(s, "游戏写 Eve.d2s、/s/Zed_x/Zed_x.d2s.meta\n", "游戏写 <char>.d2s、/s/Zed_x/<char>.d2s.meta\n");
    expect(s, "Alicea and xBob stay; *.d2s too\n", "Alicea and xBob stay; *.d2s too\n");
    // ⑦ 空输入；一份大日志（4 MB、300 条规则）秒级洗完 | (7) empty input; a large log (4 MB, 300 rules) scrubbed within seconds
    { size_t n = 1; char *o = dm_scrub_run(s, "", 0, &n); CHECK(o && n == 0, "⑦ 空输入：返回空串"); free(o); }
    { for (int i = 0; i < 300; i++) { char nm[16]; snprintf(nm, sizeof nm, "Hero%03d", i); dm_scrub_add(s, nm, "<h>", 1); }
      size_t n = 4 << 20; char *big = malloc(n); const char *line = "[dm] 秘境：本局没有资格（难度 2，角色「Hero123」）/Users/tester/save\n"; size_t ll = strlen(line);
      for (size_t i = 0; i < n; i++) big[i] = line[i % ll];
      clock_t t0 = clock(); size_t on = 0; char *o = dm_scrub_run(s, big, n, &on); double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
      const char *want = "角色「<h>」）~/save"; CHECK(o && memmem(o, on, want, strlen(want)) && !memmem(o, on, "Hero123", 7) && !memmem(o, on, "tester", 6), "⑦ 大日志：名字与家目录都换掉");
      CHECK(sec < 3, "⑦ 大日志 4 MB 洗了 %.2f 秒（应在 3 秒内）", sec); free(o); free(big); }
    dm_scrub_free(s);
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", R); if (system(cmd)) {}
    printf("t_scrub: %d 项，%d 失败\n", checks, fails);
    return fails != 0;
}
