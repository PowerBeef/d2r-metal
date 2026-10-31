// Mac App 外壳（直接包含 app_mac.m）的单测：选资源文件夹时，build_manifest.psv 在、档案文件也要齐（romfs_validate.c）才收，不全的给出清单；
// 更新检查的两个入口（启动时的后台队列、游戏线程上的选角页回调）同时到、点「更新」也在别的线程上时，共用的状态只在主线程读写（tsan 目标在线程消毒器下跑同一个程序）。
// 档案文件按本构建的字节数建成稀疏文件（不占盘、不含任何游戏内容）；版本文件由本地的假 HTTP 协议回答，不联网。
// Unit test of the Mac App shell (includes app_mac.m directly): when a resource folder is chosen, it is accepted only if build_manifest.psv is there and the archive files are complete too (romfs_validate.c); an incomplete one gets the list;
// when the update check's two entry points (a background queue at launch, the character-select callback on the game thread) arrive together and "Update" is tapped on yet another thread, the shared state is read and written on the main thread only (the tsan target runs the same program under ThreadSanitizer).
// The archive files are created as sparse files with this build's byte counts (no disk space, no game content at all); the version file is answered by a local fake HTTP protocol, with no network access.
#import <Foundation/Foundation.h>
@interface NSURLSessionConfiguration (DmTest) + (NSURLSessionConfiguration *)dm_test_ephemeral; @end
#define ephemeralSessionConfiguration dm_test_ephemeral   // 被测代码建的会话换成带假协议的那个 | the session the code under test creates gets the fake protocol
#include "../app_mac.m"
#undef ephemeralSessionConfiguration
#include "../romfs_validate.c"
static _Atomic int g_requests, g_offers;
static NSString *g_fake_page = @"http://dmtest.invalid/page/%d";   // the download page the fake version file names (%d = the request number)
@interface DmFakeHTTP : NSURLProtocol @end
@implementation DmFakeHTTP
+ (BOOL)canInitWithRequest:(NSURLRequest *)r { return [r.URL.host isEqualToString:@"dmtest.invalid"]; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)r { return r; }
- (void)startLoading {
    int n = atomic_fetch_add(&g_requests, 1); NSString *body = [NSString stringWithFormat:@"{\"build\":2,\"note\":\"n\",\"url\":\"%@\"}", [NSString stringWithFormat:g_fake_page, n]];
    NSHTTPURLResponse *rsp = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{ @"Content-Type": @"application/json" }];
    [self.client URLProtocol:self didReceiveResponse:rsp cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocol:self didLoadData:[body dataUsingEncoding:NSUTF8StringEncoding]];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {}
@end
@implementation NSURLSessionConfiguration (DmTest)
+ (NSURLSessionConfiguration *)dm_test_ephemeral { NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration]; c.protocolClasses = @[ DmFakeHTTP.class ]; return c; }
@end
void dm_update_init(int own_build, void (*on_select)(void), void (*on_click)(void)) { (void)own_build; (void)on_select; (void)on_click; }
void dm_update_offer(int new_build, const char *note, int urgent) { (void)note; (void)urgent; if (new_build) atomic_fetch_add(&g_offers, 1); }
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

        // 更新检查：两个入口同时到（启动时的后台队列 + 游戏线程的选角页回调），另一个线程一直在点「更新」 | update check: both entry points at once (the launch background queue + the game thread's character-select callback), while another thread keeps tapping "Update"
        setenv("DM_TEST_UPDATE_NOOPEN", "1", 1); g_upd_url = @"http://dmtest.invalid/latest_mac.json"; g_upd_own = 1;
        dispatch_group_t g = dispatch_group_create();
        dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ upd_check(); });
        dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ upd_check(); });
        dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ for (int i = 0; i < 2000; i++) { upd_click(); usleep(200); } });
        for (int i = 0; i < 300 && (atomic_load(&g_offers) < 1 || dispatch_group_wait(g, DISPATCH_TIME_NOW)); i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);   // 主线程转起来（主队列上的事在这里做） | run the main thread (the main-queue work happens here)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
        CHECK(atomic_load(&g_requests) == 1 && atomic_load(&g_offers) == 1, "两个入口同时到：60 秒里只查一次（请求 %d 次，提示 %d 次） | both entry points at once: one check within 60 s (requests %d, offers %d)", atomic_load(&g_requests), atomic_load(&g_offers), atomic_load(&g_requests), atomic_load(&g_offers));
        CHECK([g_upd_page isEqualToString:@"http://dmtest.invalid/page/0"], "下载页地址在主线程上记下了（%s） | the download page address was recorded on the main thread (%s)", g_upd_page.UTF8String, g_upd_page.UTF8String);
        upd_check(); CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
        CHECK(atomic_load(&g_requests) == 1, "60 秒内再来一次：不查 | another one within 60 s: not checked");
        // the launcher's scheme is a page too (an app the launcher installed opens it for the update); another host is not
        g_fake_page = @"d2rmetal://update"; g_upd_last = 0; upd_check();
        for (int i = 0; i < 100 && !g_upd_page.length; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        for (int i = 0; i < 100 && ![g_upd_page hasPrefix:@"d2rmetal:"]; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        CHECK([g_upd_page isEqualToString:@"d2rmetal://update"], "the launcher's d2rmetal:// address is accepted (%s)", g_upd_page.UTF8String);
        g_fake_page = @"https://elsewhere.invalid/x"; g_upd_last = 0; upd_check();
        for (int i = 0; i < 100 && g_upd_page; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        CHECK(!g_upd_page, "a page on another host is not (%s)", g_upd_page.UTF8String ?: "none");
        // the launcher's scheme opens only its update link: any other d2rmetal:// address is not followed
        for (NSString *other in @[@"d2rmetal://remove", @"d2rmetal://update/x", @"d2rmetal://update?run=1"]) {
            g_fake_page = @"d2rmetal://update"; g_upd_last = 0; upd_check();
            for (int i = 0; i < 100 && !g_upd_page.length; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            g_fake_page = other; g_upd_last = 0; upd_check();
            for (int i = 0; i < 100 && g_upd_page; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            CHECK(!g_upd_page, "%s is not followed (%s)", other.UTF8String, g_upd_page.UTF8String ?: "none");
        }
    }
    printf("== t_app_mac %d 项，失败 %d | %d checks, %d failed ==\n", checks, fails, checks, fails);
    return fails != 0;
}
