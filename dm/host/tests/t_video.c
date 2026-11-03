// Picture settings unit test for the Mac build of dm_video.c (takes seconds). It includes dm_video.c, so the static helpers and state are reachable
// (preset_of, apply_preset, tier_of, car_of / val_of, g_val / g_pend / g_rec); the front end's hooks, the settings directory and a fake custom settings
// store are the strong definitions in t_video_fe.c, whose g_fe picks the generic stub (G) or front_mac.m on this M6 (M). An event recorder hangs on
// g_dm_event_fn, so the 'picture' and 'video_row' events are checked too. Loading happens once per process (static state), so every case runs in a
// forked child, and run_do runs a case's own steps (Apply, Discard, Default, presets) in that child after the load. Every expectation is worked out
// by hand from the rules. The phone build of dm_video.c is t_video_phone (t_video_phone.c + t_video_phone_stubs.c, mirroring front_ios.m).
//   (1)-(8)   sw_video.txt is read key by key: only a missing or out-of-range key is filled from the first-boot values; only a file with no key at all
//             counts as a first boot
//   (9)-(16)  the recommendation version (rec_v): a player still on the old Recommended moves to the new one, changed settings stay
//   (18j)     the Mac's rows map their values to the selector one to one
//   (21)      the environment-shadow and shadow-map answers are latched once the settings are known (thread.c's KESHADOW / KSHSIZE slots)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../dm_video.c"
#if DM_PHONE
#error "t_video is the Mac build; the phone build is t_video_phone"
#endif
extern char g_dir[512];
extern int g_nodir, g_fe, g_mfx, g_rec_id, g_rec_res, g_fb_calls, g_full_calls, g_full_last, g_color_calls, g_color_last;
extern int g_tshown, g_cdef_q3d, g_cval[2], g_cpend[2], g_cset[2];

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void file_put(const char *txt) { char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); if (!txt) { unlink(p); return; } FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); }
static const char *file_get(void) { static char b[1200]; char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); FILE *f = fopen(p, "r"); if (!f) return ""; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }

// The event recorder: the last 'picture' event (how and its eleven values) and the last 'video_row' event, plus how many of each were sent
static struct { int n, v[11], rn, rv, rp; char how[16], row[16]; } g_ev;
static void ev_rec(const char *ev, const char *sig, ...) {
    (void)sig; va_list ap; va_start(ap, sig);
    if (!strcmp(ev, "picture")) { const char *h = va_arg(ap, const char *); strlcpy(g_ev.how, h ? h : "(null)", sizeof g_ev.how); for (int i = 0; i < 11; i++) g_ev.v[i] = va_arg(ap, int); g_ev.n++; }
    else if (!strcmp(ev, "video_row")) { const char *r = va_arg(ap, const char *); strlcpy(g_ev.row, r ? r : "(null)", sizeof g_ev.row); g_ev.rv = va_arg(ap, int); g_ev.rp = va_arg(ap, int); g_ev.rn++; }
    va_end(ap);
}

// One load in a child: every key's value, how often the front end was asked (first boot, full screen, colour), the file after the load (file0) and
// after the case's own steps (file), the last 'picture' event, and what `act` records in x[]
typedef struct { int v[NV], fb, full, full_last, cc, cl, evn, ev[11], x[32]; char how[16], file0[1200], file[1200]; } result;
static result child(void (*act)(result *)) {
    int pp[2]; result r; memset(&r, 0, sizeof r); if (pipe(pp)) { r.v[0] = -9; return r; }
    pid_t pid = fork();
    if (!pid) { close(pp[0]); result x; memset(&x, 0, sizeof x);
        for (int i = 0; i < NV; i++) x.v[i] = dm_video_get(k_keys[i]);
        strlcpy(x.file0, file_get(), sizeof x.file0);
        if (act) act(&x);
        x.fb = g_fb_calls; x.full = g_full_calls; x.full_last = g_full_last; x.cc = g_color_calls; x.cl = g_color_last;
        x.evn = g_ev.n; memcpy(x.ev, g_ev.v, sizeof x.ev); strlcpy(x.how, g_ev.how, sizeof x.how);
        strlcpy(x.file, file_get(), sizeof x.file);
        const char *b = (const char *)&x; size_t n = sizeof x;
        while (n) { ssize_t w = write(pp[1], b, n); if (w <= 0) _exit(1); b += w; n -= (size_t)w; }
        _exit(0); }
    close(pp[1]); char *b = (char *)&r; size_t n = sizeof r, got = 0;
    while (got < n) { ssize_t k = read(pp[0], b + got, n - got); if (k <= 0) break; got += (size_t)k; }
    if (got != n) r.v[0] = -9;
    close(pp[0]); waitpid(pid, NULL, 0); return r;
}
static result run_do(const char *txt, void (*act)(result *)) { file_put(txt); return child(act); }
static result run(const char *txt) { return run_do(txt, NULL); }
#define FULL(m, rm, s, rs, u, ru) "mode=" #m "\nrec_mode=" #rm "\nres=" #s "\nrec_res=" #rs "\nup=" #u "\nrec_up=" #ru "\n"
#define TAIL(a, ra, n, rn, c, rc) "aa=" #a "\nrec_aa=" #ra "\naniso=" #n "\nrec_aniso=" #rn "\ncolor=" #c "\nrec_color=" #rc "\n"
#define S3(s, rs) "s3d=" #s "\nrec_s3d=" #rs "\n"
#define ES(e, re) "envsh=" #e "\nrec_envsh=" #re "\n"
#define SQ(q, rq) "shq=" #q "\nrec_shq=" #rq "\n"
#define RV "rec_v=7\n"   // REC_V in dm_video.c (without it = version 1, worked out again at load): a bump changes this line, t_video_phone's and the cases below in one commit
#define NEWREC TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV   // how the later keys are filled when missing (G's front_video_rec)

// (21) the shadow latch: the first answers once the settings are known hold for the session. In a child: set or clear DM_ENV_SHADOWS / DM_SHADOW_SIZE
// (NULL = unset), ask both; then, to prove they are latched, flip both switches (DM_ENV_SHADOWS to the opposite, DM_SHADOW_SIZE=1234) and ask again.
// With nodir the settings directory is unknown at the first ask; the second ask comes after it became known, with the switches untouched
typedef struct { int e1, s1, e2, s2; } latch;
static latch run_latch(const char *txt, const char *esh, const char *shsz, int nodir) {
    file_put(txt); int pp[2]; latch l = { -9, -9, -9, -9 }; if (pipe(pp)) return l;
    pid_t pid = fork();
    if (!pid) { close(pp[0]); latch x;
        if (esh) setenv("DM_ENV_SHADOWS", esh, 1); else unsetenv("DM_ENV_SHADOWS");
        if (shsz) setenv("DM_SHADOW_SIZE", shsz, 1); else unsetenv("DM_SHADOW_SIZE");
        g_nodir = nodir; x.e1 = dm_video_env_shadows(); x.s1 = dm_video_shadow_size();
        if (nodir) g_nodir = 0; else { setenv("DM_ENV_SHADOWS", x.e1 ? "0" : "1", 1); setenv("DM_SHADOW_SIZE", "1234", 1); }
        x.e2 = dm_video_env_shadows(); x.s2 = dm_video_shadow_size();
        if (write(pp[1], &x, sizeof x) != (ssize_t)sizeof x) _exit(1); _exit(0); }
    close(pp[1]); if (read(pp[0], &l, sizeof l) != (ssize_t)sizeof l) l.e1 = -9; close(pp[0]); waitpid(pid, NULL, 0); return l;
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "%s/t_video.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_dir, 0755);
    g_dm_event_fn = ev_rec;
    result r;
    // ---- G: the generic front end
    // (1) complete file: read as is, no recommendation asked, no write-back
    r = run(FULL(0, 1, 2, 1, 0, 1) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(1) complete: 0/2/0, nothing asked (%d times), file unchanged", r.fb);
    // (2) one rec_* missing: the player's three values stay (the old code reset all to the first boot = 1/2/0), only rec_res is filled with the
    // recommendation 1; the whole file is written back
    r = run("mode=0\nrec_mode=1\nres=2\nup=0\nrec_up=1\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0, "(2) rec_res missing: the player's 0/2/0 stays, got %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    CHECK(r.fb == 1 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(2) rec_res=1 filled and the whole file written back, got\n%s", r.file);
    // (3) one rec_* out of range: same as missing
    r = run(FULL(0, 1, 2, 1, 0, 7) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(3) rec_up=7 out of range: only it becomes 1, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // (4) current value missing / out of range, recommendation present: back to the stored recommendation
    r = run("mode=5\nrec_mode=0\nrec_res=2\nup=1\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 1, "(4) mode=5 out of range, res missing: back to the stored 0, 2; up read as 1, got %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    // (5) both lines of one key missing: the current value from the first boot (2), the recommendation from the recommendation (1); the rest as read
    r = run("mode=0\nrec_mode=0\nup=0\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 0, 2, 1, 0, 0) NEWREC), "(5) both res lines missing: res=2, rec_res=1, the rest as read, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // (6) no file / empty file: first boot, all three from the first boot
    r = run(NULL);
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 1 && !strcmp(r.file, FULL(1, 1, 2, 1, 0, 1) NEWREC), "(6) no file: first boot 1/2/0, recommendation 1/1/1 saved, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    r = run("");
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0, "(6) empty file: as a first boot, got %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    // (7) a file from before the update (only the first three keys): those three stay, the later keys are filled from front_video_rec (aa 1, aniso 3,
    // color with no recommendation = 0), whole file written back
    r = run(FULL(0, 1, 2, 1, 0, 1));
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && r.v[3] == 1 && r.v[4] == 3 && r.v[5] == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC),
          "(7) older file: 0/2/0 stays, aa/aniso/color filled with 1/3/0, got %d/%d/%d %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.file);
    // (8) the later keys are read as stored; an out-of-range one goes back to its stored recommendation
    r = run(FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 2, 3, 1, 1) S3(0, 0) ES(1, 1) SQ(2, 2) RV);
    CHECK(r.v[3] == 0 && r.v[4] == 2 && r.v[5] == 1 && r.fb == 0, "(8) aa/aniso/color read as 0/2/1, nothing asked, got %d/%d/%d (asked %d times)", r.v[3], r.v[4], r.v[5], r.fb);
    r = run(FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 9, 3, 1, 1) S3(0, 0) ES(1, 1) SQ(2, 2) RV);
    CHECK(r.v[4] == 3 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 3, 3, 1, 1) S3(0, 0) ES(1, 1) SQ(2, 2) RV), "(8) aniso=9 out of range: back to the recommendation 3, got %d\n%s", r.v[4], r.file);
    // (9) old recommendation version (no rec_v), player on the old recommendation (every key equal to it, 0): moved to the new one (1/1/1, aa 1,
    // aniso 3, color 0); s3d missing and not on Native → 0
    r = run(FULL(0, 0, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 1 && r.v[2] == 1 && r.v[3] == 1 && r.v[4] == 3 && r.v[5] == 0 && r.v[6] == 0 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC),
          "(9) on the old recommendation: moved to 1/1/1 1/3/0 0, got %d/%d/%d %d/%d/%d %d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.v[6], r.file);
    // (10) old version, player changed a setting (mode=1 ≠ old recommendation 0): settings kept, only the recommendation updated
    r = run(FULL(1, 0, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 0 && r.v[2] == 0 && r.v[3] == 0 && r.v[4] == 0 && !strcmp(r.file, FULL(1, 1, 0, 1, 0, 1) TAIL(0, 1, 0, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(10) changed settings kept, recommendation updated, got %d/%d/%d %d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.file);
    // (11) native output (res=3) is accepted; s3d read as stored
    r = run(FULL(0, 1, 3, 1, 0, 1) NEWREC);
    CHECK(r.v[1] == 3 && r.fb == 0, "(11) res=3 (Native) read as stored, nothing asked, got %d (asked %d times)", r.v[1], r.fb);
    r = run(FULL(0, 1, 3, 1, 0, 1) TAIL(1, 1, 3, 3, 0, 0) S3(2, 0) ES(1, 1) SQ(2, 2) RV);
    CHECK(r.v[6] == 2, "(11) s3d=2 read as stored, got %d", r.v[6]);
    // (12) a version-3 file on the Recommended of the day (no envsh yet): moved to the current version, environment shadows on with the recommendation
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) "rec_v=3\n");
    CHECK(r.v[7] == 1 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC), "(12) version 3 on Recommended: envsh 1, got %d\n%s", r.v[7], r.file);
    // (13) envsh=0 stored under the current version: read as stored, nothing asked, nothing written
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(0, 1) SQ(2, 2) RV);
    CHECK(r.v[7] == 0 && r.fb == 0 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(0, 1) SQ(2, 2) RV), "(13) envsh=0 kept, got %d (asked %d times)\n%s", r.v[7], r.fb, r.file);
    // (14) a version-4 file on the Recommended of the day (no shq yet): moved to the current version, Ultra shadows with the recommendation
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) "rec_v=4\n");
    CHECK(r.v[8] == 2 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC), "(14) version 4 on Recommended: shq 2, got %d\n%s", r.v[8], r.file);
    // (15) shq=0 (Standard) stored under the current version: read as stored, nothing asked, nothing written; an out-of-range shq goes back to its recommendation
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(0, 2) RV);
    CHECK(r.v[8] == 0 && r.fb == 0 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(0, 2) RV), "(15) shq=0 kept, got %d (asked %d times)\n%s", r.v[8], r.fb, r.file);
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(3, 2) RV);
    CHECK(r.v[8] == 2, "(15) shq=3 out of range: back to the recommendation 2, got %d", r.v[8]);
    // (16) a version-5 file: with this test's front end (the same recommendation as before) its values stay and only rec_v moves to the current
    // version; a setting the player changed (environment shadows off) stays changed
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) "rec_v=5\n");
    CHECK(r.v[7] == 1 && r.v[8] == 2 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC), "(16) version 5 on Recommended: same values, current rec_v, got envsh %d shq %d\n%s", r.v[7], r.v[8], r.file);
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(0, 1) SQ(2, 2) "rec_v=5\n");
    CHECK(r.v[7] == 0 && r.v[8] == 2, "(16) version 5 with environment shadows turned off: kept off, got envsh %d shq %d", r.v[7], r.v[8]);

    // ---- M: front_mac.m on this M6
    // (18j) the Mac's rows map values to the selector one to one (only the phone's 3D Resolution row is reordered)
    for (int v = 0; v < 4; v++) CHECK(car_of(1, v) == v && val_of(1, v) == v, "(18j) res row on the Mac is the identity: car_of(1, %d) = %d, val_of = %d", v, car_of(1, v), val_of(1, v));

    // ---- (21) the shadow latch (G)
#define LATCH_FILE(e, re, q, rq) FULL(1, 1, 3, 3, 1, 1) TAIL(1, 1, 3, 3, 1, 1) S3(0, 0) ES(e, re) SQ(q, rq) RV
    { latch l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, NULL, 0);
      CHECK(l.e1 == 1 && l.s1 == 4096 && l.e2 == 1 && l.s2 == 4096, "(21) envsh 1, Ultra: 1 / 4096, then latched 1 / 4096; got %d / %d, then %d / %d", l.e1, l.s1, l.e2, l.s2);
      l = run_latch(LATCH_FILE(1, 1, 1, 2), NULL, NULL, 0);
      CHECK(l.s1 == 3072 && l.s2 == 3072, "(21) High: 3072, latched; got %d then %d", l.s1, l.s2);
      l = run_latch(LATCH_FILE(1, 1, 0, 2), NULL, NULL, 0);
      CHECK(l.s1 == 2100 && l.s2 == 2100, "(21) Standard: 2100 (the Switch's edge), latched; got %d then %d", l.s1, l.s2);
      l = run_latch(LATCH_FILE(0, 1, 2, 2), "1", NULL, 0);
      CHECK(l.e1 == 1 && l.e2 == 1, "(21) envsh 0 stored, DM_ENV_SHADOWS=1 forces it on, latched; got %d then %d", l.e1, l.e2);
      l = run_latch(LATCH_FILE(1, 1, 2, 2), "0", NULL, 0);
      CHECK(l.e1 == 0 && l.e2 == 0, "(21) envsh 1 stored, DM_ENV_SHADOWS=0 forces it off, latched; got %d then %d", l.e1, l.e2);
      static const char *const sz[3] = { "100", "9000", "3000" }; static const int want[3] = { 512, 8192, 3000 };
      for (int i = 0; i < 3; i++) { l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, sz[i], 0);
          CHECK(l.s1 == want[i] && l.s2 == want[i], "(21) DM_SHADOW_SIZE=%s: %d, latched; got %d then %d", sz[i], want[i], l.s1, l.s2); }
      l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, NULL, 1);
      CHECK(l.e1 == 0 && l.s1 == 2100 && l.e2 == 1 && l.s2 == 4096,
            "(21) settings directory unknown: 0 / 2100 (the Switch look), not latched, so 1 / 4096 once it is known; got %d / %d, then %d / %d", l.e1, l.s1, l.e2, l.s2); }

    file_put(NULL); rmdir(g_dir);
    printf("t_video: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
