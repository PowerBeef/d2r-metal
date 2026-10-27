#!/usr/bin/env python3
# 我们自己的功能不写原版控件（出过的事故：一页把原版储藏箱 grid 设成不可见、游戏从不重设，丢装备）。两道静态扫：
# 一、整个宿主（dm/host/*.c）不许直接写控件的可见位 +0x50 / +0x51，只许在带守卫的函数里写——dm_cursor.c ui_show_ours（名字不是 Touch 开头就报红不写）。别处要改可见，调它
# 二、位置守卫：dm_cursor.c 里写控件矩形只许在 mv_offset（原版控件）与 ui_move_ours（我们的控件）里，登记的例外见 NATIVE_FIELD_OK
# 用法：native_write_scan.py <dm/host 目录>；--selftest 先拿一段故意违规的文本验这个扫描本身会报红
# Our own features do not write the original game's widgets (a past incident: a page set the original stash grid invisible, the game never reset it, and equipment was lost). Two static scans:
# 1. The whole host (dm/host/*.c) must not write a widget's visible flag +0x50 / +0x51 directly, only inside a guarded function -- dm_cursor.c ui_show_ours (a name not starting with Touch is reported as a failure and not written). To change visibility elsewhere, call it
# 2. Position guard: in dm_cursor.c a widget rectangle may only be written in mv_offset (original widgets) and ui_move_ours (our widgets); registered exceptions are in NATIVE_FIELD_OK
# Usage: native_write_scan.py <dm/host directory>; --selftest first verifies with a deliberately violating piece of text that the scan itself reports a failure
import os, re, sys

VIS = re.compile(r'\+\s*0x5[01]\)\s*=(?!=)')
GUARDED = ('ui_show_ours',)
def vis_scan(text, where):   # 直接写可见位的行；在带守卫的函数里（定义那一行或其函数体内）的不算 | lines that write the visible flag directly; those inside a guarded function (its definition line or its body) do not count
    bad, inside = [], None
    for no, line in enumerate(text.split('\n'), 1):
        m = re.match(r'static\s+\w+\s+(\w+)\(', line)
        if m: inside = m.group(1) if m.group(1) in GUARDED else None
        code = line.split('//', 1)[0]
        if VIS.search(code) and not inside: bad.append('%s:%d 直接写了可见位（要走 %s）：%s' % (where, no, ' / '.join(GUARDED), line.strip()[:120]))
        if line.startswith('}'): inside = None
    return bad

# 第三道（位置守卫）：dm_cursor.c 里写控件矩形（+0x70 x、+0x74 y、+0x78 宽、+0x7c 高）只许在 mv_offset（原版控件，名单 + 记原值 + 必写回）
# 与 ui_move_ours（只许 Touch 开头）里。认两种写法：直接 *(int32_t *)(… + 0x7?) = …；或同一函数里先取 int32_t *名 = (int32_t *)(… + 0x7?)，再写 名[k] = / *名 =
# Third check (position guard): in dm_cursor.c, writing a widget rectangle (+0x70 x, +0x74 y, +0x78 width, +0x7c height) is allowed only in mv_offset (original-game widgets, allow list + record original value + must write back)
# and in ui_move_ours (only names starting with Touch). Two forms are recognized: a direct *(int32_t *)(... + 0x7?) = ...; or, within the same function, first taking int32_t *name = (int32_t *)(... + 0x7?) and then writing name[k] = / *name =
RECT_OK = ('mv_offset', 'ui_move_ours')
RECT_OFFS = ['0x7%x' % i for i in range(13)]   # 0x70–0x7c
# 原版控件字段写入的登记例外（一项一行：文件、字段、只许在哪些函数里写、理由）。名单外的函数写这些字段一律报红
# Registered exceptions for writes to original widget fields (one per line: file, field, which functions may write it, reason). Writes to these fields from functions not on the list are always red
NATIVE_FIELD_OK = {
    ('dm_cursor.c', '0x19c'): (('ui_note_dock_pump', 'note_dock_restore'), 'NotificationPanel 停靠目标 y：原版只在重排时按布局抄进、滑入滑出按它插值写 +0x74；'
                               '改 +0x74 会被动画每帧盖回（试过）。原值我们自己记、停靠条件一结束当场写回（面板关 / 读取画面 / 保存并退出 / 回到前端）'),
    ('dm_cursor.c', '0x74'): (('note_dock_snap',), 'NotificationPanel 矩形 y：停靠目标切换（开 / 关面板）那一刻框已停稳（原版停稳不写坐标）才写成新目标一次，动画期不写；写后下一帧核'),
}
def field_write_scan(text, where, off, ok, what):
    ptr = re.compile(r'(?:int32_t|,)\s*\*\s*(\w+)\s*=\s*\(int32_t\s*\*\)\s*\([^;)]*\+\s*%s' % off)   # 基址只看本括号：同一行别的变量读别的字段不算；逗号后的第二个声明也认 | the base only counts within its own parentheses: other variables on the same line reading other fields do not count; a second declaration after a comma is recognised too
    direct = re.compile(r'\*\s*\(int32_t\s*\*\)\s*\([^)]*\+\s*%s\)\s*=(?!=)' % off)
    bad, fn, ptrs = [], None, set()
    for no, line in enumerate(text.split('\n'), 1):
        m = re.match(r'(?:static\s+)?\w+\s+\*?(\w+)\(', line)
        if m and not line.startswith(' '): fn, ptrs = m.group(1), set()
        code = line.split('//', 1)[0]
        for pm in ptr.finditer(code): ptrs.add(pm.group(1))
        code = ptr.sub('', code)   # 声明那一处（int32_t *名 = …）本身不算写 | the declaration itself (int32_t *name = ...) does not count as a write
        hit = direct.search(code) or any(re.search(r'(?<![\w.>])(?:\*\s*%s|%s\s*\[\s*\d\s*\])\s*=(?!=)' % (q, q), code) for q in ptrs)
        if hit and fn not in ok: bad.append('%s:%d 在 %s 里直接写了%s：%s' % (where, no, fn, what, line.strip()[:120]))
    return bad
def rect_scan(text, where):
    exc = [off for (f, off) in NATIVE_FIELD_OK if where.startswith(f) and off in RECT_OFFS]   # 矩形字段的登记例外：从矩形扫描里拿出来，按例外单独扫 | registered exceptions for rectangle fields: taken out of the rectangle scan and scanned separately as exceptions
    bad = field_write_scan(text, where, '(?:%s)\\b' % '|'.join(o for o in RECT_OFFS if o not in exc), RECT_OK, '控件矩形（原版控件走 mv_offset、我们的走 ui_move_ours）')
    for (f, off), (fns, why) in NATIVE_FIELD_OK.items():
        if where.startswith(f): bad += field_write_scan(text, where, re.escape(off) + r'\b', fns + (RECT_OK if off in RECT_OFFS else ()), '原版控件字段 +%s（登记例外只许在 %s 里写）' % (off, ' / '.join(fns)))
    return bad

def main():
    if '--selftest' in sys.argv:
        if not vis_scan('static void f(uint64_t w) {\n    *(uint8_t *)(w + 0x50) = 0;\n}', 'selftest') or vis_scan('static void ui_show_ours(uint64_t w, int on) {\n    *(uint8_t *)(w + 0x50) = 1;\n}\n    if (*(uint8_t *)(w + 0x50) == 1) x();', 'selftest') \
           or not rect_scan('static void f(uint64_t w) {\n    int32_t *r = (int32_t *)(w + 0x70); r[0] = 5;\n}', 'selftest') or not rect_scan('static void g(uint64_t w) {\n    *(int32_t *)(w + 0x74) = 3;\n}', 'selftest') \
           or rect_scan('static void ui_note_dock_pump(uint64_t w) {\n    int32_t *t = (int32_t *)(w + 0x19c), h = *(int32_t *)(w + 0x7c); *t = h;\n}', 'dm_cursor.c') \
           or not rect_scan('static void n(uint64_t w) {\n    int32_t *t = (int32_t *)(w + 0x19c); *t = 1;\n}', 'dm_cursor.c') \
           or not rect_scan('static void n2(uint64_t w) {\n    int32_t *t = (int32_t *)(w + 0x19c), *ry = (int32_t *)(w + 0x74); *ry = 1;\n}', 'dm_cursor.c') \
           or rect_scan('static void k(uint64_t w) {\n    int32_t *rx = (int32_t *)(w + 0x70), ww = *(int32_t *)(w + 0x78); if (*rx) x();\n}\nstatic void mv_offset(uint64_t w) {\n    int32_t *v = (int32_t *)(w + 0x70); *v = 1;\n}\nstatic int h(float *r) {\n    r[0] = 1.0f; int x = *(int32_t *)(w + 0x70); return x;\n}', 'selftest'):
            print('✗ native_write_scan 自检失败：该报的没报或不该报的报了'); return 1
        print('✓ native_write_scan 自检：违规行会报红、守卫函数里的写不算')
    host = next(a for a in sys.argv[1:] if not a.startswith('--'))
    bad = []
    srcs = sorted(f for f in os.listdir(host) if f.endswith('.c'))
    for f in srcs: bad += vis_scan(open(os.path.join(host, f), encoding='utf-8', errors='replace').read(), f)
    bad += rect_scan(open(os.path.join(host, 'dm_cursor.c'), encoding='utf-8').read(), 'dm_cursor.c')
    if bad:
        print('✗ 我们的功能碰了原版控件（可见位只许走带守卫的函数；控件矩形只许走 mv_offset / ui_move_ours）：'); [print('   ' + b) for b in bad]; return 1
    print('✓ 宿主 %d 个源文件没有绕过守卫直接写可见位；dm_cursor.c 写控件矩形只在 mv_offset / ui_move_ours 里' % len(srcs)); return 0

if __name__ == '__main__':
    sys.exit(main())
