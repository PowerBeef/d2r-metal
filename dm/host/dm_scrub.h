#pragma once
// 分享日志脱敏（实现与规则见 dm_scrub.c）：Mac「帮助 → 分享日志」（front_mac.m）与 iOS「分享诊断日志」（dm_sharelog.m）共用
// Share-log scrubbing (implementation and rules in dm_scrub.c): shared by the Mac "Help -> Share Log" (front_mac.m) and the iOS "Share Diagnostics" (dm_sharelog.m)
#include <stddef.h>
typedef struct dm_scrub dm_scrub;
dm_scrub *dm_scrub_new(void);
void dm_scrub_free(dm_scrub *s);
// 加一条字面替换：needle 出现处换成 repl。word = 1：needle 首 / 尾是 ASCII 字母数字时，那一侧的邻字不许也是（短名字不吃掉别的词的一部分）。
// needle 短于 2 字节、或已经加过，不加
// Add a literal replacement: occurrences of needle become repl. word = 1: where needle starts / ends with an ASCII letter or digit, the neighbour on that side must not be one too (a short name does not eat part of another word).
// A needle shorter than 2 bytes, or one already added, is skipped
void dm_scrub_add(dm_scrub *s, const char *needle, const char *repl, int word);
// 从存档目录收角色名：<存档>、<存档>.work、<存档>.cloudbak 下的 <名>.d2s 与 <存档>.cloudstate 里的单位名，按名字排序编号成 <char1> <char2> …。返回新收到几个
// Collect character names from the save directory: <name>.d2s under <save>, <save>.work and <save>.cloudbak, and the unit names in <save>.cloudstate, numbered as <char1> <char2> ... in name order. Returns how many new ones were found
int dm_scrub_add_chars(dm_scrub *s, const char *save);
// 洗一遍：返回 malloc 的新缓冲（调用方 free），*on = 长度；内存不够返回 NULL
// Scrub once: returns a new malloc'd buffer (the caller frees it), *on = its length; NULL when out of memory
char *dm_scrub_run(const dm_scrub *s, const void *in, size_t n, size_t *on);
