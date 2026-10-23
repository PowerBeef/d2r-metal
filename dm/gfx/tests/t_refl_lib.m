// 本机着色器库整库反射检查：现行翻译链版本的库（$DM_ROOT/build/cache/metal_shader_cache/b-<V>/，V 取自 dmsh_chain.h）里每个 <键>_b.json
// 都过一遍回放层真用的 nvn_refl_from_json，有一个被拒就失败（被拒的着色器载入失败，绘制少了那一阶段）。只读本机文件，不往仓库写任何东西；
// 库不在（没出包的机器）就跳过、不算失败。
// Whole-library reflection check on this machine: every <key>_b.json in the library of the current translation chain version ($DM_ROOT/build/cache/metal_shader_cache/b-<V>/, V from dmsh_chain.h)
// goes through nvn_refl_from_json, the parser the replay really uses; a single rejection fails the check (a rejected shader fails to load and its draws lose that stage). It only reads local files and writes nothing into the repo;
// when the library is absent (a machine that has not packaged it), it skips and does not count as a failure.
#import "../nvn/nvn_refl.h"
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "../nvn/dmsh_chain.h"

int main(void) { @autoreleasepool {
    const char *root = getenv("DM_ROOT");
    NSString *dir = [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache/b-%s", root ?: getenv("HOME"), root ? "" : "/dm-work", DMSH_CHAIN];
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
    if (!names) { printf("t_refl_lib：跳过（本机没有着色器库 %s）\n", dir.UTF8String); return 0; }
    static nvn_refl r; char err[160];
    int n = 0, bad = 0, maxb = 0, maxaid = -1;
    for (NSString *f in [names sortedArrayUsingSelector:@selector(compare:)]) {
        if (![f hasSuffix:@"_b.json"]) continue;
        @autoreleasepool {
            NSData *d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:f]];
            NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
            n++;
            if (![j isKindOfClass:[NSDictionary class]]) { if (bad++ < 10) printf("  %s：json 读不出\n", f.UTF8String); continue; }
            if (nvn_refl_from_json(j, &r, err, sizeof err)) { if (bad++ < 10) printf("  %s：%s\n", f.UTF8String, err); continue; }
            if (r.nb > maxb) maxb = r.nb;
            for (int i = 0; i < r.nb; i++) if (r.b[i].aid > maxaid) maxaid = r.b[i].aid;
        }
    }
    printf("t_refl_lib：库 b-%s 共 %d 份反射，被拒 %d；最多绑定 %d、最大参数序号 %d（上限 %d）%s\n", DMSH_CHAIN, n, bad, maxb, maxaid, NVN_REFL_MAXB, bad || !n ? " ✗" : " ✓");
    return bad || !n;
} }
