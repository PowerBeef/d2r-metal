// 手柄屏幕键盘的前端部分（见 dm_vkbd_ui.h；Mac App 与 iOS 共用，只用 QuartzCore / CoreText，不分 AppKit / UIKit）。
// 用手柄玩时游戏要文字（起名、金币数量、编年史搜索），front_text_input 改走这里：游戏画面下部画一块游戏风格的键盘（深色底、金边金字、原版字体），
// 手柄归它（dm_gamepad_capture）；玩家改用键盘鼠标 / 触屏（前端看到按键、点击、触摸）就收起、把已打的字交给前端的输入框（dm_vkbd_ui_handoff）。游戏那边照旧冻着客体时钟等结果（hle_swkbd.c）。
// 画法：每次有变化用 CoreGraphics 把整块面板画成一张图，放进加在游戏画面上面的 CALayer（不经游戏的出图，所以 DM_TEST_PRESENT_SHOT 要另外叠，见 dm_vkbd_ui_snapshot）。
// 字：原版的 Exocet（标题）与 Formal436（正文），缺的字找 BlizzardGlobal；运行时从资源包读出来、只建字体描述不注册（不进仓库、不进 App 包；秘境任务栏 dm_rift_hud.m 也用这里的 dm_game_font）；都没有的字（⌫ ⇧）CoreText 自动换系统字体。
// Front-end part of the on-screen controller keyboard (see dm_vkbd_ui.h; shared by the Mac App and iOS, using only QuartzCore / CoreText, no AppKit / UIKit).
// When the game asks for text (naming, gold quantity, Chronicle search) while it is played with a controller, front_text_input comes here instead: a game-style keyboard (dark panel, gold frame and lettering, the original font) is drawn over the lower part of the picture,
// the controllers belong to it (dm_gamepad_capture); when the player turns to keyboard and mouse / touch (the front end sees a key press, click or touch) it closes and hands what was typed to the front end's input box (dm_vkbd_ui_handoff). The game keeps its guest clock frozen and waits for the result as before (hle_swkbd.c).
// Drawing: on every change the whole panel is drawn into one image with CoreGraphics and put into a CALayer above the game picture (it does not pass through the game's presentation, so DM_TEST_PRESENT_SHOT composites it separately, see dm_vkbd_ui_snapshot).
// Fonts: the original game's Exocet (headings) and Formal436 (body), with BlizzardGlobal for characters they lack; read from the asset pack at run time into font descriptors, not registered (not in the repository, not in the App bundle; the rift quest bar dm_rift_hud.m uses dm_game_font from here too); characters none of them has (⌫ ⇧) fall back to a system font in CoreText automatically.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include "dm_vkbd_ui.h"
#include "dm_vkbd.h"
#include "dm_input.h"
#include "dm_i18n.h"
#include "hle.h"

void dm_gamepad_capture(int on); void dm_gamepad_captured(uint64_t *b, int32_t *lx, int32_t *ly); int dm_gamepad_mode(void);   // dm_gamepad.m
int dm_padmode_glyph_want(void);   // dm_padmode.c
int dm_name_taken(const char *name); int dm_name_gen(char *out, int cap, int (*taken)(const char *));   // dm_actions.h（宿主） | dm_actions.h (host)
extern _Atomic int g_fe_name_no_accept;   // dm_cursor.c：这次返回的是替玩家抽的随机名，只填进名字框不自动接受 | dm_cursor.c: this result is a random name drawn for the player; it only fills the name box and is not auto-accepted
extern int g_swkbd_name_panel, g_swkbd_numeric_max;   // hle_swkbd.c

// ---- 原版字体（资源包读法同 dm/ios/tools/touch_layout.py read_layout） | the original font (asset pack read the same way as dm/ios/tools/touch_layout.py read_layout) ----
// data/build_manifest.psv 按路径后缀找行 → 第 7 列编码键（第 10、11 字节 = 数据档号，12–16 = 偏移）、第 8 列原始长度、第 9 列编码（n 原样 / z 整块 zlib / 其余 BLTE 分块：每块首字节 N 原样、Z = zlib）
// data/build_manifest.psv: find the row by path suffix → column 7 is the encoding key (bytes 10 and 11 = data archive number, 12–16 = offset), column 8 the raw length, column 9 the encoding (n as-is / z whole-block zlib / otherwise BLTE chunks: first byte of each chunk N as-is, Z = zlib)
static NSData *zinflate(const uint8_t *p, size_t n) {   // zlib（带 2 字节头）→ 原文；Foundation 的 Zlib 算法是裸 deflate，跳过头 | zlib (with 2-byte header) → plaintext; Foundation's Zlib algorithm is raw deflate, so skip the header
    if (n < 2) return nil; return [[NSData dataWithBytes:p + 2 length:n - 2] decompressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:nil];
}
// 清单约 30 MB、13 万行。映射进来按字节扫一遍，只看路径列的后缀，要找的几行都找到就停（字体在约三分之一处），不给每行建对象。
// 原先每找一个字体就把整份清单读成字符串、拆成 13 万个行对象再逐行拆列，三个字体三遍：秘境任务栏第一帧在主线程上约 70–100 MB 临时内存，没有自动释放池
// The manifest is about 30 MB and 132k rows. It is mapped and scanned once byte by byte, looking only at the suffix of the path column, and the scan stops once every wanted row is found (the fonts are about a third of the way in); no object is created per row.
// It used to read the whole manifest into a string for every font, split it into 132k row objects and split each row into columns, three times for three fonts: about 70-100 MB of transient memory on the main thread at the first rift quest bar frame, with no autorelease pool
typedef struct { const char *suffix; uint8_t ek[16]; long len; char enc[8]; int ok; } romfs_row;   // ok：0 没找到、1 找到且能用、-1 找到但编码键不对 | ok: 0 not found, 1 found and usable, -1 found but the encoding key is malformed
static int hex2(const char *p) {   // 两位十六进制 → 0–255，不是十六进制返回 -1 | two hex digits -> 0-255, -1 if not hex
    int v = 0; for (int i = 0; i < 2; i++) { char c = p[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return -1; v = v << 4 | d; }
    return v;
}
static void manifest_find(NSString *data, romfs_row *row, int n) {
    NSData *m = [NSData dataWithContentsOfFile:[data stringByAppendingPathComponent:@"build_manifest.psv"] options:NSDataReadingMappedIfSafe error:nil];
    const char *p = m.bytes, *end = p + m.length; int left = n;
    while (p && p < end && left) {
        const char *e = memchr(p, '\n', (size_t)(end - p)); if (!e) e = end;
        const char *f[12]; int nf = 1; f[0] = p;
        if (*p != '#') for (const char *q = p; q < e && nf < 12; q++) if (*q == '|') f[nf++] = q + 1;
        if (nf >= 11) {   // 第 10 列路径 = f[9] 到 f[10] 前那条竖线 | column 10, the path = from f[9] to the bar before f[10]
            size_t plen = (size_t)(f[10] - 1 - f[9]);
            for (int i = 0; i < n; i++) { size_t sl = strlen(row[i].suffix);
                if (row[i].ok || plen < sl || strncasecmp(f[9] + plen - sl, row[i].suffix, sl)) continue;
                left--; row[i].ok = -1;
                if (f[7] - 1 - f[6] < 32) break;   // 编码键不足 32 位十六进制 | encoding key shorter than 32 hex digits
                int bad = 0; for (int k = 0; k < 16; k++) { int v = hex2(f[6] + 2 * k); if (v < 0) bad = 1; row[i].ek[k] = (uint8_t)v; }
                if (bad) break;
                row[i].len = strtol(f[7], NULL, 10);
                size_t el = (size_t)(f[9] - 1 - f[8]); if (el >= sizeof row[i].enc) el = sizeof row[i].enc - 1; memcpy(row[i].enc, f[8], el); row[i].enc[el] = 0;
                row[i].ok = 1; break; }
        }
        p = e + 1;
    }
}
static NSData *romfs_read(NSString *data, const romfs_row *rw) {
    if (rw->ok != 1) return nil;
    const uint8_t *k = rw->ek; uint64_t off = 0; for (int i = 11; i < 16; i++) off = off << 8 | k[i]; long rawlen = rw->len;
    NSFileHandle *h = [NSFileHandle fileHandleForReadingAtPath:[data stringByAppendingPathComponent:[NSString stringWithFormat:@"%02d-%08d.data", k[9], k[10]]]]; if (!h) return nil;
    [h seekToFileOffset:off]; NSData *raw = [h readDataOfLength:rawlen]; [h closeFile]; const uint8_t *r = raw.bytes; if ((long)raw.length != rawlen) return nil;
    if (!strcmp(rw->enc, "n")) return raw;
    if (!strcmp(rw->enc, "z")) return zinflate(r, raw.length);
    uint32_t hs = (uint32_t)r[4] << 24 | r[5] << 16 | r[6] << 8 | r[7], nch = (uint32_t)r[9] << 16 | r[10] << 8 | r[11], pos = hs; NSMutableData *o = [NSMutableData data];
    for (uint32_t i = 0; i < nch; i++) { const uint8_t *e = r + 12 + i * 24; uint32_t es = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3];
        if (pos + es > raw.length) return nil; const uint8_t *b = r + pos;
        if (b[0] == 'N') [o appendBytes:b + 1 length:es - 1]; else { NSData *d = zinflate(b + 1, es - 1); if (!d) return nil; [o appendData:d]; } pos += es; }
    return o;
}
// 原版的三种字：0 BlizzardGlobal（中日韩等全字符集，秘境任务栏用）、1 Exocet（标题）、2 Formal436（正文）。1、2 缺的字（汉字、⌫ 等）先找 BlizzardGlobal，再由 CoreText 换系统字体。
// 只建字体描述、不在系统里注册（注册的接口 macOS 15 起已废弃）。整个进程只读一次（dispatch_once），任意线程可调；dm_game_font_preload 在后台线程上先读好，
// 主线程用 dm_game_font_ready 判断读好了没有、没好就先用系统字，不在主线程上等清单与解压
// The original game's three fonts: 0 BlizzardGlobal (full CJK etc. character set, used by the rift quest bar), 1 Exocet (headings), 2 Formal436 (body text). Characters 1 and 2 lack (Chinese, ⌫ etc.) go to BlizzardGlobal first, then CoreText falls back to a system font.
// Only font descriptors are created; nothing is registered with the system (the registration API is deprecated since macOS 15). Read once per process (dispatch_once), callable from any thread; dm_game_font_preload reads them ahead on a background thread,
// and the main thread checks dm_game_font_ready and uses a system font until they are ready, instead of waiting for the manifest scan and decompression on the main thread
static CTFontDescriptorRef g_font_desc[3]; static dispatch_once_t g_font_once; static _Atomic int g_font_ready;
static void fonts_load(void) {
    @autoreleasepool {
        static const char *const file[3] = {"blizzardglobal-v5_81.ttf", "exocetblizzardot-medium.otf", "formal436bt.ttf"};
        char suf[3][64]; romfs_row row[3]; memset(row, 0, sizeof row);
        for (int i = 0; i < 3; i++) { snprintf(suf[i], sizeof suf[i], "/hd/ui/fonts/%s", file[i]); row[i].suffix = suf[i]; }
        const char *root = getenv("DM_ROMFS"); NSString *data = root ? [@(root) stringByAppendingPathComponent:@"data"] : nil;
        if (data) manifest_find(data, row, 3);
        for (int i = 0; i < 3; i++) { NSData *d = data ? romfs_read(data, &row[i]) : nil; g_font_desc[i] = d ? CTFontManagerCreateFontDescriptorFromData((__bridge CFDataRef)d) : NULL;
            if (i && g_font_desc[i] && g_font_desc[0]) { CTFontDescriptorRef c = CTFontDescriptorCreateCopyWithAttributes(g_font_desc[i], (__bridge CFDictionaryRef)@{(id)kCTFontCascadeListAttribute: @[(__bridge id)g_font_desc[0]]}); CFRelease(g_font_desc[i]); g_font_desc[i] = c; } }
        LOG("原版字体：BlizzardGlobal %s、Exocet %s、Formal436 %s", g_font_desc[0] ? "有" : "缺", g_font_desc[1] ? "有" : "缺", g_font_desc[2] ? "有" : "缺");
    }
    atomic_store(&g_font_ready, 1);
}
static CTFontRef game_font(int which, double size) {
    dispatch_once(&g_font_once, ^{ fonts_load(); });
    return g_font_desc[which] ? CTFontCreateWithFontDescriptor(g_font_desc[which], size, NULL) : NULL;
}
CTFontRef dm_game_font(double size) { return game_font(0, size); }
int dm_game_font_ready(void) { return atomic_load(&g_font_ready); }
void dm_game_font_preload(void) {   // 后台线程上读好字体（只发一次）；读的时候主线程若来要字，dispatch_once 让它等这一次读完 | read the fonts on a background thread (sent once); if the main thread asks for a font meanwhile, dispatch_once makes it wait for this read to finish
    static _Atomic int asked; if (atomic_exchange(&asked, 1)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ dispatch_once(&g_font_once, ^{ fonts_load(); }); });
}

// ---- 状态（主线程） | state (main thread) ----
static dm_vkbd g_k;
static CALayer *g_kl;
static NSTimer *g_tick;
static void (*g_where)(dm_vkbd_host *);
static dm_vkbd_host g_h;
static dm_vkbd_geo g_geo;
static CGRect g_in_pic;            // 面板在游戏画面里的位置（点，原点左上） | the panel's rectangle in the game picture (points, origin at top left)
static CGImageRef g_img;           // 最近画的一张（测试截图叠用） | the latest drawing (for test screenshots)
static NSString *g_title;
static int g_glyph = -1;
static void (^g_finish)(int result, const char *text);   // result：1 确定 / 0 取消 / 2 交给前端的输入框 | result: 1 OK / 0 cancel / 2 handed to the front end's input box
static int g_handoff;

void dm_vkbd_ui_set_host(void (*where)(dm_vkbd_host *h)) { g_where = where; }
int dm_vkbd_ui_active(void) { return g_kl != nil; }
int dm_vkbd_wanted(void) {
    const char *e = getenv("DM_VKBD"); if (e && *e) return atoi(e) != 0;
    return dm_gamepad_mode() && dm_padmode_glyph_want() != DM_GLYPH_KBD;   // iOS：手柄模式；Mac App：手柄有过输入且之后没再用键盘（键帽跟着最近的设备） | iOS: controller mode; Mac App: the controller gave input and the keyboard was not used since (the key caps follow the latest device)
}

// ---- 画 | drawing ----
static CGColorRef rgb(int r, int g, int b, double a) {   // 缓存：同一组值只建一次（几十种，常驻） | cached: each value set is created once (a few dozen, kept)
    static struct { uint32_t k; CGColorRef c; } cache[64]; static int n; uint32_t key = (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | (uint32_t)(a * 255);
    for (int i = 0; i < n; i++) if (cache[i].k == key) return cache[i].c;
    CGColorRef c = CGColorCreateSRGB(r / 255.0, g / 255.0, b / 255.0, a); if (n < 64) { cache[n].k = key; cache[n].c = c; n++; } return c;
}
#define GOLD rgb(199, 179, 119, 1)   // 暗黑 2 金色字 | Diablo II gold text
static int g_font_kind = 2;   // 下面 draw_line 用哪种字（1 标题 / 2 正文） | which font draw_line below uses (1 heading / 2 body)
static CTFontRef font(CGFloat size) {
    CTFontRef f = game_font(g_font_kind, size); if (!f) f = game_font(0, size); return f ? f : CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, NULL);
}
// 一行字（几段不同颜色：ss[i] 用 cs[i]）画进 r：放不下就按比例缩小字号；align 0 左 1 中 | one line of text (segments in different colors: ss[i] in cs[i]) drawn into r: shrunk proportionally when it does not fit; align 0 left 1 center
static void draw_line(CGContextRef c, NSArray<NSString *> *ss, const CGColorRef *cs, CGFloat size, CGRect r, int align) {
    for (int pass = 0; pass < 2; pass++) {
        CTFontRef f = font(size); NSMutableAttributedString *as = [NSMutableAttributedString new];
        for (NSUInteger i = 0; i < ss.count; i++) if (ss[i].length) [as appendAttributedString:[[NSAttributedString alloc] initWithString:ss[i] attributes:@{(id)kCTFontAttributeName: (__bridge id)f, (id)kCTForegroundColorAttributeName: (__bridge id)cs[i]}]];
        CFRelease(f);
        CTLineRef ln = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
        CGFloat asc = 0, desc = 0; double w = CTLineGetTypographicBounds(ln, &asc, &desc, NULL);
        if (pass == 0 && w > r.size.width && w > 0) { size *= r.size.width / w; CFRelease(ln); continue; }
        CGContextSaveGState(c); CGContextSetShadowWithColor(c, CGSizeMake(g_h.scale, -g_h.scale), 0, rgb(0, 0, 0, 0.85));
        CGContextSetTextPosition(c, align ? r.origin.x + (r.size.width - w) / 2 : r.origin.x, r.origin.y + (r.size.height - (asc + desc)) / 2 + asc);
        CTLineDraw(ln, c); CGContextRestoreGState(c); CFRelease(ln); return;
    }
}
static void box(CGContextRef c, CGRect r, CGFloat rad, CGColorRef fill, CGColorRef line, CGFloat lw) {
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectInset(r, lw / 2, lw / 2), rad, rad, NULL);
    CGContextAddPath(c, p); CGContextSetFillColorWithColor(c, fill); CGContextFillPath(c);
    if (line && lw > 0) { CGContextAddPath(c, p); CGContextSetStrokeColorWithColor(c, line); CGContextSetLineWidth(c, lw); CGContextStrokePath(c); }
    CGPathRelease(p);
}
static NSString *U(const char *key) { return @(dm_ui(key)); }
static NSString *key_label(int i) {
    const dm_vkey *v = &g_k.keys[i];
    switch (v->act) {
        case DM_VK_CHAR: { char ch = v->ch; if (ch >= 'a' && ch <= 'z' && g_k.shift) ch = (char)(ch - 'a' + 'A'); return [NSString stringWithFormat:@"%c", ch]; }
        case DM_VK_BACK: return @"⌫";
        case DM_VK_SHIFT: return g_k.shift == 2 ? @"⇪" : @"⇧";
        case DM_VK_SPACE: return U("dmKbdSpace");
        case DM_VK_OK: return U("dmConfirm");
        case DM_VK_CANCEL: return U("dmCancel");
        case DM_VK_MAX: return U("dmKbdMax");
        case DM_VK_HALF: return U("dmKbdHalf");
    }
    return @"";
}
// 提示条的键名：跟着最近用的手柄（Switch 字母 / Xbox 按位置 / PS 符号，键位见 dm_padmap.h）；键盘键帽时写实体键 | button names in the legend: follow the controller used last (Switch letters / Xbox by position / PS symbols, mapping in dm_padmap.h); with keyboard key caps, the physical keys
static NSString *btn(uint64_t b) {
    static NSString *const pad[] = {@"A", @"B", @"X", @"Y", @"+", @"−"}, *const xbox[] = {@"A", @"B", @"Y", @"X", @"☰", @"⧉"}, *const ps[] = {@"✕", @"○", @"□", @"△", @"OPTIONS", @"CREATE"};
    int i = b == DM_BTN_A ? 0 : b == DM_BTN_B ? 1 : b == DM_BTN_X ? 2 : b == DM_BTN_Y ? 3 : b == DM_BTN_PLUS ? 4 : 5;
    return (g_glyph == DM_GLYPH_XBOX ? xbox : g_glyph == DM_GLYPH_PS ? ps : pad)[i];
}
static void draw_legend(CGContextRef c, CGRect r, CGFloat size) {
    NSMutableArray<NSString *> *s = [NSMutableArray array]; CGColorRef cs[24], gr = rgb(176, 166, 140, 1); int n = 0;
#define ITEM(k, what) do { if (n) { [s addObject:@"     "]; cs[n++] = gr; } [s addObject:k]; cs[n++] = GOLD; [s addObject:[@" " stringByAppendingString:U(what)]]; cs[n++] = gr; } while (0)
    if (g_glyph == DM_GLYPH_KBD) { ITEM(@"⏎", "dmConfirm"); ITEM(@"⎋", "dmCancel"); ITEM(@"⌫", "dmKbdDelete"); }
    else {
        ITEM(btn(DM_BTN_A), "dmKbdType"); ITEM(btn(DM_BTN_B), "dmKbdDelete");
        if (g_k.kind != DM_VKBD_NUM) ITEM(btn(DM_BTN_X), "dmKbdShift");
        if (g_k.kind == DM_VKBD_TEXT) ITEM(btn(DM_BTN_Y), "dmKbdSpace");
        if (g_k.kind == DM_VKBD_NUM) ITEM(btn(DM_BTN_Y), "dmKbdMax");
        ITEM(btn(DM_BTN_PLUS), "dmConfirm"); ITEM(btn(DM_BTN_MINUS), "dmCancel");
    }
#undef ITEM
    draw_line(c, s, cs, size, r, 1);
}
static void redraw(void) {
    if (!g_kl) return;
    CGFloat sc = g_h.scale > 0 ? g_h.scale : 2, W = g_geo.w, H = g_geo.h;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, (size_t)ceil(W * sc), (size_t)ceil(H * sc), 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs); if (!c) return;
    CGContextScaleCTM(c, sc, sc); CGContextTranslateCTM(c, 0, H); CGContextScaleCTM(c, 1, -1);   // 之后都按原点左上画 | from here on, draw with the origin at top left
    CGContextSetTextMatrix(c, CGAffineTransformMakeScale(1, -1));
    const dm_vkbd_geo *g = &g_geo; CGFloat pad = g->pad, rad = g->key_h * 0.08f;
    box(c, CGRectMake(0, 0, W, H), rad * 1.5, rgb(14, 12, 10, 1), GOLD, 1.5);   // 不透明：底下游戏页的提示字不透上来 | opaque: the game page's prompt text underneath does not show through
    box(c, CGRectInset(CGRectMake(0, 0, W, H), 4, 4), rad, rgb(14, 12, 10, 0), rgb(66, 57, 40, 1), 1);
    g_font_kind = 1; draw_line(c, @[g_title], (CGColorRef[]){GOLD}, g->title_h * 0.9f, CGRectMake(pad, g->title_y, W - 2 * pad, g->title_h), 1); g_font_kind = 2;
    CGRect fr = CGRectMake(pad, g->field_y, W - 2 * pad, g->field_h);
    box(c, fr, rad, rgb(0, 0, 0, 0.7), rgb(112, 98, 70, 1), 1);
    NSString *txt = @(g_k.text) ?: @"";
    CGRect tr = CGRectInset(fr, g->field_h * 0.3f, 0);
    if (!g_k.len && g_k.kind == DM_VKBD_NAME) draw_line(c, @[@"_ ", U("dmNameBlankRandom")], (CGColorRef[]){GOLD, rgb(128, 116, 92, 1)}, g->field_h * 0.55f, tr, 0);
    else draw_line(c, @[txt, @"_"], (CGColorRef[]){rgb(242, 234, 212, 1), GOLD}, g->field_h * 0.6f, tr, 0);
    NSString *hint = nil; CGColorRef hc = rgb(150, 140, 115, 1);
    if (g_k.hint) { hc = rgb(224, 92, 70, 1);
        hint = g_k.hint == DM_VKBD_HINT_FULL ? U("dmKbdFull") : g_k.hint == DM_VKBD_HINT_SHORT ? U("dmNameTooShort") : g_k.hint == DM_VKBD_HINT_EDGE ? U("dmNameEdge") : U("dmNameSep"); }
    else if (g_k.kind == DM_VKBD_NAME) hint = U("dmNameRule");
    else if (g_k.kind == DM_VKBD_NUM) hint = [NSString stringWithFormat:@"0 – %d", g_k.num_max];
    if (hint) draw_line(c, @[hint], &hc, g->hint_h * 0.85f, CGRectMake(pad, g->hint_y, W - 2 * pad, g->hint_h), 1);
    for (int i = 0; i < g_k.nkeys; i++) {
        float kr[4]; dm_vkbd_key_rect(&g_k, g, i, kr); CGRect r = CGRectMake(kr[0], kr[1], kr[2], kr[3]);
        int foc = i == g_k.focus, dim = dm_vkbd_key_dim(&g_k, i), act = g_k.keys[i].act;
        int lit = act == DM_VK_SHIFT && g_k.shift;   // ⇧ 开着时金色 | ⇧ is gold while on
        box(c, r, rad, foc ? rgb(100, 77, 36, 1) : rgb(36, 31, 26, 1), foc ? rgb(236, 206, 128, 1) : rgb(80, 70, 52, 1), foc ? 2 : 1);
        CGColorRef lc = dim ? rgb(104, 96, 80, 1) : foc ? rgb(255, 250, 236, 1) : lit ? GOLD : rgb(224, 214, 186, 1);
        draw_line(c, @[key_label(i)], &lc, g->key_h * (act == DM_VK_CHAR || act == DM_VK_BACK || act == DM_VK_SHIFT ? 0.5f : 0.4f), CGRectInset(r, g->key_h * 0.1f, 0), 1);
    }
    draw_legend(c, CGRectMake(pad, g->legend_y, W - 2 * pad, g->legend_h), g->legend_h * 0.85f);
    CGImageRef img = CGBitmapContextCreateImage(c); CGContextRelease(c);
    [CATransaction begin]; [CATransaction setDisableActions:YES]; g_kl.contents = (__bridge id)img; [CATransaction commit];
    if (g_img) CGImageRelease(g_img); g_img = img;
}
static void place(void) {
    if (g_where) g_where(&g_h);
    CGRect p = g_h.pic; float w = g_k.kind == DM_VKBD_NUM ? (float)fmin(p.size.width * 0.3, p.size.height * 0.46) : (float)fmin(p.size.width * 0.46, p.size.height * 0.8);
    dm_vkbd_geom(&g_k, w, &g_geo);
    float maxh = (float)p.size.height * 0.9f; if (g_geo.h > maxh) dm_vkbd_geom(&g_k, w * maxh / g_geo.h, &g_geo);   // 矮画面（手机横屏）按高收 | a short picture (phone landscape) is limited by height
    float x0 = ((float)p.size.width - g_geo.w) / 2, y0 = (float)p.size.height * 0.975f - g_geo.h;   // 贴画面下部，留 2.5% 边（起名页的名字框在画面中部，不盖住它） | along the bottom of the picture with a 2.5% margin (the naming page's name box is mid-picture and stays uncovered)
    g_in_pic = CGRectMake(x0, y0, g_geo.w, g_geo.h);
    CGRect f = CGRectMake(p.origin.x + x0, g_h.y_up ? p.origin.y + p.size.height - y0 - g_geo.h : p.origin.y + y0, g_geo.w, g_geo.h);
    [CATransaction begin]; [CATransaction setDisableActions:YES]; g_kl.frame = f; g_kl.contentsScale = g_h.scale > 0 ? g_h.scale : 2; [CATransaction commit];
}

// ---- 开 / 关 / 输入 | open / close / input ----
int dm_name_blank_random(char *out, int cap) {
    char nm[32];
    // 日志句式同宿主 dm_actions.c，uireg two_names 按它判 | log wording is the same as the host's dm_actions.c; uireg two_names judges by it
    if (dm_name_gen(nm, sizeof nm, dm_name_taken)) { snprintf(out, (size_t)cap, "%s", nm); atomic_store(&g_fe_name_no_accept, 1); LOG("起名：随机名 → 名字框「%s」（名字留空点确定）", nm); return 1; }
    LOG("起名：随机名 → 名字框「」【红】（已有同名角色，试满 64 次仍重名），名字框不动"); return 0;
}
static void finish_now(void) {
    if (!g_kl) return;
    [g_tick invalidate]; g_tick = nil; [g_kl removeFromSuperlayer]; g_kl = nil;
    dm_gamepad_capture(0);
    int ok = g_k.done == DM_VKBD_OK, hand = g_handoff; g_handoff = 0; char text[DM_VKBD_TEXTCAP]; snprintf(text, sizeof text, "%s", ok || hand ? g_k.text : "");
    // 名字留空点确定 = 随机取名（同 iOS 系统输入框：dm_namegen.c 抽一个不重名的，只填进名字框、不自动接受）
    // Leaving the name empty and choosing OK = random name (as the iOS system input box: dm_namegen.c draws one that collides with no character; it only fills the name box and is not auto-accepted)
    if (ok && g_k.kind == DM_VKBD_NAME && !*text && !dm_name_blank_random(text, sizeof text)) ok = 0;
    if (hand) LOG("屏幕键盘：键盘 / 鼠标 / 触屏有输入 → 收起，已打的「%s」交给输入框", text);
    else LOG("屏幕键盘：%s%s%s", ok ? "确定「" : "取消", ok ? text : "", ok ? "」" : "");
    void (^f)(int, const char *) = g_finish; g_finish = nil; if (f) f(hand ? 2 : ok, text);
}
static void after(int changed) { if (changed) redraw(); if (g_k.done) finish_now(); }
static void tick(void) {
    uint64_t b; int32_t lx, ly; dm_gamepad_captured(&b, &lx, &ly);
    int gl = dm_padmode_glyph_want(), ch = gl != g_glyph; g_glyph = gl;   // 换了手柄（键名换一套）也重画 | redraw when another controller takes over too (other button names)
    after(dm_vkbd_pad(&g_k, b, lx, ly, CACurrentMediaTime()) | ch);
}
static void begin(int kind, const char *title, const char *initial, int max_chars, int num_max, void (^finish)(int, const char *)) {
    if (g_kl || !g_where) { LOG("屏幕键盘：%s，按取消返回", g_kl ? "已有一个开着" : "前端没登记画在哪"); finish(0, ""); return; }
    dm_gamepad_capture(1);
    uint64_t b; int32_t lx, ly; dm_gamepad_captured(&b, &lx, &ly);
    dm_vkbd_open(&g_k, kind, initial, max_chars, num_max, b);
    g_title = @(title); g_glyph = dm_padmode_glyph_want(); g_finish = finish;
    g_where(&g_h);
    g_kl = [CALayer layer]; g_kl.zPosition = 10; g_kl.contentsGravity = kCAGravityResize;
    [(__bridge CALayer *)g_h.layer addSublayer:g_kl];
    place(); redraw();
    g_tick = [NSTimer timerWithTimeInterval:1 / 60.0 repeats:YES block:^(NSTimer *t) { (void)t; tick(); }];
    [NSRunLoop.mainRunLoop addTimer:g_tick forMode:NSRunLoopCommonModes];
    LOG("屏幕键盘：弹出（%s，上限 %d 字节，最大值 %d，面板 %.0f×%.0f 点）", kind == DM_VKBD_NAME ? "起名" : kind == DM_VKBD_NUM ? "数量" : "文字", g_k.max_bytes, num_max, g_geo.w, g_geo.h);
}

int dm_vkbd_input(const char *title, const char *initial, int max_chars, char *out, int out_cap) {
    int num = g_swkbd_numeric_max, kind = num >= 0 ? DM_VKBD_NUM : g_swkbd_name_panel ? DM_VKBD_NAME : DM_VKBD_TEXT;
    NSString *t = title && *title ? @(title) : U(kind == DM_VKBD_NAME ? "dmNameTitle" : kind == DM_VKBD_NUM ? "dmQtyTitle" : "dmInputTitle");
    NSString *init = initial ? @(initial) : @"";
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block int ok = 0; __block NSString *res = nil;
    dispatch_async(dispatch_get_main_queue(), ^{ begin(kind, t.UTF8String, init.UTF8String, max_chars, num, ^(int o, const char *s) { ok = o; res = @(s); dispatch_semaphore_signal(done); }); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (!ok) return 0;
    snprintf(out, (size_t)out_cap, "%s", res.UTF8String);
    return ok;
}

int dm_vkbd_ui_cancel(void) { if (!g_kl) return 0; dm_vkbd_cancel(&g_k); finish_now(); return 1; }
void dm_vkbd_ui_handoff(void) { if (!g_kl) return; g_handoff = 1; dm_vkbd_cancel(&g_k); finish_now(); }
void dm_vkbd_ui_relayout(void) { if (g_kl) { place(); redraw(); } }
void dm_vkbd_ui_key(int key, const char *chars) {
    if (!g_kl) return;
    int ch = 0;
    switch (key) {
        case DM_VKBD_KEY_TEXT: for (const char *p = chars; p && *p; p++) ch |= dm_vkbd_type(&g_k, *p); break;
        case DM_VKBD_KEY_BACK: if (g_k.len) ch = dm_vkbd_backspace(&g_k); break;   // 实体退格删空了不当取消 | the physical backspace never cancels when empty
        case DM_VKBD_KEY_OK: ch = dm_vkbd_ok(&g_k); break;
        case DM_VKBD_KEY_CANCEL: dm_vkbd_cancel(&g_k); ch = 1; break;
        default: ch = dm_vkbd_move(&g_k, key == DM_VKBD_KEY_LEFT ? -1 : key == DM_VKBD_KEY_RIGHT ? 1 : 0, key == DM_VKBD_KEY_UP ? -1 : key == DM_VKBD_KEY_DOWN ? 1 : 0); break;
    }
    after(ch);
}
CGImageRef dm_vkbd_ui_snapshot(CGRect *in_pic) { if (!g_kl || !g_img) return NULL; if (in_pic) *in_pic = g_in_pic; return CGImageRetain(g_img); }
