// 着色器键的取得时刻：nvnProgramSetShaders 时就对控制段取 SHA-256（前 16 位十六进制 = 着色器键），回放解析程序时只用存下的键、不再读控制段。
// 现场：游戏 SetShaders 之后改写 / 释放了控制段内存（NVN 只要求数据段常驻），之后才第一次画到这个程序。
// 期望值不靠被测实现：键由本测试对控制段原文另算；回放侧预先登记「这个键 → 一个占位着色器」，按键解析到占位着色器即说明用的是 SetShaders 时的内容。
// When the shader key is taken: the SHA-256 of the control section (first 16 hex digits = the shader key) is taken at nvnProgramSetShaders, and when the replay resolves the program it only uses the stored keys and no longer reads the control section.
// Scene: after SetShaders the game rewrites / frees the control section memory (NVN only requires the data section to stay resident), and only then draws with this program for the first time.
// The expected values do not depend on the implementation under test: the key is computed separately by this test from the original control section; the replay side pre-registers "this key -> a placeholder shader", and resolving to the placeholder shows the content at SetShaders time was used.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"
uint64_t g_t0_ns;   // 宿主 main.c 的进程起点（hle_nvn 逐帧 @t_s 用），单测替身 | process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int failed, cases;
#define EXPECT(n, w, v) do { long long a_ = (long long)(w), b_ = (long long)(v); cases++; printf("%-52s 期望 %lld → 真结果 %lld %s\n", n, a_, b_, a_ == b_ ? "✓" : "✗"); failed += a_ != b_; } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    // 控制段：0x880 字节的已知图案（不是游戏内容）；片元阶段，第 1 段 | Control section: a known 0x880-byte pattern (not game content); fragment stage, one stage
    static uint8_t ctl[0x880]; for (int i = 0; i < (int)sizeof ctl; i++) ctl[i] = (uint8_t)(i * 7 + 3);
    uint8_t md[32]; CC_SHA256(ctl, sizeof ctl, md);
    NSString *key = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
    g_shmap = [NSMutableDictionary dictionary]; g_shmap[key] = (id)@"像素";   // 映射表只有这一个键（不读盘） | The mapping table has only this one key (nothing read from disk)
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *ph = [NvShader new]; ph->stage = 1; g_shcache[[key stringByAppendingString:@"_b"]] = ph;   // 占位着色器 | Placeholder shader

    NVNprogram pg; NVNshaderData sd = {0x1000, ctl};
    h_nvnProgramInitialize(&pg, NULL);
    EXPECT("SetShaders 成功", 1, h_nvnProgramSetShaders(&pg, 1, &sd));
    memset(ctl, 0x5a, sizeof ctl);   // 游戏改写了控制段内存 | The game rewrote the control section memory
    __unsafe_unretained NvProg *pr = prog_get(snap_prog(&pg));
    EXPECT("解析出程序", 1, pr != nil);
    EXPECT("片元阶段按 SetShaders 时的键解析到占位着色器", 1, pr && pr->sh[1] == ph);

    // 换着色器：新序号、新键；旧控制段内容已无关 | Swap shaders: new serial, new key; the old control section content no longer matters
    static uint8_t ctl2[0x880]; for (int i = 0; i < (int)sizeof ctl2; i++) ctl2[i] = (uint8_t)(i * 13 + 1);
    CC_SHA256(ctl2, sizeof ctl2, md);
    NSString *key2 = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
    g_shmap[key2] = (id)@"顶点"; NvShader *ph2 = [NvShader new]; ph2->stage = 0; g_shcache[[key2 stringByAppendingString:@"_b"]] = ph2;
    NVNshaderData sd2 = {0x2000, ctl2};
    EXPECT("再次 SetShaders 成功", 1, h_nvnProgramSetShaders(&pg, 1, &sd2));
    memset(ctl2, 0xa5, sizeof ctl2);
    pr = prog_get(snap_prog(&pg));
    EXPECT("换着色器后按新键解析到顶点占位着色器", 1, pr && pr->sh[0] == ph2 && !pr->sh[1]);
    h_nvnProgramFinalize(&pg);
    printf("== %d 项，失败 %d ==\n", cases, failed);
    return failed != 0;
} }
