// 游戏状态上报给系统（StateReporting，macOS / iOS 27 起；diag.c 推出 Menu / Loading / Playing / Paused / Background）：系统的 Metal 性能记录按状态分段，
// metalperftrace overview --aggregate --domain GameState 能按「局内」「读取」分开看帧率。缺省关，DM_STATEREP=1 开（基准测试会开）。
// 框架在运行时加载（宿主最低支持 macOS 26，不链接它）；系统没有这个框架时什么都不做
// Game state reported to the system (StateReporting, macOS / iOS 27 and later; diag.c derives Menu / Loading / Playing / Paused / Background): the system's Metal performance recording is segmented by state,
// so metalperftrace overview --aggregate --domain GameState shows frame rates separately for "playing" and "loading". Off by default; DM_STATEREP=1 turns it on (the benchmarks do).
// The framework is loaded at run time (the host supports macOS 26 and does not link it); nothing happens when the system lacks it
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "dm_events.h"
#if __has_include(<StateReporting/StateReporting.h>)
#import <StateReporting/StateReporting.h>
#define DM_HAVE_SR 1
#endif

#ifdef DM_HAVE_SR
static id g_rep;
static void report(const char *label, int area, int players) {
    @autoreleasepool {
        NSDictionary *meta = area > 0 ? @{@"area": @(area), @"players": @(players)} : nil;
        if (@available(macOS 27.0, iOS 27.0, *)) [(SRStateReporter *)g_rep reportTransitionToStateLabel:@(label) stableMetadata:nil volatileMetadata:meta];
    }
}
#endif
__attribute__((constructor)) static void staterep_init(void) {
    const char *e = getenv("DM_STATEREP");
    if (!e || strcmp(e, "1")) return;
#ifdef DM_HAVE_SR
    if (!dlopen("/System/Library/Frameworks/StateReporting.framework/StateReporting", RTLD_LAZY)) { fprintf(stderr, "[dm] DM_STATEREP：系统没有 StateReporting（需 macOS / iOS 27），不上报 | StateReporting not available, not reporting\n"); return; }
    if (@available(macOS 27.0, iOS 27.0, *)) {
        Class c = NSClassFromString(@"SRStateReporter");
        if (c) g_rep = [(id)c reporterForDomain:@"GameState"];
    }
    if (g_rep) g_dm_state_fn = report;
#endif
}
