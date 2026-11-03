// Phone picture settings unit test (takes seconds): dm_video.c built on the Mac with -DDM_PHONE=1, the only build that compiles its phone branches outside the iOS package.
// It includes dm_video.c to reach the static helpers (car_of / val_of, tier_of, perf_up, preset_of, apply_preset, g_pend / g_rec). The front end's hooks are the strong
// definitions in t_video_phone_stubs.c, which mirror front_ios.m and win over dm_video.c's weak ones at link time (weak definitions are never inlined).
// Loading happens once per process (static state), so each case runs in a forked child; the pure mapping checks are the exception. Every expectation is worked out by hand from the rules.
//   (1) no file: first boot = 75% + MetalFX, SMAA, 16x, Accurate; the file is written and the colour handed to the front end
//   (2) res 0..4 load as stored: 3D at 75 / 85 / 100 / 67 / 50 % (res 4 is out of range on the Mac)
//   (3) res 5 is out of range: back to the stored recommendation (75%), the file is rewritten
//   (4) car_of / val_of: the 3D Resolution row lists 50 / 67 / 75 / 85 / 100 % while the stored values keep their meaning
//   (5) presets: on the phone Performance / Quality set only resolution and upscaling; Recommended, Custom, the dirty bit
//   (6) a recommendation-version bump while on the old Recommended: moved to the new one
//   (7) the same bump with a resolution the player chose: settings kept, only the recommendation updated
//   (8) the phone's 3D scale comes from res and ignores s3d
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../dm_video.c"
#if !DM_PHONE
#error "t_video_phone needs -DDM_PHONE=1"
#endif
extern char g_dir[512];
extern int g_fb_calls, g_color_calls, g_color_last;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void file_put(const char *txt) { char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); if (!txt) { unlink(p); return; } FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); }
static const char *file_get(void) { static char b[700]; char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); FILE *f = fopen(p, "r"); if (!f) return ""; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }
// One load in a child: every key's value, the 3D scale, how often the front end was asked (first boot, colour), the file after write-back, and what `more` records
typedef struct { int v[NV], fb, cc, cl, x[24]; float s3; char file[700]; } result;
static result child_run(const char *txt, void (*more)(result *)) {
    file_put(txt); int pp[2]; result r; memset(&r, 0, sizeof r); if (pipe(pp)) { r.v[0] = -9; return r; }
    pid_t pid = fork();
    if (!pid) { close(pp[0]); result x; memset(&x, 0, sizeof x);
        for (int i = 0; i < NV; i++) x.v[i] = dm_video_get(k_keys[i]);
        x.s3 = dm_video_scale3d(); x.fb = g_fb_calls; x.cc = g_color_calls; x.cl = g_color_last;
        strlcpy(x.file, file_get(), sizeof x.file); if (more) more(&x);
        if (write(pp[1], &x, sizeof x) != (ssize_t)sizeof x) _exit(1); _exit(0); }
    close(pp[1]); if (read(pp[0], &r, sizeof r) != (ssize_t)sizeof r) r.v[0] = -9; close(pp[0]); waitpid(pid, NULL, 0); return r;
}
#define FULL(m, rm, s, rs, u, ru) "mode=" #m "\nrec_mode=" #rm "\nres=" #s "\nrec_res=" #rs "\nup=" #u "\nrec_up=" #ru "\n"
#define TAIL(a, ra, n, rn, c, rc) "aa=" #a "\nrec_aa=" #ra "\naniso=" #n "\nrec_aniso=" #rn "\ncolor=" #c "\nrec_color=" #rc "\n"
#define S3(s, rs) "s3d=" #s "\nrec_s3d=" #rs "\n"
#define ES(e, re) "envsh=" #e "\nrec_envsh=" #re "\n"
#define SQ(q, rq) "shq=" #q "\nrec_shq=" #rq "\n"
#define RV "rec_v=7\n"   // REC_V in dm_video.c: a bump changes this line and the cases below in the same commit
// The phone's first-boot file (front_ios.m: 75% + MetalFX; SMAA, 16x, Accurate; no s3d / envsh / shq recommendation), with res as given
#define CUR(s) FULL(0, 0, s, 0, 1, 1) TAIL(1, 1, 3, 3, 1, 1) S3(0, 0) ES(0, 0) SQ(0, 0) RV

// (5) in the child, right after the first-boot load (every pending value -1)
static void presets(result *x) {
    int n = 0, other = 0; static const int ks[] = { K_MODE, K_AA, K_ANISO, K_COLOR, K_S3D, K_ESH, K_SHQ };
    x->x[n++] = preset_of();                                                                    // 0
    x->x[n++] = tier_of(K_RES, 0); x->x[n++] = tier_of(K_RES, 1);                               // 1, 2
    x->x[n++] = tier_of(K_UP, 0); x->x[n++] = tier_of(K_UP, 1);                                 // 3, 4
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) for (int t = 0; t < 2; t++) other += tier_of(ks[i], t) != -1;
    x->x[n++] = other; x->x[n++] = perf_up();                                                   // 5, 6
    apply_preset(P_PERF); other = 0; for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) other += atomic_load(&g_pend[ks[i]]) != -1;
    x->x[n++] = atomic_load(&g_pend[K_RES]); x->x[n++] = atomic_load(&g_pend[K_UP]); x->x[n++] = other;   // 7, 8, 9
    x->x[n++] = preset_of(); x->x[n++] = dm_video_dirty();                                      // 10, 11
    apply_preset(P_QUAL);
    x->x[n++] = atomic_load(&g_pend[K_RES]); x->x[n++] = atomic_load(&g_pend[K_UP]); x->x[n++] = preset_of();   // 12, 13, 14
    atomic_store(&g_pend[K_RES], 1); x->x[n++] = preset_of();                                    // 15
    apply_preset(P_REC); x->x[n++] = preset_of(); x->x[n++] = dm_video_dirty();                // 16, 17
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "%s/t_video_phone.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_dir, 0755);
    result r;
    // (1) no file: first boot
    r = child_run(NULL, NULL);
    CHECK(r.v[K_MODE] == 0 && r.v[K_RES] == 0 && r.v[K_UP] == 1 && r.v[K_AA] == 1 && r.v[K_ANISO] == 3 && r.v[K_COLOR] == 1 && r.v[K_S3D] == 0 && r.v[K_ESH] == 0 && r.v[K_SHQ] == 0,
          "(1) first boot: expect mode 0 res 0 up 1 aa 1 aniso 3 color 1 s3d 0 envsh 0 shq 0, got %d %d %d %d %d %d %d %d %d",
          r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.v[6], r.v[7], r.v[8]);
    CHECK(r.fb == 1 && !strcmp(r.file, CUR(0)), "(1) first boot: the recommendation asked once (%d) and written, got\n%s", r.fb, r.file);
    CHECK(r.s3 == 0.75f, "(1) first boot: 3D scale 0.75, got %g", r.s3);
    CHECK(r.cc == 1 && r.cl == 1, "(1) first boot: Accurate handed to the front end once, got %d calls, last %d", r.cc, r.cl);
    r = child_run("", NULL);
    CHECK(r.v[K_RES] == 0 && r.v[K_UP] == 1 && r.fb == 1 && !strcmp(r.file, CUR(0)), "(1) empty file: as a first boot, got res %d up %d (asked %d times)\n%s", r.v[K_RES], r.v[K_UP], r.fb, r.file);

    // (2) a complete current file loads as stored, every res tier included
    { static const char *const f[5] = { CUR(0), CUR(1), CUR(2), CUR(3), CUR(4) }; static const float s[5] = { 0.75f, 0.85f, 1.0f, 0.67f, 0.5f };
      for (int t = 0; t < 5; t++) { r = child_run(f[t], NULL);
          CHECK(r.v[K_RES] == t && r.s3 == s[t] && r.fb == 0 && !strcmp(r.file, f[t]) && r.cc == 1,
                "(2) res=%d: expect res %d, 3D scale %g, nothing asked or written, Accurate handed over; got res %d, scale %g, asked %d, colour calls %d\n%s",
                t, t, s[t], r.v[K_RES], r.s3, r.fb, r.cc, r.file); } }

    // (3) res=5 is out of range: back to the stored recommendation (0 = 75%), the file rewritten with res=0
    r = child_run(FULL(0, 0, 5, 0, 1, 1) TAIL(1, 1, 3, 3, 1, 1) S3(0, 0) ES(0, 0) SQ(0, 0) RV, NULL);
    CHECK(r.v[K_RES] == 0 && r.s3 == 0.75f && !strcmp(r.file, CUR(0)), "(3) res=5: back to res 0 (75%%), got res %d scale %g\n%s", r.v[K_RES], r.s3, r.file);

    // (4) the 3D Resolution row (row 1 = res) against the stored values; every other row is the identity
    { static const int car[5] = { 2, 3, 4, 1, 0 }, val[5] = { 4, 3, 0, 1, 2 };
      for (int v = 0; v < 5; v++) {
          CHECK(car_of(1, v) == car[v], "(4) car_of(res, %d): expect %d, got %d", v, car[v], car_of(1, v));
          CHECK(val_of(1, v) == val[v], "(4) val_of(res, %d): expect %d, got %d", v, val[v], val_of(1, v));
          CHECK(val_of(1, car_of(1, v)) == v, "(4) val_of(car_of(res, %d)) round trip: got %d", v, val_of(1, car_of(1, v))); }
      CHECK(car_of(2, 1) == 1 && val_of(2, 1) == 1, "(4) the color row is the identity: got car %d val %d", car_of(2, 1), val_of(2, 1));
      CHECK(car_of(1, 7) == 7 && val_of(1, 7) == 7, "(4) an out-of-range value passes through: got car %d val %d", car_of(1, 7), val_of(1, 7)); }

    // (5) presets, after the first-boot load
    r = child_run(NULL, presets);
    CHECK(r.x[0] == P_REC, "(5) first boot shows Recommended, got %d", r.x[0]);
    CHECK(r.x[1] == 0 && r.x[2] == 2 && r.x[3] == 0 && r.x[4] == 1, "(5) tiers: res 0 / 2 (75 / 100%%), up 0 / 1 (Standard / MetalFX); got res %d / %d, up %d / %d", r.x[1], r.x[2], r.x[3], r.x[4]);
    CHECK(r.x[5] == 0, "(5) the phone presets leave mode, aa, aniso, color, s3d, envsh and shq alone: %d tiers set", r.x[5]);
    CHECK(r.x[6] == 0, "(5) Performance upscaling on the phone is Standard, got %d", r.x[6]);
    CHECK(r.x[7] == 0 && r.x[8] == 0 && r.x[9] == 0 && r.x[10] == P_PERF && r.x[11] == 1,
          "(5) Performance: pending res 0 up 0, nothing else, shown as Performance, dirty; got res %d up %d, %d others, preset %d, dirty %d", r.x[7], r.x[8], r.x[9], r.x[10], r.x[11]);
    CHECK(r.x[12] == 2 && r.x[13] == 1 && r.x[14] == P_QUAL, "(5) Quality: pending res 2 up 1, shown as Quality; got res %d up %d, preset %d", r.x[12], r.x[13], r.x[14]);
    CHECK(r.x[15] == P_CUSTOM, "(5) res 85%% with MetalFX matches no preset: Custom, got %d", r.x[15]);
    CHECK(r.x[16] == P_REC && r.x[17] == 0, "(5) Recommended again: shown as Recommended, not dirty; got preset %d, dirty %d", r.x[16], r.x[17]);

    // (6) an old recommendation version (2) with the player on it: moved to the new one (SMAA, 16x, Accurate), s3d / envsh / shq filled with 0
    r = child_run(FULL(0, 0, 0, 0, 1, 1) TAIL(0, 0, 0, 0, 0, 0) "rec_v=2\n", NULL);
    CHECK(r.v[K_AA] == 1 && r.v[K_ANISO] == 3 && r.v[K_COLOR] == 1 && r.v[K_S3D] == 0 && r.v[K_ESH] == 0 && r.v[K_SHQ] == 0 && !strcmp(r.file, CUR(0)),
          "(6) on the old Recommended: moved to aa 1 aniso 3 color 1, got aa %d aniso %d color %d s3d %d envsh %d shq %d\n%s",
          r.v[K_AA], r.v[K_ANISO], r.v[K_COLOR], r.v[K_S3D], r.v[K_ESH], r.v[K_SHQ], r.file);
    CHECK(r.cc == 1 && r.cl == 1, "(6) Accurate handed to the front end once, got %d calls, last %d", r.cc, r.cl);

    // (7) the same with res=2 (100%) chosen by the player: settings kept, only the recommendation updated
    r = child_run(FULL(0, 0, 2, 0, 1, 1) TAIL(0, 0, 0, 0, 0, 0) "rec_v=2\n", NULL);
    CHECK(r.v[K_RES] == 2 && r.v[K_AA] == 0 && r.v[K_ANISO] == 0 && r.v[K_COLOR] == 0 && r.cc == 0
          && !strcmp(r.file, FULL(0, 0, 2, 0, 1, 1) TAIL(0, 1, 0, 3, 0, 1) S3(0, 0) ES(0, 0) SQ(0, 0) RV),
          "(7) changed settings kept: expect res 2, aa / aniso / color 0, no colour call; got res %d aa %d aniso %d color %d, %d colour calls\n%s",
          r.v[K_RES], r.v[K_AA], r.v[K_ANISO], r.v[K_COLOR], r.cc, r.file);

    // (8) the phone ignores s3d: res 0 with s3d 2 stored is still 75%
    r = child_run(FULL(0, 0, 0, 0, 1, 1) TAIL(1, 1, 3, 3, 1, 1) S3(2, 0) ES(0, 0) SQ(0, 0) RV, NULL);
    CHECK(r.v[K_S3D] == 2 && r.s3 == 0.75f, "(8) s3d=2 stored, res 0: 3D scale 0.75, got s3d %d scale %g", r.v[K_S3D], r.s3);

    file_put(NULL); rmdir(g_dir);
    printf("t_video_phone: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
