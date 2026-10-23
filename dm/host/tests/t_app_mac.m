// Mac App 外壳（直接包含 app_mac.m）的单测：选资源文件夹时，build_manifest.psv 在、档案文件也要齐（romfs_validate.c）才收，不全的给出清单。
// 档案文件按本构建的字节数建成稀疏文件（不占盘、不含任何游戏内容）。
// Unit test of the Mac App shell (includes app_mac.m directly): when a resource folder is chosen, it is accepted only if build_manifest.psv is there and the archive files are complete too (romfs_validate.c); an incomplete one gets the list.
// The archive files are created as sparse files with this build's byte counts (no disk space, no game content at all).
#import <Foundation/Foundation.h>
#include "../app_mac.m"
#include "../romfs_validate.c"
void dm_update_init(int own_build, void (*on_select)(void), void (*on_click)(void)) { (void)own_build; (void)on_select; (void)on_click; }
void dm_update_offer(int new_build, const char *note, int urgent) { (void)new_build; (void)note; (void)urgent; }
void dm_update_safe_left(int units) { (void)units; }
int dm_custom_get(const char *key) { (void)key; return 0; }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL "); } else printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

// 在 dir 里建一个资源根：data/build_manifest.psv + 必需档案（skip = 少建第几个，short_i = 第几个短 1 字节；-1 = 不动） | build a resource root in dir: data/build_manifest.psv + the required archives (skip = which one to leave out, short_i = which one is 1 byte short; -1 = none)
static NSString *make_root(NSString *dir, int skip, int short_i, int archives) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *data = [dir stringByAppendingPathComponent:@"data"];
    [fm removeItemAtPath:dir error:nil]; [fm createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
    [@"manifest" writeToFile:[data stringByAppendingPathComponent:@"build_manifest.psv"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
    for (size_t i = 0; archives && i < NEED_N; i++) { if ((int)i == skip) continue;
        NSString *p = [data stringByAppendingPathComponent:@(need[i])]; FILE *f = fopen(p.fileSystemRepresentation, "w");
        if (f) { ftruncate(fileno(f), (off_t)(size[i] - ((int)i == short_i))); fclose(f); } }
    return dir;
}
int main(void) {
    @autoreleasepool {
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_app_mac_%d", getpid()]];
        NSString *sv = [tmp stringByAppendingPathComponent:@"save"]; [NSFileManager.defaultManager createDirectoryAtPath:sv withIntermediateDirectories:YES attributes:nil error:nil];
        setenv("DM_SAVE", sv.fileSystemRepresentation, 1); setenv("DM_LANG", "en-US", 1);   // 提示文字取英文列 | the prompt text comes from the English column
        NSString *why = nil, *r;

        r = romfs_root(make_root([tmp stringByAppendingPathComponent:@"full"], -1, -1, 1), &why);
        CHECK(r && [r.lastPathComponent isEqualToString:@"full"], "齐全的资源根照收 | a complete resource root is accepted");
        r = romfs_root([[tmp stringByAppendingPathComponent:@"full"] stringByAppendingPathComponent:@"data"], &why);
        CHECK(r && [r.lastPathComponent isEqualToString:@"full"], "直接选 data 那一层也照收（用它的上一层） | choosing the data level itself is accepted too (its parent is used)");
        make_root([[tmp stringByAppendingPathComponent:@"up"] stringByAppendingPathComponent:@"RomFS"], -1, -1, 1);
        r = romfs_root([tmp stringByAppendingPathComponent:@"up"], &why);
        CHECK(r && [r.lastPathComponent isEqualToString:@"RomFS"], "选到上一层（Update）往里找一层 | choosing the level above (Update) searches one level down");

        why = nil; r = romfs_root(make_root([tmp stringByAppendingPathComponent:@"bare"], -1, -1, 0), &why);
        printf("    %s\n", why.UTF8String);
        CHECK(!r && [why containsString:@"0 / 18"] && [g_romfs_title isEqualToString:@"The game data is incomplete"], "只有清单、没有档案：不收，给清单与「不全」标题 | manifest only, no archives: refused, with the list and the \"incomplete\" title");
        why = nil; r = romfs_root(make_root([tmp stringByAppendingPathComponent:@"miss"], 3, -1, 1), &why);
        CHECK(!r && [why containsString:@"17 / 18"] && [why containsString:@(need[3])], "缺一个档案：不收，点名缺的那个 | one archive missing: refused, naming the missing one");
        why = nil; r = romfs_root(make_root([tmp stringByAppendingPathComponent:@"short"], -1, 5, 1), &why);
        CHECK(!r && [why containsString:@"Incomplete"] && [why containsString:@(need[5])], "一个档案没拷完：不收，点名没拷完的那个 | one archive half-copied: refused, naming the incomplete one");
        why = nil; r = romfs_root([[tmp stringByAppendingPathComponent:@"bare"] stringByAppendingPathComponent:@"data"], &why);
        CHECK(!r && [why containsString:@"0 / 18"], "直接选不全的 data 那一层同样不收 | choosing an incomplete data level directly is refused as well");

        [NSFileManager.defaultManager createDirectoryAtPath:[tmp stringByAppendingPathComponent:@"other"] withIntermediateDirectories:YES attributes:nil error:nil];
        why = nil; r = romfs_root([tmp stringByAppendingPathComponent:@"other"], &why);
        CHECK(!r && [why containsString:@"build_manifest.psv"] && !g_romfs_title, "不是资源文件夹：原因与标题照旧 | not a resource folder: reason and title as before");
        [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];

    }
    printf("== t_app_mac %d 项，失败 %d | %d checks, %d failed ==\n", checks, fails, checks, fails);
    return fails != 0;
}
