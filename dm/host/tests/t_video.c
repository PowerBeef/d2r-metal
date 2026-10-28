// 画面设置读盘单测（秒级）：dm_video.c 的 sw_video.txt 逐键读。缺的、越界的那一个键才按首次启动的值补，玩家设好的其余几项照留；
// 一个键都没有才算首次启动。dm_video.c 单独编译，这里给强定义的 front_video_firstboot（推荐 1/1/1、首次启动的当前值 1/2/0）与设置目录；
// 读盘只做一次（进程内静态），每一组在子进程里跑。期望都是对着规则手推的。
// Picture settings load unit test (takes seconds): sw_video.txt in dm_video.c is read key by key. Only a missing or out-of-range key is filled from the first-boot values, the player's other settings stay;
// only a file with no key at all counts as a first boot. dm_video.c is compiled on its own; this file gives a strong front_video_firstboot (recommendation 1/1/1, first-boot current values 1/2/0) and the settings directory;
// loading happens once per process (static state), so each group runs in a child process. Every expectation is worked out by hand from the rules.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../hle.h"
int dm_video_get(const char *key);
module_t g_mod[MAX_MOD]; int g_nmod;
static char g_dir[512];
static int g_fb_calls;
int dm_custom_path(const char *name, char *p, size_t n) { snprintf(p, n, "%s/%s", g_dir, name); return 1; }
int front_video_firstboot(int rec[3], int cur[3]) { g_fb_calls++; rec[0] = rec[1] = rec[2] = 1; cur[0] = 1; cur[1] = 2; cur[2] = 0; return 1; }
// 后加的键一个个问：aa 推荐 1、aniso 推荐 3、color 没有推荐（-1 → 0，原先的样子） | later keys are asked one by one: aa recommends 1, aniso 3, color has none (-1 → 0, the old look)
int front_video_rec(const char *key) { return !strcmp(key, "aa") ? 1 : !strcmp(key, "aniso") ? 3 : -1; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void file_put(const char *txt) { char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); if (!txt) { unlink(p); return; } FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); }
static const char *file_get(void) { static char b[640]; char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); FILE *f = fopen(p, "r"); if (!f) return ""; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }
// 子进程里读一次盘：返回 mode / res / up、首次启动推荐被问了几次、写回后的文件 | load once in a child: returns mode / res / up, how many times the first-boot recommendation was asked, the file after write-back
typedef struct { int v[7], fb; char file[640]; } result;
static result run(const char *txt) {
    file_put(txt); int pp[2]; result r; memset(&r, 0, sizeof r); if (pipe(pp)) return r;
    pid_t pid = fork();
    if (!pid) { close(pp[0]); result x; memset(&x, 0, sizeof x); x.v[0] = dm_video_get("mode"); x.v[1] = dm_video_get("res"); x.v[2] = dm_video_get("up");
        x.v[3] = dm_video_get("aa"); x.v[4] = dm_video_get("aniso"); x.v[5] = dm_video_get("color"); x.v[6] = dm_video_get("s3d"); x.fb = g_fb_calls;
        strlcpy(x.file, file_get(), sizeof x.file); if (write(pp[1], &x, sizeof x) != (ssize_t)sizeof x) _exit(1); _exit(0); }
    close(pp[1]); if (read(pp[0], &r, sizeof r) != (ssize_t)sizeof r) r.v[0] = -9; close(pp[0]); waitpid(pid, NULL, 0); return r;
}
#define FULL(m, rm, s, rs, u, ru) "mode=" #m "\nrec_mode=" #rm "\nres=" #s "\nrec_res=" #rs "\nup=" #u "\nrec_up=" #ru "\n"
#define TAIL(a, ra, n, rn, c, rc) "aa=" #a "\nrec_aa=" #ra "\naniso=" #n "\nrec_aniso=" #rn "\ncolor=" #c "\nrec_color=" #rc "\n"
#define S3(s, rs) "s3d=" #s "\nrec_s3d=" #rs "\n"
#define RV "rec_v=3\n"   // 推荐值版本（dm_video.c REC_V）；没有这行 = 第 1 版，读盘时重算推荐 | recommendation version (REC_V in dm_video.c); without it = version 1, recalculated at load
#define NEWREC TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) RV   // 后加的键缺时补成的样子（front_video_rec） | how the later keys are filled when missing (front_video_rec)

int main(void) {
    snprintf(g_dir, sizeof g_dir, "%s/t_video.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_dir, 0755);
    result r;
    // ① 整份齐全：照读，不问推荐、不写回 | (1) complete file: read as is, no recommendation asked, no write-back
    r = run(FULL(0, 1, 2, 1, 0, 1) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "① 齐全：0/2/0、没问推荐（%d 次）、文件不变", r.fb);
    // ② 缺一个 rec_*：玩家的三项照留（旧版整份回首次启动 = 1/2/0），只补 rec_res = 推荐 1；整份写回 | (2) one rec_* missing: the player's three values stay (the old code reset all to the first boot = 1/2/0), only rec_res is filled with the recommendation 1; the whole file is written back
    r = run("mode=0\nrec_mode=1\nres=2\nup=0\nrec_up=1\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0, "② 缺 rec_res：玩家设的 0/2/0 照留，实为 %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    CHECK(r.fb == 1 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "② 补上 rec_res=1 写回整份，实为\n%s", r.file);
    // ③ 一个 rec_* 越界：同缺 | (3) one rec_* out of range: same as missing
    r = run(FULL(0, 1, 2, 1, 0, 7) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "③ rec_up=7 越界：只把它补成 1，实为 %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // ④ 当前值缺 / 越界、推荐在：回存着的推荐 | (4) current value missing / out of range, recommendation present: back to the stored recommendation
    r = run("mode=5\nrec_mode=0\nrec_res=2\nup=1\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 1, "④ mode=5 越界、res 缺：回存着的推荐 0、2，up 照读 1，实为 %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    // ⑤ 一个键两样都缺：当前值按首次启动的（2），推荐按推荐（1）；其余照读 | (5) both lines of one key missing: the current value from the first boot (2), the recommendation from the recommendation (1); the rest as read
    r = run("mode=0\nrec_mode=0\nup=0\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 0, 2, 1, 0, 0) NEWREC), "⑤ res 两行都缺：res=2、rec_res=1，其余照读，实为 %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // ⑥ 没有文件 / 空文件：首次启动，三项都按首次启动 | (6) no file / empty file: first boot, all three from the first boot
    r = run(NULL);
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 1 && !strcmp(r.file, FULL(1, 1, 2, 1, 0, 1) NEWREC), "⑥ 没有文件：首次启动 1/2/0，推荐 1/1/1 写盘，实为 %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    r = run("");
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0, "⑥ 空文件：同首次启动，实为 %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    // ⑦ 更新前的旧文件（只有前三个键）：三项照留，后加的键按 front_video_rec 补（aa 1、aniso 3、color 没有推荐 = 0），整份写回
    // (7) a file from before the update (only the first three keys): those three stay, the later keys are filled from front_video_rec (aa 1, aniso 3, color with no recommendation = 0), whole file written back
    r = run(FULL(0, 1, 2, 1, 0, 1));
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && r.v[3] == 1 && r.v[4] == 3 && r.v[5] == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC),
          "⑦ 旧文件：0/2/0 照留，aa/aniso/color 补成 1/3/0，实为 %d/%d/%d %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.file);
    // ⑧ 后加的键照读；越界的那一个回存着的推荐 | (8) the later keys are read as stored; an out-of-range one goes back to its stored recommendation
    r = run(FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 2, 3, 1, 1) S3(0, 0) RV);
    CHECK(r.v[3] == 0 && r.v[4] == 2 && r.v[5] == 1 && r.fb == 0, "⑧ 照读 aa/aniso/color = 0/2/1、没问推荐，实为 %d/%d/%d（问了 %d 次）", r.v[3], r.v[4], r.v[5], r.fb);
    r = run(FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 9, 3, 1, 1) S3(0, 0) RV);
    CHECK(r.v[4] == 3 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 3, 3, 1, 1) S3(0, 0) RV), "⑧ aniso=9 越界：回推荐 3，实为 %d\n%s", r.v[4], r.file);
    // ⑨ 推荐值版本旧（没有 rec_v）、玩家停在旧推荐上（各键都等于旧推荐 0）：一起换成新推荐（1/1/1、aa 1、aniso 3、color 0）；s3d 缺且没用原生出图 → 0
    // (9) old recommendation version (no rec_v), player on the old recommendation (every key equal to it, 0): moved to the new one (1/1/1, aa 1, aniso 3, color 0); s3d missing and not on Native → 0
    r = run(FULL(0, 0, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 1 && r.v[2] == 1 && r.v[3] == 1 && r.v[4] == 3 && r.v[5] == 0 && r.v[6] == 0 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC),
          "⑨ 停在旧推荐：换成新推荐 1/1/1 1/3/0 0，实为 %d/%d/%d %d/%d/%d %d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.v[6], r.file);
    // ⑩ 版本旧、玩家改过（mode=1 ≠ 旧推荐 0）：设置照留，只换推荐 | (10) old version, player changed a setting (mode=1 ≠ old recommendation 0): settings kept, only the recommendation updated
    r = run(FULL(1, 0, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 0 && r.v[2] == 0 && r.v[3] == 0 && r.v[4] == 0 && !strcmp(r.file, FULL(1, 1, 0, 1, 0, 1) TAIL(0, 1, 0, 3, 0, 0) S3(0, 0) RV),
          "⑩ 改过的照留、推荐换新，实为 %d/%d/%d %d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.file);
    // ⑪ 原生出图（res=3）读得进；s3d 照读 | (11) native output (res=3) is accepted; s3d read as stored
    r = run(FULL(0, 1, 3, 1, 0, 1) NEWREC);
    CHECK(r.v[1] == 3 && r.fb == 0, "⑪ res=3（原生）照读、没问推荐，实为 %d（问了 %d 次）", r.v[1], r.fb);
    r = run(FULL(0, 1, 3, 1, 0, 1) TAIL(1, 1, 3, 3, 0, 0) S3(2, 0) RV);
    CHECK(r.v[6] == 2, "⑪ s3d=2 照读，实为 %d", r.v[6]);
    file_put(NULL); rmdir(g_dir);
    printf("t_video: %d 项，%d 失败\n", checks, fails);
    return fails != 0;
}
