// 反射转换单测：真实 B 反射 json（Switch 原版翻译，build/cache/metal_shader_cache/<键>_b.json）→ C 数组，期望按 json 原文手抄。
// 哑贴图类型 MTLTextureType：1D=0 1DArray=1 2D=2 2DArray=3 Cube=5 CubeArray=6 3D=7
// Reflection conversion unit tests: real B reflection json (translation of the original Switch shaders, build/cache/metal_shader_cache/<key>_b.json) → C arrays, expectations hand-copied from the json text.
// Dummy texture type MTLTextureType: 1D=0 1DArray=1 2D=2 2DArray=3 Cube=5 CubeArray=6 3D=7
// 阴影比较采样的槽有两种 MSL 写法，取决于 spirv-cross 版本（翻译链在 setup.sh 里核 1.4.357，更新的版本也能用）：
//   1.4.357：声明成 depth2d_array<float> / depthcube<float> → 哑贴图必须是深度格式（像素类 = 深度）；
//   更新版（本机 1.4.363）：声明成 texture2d_array<float> / texturecube<float>，用处经 spvDepthCast 转成深度类型 → 哑贴图按浮点类给 RGBA8 全零。
// 两种都接受、按 json 原文选期望；后一种在没绑贴图时的读值由下面的「深度转换读值」一节在本机 GPU 上实测：比较采样读出 0（与「未绑读 0」一致）。
// A shadow-compare slot has two MSL forms, depending on the spirv-cross version (setup.sh verifies 1.4.357 for the translation chain; newer versions work too):
//   1.4.357: declared as depth2d_array<float> / depthcube<float> -> the dummy texture must have a depth format (pixel class = depth);
//   newer (1.4.363 on this machine): declared as texture2d_array<float> / texturecube<float>, with the uses cast to depth types via spvDepthCast -> the dummy follows the float class, an all-zero RGBA8.
// Both are accepted, the expectation is picked from the json text; for the latter, the value read when no texture is bound is measured on this machine's GPU by the "depth-cast read" section below: compare sampling reads 0 (consistent with "unbound reads 0").
#import "../nvn/nvn_refl.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/dmsh_chain.h"

enum { C = NVN_RB_CBUF, S = NVN_RB_SAMP, T = NVN_RB_TEX, I = NVN_RB_IMG, TB = NVN_RB_TBUF };
enum { F = NVN_DPF_FLOAT, U = NVN_DPF_UINT, D = NVN_DPF_DEPTH };
static int fail, n;
static void check(const char *what, long want, long got) {
    n++; int ok = want == got; fail += !ok;
    if (!ok) printf("  %-40s 期望 %ld → 真结果 %ld ✗\n", what, want, got);
}
static NSDictionary *load(const char *key) {
    NSString *p = [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache/b-%s/%s.json", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", DMSH_CHAIN, key];   // 现行翻译链版本的那一份库 | the library of the current translation chain version
    NSData *d = [NSData dataWithContentsOfFile:p];
    if (!d) { printf("【红】缺反射样本 %s\n", p.UTF8String); fail++; return nil; }
    return [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
}

int main(void) { @autoreleasepool {
    nvn_refl r; char err[160] = "";
    {   int f0 = fail;   // A 路线已退役：PC 链反射（无 route = B）必须被拒 | Route A is retired: PC-chain reflection (no route = B) must be rejected
        NSDictionary *pc = @{@"arg_buffer_index": @0, @"bindings": @[@{@"type": @"cbuffer", @"reg": @10, @"arg_id": @0, @"access": @"constant", @"msl_type": @"constant cb1_struct*"}], @"vertex_inputs": @[]};
        check("PC 链反射（无 route = B）报失败", 1, nvn_refl_from_json(pc, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "负对照：PC 链反射被拒（A 路线已退役）", fail == f0 ? "✓" : "✗");
    }

    // B 路线（Switch 原版翻译，tools/dmsh_build.py 出）：真实反射 ps_models_p#0 / vs_models_v#0，期望按 json 原文手抄
    // Route B (translation of the original Switch shaders, produced by tools/dmsh_build.py): real reflection ps_models_p#0 / vs_models_v#0, expectations hand-copied from the json text
    {   NSDictionary *j = load("5c1ca4e7c3e4657c_b");
        if (j) { int f1 = fail;
            check("B 片元：转换成功", 0, nvn_refl_from_json(j, &r, err, sizeof err));
            check("B 片元：B 标记", 1, r.route_b); check("B 片元：参数缓冲槽", 0, r.abi); check("B 片元：绑定数", 44, r.nb); check("B 片元：顶点输入数", 0, r.nv);
            check("B 片元：有 c1", 1, r.has_c1); check("B 片元：c1 长 256（控制段字段）", 256, r.c1_len);
            uint32_t c1w; memcpy(&c1w, r.c1, 4); check("B 片元：c1 首字 = −√1.5（0xbf9cc471）", 0xbf9cc471u, c1w);
            NSString *t28 = j[@"bindings"][28][@"msl_type"], *t34 = j[@"bindings"][34][@"msl_type"];
            check("B 绑定[28] 是两种已知写法之一", 1, [t28 isEqual:@"depth2d_array<float>"] || [t28 isEqual:@"texture2d_array<float>"]);
            check("B 绑定[34] 是两种已知写法之一", 1, [t34 isEqual:@"depthcube<float>"] || [t34 isEqual:@"texturecube<float>"]);
            int d28 = [t28 hasPrefix:@"depth"] ? D : F, d34 = [t34 hasPrefix:@"depth"] ? D : F;
            struct { int i, kind, reg, aid, rw, dtt, dpf; } w[] = {
                {0, NVN_RB_SUPPORT, 0, 0, 0, -1, -1}, {1, C, 10, 1, 0, -1, -1}, {4, NVN_RB_C1, 0, 4, 0, -1, -1}, {7, NVN_RB_DRV0, 0, 7, 0, -1, -1},
                {8, NVN_RB_DRV2, 0, 8, 0, -1, -1}, {11, NVN_RB_SBUFP, 4, 11, 0, -1, -1}, {13, TB, 8, 13, 0, -1, -1}, {14, T, 41, 14, 0, 2, F},
                {15, S, 3, 15, 0, -1, -1}, {28, T, 39, 28, 0, 3, d28}, {34, T, 33, 34, 0, 5, d34}, {36, T, 46, 36, 0, 7, F}};
            for (unsigned k = 0; k < sizeof w / sizeof *w; k++) {
                const nvn_rbind *g = &r.b[w[k].i]; char m[64];
                snprintf(m, sizeof m, "B 绑定[%d] 种类", w[k].i); check(m, w[k].kind, g->kind);
                snprintf(m, sizeof m, "B 绑定[%d] 槽号", w[k].i); check(m, w[k].reg, g->reg);
                snprintf(m, sizeof m, "B 绑定[%d] 参数序号", w[k].i); check(m, w[k].aid, g->aid);
                snprintf(m, sizeof m, "B 绑定[%d] 读写", w[k].i); check(m, w[k].rw, g->rw);
                if (w[k].dtt >= 0) { snprintf(m, sizeof m, "B 绑定[%d] 哑贴图类型", w[k].i); check(m, w[k].dtt, g->dtt);
                                     snprintf(m, sizeof m, "B 绑定[%d] 哑贴图像素类", w[k].i); check(m, w[k].dpf, g->dpf); }
            }
            printf("%-60s %s\n", "B 片元 ps_models_p#0：44 个绑定（含 support / c1 / 驱动段 / 存储缓冲指针）", fail == f1 ? "✓" : "✗");
        } }
    {   NSDictionary *j = load("54f730a6c0efc118_b");
        if (j) { int f1 = fail;
            check("B 顶点：转换成功", 0, nvn_refl_from_json(j, &r, err, sizeof err));
            check("B 顶点：绑定数", 2, r.nb); check("B 顶点：输入数", 4, r.nv); check("B 顶点：c1 长 0（控制段 +0x704 − +0x700）→ 无 c1", 0, r.has_c1);
            for (int i = 0; i < 4 && i < r.nv; i++) { char m[48]; snprintf(m, sizeof m, "B 顶点输入[%d] 属性号 = 位置", i); check(m, i, r.v[i].nvn); check(m, i, r.v[i].loc); check("B 顶点分量 float", NVN_CT_FLOAT, r.v[i].ct); }
            check("B 顶点：绑定[0] uniform 槽 0", 0, r.b[0].reg); check("B 顶点：绑定[1] uniform 槽 3", 3, r.b[1].reg);
            printf("%-60s %s\n", "B 顶点 vs_models_v#0：4 个属性（属性号 = MSL attribute）", fail == f1 ? "✓" : "✗");
        } }
    {   int f1 = fail;
        NSDictionary *b1 = @{@"route": @"B", @"bindings": @[@{@"kind": @"foo", @"arg_id": @0, @"msl_name": @"x", @"msl_type": @"constant _16*"}]};
        NSDictionary *b2 = @{@"route": @"B", @"bindings": @[@{@"kind": @"tex", @"reg": [NSNull null], @"arg_id": @0, @"msl_name": @"fp_t_x", @"msl_type": @"texture2d<float>"}]};
        NSDictionary *b3 = @{@"route": @"B", @"bindings": @[@{@"kind": @"cbuf", @"reg": @0, @"arg_id": @(NVN_REFL_MAXB), @"msl_name": @"fp_c3", @"msl_type": @"constant _16*"}]};
        NSDictionary *b4 = @{@"route": @"B", @"bindings": @[@{@"kind": @"cbuf", @"reg": @0, @"arg_id": @(-1), @"msl_name": @"fp_c3", @"msl_type": @"constant _16*"}]};
        check("B 未知种类报失败", 1, nvn_refl_from_json(b1, &r, err, sizeof err) != 0);
        check("B 贴图没解出槽号报失败", 1, nvn_refl_from_json(b2, &r, err, sizeof err) != 0);
        check("B 参数序号 = 上限报失败（回放按序号写定长数组）", 1, nvn_refl_from_json(b3, &r, err, sizeof err) != 0);
        check("B 参数序号为负报失败", 1, nvn_refl_from_json(b4, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "B 负对照：未知种类 / 槽号缺失 / 参数序号越界", fail == f1 ? "✓" : "✗");
    }
    {   // 深度转换读值：没绑贴图的阴影槽（更新版写法）给的是 RGBA8 全零哑贴图（nvn_mtl.m dummy_tex），着色器经 spvDepthCast 做比较采样。
        // 期望：8 种比较函数、两个参考值，二维数组与立方体都读出 0。对照：同样全零的 Depth32Float 会按比较结果读出 1（Always 时）
        // Depth-cast read: an unbound shadow slot (newer form) gets an all-zero RGBA8 dummy (nvn_mtl.m dummy_tex), and the shader compare-samples it through spvDepthCast.
        // Expected: with all 8 compare functions and two reference values, both the 2D array and the cube read 0. Control: the same all-zero Depth32Float reads 1 by the comparison (with Always)
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); int f1 = fail;
        if (!dev) printf("%-60s 跳过（没有 Metal 设备）\n", "深度转换读值");
        else {
            NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "template<typename T> static inline depth2d_array<T> spvDepthCast(texture2d_array<T> t) { return reinterpret_cast<thread const depth2d_array<T> &>(t); }\n"
                "template<typename T> static inline depthcube<T> spvDepthCast(texturecube<T> t) { return reinterpret_cast<thread const depthcube<T> &>(t); }\n"
                "struct A { texture2d_array<float> t [[id(0)]]; sampler s [[id(1)]]; texturecube<float> c [[id(2)]]; };\n"
                "kernel void k(constant A &a [[buffer(0)]], device float *o [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
                "  float r = (i & 1) ? -0.5 : 0.5;\n"
                "  o[i] = i < 2 ? spvDepthCast(a.t).sample_compare(a.s, float2(0.5), 0, r) : spvDepthCast(a.c).sample_compare(a.s, float3(0.3, 0.5, 0.8), r); }\n";
            NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e]; id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
            id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil; id<MTLCommandQueue> q = [dev newCommandQueue];
            check("深度转换读值：编译内核", 1, ps != nil);
            for (int depth = 0; ps && depth < 2; depth++) {
                MTLPixelFormat pf = depth ? MTLPixelFormatDepth32Float : MTLPixelFormatRGBA8Unorm; id<MTLTexture> tx[2];
                for (int c = 0; c < 2; c++) { MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = c ? MTLTextureTypeCube : MTLTextureType2DArray; d.pixelFormat = pf;
                    d.width = d.height = 1; d.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead | (depth ? MTLTextureUsageRenderTarget : 0); tx[c] = [dev newTextureWithDescriptor:d];
                    if (!depth) { uint8_t z[16] = {0}; for (int f = 0; f < (c ? 6 : 1); f++) [tx[c] replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; } }
                if (depth) { id<MTLCommandBuffer> cb = [q commandBuffer];   // 深度对照：清成 0 | Depth control: cleared to 0
                    for (int c = 0; c < 2; c++) for (int f = 0; f < (c ? 6 : 1); f++) { MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                        rp.depthAttachment.texture = tx[c]; rp.depthAttachment.slice = f; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0; rp.depthAttachment.storeAction = MTLStoreActionStore;
                        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; }
                    [cb commit]; [cb waitUntilCompleted]; }
                int hits = 0;
                for (int cf = MTLCompareFunctionNever; cf <= MTLCompareFunctionAlways; cf++) {
                    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES; sd.compareFunction = (MTLCompareFunction)cf; id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
                    id<MTLArgumentEncoder> ae = [fn newArgumentEncoderWithBufferIndex:0]; id<MTLBuffer> ab = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
                    [ae setArgumentBuffer:ab offset:0]; [ae setTexture:tx[0] atIndex:0]; [ae setSamplerState:ss atIndex:1]; [ae setTexture:tx[1] atIndex:2];
                    id<MTLBuffer> ob = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(ob.contents, 0x7f, 16);
                    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
                    [en setComputePipelineState:ps]; [en setBuffer:ab offset:0 atIndex:0]; [en setBuffer:ob offset:0 atIndex:1];
                    [en useResource:tx[0] usage:MTLResourceUsageRead]; [en useResource:tx[1] usage:MTLResourceUsageRead];
                    [en dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
                    const float *o = ob.contents; for (int i = 0; i < 4; i++) hits += o[i] != 0;
                }
                if (depth) check("深度转换读值：对照 Depth32Float 全零在 Always 等比较下读出非 0", 1, hits > 0);
                else check("深度转换读值：RGBA8 全零哑贴图在 8 种比较、2 个参考值下全读 0", 0, hits);
            }
            printf("%-60s %s\n", "深度转换读值：没绑贴图的阴影槽（spvDepthCast）读出 0", fail == f1 ? "✓" : "✗");
        }
    }
    printf("== %d 项，失败 %d ==\n", n, fail);
    return fail != 0;
} }
