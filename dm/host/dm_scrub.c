// 分享日志脱敏：玩家把日志发给别人之前，把能认出人的字样换成占位符。纯逻辑、不碰游戏内存，单测 tests/t_scrub.c 直接测。
// 两类规则：
//   ① 字面替换（调用方按本机加）：家目录 → ~、iOS 的包名（带开发者名字）→ <bundle-id>、显示器型号 → <display>、/Volumes 下挂着的盘名、
//      Mac 的登录名 / 电脑名，以及从存档目录收来的角色名（dm_scrub_add_chars）→ <char1> <char2> …（同一个角色在整份日志里编号不变，还能对上号）。
//      长的先比，同一处只换一次。
//   ② 固定的形状（不用登记）：
//      · 路径里的 UUID（前面是 /：iOS 的应用容器、Mac 上跑的 iOS 包的沙盒）→ /<uuid>；尖括号里的映像 UUID（消毒器报告的模块表）照留，符号化要用
//      · /var/folders/<两级> → /var/folders/<tmp>（Mac 每个用户的临时目录名）
//      · /Volumes/<盘名>（没挂着的盘、上一局的日志）→ /Volumes/<volume>：盘名取到 / 、换行、引号、括号或中文标点为止，空格后面接着英文字母数字才算盘名的一部分
//      · <名>.d2s（存档里已经没有的角色，如上一局删掉的）→ <char>.d2s
// 有意不换：构建号、机型（hw.model / hw.machine）、系统版本、语言——排查要用，也认不出人。
// Share-log scrubbing: before the player sends a log to someone else, replace whatever could identify them with placeholders. Pure logic, never touching game memory; the unit test tests/t_scrub.c tests it directly.
// Two kinds of rules:
//   (1) Literal replacements (added by the caller for this machine): home directory -> ~, the iOS bundle ID (it carries the developer's name) -> <bundle-id>, display models -> <display>, mounted disk names under /Volumes,
//       the Mac login / computer name, and character names collected from the save directory (dm_scrub_add_chars) -> <char1> <char2> ... (one character keeps one number across the whole log, so lines still match up).
//       Longer needles are tried first; each place is replaced once.
//   (2) Fixed shapes (no registration needed):
//       - a UUID in a path (preceded by /: the iOS app container, the sandbox of the iOS package run on a Mac) -> /<uuid>; image UUIDs in angle brackets (the module table of sanitizer reports) stay, symbolication needs them
//       - /var/folders/<two levels> -> /var/folders/<tmp> (the Mac's per-user temporary directory name)
//       - /Volumes/<disk name> (disks no longer mounted, the previous run's log) -> /Volumes/<volume>: the name runs up to /, a line break, a quote, a bracket or Chinese punctuation; a space belongs to the name only when an ASCII letter or digit follows it
//       - <name>.d2s (characters no longer in the save, e.g. deleted since the previous run) -> <char>.d2s
// Deliberately kept: build number, device model (hw.model / hw.machine), OS version, language -- needed for diagnosis and they do not identify anyone.
#include "dm_scrub.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { char *nd, *rp; size_t nl, rl; int word; } sc_rule;
struct dm_scrub { sc_rule *r; int n, cap, nchar; };

dm_scrub *dm_scrub_new(void) { return calloc(1, sizeof(dm_scrub)); }
void dm_scrub_free(dm_scrub *s) { if (!s) return; for (int i = 0; i < s->n; i++) { free(s->r[i].nd); free(s->r[i].rp); } free(s->r); free(s); }
void dm_scrub_add(dm_scrub *s, const char *needle, const char *repl, int word) {
    if (!s || !needle || !repl) return; size_t nl = strlen(needle); if (nl < 2) return;
    for (int i = 0; i < s->n; i++) if (s->r[i].nl == nl && !memcmp(s->r[i].nd, needle, nl)) return;
    if (s->n == s->cap) { int c = s->cap ? s->cap * 2 : 16; sc_rule *r = realloc(s->r, (size_t)c * sizeof *r); if (!r) return; s->r = r; s->cap = c; }
    sc_rule *r = &s->r[s->n]; r->nd = strdup(needle); r->rp = strdup(repl); if (!r->nd || !r->rp) { free(r->nd); free(r->rp); return; }
    r->nl = nl; r->rl = strlen(repl); r->word = word; s->n++;
}

// ---- 角色名 | Character names
typedef struct { char (*v)[64]; int n, cap; } sc_names;
static void name_add(sc_names *a, const char *nm, size_t l) {
    if (l < 2 || l >= 64 || nm[0] == '.' || nm[0] == '_') return;
    for (size_t i = 0; i < l; i++) if ((unsigned char)nm[i] < 0x20 || nm[i] == '/' || nm[i] == '\t') return;
    for (int i = 0; i < a->n; i++) if (strlen(a->v[i]) == l && !memcmp(a->v[i], nm, l)) return;
    if (a->n == a->cap) { int c = a->cap ? a->cap * 2 : 16; char (*v)[64] = realloc(a->v, (size_t)c * 64); if (!v) return; a->v = v; a->cap = c; }
    memcpy(a->v[a->n], nm, l); a->v[a->n][l] = 0; a->n++;
}
static void walk_d2s(sc_names *a, const char *dir, int depth) {   // dir 下（含 depth 层子目录）的 <名>.d2s | <name>.d2s under dir (including depth levels of subdirectories)
    DIR *d = opendir(dir); struct dirent *e; if (!d) return;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[2048]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); if (lstat(p, &st)) continue;
        size_t l = strlen(e->d_name);
        if (S_ISREG(st.st_mode) && l > 4 && !strcmp(e->d_name + l - 4, ".d2s")) name_add(a, e->d_name, l - 4);
        else if (S_ISDIR(st.st_mode) && depth > 0) walk_d2s(a, p, depth - 1); }
    closedir(d);
}
static int cmp_name(const void *x, const void *y) { return strcmp((const char *)x, (const char *)y); }
int dm_scrub_add_chars(dm_scrub *s, const char *save) {
    if (!s || !save || !*save) return 0; sc_names a = {0}; char p[2048];
    walk_d2s(&a, save, 2);                                                  // <存档>/<名>/<名>.d2s | <save>/<name>/<name>.d2s
    snprintf(p, sizeof p, "%s.work", save); walk_d2s(&a, p, 2);             // 事务式存档的工作目录 | the working directory of the transactional save
    snprintf(p, sizeof p, "%s.cloudbak", save); walk_d2s(&a, p, 3);        // iCloud 同步的备份：<备份>/<名>-<时刻>/<名>/<名>.d2s | iCloud sync backups: <backup>/<name>-<stamp>/<name>/<name>.d2s
    snprintf(p, sizeof p, "%s.cloudstate", save); FILE *f = fopen(p, "r");   // 同步过的单位（含别的设备删掉的）：每行「单位\t…」 | units ever synced (including ones deleted on another device): each line is "unit\t..."
    char ln[256]; while (f && fgets(ln, sizeof ln, f)) { char *t = strchr(ln, '\t'); if (t) name_add(&a, ln, (size_t)(t - ln)); }
    if (f) fclose(f);
    if (a.n) qsort(a.v, (size_t)a.n, 64, cmp_name);
    int added = 0;
    for (int i = 0; i < a.n; i++) { int had = s->n; char rp[32]; snprintf(rp, sizeof rp, "<char%d>", s->nchar + 1); dm_scrub_add(s, a.v[i], rp, 1); if (s->n > had) { s->nchar++; added++; } }
    free(a.v); return added;
}

// ---- 洗 | Scrubbing
typedef struct { char *b; size_t n, cap; int bad; } sc_buf;
static void put(sc_buf *o, const void *p, size_t n) {
    if (o->bad) return;
    if (o->n + n > o->cap) { size_t c = o->cap ? o->cap : 4096; while (c < o->n + n) c *= 2; char *b = realloc(o->b, c); if (!b) { o->bad = 1; return; } o->b = b; o->cap = c; }
    memcpy(o->b + o->n, p, n); o->n += n;
}
static int alnum(unsigned char c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }
static int hex(unsigned char c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
// 中文标点（UTF-8 三字节）：「」（），：、。 | Chinese punctuation (three UTF-8 bytes each): 「」（），：、。
static const char *const k_cjk_stop[] = { "\xe3\x80\x8c", "\xe3\x80\x8d", "\xef\xbc\x88", "\xef\xbc\x89", "\xef\xbc\x8c", "\xef\xbc\x9a", "\xe3\x80\x81", "\xe3\x80\x82" };
static int cjk_stop_at(const unsigned char *p, size_t left) {   // p 起是不是一个中文标点 | whether a Chinese punctuation mark starts at p
    if (left < 3 || p[0] < 0xe0) return 0; for (size_t k = 0; k < sizeof k_cjk_stop / sizeof *k_cjk_stop; k++) if (!memcmp(p, k_cjk_stop[k], 3)) return 1; return 0;
}
static int cjk_stop_end(const char *b, size_t end) {            // b[end-3..end) 是不是一个中文标点 | whether b[end-3..end) is a Chinese punctuation mark
    return end >= 3 && cjk_stop_at((const unsigned char *)b + end - 3, 3);
}
static int uuid_at(const unsigned char *p, size_t left) {       // p 起 36 字节是 8-4-4-4-12 的十六进制，且后面不再接字母数字 | 36 bytes from p are 8-4-4-4-12 hex, not followed by another letter or digit
    if (left < 36) return 0;
    for (int i = 0; i < 36; i++) { if (i == 8 || i == 13 || i == 18 || i == 23) { if (p[i] != '-') return 0; } else if (!hex(p[i])) return 0; }
    return left == 36 || !alnum(p[36]);
}
static size_t volume_len(const unsigned char *p, size_t left) {  // /Volumes/ 之后盘名的长度 | length of the disk name after /Volumes/
    size_t j = 0;
    while (j < left) { unsigned char c = p[j];
        if (c == '/' || c < 0x20 || c == '"' || c == '\'' || c == '(' || c == ')' || c == '[' || c == ']' || c == '<' || c == '>' || c == ',' || c == ';' || cjk_stop_at(p + j, left - j)) break;
        if (c == ' ' && !(j + 1 < left && alnum(p[j + 1]))) break;
        j++; }
    return j;
}
char *dm_scrub_run(const dm_scrub *s, const void *in_, size_t n, size_t *on) {
    const unsigned char *in = in_; sc_buf o = {0}; int nr = s ? s->n : 0;
    // 规则按首字节分桶、桶内长的在前 | rules bucketed by first byte, longer ones first within a bucket
    int *ord = nr ? malloc((size_t)nr * sizeof *ord) : NULL, start[257] = {0}; if (nr && !ord) return NULL;
    for (int i = 0; i < nr; i++) start[(unsigned char)s->r[i].nd[0] + 1]++;
    for (int c = 0; c < 256; c++) start[c + 1] += start[c];
    { int fill[256]; memcpy(fill, start, sizeof fill); for (int i = 0; i < nr; i++) ord[fill[(unsigned char)s->r[i].nd[0]]++] = i; }
    for (int c = 0; c < 256; c++) for (int i = start[c] + 1; i < start[c + 1]; i++) for (int j = i; j > start[c] && s->r[ord[j]].nl > s->r[ord[j - 1]].nl; j--) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    o.cap = n + n / 16 + 64; o.b = malloc(o.cap); if (!o.b) { free(ord); return NULL; }   // 替换后多半更短：先按原长加一点留 | the result is mostly shorter: start from the input length plus a margin
    for (size_t i = 0; i < n && !o.bad; ) {
        unsigned char c = in[i]; int hit = 0;
        for (int k = start[c]; k < start[c + 1]; k++) { const sc_rule *r = &s->r[ord[k]];
            if (r->nl > n - i || memcmp(in + i, r->nd, r->nl)) continue;
            if (r->word && alnum((unsigned char)r->nd[0]) && i > 0 && alnum(in[i - 1])) continue;
            if (r->word && alnum((unsigned char)r->nd[r->nl - 1]) && i + r->nl < n && alnum(in[i + r->nl])) continue;
            put(&o, r->rp, r->rl); i += r->nl; hit = 1; break; }
        if (hit) continue;
        if (c == '/' && uuid_at(in + i + 1, n - i - 1)) { put(&o, "/<uuid>", 7); i += 37; continue; }
        if (c == '/' && n - i > 13 && !memcmp(in + i, "/var/folders/", 13)) {
            size_t a = i + 13, b; for (b = a; b < n && in[b] != '/' && in[b] > 0x20; b++) {}
            size_t d = b + 1, e; for (e = d; e < n && in[e] != '/' && in[e] > 0x20; e++) {}
            if (b > a && b < n && in[b] == '/' && e > d && b - a <= 8 && e - d <= 64) { put(&o, "/var/folders/<tmp>", 18); i = e; continue; } }
        if (c == '/' && n - i > 9 && !memcmp(in + i, "/Volumes/", 9)) { size_t l = volume_len(in + i + 9, n - i - 9); if (l) { put(&o, "/Volumes/<volume>", 17); i += 9 + l; continue; } }
        if (c == '.' && n - i >= 4 && !memcmp(in + i, ".d2s", 4) && (n - i == 4 || !alnum(in[i + 4])) && o.n && o.b[o.n - 1] != '>') {   // 前面那段名字（到分隔符为止）换成 <char> | the name just before it (back to a separator) becomes <char>
            size_t k = o.n;
            while (k > 0) { unsigned char x = (unsigned char)o.b[k - 1]; if (cjk_stop_end(o.b, k)) break; if (alnum(x) || x == '-' || x == '_' || x >= 0x80) k--; else break; }
            if (k < o.n) { o.n = k; put(&o, "<char>", 6); } }
        put(&o, in + i, 1); i++;
    }
    free(ord);
    if (o.bad) { free(o.b); return NULL; }
    *on = o.n; return o.b;
}
