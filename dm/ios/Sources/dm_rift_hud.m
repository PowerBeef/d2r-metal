// 秘境爬塔左侧任务栏：放在左边并缩小，布局参考暗黑 3 右侧任务栏，倒计时条和整体风格按暗黑 2。
// 尺寸数字照设计稿：设计稿按 Mac 窗口 2 倍像素画（游戏区 1334×614），
// 这里以 u = 游戏区高 / 614 为单位换算。数据由 dm/host/dm_rift.c 出（dm_rift_hud），前端每帧调（front_ios.m tickTouchControls）。
// 字：原版 BlizzardGlobal——运行时从资源包读出来（dm_vkbd_ui.m dm_game_font，不进仓库、不进 App 包）；读不到就用系统粗体。
// Left-side quest bar for the rift tower climb: placed on the left and shrunk; the layout follows Diablo 3's right-side quest bar, the countdown bar and overall style follow Diablo 2.
// Size numbers follow the design mock: the mock is drawn at 2x pixels of the Mac window (game area 1334×614),
// and here everything is converted with u = game area height / 614 as the unit. Data comes from dm/host/dm_rift.c (dm_rift_hud); the front end calls it every frame (front_ios.m tickTouchControls).
// Font: the original game's BlizzardGlobal -- read from the asset pack at runtime (dm_vkbd_ui.m dm_game_font; not in the repository, not in the App bundle); if it cannot be read, use the system bold font.
#import <UIKit/UIKit.h>
#include "dm_i18n.h"
#import <CoreText/CoreText.h>
#include "dm_vkbd_ui.h"
#include "../../host/dm_rift.h"

// 原版 BlizzardGlobal（中文、数字都是它；dm_vkbd_ui.m 从资源包读）；按字号缓存。
// 第一次要字时字体还没读：在后台线程上读（扫资源包清单 + 解压字体，不放在主线程这一帧里），读好之前先用系统粗体、不进缓存，读好后的那一帧起换成原版字
// The original game's BlizzardGlobal (used for both Chinese text and digits; read from the asset pack by dm_vkbd_ui.m); cached per size.
// The first time a font is wanted it has not been read yet: it is read on a background thread (scanning the asset pack manifest + decompressing the font, kept out of this main-thread frame); until then the system bold font is used and not cached, and from the frame after it is ready the original font is used
static UIFont *d2font(CGFloat size) {
    static NSMutableDictionary<NSNumber *, UIFont *> *cache; if (!cache) cache = [NSMutableDictionary dictionary];
    UIFont *f = cache[@(size)]; if (f) return f;
    if (!dm_game_font_ready()) { dm_game_font_preload(); return [UIFont boldSystemFontOfSize:size]; }
    CTFontRef ct = dm_game_font(size); f = ct ? (__bridge_transfer UIFont *)ct : [UIFont boldSystemFontOfSize:size];   // CTFont 与 UIFont 免费桥接 | CTFont is toll-free bridged with UIFont
    cache[@(size)] = f; return f;
}
static UIColor *rgb(int r, int g, int b, CGFloat a) { return [UIColor colorWithRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a]; }
static CATextLayer *text_layer(void) { CATextLayer *t = [CATextLayer layer]; t.contentsScale = UIScreen.mainScreen.scale; t.shadowOpacity = 0.86; t.shadowOffset = CGSizeMake(1, 1); t.shadowRadius = 0; t.shadowColor = UIColor.blackColor.CGColor; return t; }
static void set_text(CATextLayer *t, NSString *s, UIFont *f, UIColor *c, CGRect fr, NSString *align) {
    t.frame = fr; t.alignmentMode = align; t.foregroundColor = c.CGColor; t.font = (__bridge CFTypeRef)f; t.fontSize = f.pointSize;
    if (![t.string isEqual:s]) t.string = s;
}

void dm_rift_hud_draw(UIView *host, CGRect wr, BOOL hide) {
    static CALayer *box, *lineT, *lineB, *lineL, *track, *ticks[9]; static CAGradientLayer *fill; static CAShapeLayer *pin[2]; static CATextLayer *title, *sub, *timer, *goal;
    dm_rift_hud_t h; int on = !hide && dm_rift_hud(&h);
    if (!box) { if (!on) return;
        box = [CALayer layer]; box.backgroundColor = rgb(0, 0, 0, 170 / 255.0).CGColor;
        lineT = [CALayer layer]; lineB = [CALayer layer]; lineL = [CALayer layer]; track = [CALayer layer]; fill = [CAGradientLayer layer];
        title = text_layer(); sub = text_layer(); timer = text_layer(); goal = text_layer();
        for (CALayer *l in @[lineT, lineB, lineL, title, sub, timer, goal, track]) [box addSublayer:l];
        [track addSublayer:fill]; for (int i = 0; i < 9; i++) { ticks[i] = [CALayer layer]; ticks[i].backgroundColor = rgb(0, 0, 0, 110 / 255.0).CGColor; [track addSublayer:ticks[i]]; }
        for (int i = 0; i < 2; i++) { pin[i] = [CAShapeLayer layer]; pin[i].fillColor = rgb(160, 135, 80, 1).CGColor; pin[i].strokeColor = rgb(60, 48, 30, 1).CGColor; [box addSublayer:pin[i]]; }
        [host.layer addSublayer:box]; }
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    box.hidden = !on;
    if (on) {
        CGFloat u = wr.size.height / 614, W = 250 * u, H = 78 * u;
        box.frame = CGRectMake(wr.origin.x + 28 * u, wr.origin.y + 150 * u, W, H);
        lineT.frame = CGRectMake(0, 0, W, MAX(1, u)); lineB.frame = CGRectMake(0, H - MAX(1, u), W, MAX(1, u)); lineL.frame = CGRectMake(0, 0, 2 * u, H);
        lineT.backgroundColor = lineB.backgroundColor = rgb(120, 100, 60, 0.9).CGColor; lineL.backgroundColor = rgb(150, 125, 75, 1).CGColor;
        int g = h.guardian, s = h.secs_left < 0 ? 0 : h.secs_left; UIColor *gold = rgb(199, 179, 119, 1), *red = rgb(200, 50, 40, 1), *white = rgb(215, 210, 200, 1), *grey = rgb(150, 145, 135, 1);
        set_text(title, @(dm_ui("dmRiftHudTitle")), d2font(17 * u), g ? red : gold, CGRectMake(10 * u, 6 * u, 40 * u, 22 * u), kCAAlignmentLeft);
        NSString *st = h.done ? (h.inc ? [NSString stringWithFormat:@(dm_ui("dmRiftHudDone")), h.tier] : [NSString stringWithFormat:@(dm_ui("dmRiftHudLate")), h.tier]) : g ? @(dm_ui("dmRiftHudGuard")) : [NSString stringWithFormat:@(dm_ui("dmRiftTierN")), h.tier];
        set_text(sub, st, d2font(14 * u), white, CGRectMake(48 * u, 9 * u, 130 * u, 19 * u), kCAAlignmentLeft);
        set_text(timer, [NSString stringWithFormat:@"%d:%02d", s / 60, s % 60], d2font(17 * u), (s <= 60 && !h.done) ? red : gold, CGRectMake(W - 90 * u, 6 * u, 80 * u, 22 * u), kCAAlignmentRight);
        NSString *gl = h.done ? (!h.inc ? @(dm_ui("dmRiftHudSame")) : h.next > h.tier ? [NSString stringWithFormat:@(dm_ui("dmRiftHudNext")), h.next] : @(dm_ui("dmRiftHudTop"))) : h.goal == 5 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudOverKillN")), h.gdead, h.gtotal] : h.goal == 4 ? @(dm_ui("dmRiftHudOverKill")) : h.goal == 3 ? @(dm_ui("dmRiftHudOver"))
            : (h.goal == 1 || h.goal == 2) && h.nwave > 1 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudKillWave")), h.wave, h.nwave, h.gdead, h.gtotal] : h.goal == 2 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudKillN")), h.gdead, h.gtotal] : h.goal == 1 ? @(dm_ui("dmRiftHudKill")) : [NSString stringWithFormat:@(dm_ui("dmRiftHudGoal")), (int)(h.progress * 100)];   // 已超时：第二行换成这一句，优先于「击败守护者」/ 进度目标（第一行照旧写第几层 / 守护者现身） | overtime: the second line becomes this sentence, taking priority over "Slay the Rift Guardian" / the progress goal (the first line still says which level / that the Guardian has appeared)
        set_text(goal, gl, d2font(12 * u), grey, CGRectMake(10 * u, 32 * u, W - 20 * u, 17 * u), kCAAlignmentLeft);
        CGRect tr = CGRectMake(11 * u, 55 * u, W - 22 * u, 10 * u);   // 含 1 单位外框（条 8 + 外框各 1）   // 进度条：外框暗边 + 深底，填充金褐（守护者转暗红），每 10% 一道刻痕，两端菱形钉 | Includes the 1-unit outer frame (bar 8 + frame 1 on each side)   // Progress bar: dark-edged outer frame + deep background, golden-brown fill (turns dark red for the guardian), a notch every 10%, diamond studs at both ends
        track.frame = tr; track.backgroundColor = rgb(25, 20, 14, 1).CGColor; track.borderColor = rgb(95, 78, 48, 1).CGColor; track.borderWidth = MAX(1, u);
        CGFloat bw = MAX(1, u); fill.frame = CGRectMake(bw, bw, (tr.size.width - 2 * bw) * (h.done ? 1 : h.progress), tr.size.height - 2 * bw);
        fill.colors = g ? @[(id)rgb(190, 50, 40, 1).CGColor, (id)rgb(110, 20, 15, 1).CGColor] : @[(id)rgb(205, 160, 80, 1).CGColor, (id)rgb(140, 100, 40, 1).CGColor];
        for (int i = 0; i < 9; i++) ticks[i].frame = CGRectMake(bw + (tr.size.width - 2 * bw) * (i + 1) / 10, bw, MAX(1, u), tr.size.height - 2 * bw);
        for (int i = 0; i < 2; i++) { CGFloat cx = i ? tr.origin.x + tr.size.width + 4 * u : tr.origin.x - 4 * u, cy = CGRectGetMidY(tr), r = 4 * u; UIBezierPath *p = [UIBezierPath bezierPath];
            [p moveToPoint:CGPointMake(cx, cy - r)]; [p addLineToPoint:CGPointMake(cx + r, cy)]; [p addLineToPoint:CGPointMake(cx, cy + r)]; [p addLineToPoint:CGPointMake(cx - r, cy)]; [p closePath]; pin[i].path = p.CGPath; pin[i].lineWidth = MAX(1, u * 0.8); }
    }
    [CATransaction commit];
}
