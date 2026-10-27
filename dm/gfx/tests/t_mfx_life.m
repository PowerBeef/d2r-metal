// MetalFX 放大器生命期单测：新尺寸连续要够 MFX_SETTLE 次才建（拉窗口的每一步不各建一套）；换尺寸 / 关掉「放大方式」时，
// 换下的放大器与两张贴图在出图那份等之后 4 次出图、3D 放大趟那份等本次提交跑完才真正释放（Metal 4 命令缓冲不持有它们）。用弱引用看对象何时没了
// Unit test for the MetalFX scaler lifetime: a new size is built only after MFX_SETTLE requests in a row (not one set per window-resize step); on a size change / "Upscaling" turned off,
// the replaced scaler and both textures are really released only after 4 more presents (present copy) or after this submission has run (3D upscale pass copy), since Metal 4 command buffers do not retain them. Weak references show when the objects are gone
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w=(want), g=(got); count++; printf("%s 期望 %llu → 真结果 %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w==g?"✓":"✗"); fail += w != g; } while (0)
static int ready(mfx_t *m, NSUInteger iw, NSUInteger ow) { return mfx_ready(m, MFX_M4, MTLPixelFormatRGBA8Unorm, iw, iw * 9 / 16, ow, ow * 9 / 16); }
int main(void) { setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
#if MFX_M4
    int ok = [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev];
#else
    int ok = [MTLFXSpatialScalerDescriptor supportsDevice:g_dev];
#endif
    if (!ok) { printf("本机不支持 MetalFX 空间放大，跳过\n== 0 项，失败 0 ==\n"); return 0; }
    __weak id ws, wi, wo;
    // ① 出图那份：前 MFX_SETTLE − 1 次只记尺寸、不建；第 MFX_SETTLE 次建好；同尺寸再要直接用
    // ① Present copy: the first MFX_SETTLE − 1 requests only note the size and build nothing; request MFX_SETTLE builds; the same size again is used directly
    @autoreleasepool {
        int early = 0; for (int i = 0; i < MFX_SETTLE - 1; i++) early += ready(&g_mfx_pres, 320, 640);
        EXPECT("新尺寸稳定前不建", 0, (uint64_t)early); EXPECT("稳定前没有放大器", 0, (uint64_t)(g_mfx_pres.scaler != nil));
        EXPECT("第 MFX_SETTLE 次建好", 1, (uint64_t)ready(&g_mfx_pres, 320, 640));
        EXPECT("同尺寸再要直接用", 1, (uint64_t)ready(&g_mfx_pres, 320, 640));
        ws = g_mfx_pres.scaler; wi = g_mfx_pres.in; wo = g_mfx_pres.out;
        // ② 拉窗口：尺寸每次都变，一套也不建，旧的那套一直留着（还没换下）
        // ② Window resize: the size changes every time, nothing is built, and the old set stays (not replaced yet)
        int built = 0; for (int i = 0; i < 3 * MFX_SETTLE; i++) built += ready(&g_mfx_pres, 320, 700 + 8 * i);
        EXPECT("拉窗口每步换尺寸：一套也不建", 0, (uint64_t)built); EXPECT("拉窗口时旧放大器还在原位", 1, (uint64_t)(g_mfx_pres.scaler == ws));
        // ③ 停下来：同一新尺寸要够次数才重建，旧的进退役表（还活着）
        // ③ Stop: the same new size rebuilds after enough requests, and the old set goes to the retire list (still alive)
        for (int i = 0; i < MFX_SETTLE; i++) built += ready(&g_mfx_pres, 320, 1000);
        EXPECT("停下后重建一次", 1, (uint64_t)built); EXPECT("新尺寸的放大器", 1000, (uint64_t)g_mfx_pres.ow);
    }
    EXPECT("换下的旧放大器：重建后仍活着（等在途的帧）", 1, (uint64_t)(ws != nil)); EXPECT("换下的旧输入贴图仍活着", 1, (uint64_t)(wi != nil)); EXPECT("换下的旧输出贴图仍活着", 1, (uint64_t)(wo != nil));
    for (int i = 0; i < 3; i++) @autoreleasepool { mfx_retire_tick(); }
    EXPECT("3 次出图后仍活着", 1, (uint64_t)(ws != nil && wi != nil && wo != nil));
    @autoreleasepool { mfx_retire_tick(); }
    EXPECT("第 4 次出图后放大器释放", 0, (uint64_t)(ws != nil)); EXPECT("第 4 次出图后两张贴图释放", 0, (uint64_t)(wi != nil || wo != nil));
    // ④ 关掉「放大方式」：出图路径每帧见开关关着就放下，同样等 4 次出图
    // ④ "Upscaling" turned off: the present path puts the set down once it sees the switch off, again waiting 4 presents
    @autoreleasepool { ws = g_mfx_pres.scaler; wi = g_mfx_pres.in; EXPECT("单测里开关恒关（没链宿主）", 0, (uint64_t)mfx_on()); mfx_drop(&g_mfx_pres); }
    EXPECT("关掉后表项清空", 0, (uint64_t)(g_mfx_pres.scaler != nil || g_mfx_pres.in != nil || g_mfx_pres.out != nil || g_mfx_pres.ow != 0));
    EXPECT("关掉当帧仍活着", 1, (uint64_t)(ws != nil && wi != nil));
    for (int i = 0; i < 4; i++) @autoreleasepool { mfx_retire_tick(); }
    EXPECT("关掉后 4 次出图释放", 0, (uint64_t)(ws != nil || wi != nil));
    // ⑤ 3D 放大趟那份（编在主命令缓冲里）：换下的走回放的延迟释放，等本次提交完成
    // ⑤ The 3D upscale pass copy (encoded in the main command buffer): what is replaced goes through the replay's deferred release and waits for this submission to complete
    @autoreleasepool {
        for (int i = 0; i < MFX_SETTLE; i++) ready(&g_mfx_3d, 480, 640);
        EXPECT("3D 放大趟：稳定后建好", 1, (uint64_t)(g_mfx_3d.scaler != nil));
        ws = g_mfx_3d.scaler; wi = g_mfx_3d.in;
        for (int i = 0; i < MFX_SETTLE; i++) ready(&g_mfx_3d, 400, 640);   // 动态 3D 比例换了一档 | the dynamic 3D scale moved one step
        EXPECT("3D 放大趟：新尺寸重建", 400, (uint64_t)g_mfx_3d.iw);
        defer_drain();
    }
    EXPECT("3D 放大趟：提交没完成前旧的仍活着", 1, (uint64_t)(ws != nil && wi != nil));
    g_done = g_fence + 1;
    @autoreleasepool { defer_drain(); }
    EXPECT("3D 放大趟：提交完成后旧放大器释放", 0, (uint64_t)(ws != nil));
    @autoreleasepool { res_commit(); }   // 贴图移出驻留集要等下一次提交驻留集才生效（驻留集持有它到那时） | a texture leaving the residency set takes effect at the next residency-set commit (the set holds it until then)
    EXPECT("3D 放大趟：驻留集提交后旧贴图释放", 0, (uint64_t)(wi != nil));
    @autoreleasepool { ws = g_mfx_3d.scaler; mfx3d_sub_begin(); }   // 开关关着：提交开头放下 | switch off: put down at the start of the submission
    EXPECT("3D 放大趟：开关关着，提交开头放下", 0, (uint64_t)(g_mfx_3d.scaler != nil));
    EXPECT("3D 放大趟：放下当时仍活着", 1, (uint64_t)(ws != nil));
    g_done = g_fence + 1;
    @autoreleasepool { defer_drain(); }
    EXPECT("3D 放大趟：放下后提交完成即释放", 0, (uint64_t)(ws != nil));
    // ⑥ 截图那份不等稳定（一次截图只要一次）
    // ⑥ The screenshot copy does not wait for the size to settle (one screenshot makes one request)
    @autoreleasepool { EXPECT("截图：第一次要就建好", 1, (uint64_t)mfx_ready(&g_mfx_grab, 0, MTLPixelFormatRGBA8Unorm, 320, 180, 640, 360)); mfx_drop(&g_mfx_grab); for (int i = 0; i < 4; i++) mfx_retire_tick(); }
    printf("== %d 项，失败 %d ==\n", count, fail); return fail != 0; }
