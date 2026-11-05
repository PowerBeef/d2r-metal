// Picture settings unit test for the Mac build of dm_video.c (takes seconds). It includes dm_video.c, so the static helpers and state are reachable
// (preset_of, apply_preset, tier_of, car_of / val_of, g_val / g_pend / g_rec); the front end's hooks, the settings directory and a fake custom settings
// store are the strong definitions in t_video_fe.c, whose g_fe picks the generic stub (G) or front_mac.m on this M6 (M). An event recorder hangs on
// g_dm_event_fn, so the 'picture' and 'video_row' events are checked too. Loading happens once per process (static state), so every case runs in a
// forked child, and run_do runs a case's own steps (Apply, Discard, Default, presets) in that child after the load. Every expectation is worked out
// by hand from the rules. The phone build of dm_video.c is t_video_phone (t_video_phone.c + t_video_phone_stubs.c, mirroring front_ios.m).
//   (1)-(8)   sw_video.txt is read key by key: only a missing or out-of-range key is filled from the first-boot values; only a file with no key at all
//             counts as a first boot
//   (9)-(16)  the recommendation version (rec_v): a player still on the old Recommended moves to the new one, changed settings stay
//   (17)      rec_id: the recommendation is worked out again when the machine (GPU, screen) changed; the 'picture' event's how
//   (19)      loading is hardened: an unreadable file, junk values, a newer version, unknown keys, over-long lines
//   (18)      M: the presets, Apply / Discard / Default and the display preferences on this M6
//   (18j)     the Mac's rows map their values to the selector one to one
//   (21)      the environment-shadow and shadow-map answers are latched once the settings are known (thread.c's KESHADOW / KSHSIZE slots)
//   (22)      the Frame Rate row's labels name the screen's real rates (fps_labels), and keep the layout's 60 / 30 fps where those are the rates
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
extern int g_fgok; extern int g_cval[], g_cpend[];
extern int g_nodir, g_fe, g_mfx, g_fe_rec_id, g_fe_rec_res, g_fb_calls, g_full_calls, g_full_last, g_color_calls, g_color_last;
extern int g_tshown, g_cdef_q3d, g_cval[2], g_cpend[2], g_cset[2];

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void file_put(const char *txt) { char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); if (!txt) { unlink(p); return; } FILE *f = fopen(p, "w"); fputs(txt, f); fclose(f); }
static void file_putn(const char *b, size_t n) { char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); FILE *f = fopen(p, "w"); fwrite(b, 1, n, f); fclose(f); }   // bytes, NULs included
static const char *file_get(void) { static char b[1200]; char p[600]; snprintf(p, sizeof p, "%s/sw_video.txt", g_dir); FILE *f = fopen(p, "r"); if (!f) return ""; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b; }

// The event recorder: the last 'picture' event (how and its eleven values) and the last 'video_row' event, plus how many of each were sent
static struct { int n, v[12], rn, rv, rp; char how[16], row[16]; } g_ev;
static void ev_rec(const char *ev, const char *sig, ...) {
    (void)sig; va_list ap; va_start(ap, sig);
    if (!strcmp(ev, "picture")) { const char *h = va_arg(ap, const char *); strlcpy(g_ev.how, h ? h : "(null)", sizeof g_ev.how); for (int i = 0; i < 12; i++) g_ev.v[i] = va_arg(ap, int); g_ev.n++; }
    else if (!strcmp(ev, "video_row")) { const char *r = va_arg(ap, const char *); strlcpy(g_ev.row, r ? r : "(null)", sizeof g_ev.row); g_ev.rv = va_arg(ap, int); g_ev.rp = va_arg(ap, int); g_ev.rn++; }
    va_end(ap);
}

// One load in a child: every key's value, how often the front end was asked (first boot, full screen, colour), the file after the load (file0) and
// after the case's own steps (file), the last 'picture' event, and what `act` records in x[]
typedef struct { int v[NV], fb, full, full_last, cc, cl, evn, ev[12], x[32]; char how[16], file0[1200], file[1200]; } result;
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
#define SQ(q, rq) "shq=" #q "\nrec_shq=" #rq "\nfg=0\nrec_fg=0\n"   // fg, the key after shq, stays 0 unless a case says so (G recommends none, M only with g_fgok)
#define RV "rec_v=9\n"   // REC_V in dm_video.c (without it = version 1, worked out again at load): a bump changes this line, t_video_phone's and the cases below in one commit
#define NEWREC TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV   // how the later keys are filled when missing (G's front_video_rec)

// (21) the shadow latch: the first answers once the settings are known hold for the session. In a child: set or clear DM_ENV_SHADOWS / DM_SHADOW_SIZE
// (NULL = unset), ask both; then, to prove they are latched, flip both switches (DM_ENV_SHADOWS to the opposite, DM_SHADOW_SIZE=1234) and ask again.
// With nodir the settings directory is unknown at the first ask; the second ask comes after it became known, with the switches untouched
typedef struct { int e1, s1, e2, s2, fe, fs; } latch;   // fe / fs: the forced flags after the first asks (no restart hint for a forced value)
static latch run_latch(const char *txt, const char *esh, const char *shsz, int nodir) {
    file_put(txt); int pp[2]; latch l = { -9, -9, -9, -9, -9, -9 }; if (pipe(pp)) return l;
    pid_t pid = fork();
    if (!pid) { close(pp[0]); latch x;
        if (esh) setenv("DM_ENV_SHADOWS", esh, 1); else unsetenv("DM_ENV_SHADOWS");
        if (shsz) setenv("DM_SHADOW_SIZE", shsz, 1); else unsetenv("DM_SHADOW_SIZE");
        g_nodir = nodir; x.e1 = dm_video_env_shadows(); x.s1 = dm_video_shadow_size(); x.fe = atomic_load(&g_esh_forced); x.fs = atomic_load(&g_shq_forced);
        if (nodir) g_nodir = 0; else { setenv("DM_ENV_SHADOWS", x.e1 ? "0" : "1", 1); setenv("DM_SHADOW_SIZE", "1234", 1); }
        x.e2 = dm_video_env_shadows(); x.s2 = dm_video_shadow_size();
        if (write(pp[1], &x, sizeof x) != (ssize_t)sizeof x) _exit(1); _exit(0); }
    close(pp[1]); if (read(pp[0], &l, sizeof l) != (ssize_t)sizeof l) l.e1 = -9; close(pp[0]); waitpid(pid, NULL, 0); return l;
}

// A case's steps, run in the child after the load
static void act_aa0(result *x) { (void)x; atomic_store(&g_pend[K_AA], 0); dm_video_on_uimsg("OptionsPanelMessage", "AcceptChanges", ""); }
static void act_preset(result *x) { x->x[0] = preset_of(); }
static void cancel(void) { dm_video_on_uimsg("OptionsPanelMessage", "CancelChanges", ""); }
static void accept(void) { dm_video_on_uimsg("OptionsPanelMessage", "AcceptChanges", ""); }
static void act_18b(result *x) {
    apply_preset(P_PERF); static const int k[9] = { K_RES, K_UP, K_AA, K_S3D, K_ESH, K_SHQ, K_ANISO, K_MODE, K_COLOR };
    for (int i = 0; i < 9; i++) x->x[i] = atomic_load(&g_pend[k[i]]);
    x->x[9] = preset_of(); x->x[10] = dm_video_dirty(); cancel(); x->x[11] = preset_of(); x->x[12] = dm_video_dirty();
}
static void act_18c(result *x) { apply_preset(P_QUAL); x->x[0] = dm_video_dirty(); x->x[1] = preset_of(); cancel(); x->x[2] = preset_of(); }
static void act_18d(result *x) { apply_preset(P_QUAL); accept(); x->x[0] = preset_of(); }
static void act_18i(result *x) { apply_preset(P_PERF); x->x[0] = atomic_load(&g_pend[K_UP]); cancel(); apply_preset(P_QUAL); x->x[1] = atomic_load(&g_pend[K_UP]); }
static void act_18e(result *x) { x->x[0] = preset_of(); apply_preset(P_REC); x->x[1] = atomic_load(&g_pend[K_MODE]); accept(); x->x[2] = g_full_calls; x->x[3] = preset_of(); }
static void act_18f(result *x) {
    g_tshown = 1; atomic_store(&g_crows, (1 << 10) | (1 << 12));   // the Video tab is on screen with its Frame Rate and Frame Drops rows
    dm_video_on_uimsg("OptionsPanelMessage", "ResetToDefaults", "");
    x->x[0] = atomic_load(&g_pend[K_MODE]); x->x[1] = g_cset[0]; x->x[2] = g_cset[1]; x->x[3] = preset_of(); accept();
}
static void act_18g(result *x) { (void)x; atomic_store(&g_pend[K_COLOR], 0); accept(); }
static void act_18k(result *x) { x->x[0] = preset_of(); apply_preset(P_REC); x->x[1] = atomic_load(&g_pend[K_COLOR]); }

// (24): what the Video tab's rows do to Frame Generation, as dm_video_pump calls them. Index of the row by its name: k_rows
static int row_of(const char *name) { for (int i = 0; i < NR; i++) if (!strcmp(k_rows[i], name)) return i; return -1; }
static void clear_pend(void) { for (int i = 0; i < NV; i++) atomic_store(&g_pend[i], -1); dm_custom_set_pending("eco_fps", -1); }
static void act_fg(result *x) {
    x->x[0] = preset_of();
    fg_rules(row_of("upscale"), 1); atomic_store(&g_pend[K_UP], 1); x->x[1] = atomic_load(&g_pend[K_FG]); x->x[2] = atomic_load(&g_pend[K_UP]); clear_pend();
    dm_custom_set_pending("eco_fps", 1); atomic_store(&g_pend[K_UP], 1); atomic_store(&g_pend[K_FG], 1); fg_rules(row_of("fg"), 1);
    x->x[3] = atomic_load(&g_pend[K_FG]); x->x[4] = atomic_load(&g_pend[K_UP]); x->x[5] = dm_custom_shown_value("eco_fps"); clear_pend();
    dm_custom_set_pending("eco_fps", 1); x->x[6] = row_want(row_of("fg")); x->x[7] = shown(K_FG); x->x[8] = preset_of(); clear_pend();
    apply_preset(P_PERF); x->x[9] = atomic_load(&g_pend[K_FG]); clear_pend(); apply_preset(P_QUAL); x->x[10] = atomic_load(&g_pend[K_FG]); clear_pend();
    atomic_store(&g_val[K_UP], 1); atomic_store(&g_val[K_FG], 0); apply_preset(P_REC); x->x[11] = atomic_load(&g_pend[K_UP]); x->x[12] = atomic_load(&g_pend[K_FG]);
}
int main(void) {
    snprintf(g_dir, sizeof g_dir, "%s/t_video.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid()); mkdir(g_dir, 0755);
    g_dm_event_fn = ev_rec;
    result r;
    // ---- G: the generic front end
    // (1) complete file: read as is, no recommendation asked, no write-back
    r = run(FULL(0, 1, 2, 1, 0, 1) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(1) complete: 0/2/0, nothing asked (%d times), file unchanged", r.fb);
    CHECK(r.evn == 1 && !strcmp(r.how, "read"), "(1) one 'picture' event, how=read; got %d, '%s'", r.evn, r.how);
    // (2) one rec_* missing: the player's three values stay (the old code reset all to the first boot = 1/2/0), only rec_res is filled with the
    // recommendation 1; the whole file is written back
    r = run("mode=0\nrec_mode=1\nres=2\nup=0\nrec_up=1\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0, "(2) rec_res missing: the player's 0/2/0 stays, got %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    CHECK(r.fb == 1 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(2) rec_res=1 filled and the whole file written back, got\n%s", r.file);
    CHECK(!strcmp(r.how, "filled"), "(2) picture how=filled, got '%s'", r.how);
    // (3) one rec_* out of range: same as missing
    r = run(FULL(0, 1, 2, 1, 0, 7) NEWREC);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) NEWREC), "(3) rec_up=7 out of range: only it becomes 1, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // (3b) Upscaling's third option (Temporal) is a value like the others, and the fourth is out of range
    r = run("mode=0\nrec_mode=1\nres=2\nup=2\nrec_up=1\n" RV);
    CHECK(r.v[2] == 2, "(3b) up=2 (Temporal) read and kept, got %d", r.v[2]);
    r = run("mode=0\nrec_mode=1\nres=2\nup=3\nrec_up=1\n" RV);
    CHECK(r.v[2] == 1, "(3b) up=3 out of range: back to the recommendation 1, got %d", r.v[2]);
    // (4) current value missing / out of range, recommendation present: back to the stored recommendation
    r = run("mode=5\nrec_mode=0\nrec_res=2\nup=1\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 1, "(4) mode=5 out of range, res missing: back to the stored 0, 2; up read as 1, got %d/%d/%d", r.v[0], r.v[1], r.v[2]);
    // (5) both lines of one key missing: the current value from the first boot (2), the recommendation from the recommendation (1); the rest as read
    r = run("mode=0\nrec_mode=0\nup=0\nrec_up=0\n" RV);
    CHECK(r.v[0] == 0 && r.v[1] == 2 && r.v[2] == 0 && !strcmp(r.file, FULL(0, 0, 2, 1, 0, 0) NEWREC), "(5) both res lines missing: res=2, rec_res=1, the rest as read, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    // (6) no file / empty file: first boot, all three from the first boot
    r = run(NULL);
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 1 && !strcmp(r.file, FULL(1, 1, 2, 1, 0, 1) NEWREC), "(6) no file: first boot 1/2/0, recommendation 1/1/1 saved, got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    { static const int want[12] = { 1, 2, 0, 1, 3, 0, 0, 1, 2, 0, REC_V, -1 }; int same = !memcmp(r.ev, want, sizeof want);
      CHECK(r.evn == 1 && !strcmp(r.how, "first") && same, "(6) picture how=first with the values loaded, rec_v %d, no rec_id; got %d events, '%s', %d %d %d … rec_v %d rec_id %d",
            REC_V, r.evn, r.how, r.ev[0], r.ev[1], r.ev[2], r.ev[10], r.ev[11]); }
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
    CHECK(!strcmp(r.how, "update"), "(9) picture how=update, got '%s'", r.how);
    // (10) old version, Display Mode changed (mode=1 ≠ old recommendation 0) but every graphics key on the old Recommended: Display Mode and Color are
    // display preferences that never decide "on Recommended" (pref_key), so the graphics keys move to the new recommendation and mode stays
    r = run(FULL(1, 0, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 1 && r.v[2] == 1 && r.v[3] == 1 && r.v[4] == 3 && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(10) display mode changed, graphics on the old Recommended: graphics moved to 1/1 1/3, mode kept 1; got %d/%d/%d %d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.file);
    // (10b) old version, a graphics key changed (res 2 ≠ 0): settings kept, only the recommendation updated
    r = run(FULL(1, 0, 2, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0 && r.v[3] == 0 && r.v[4] == 0 && !strcmp(r.file, FULL(1, 1, 2, 1, 0, 1) TAIL(0, 1, 0, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(10b) a graphics key changed: settings kept, recommendation updated; got %d/%d/%d %d/%d\n%s", r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.file);
    // (10c) old version, a windowed player (mode 0 ≠ recommended 1) on the old graphics recommendation: the graphics keys move, the window stays
    r = run(FULL(0, 1, 0, 0, 0, 0) TAIL(0, 0, 0, 0, 0, 0));
    CHECK(r.v[0] == 0 && r.v[1] == 1 && r.v[2] == 1 && !strcmp(r.file, FULL(0, 1, 1, 1, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(10c) windowed on the old graphics Recommended: graphics moved, still windowed; got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
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

    // ---- (19) loading is hardened (G)
    // (19a) an unreadable file: this launch uses the first-boot values and leaves the file as it is
    if (geteuid() != 0) {   // root reads any file
        char fp[600]; snprintf(fp, sizeof fp, "%s/sw_video.txt", g_dir); file_put(FULL(0, 1, 2, 1, 0, 1) NEWREC); chmod(fp, 0);
        r = child(NULL); chmod(fp, 0644);
        CHECK(r.v[0] == 1 && r.v[1] == 2 && r.v[2] == 0 && r.fb == 1 && !strcmp(file_get(), FULL(0, 1, 2, 1, 0, 1) NEWREC),
              "(19a) unreadable: first-boot values 1/2/0 (%d/%d/%d, asked %d times), file left alone\n%s", r.v[0], r.v[1], r.v[2], r.fb, file_get());
        CHECK(!strcmp(r.how, "unreadable"), "(19a) picture how=unreadable, got '%s'", r.how); }
    // (19b) junk values count as out of range: back to the stored recommendation
    r = run("mode=yes\nrec_mode=1\nres=\nrec_res=1\nup=0\nrec_up=1\n" TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV);
    CHECK(r.v[0] == 1 && r.v[1] == 1 && r.v[2] == 0 && strstr(r.file, FULL(1, 1, 1, 1, 0, 1)) == r.file, "(19b) mode=yes, res=: back to 1 / 1, up 0 read; got %d/%d/%d\n%s", r.v[0], r.v[1], r.v[2], r.file);
    r = run("mode=1\nrec_mode=1\nres=2x\nrec_res=1\nup=0\nrec_up=1\n" TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV);
    CHECK(r.v[1] == 1, "(19b) res=2x: back to 1, got %d", r.v[1]);
    // (19c) a newer file (rec_v 99) with a key out of range: filled in memory, the file is not rewritten (a newer version wrote it)
#define NEWER(m, rm, s, rs, u, ru) FULL(m, rm, s, rs, u, ru) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) "rec_v=99\n"
    r = run(NEWER(0, 1, 9, 1, 0, 1));
    CHECK(r.v[1] == 1 && !strcmp(r.file, NEWER(0, 1, 9, 1, 0, 1)), "(19c) newer file, res=9: res 1 in memory (%d), file unchanged\n%s", r.v[1], r.file);
    CHECK(!strcmp(r.how, "newer") && r.ev[10] == 99, "(19c) picture how=newer, rec_v 99; got '%s', %d", r.how, r.ev[10]);
    // (19d) an unknown line is kept: not rewritten at load, and written back after the known keys by the next Apply
    r = run_do(FULL(0, 1, 2, 1, 0, 1) NEWREC "future=3\n", act_aa0);
    CHECK(!strcmp(r.file0, FULL(0, 1, 2, 1, 0, 1) NEWREC "future=3\n") && !strcmp(r.file, FULL(0, 1, 2, 1, 0, 1) TAIL(0, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV "future=3\n"),
          "(19d) unknown line kept\n%s", r.file);
    CHECK(r.evn == 2 && !strcmp(r.how, "apply") && r.ev[3] == 0, "(19d) a second 'picture' event after the Apply: how=apply, aa 0; got %d events, '%s', aa %d", r.evn, r.how, r.ev[3]);
    // (19e) the same in a newer file: its rec_v and the unknown line survive the Apply
    r = run_do(NEWER(0, 1, 2, 1, 0, 1) "future=3\n", act_aa0);
    CHECK(strstr(r.file, "rec_v=99\n") && strstr(r.file, "future=3\n") && strstr(r.file, "\naa=0\n"), "(19e) newer file: rec_v=99 and future=3 kept\n%s", r.file);
    // (19f) lines longer than 62 characters (the old 64-byte read buffer) are skipped whole: no piece of one is read as a key or kept
    r = run_do(FULL(0, 1, 1, 1, 0, 1) NEWREC "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxres=2\n"
               "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy=123456789\n", act_aa0);
    CHECK(r.v[1] == 1 && !strncmp(r.file0, FULL(0, 1, 1, 1, 0, 1) NEWREC, strlen(FULL(0, 1, 1, 1, 0, 1) NEWREC)) && strstr(r.file0, "res=2\n") &&
          !strcmp(r.file, FULL(0, 1, 1, 1, 0, 1) TAIL(0, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(19f) over-long lines: res stays 1 (%d), not rewritten at load, gone after the Apply\n%s", r.v[1], r.file);
    // (19h) a line with a NUL byte in it is read up to the NUL ('res=2'), and the line after it is read as usual: the over-long test counts the bytes
    // read, not strlen (which took the NUL for the end of a long line's first piece and skipped the intact rec_res line after it)
    { static const char nul[] = "mode=0\nrec_mode=1\nres=2\0junk\nrec_res=1\nup=0\nrec_up=1\n" NEWREC;
      file_putn(nul, sizeof nul - 1); r = child(NULL);
      CHECK(r.v[K_RES] == 2 && r.fb == 0 && !strcmp(r.how, "read"), "(19h) NUL in a line: res 2 (%d), rec_res read (asked %d times), how=read ('%s')", r.v[K_RES], r.fb, r.how); }
    // (19i) the limit, as the old 64-byte buffer had it: an unknown line of 62 characters is kept, one of 63 is skipped; a last line of 62 characters
    // without its newline is kept too (written back with one)
    { char a[63], b[64], c[63], txt[700]; memset(a, 'a', 62); memcpy(a, "keep=", 5); a[62] = 0; memset(b, 'b', 63); memcpy(b, "drop=", 5); b[63] = 0;
      memset(c, 'c', 62); memcpy(c, "last=", 5); c[62] = 0;
      snprintf(txt, sizeof txt, "%s%s\n%s\n%s", FULL(0, 1, 2, 1, 0, 1) NEWREC, a, b, c); r = run_do(txt, act_aa0);
      char an[64], cn[64]; snprintf(an, sizeof an, "%s\n", a); snprintf(cn, sizeof cn, "%s\n", c);
      CHECK(strstr(r.file, an) && strstr(r.file, cn) && !strstr(r.file, "drop="), "(19i) 62 characters kept, 63 skipped, a last line without its newline kept\n%s", r.file); }

    // (19g) a newer file keeps its own rec_id: no move on another machine, nothing written at load, and an Apply keeps both its rec_v and its rec_id
    g_fe_rec_id = 2222;
    r = run_do(NEWER(0, 1, 2, 1, 0, 1) "rec_id=1111\n", act_aa0);
    { const char *tail = "rec_v=99\nrec_id=1111\n"; size_t n = strlen(r.file), m = strlen(tail);
      CHECK(r.fb == 0 && !strcmp(r.file0, NEWER(0, 1, 2, 1, 0, 1) "rec_id=1111\n") && n >= m && !strcmp(r.file + n - m, tail), "(19g) newer file: rec_id 1111 kept (asked %d times)\n%s", r.fb, r.file); }

    // ---- (17) rec_id: the machine the recommendation was worked out for (front_video_rec_id; front_mac.m hashes the GPU name, its cores, Native's size and MetalFX).
    // When it differs from the stored one, the recommendation is worked out again at launch: a player still on Recommended moves, everyone else keeps their settings
    // (17a) a current file with no rec_id yet: worked out again once (same values here), and the id saved
    r = run(FULL(1, 1, 1, 1, 1, 1) NEWREC);
    CHECK(r.fb == 1 && r.v[K_RES] == 1 && !strcmp(r.how, "rescreen") && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC "rec_id=2222\n"),
          "(17a) no rec_id: asked once (%d), res 1 (%d), how=rescreen ('%s'), rec_id saved\n%s", r.fb, r.v[K_RES], r.how, r.file);
    // (17b) the same machine: read, nothing asked or written
    r = run(FULL(1, 1, 1, 1, 1, 1) NEWREC "rec_id=2222\n");
    CHECK(r.fb == 0 && !strcmp(r.how, "read") && !strcmp(r.file, FULL(1, 1, 1, 1, 1, 1) NEWREC "rec_id=2222\n") && r.ev[11] == 2222,
          "(17b) same rec_id: nothing asked (%d), how=read ('%s'), file unchanged, event rec_id %d\n%s", r.fb, r.how, r.ev[11], r.file);
    // (17c) another machine whose recommendation is Native (res 3): a player on Recommended moves
    g_fe_rec_res = 3;
    r = run(FULL(1, 1, 1, 1, 1, 1) NEWREC "rec_id=1111\n");
    CHECK(r.v[K_RES] == 3 && !strcmp(r.how, "rescreen") && !strcmp(r.file, FULL(1, 1, 3, 3, 1, 1) TAIL(1, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV "rec_id=2222\n"),
          "(17c) moved: res 3 (%d), how=rescreen ('%s')\n%s", r.v[K_RES], r.how, r.file);
    // (17d) the same with a graphics key changed (aa 0): settings kept, only the recommendation and the id updated
    r = run(FULL(1, 1, 1, 1, 1, 1) TAIL(0, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV "rec_id=1111\n");
    CHECK(r.v[K_RES] == 1 && !strcmp(r.file, FULL(1, 1, 1, 3, 1, 1) TAIL(0, 1, 3, 3, 0, 0) S3(0, 0) ES(1, 1) SQ(2, 2) RV "rec_id=2222\n"), "(17d) changed settings kept: res 1 (%d)\n%s", r.v[K_RES], r.file);
    // (17e) a newer file on another machine: no move, nothing written
    r = run(NEWER(1, 1, 1, 1, 1, 1) "rec_id=1111\n");
    CHECK(r.v[K_RES] == 1 && !strcmp(r.how, "newer") && !strcmp(r.file, NEWER(1, 1, 1, 1, 1, 1) "rec_id=1111\n"), "(17e) newer: res 1 (%d), how=newer ('%s'), file unchanged\n%s", r.v[K_RES], r.how, r.file);
    g_fe_rec_id = -1; g_fe_rec_res = 1;

    // ---- M: front_mac.m on this M6 (Recommended = Native 100%, MetalFX, SMAA, 16x, Accurate, environment shadows, Ultra: the same values as Quality)
    g_fe = 'M';
    // (18a) first boot shows Recommended
    r = run_do(NULL, act_preset);
    CHECK(r.x[0] == P_REC && r.v[K_MODE] == 1 && r.v[K_RES] == 3 && r.v[K_COLOR] == 1, "(18a) first boot: Recommended (mode 1, Native, Accurate); got preset %d, mode %d res %d color %d", r.x[0], r.v[K_MODE], r.v[K_RES], r.v[K_COLOR]);
    // (18b) Performance: pending res 1600, MetalFX, FXAA, 3D 100%, no environment shadows, Standard; anisotropy and the display keys left alone;
    // shown as Performance and dirty; Discard → Recommended, clean
    r = run_do(NULL, act_18b);
    CHECK(r.x[0] == 0 && r.x[1] == 1 && r.x[2] == 0 && r.x[3] == 0 && r.x[4] == 0 && r.x[5] == 0 && r.x[6] == -1 && r.x[7] == -1 && r.x[8] == -1,
          "(18b) Performance pending: res 0 up 1 aa 0 s3d 0 envsh 0 shq 0, aniso / mode / color untouched; got %d %d %d %d %d %d, %d / %d / %d", r.x[0], r.x[1], r.x[2], r.x[3], r.x[4], r.x[5], r.x[6], r.x[7], r.x[8]);
    CHECK(r.x[9] == P_PERF && r.x[10] == 1 && r.x[11] == P_REC && r.x[12] == 0, "(18b) shown as Performance (%d), dirty (%d); after Discard Recommended (%d), clean (%d)", r.x[9], r.x[10], r.x[11], r.x[12]);
    // (18c) Quality on this M6 equals Recommended: nothing to apply, but the row keeps showing Quality (it used to snap back to Recommended); Discard → Recommended
    r = run_do(NULL, act_18c);
    CHECK(r.x[0] == 0 && r.x[1] == P_QUAL && r.x[2] == P_REC, "(18c) Quality picked: clean (%d), shown as Quality (%d); after Discard Recommended (%d)", r.x[0], r.x[1], r.x[2]);
    // (18d) Quality applied: still shown as Quality, and nothing changed, so the file is not rewritten
    r = run_do(NULL, act_18d);
    CHECK(r.x[0] == P_QUAL && !strcmp(r.file, r.file0), "(18d) Quality applied: shown as Quality (%d), file unchanged\n%s", r.x[0], r.file);
    // (18i) without MetalFX: Performance and Quality both set Standard upscaling
    g_mfx = 0; r = run_do(NULL, act_18i); g_mfx = 1;
    CHECK(r.x[0] == 0 && r.x[1] == 0, "(18i) no MetalFX: Performance up %d, Quality up %d (both 0)", r.x[0], r.x[1]);
#define M_REC(mode, color) "mode=" #mode "\nrec_mode=1\nres=3\nrec_res=3\nup=1\nrec_up=1\naa=1\nrec_aa=1\naniso=3\nrec_aniso=3\ncolor=" #color "\nrec_color=1\n" S3(0, 0) ES(1, 1) SQ(2, 2) RV
    // (18e) a windowed player with every other key on this Mac's Recommended: on Recommended; picking Recommended leaves the window alone, and Apply
    // does not switch to full screen
    r = run_do(M_REC(0, 1), act_18e);
    CHECK(r.x[0] == P_REC && r.x[1] == -1 && r.x[2] == 0 && r.x[3] == P_REC, "(18e) windowed on Recommended: shown as Recommended (%d), mode not pending (%d), no full-screen switch (%d calls), still Recommended (%d)",
          r.x[0], r.x[1], r.x[2], r.x[3]);
    // (18f) Default on the Video tab (windowed file): every key back to the recommendation, the window included, and the page's Frame Rate / Frame Drops
    // rows back to their defaults; Apply then switches to full screen
    r = run_do(M_REC(0, 1), act_18f);
    CHECK(r.x[0] == 1 && r.x[1] == 0 && r.x[2] == 1 && r.x[3] == P_REC, "(18f) Default: mode pending 1 (%d), eco_fps set to 0 (%d), q3d to its default 1 (%d), shown as Recommended (%d)", r.x[0], r.x[1], r.x[2], r.x[3]);
    CHECK(r.full == 1 && r.full_last == 1 && strstr(r.file, "mode=1\n") == r.file, "(18f) Apply: full screen once (%d calls, last %d), mode=1 saved\n%s", r.full, r.full_last, r.file);
    // (18g) Colour applied: handed to the front end and saved
    r = run_do(M_REC(1, 1), act_18g);
    CHECK(r.cl == 0 && strstr(r.file, "\ncolor=0\n"), "(18g) Vivid applied: front end told %d, saved\n%s", r.cl, r.file);
    // (18h) a windowed player across a rec_v bump, with 3D 75% the old recommendation: the graphics keys move to the new one (3D 100%), the window stays
    r = run("mode=0\nrec_mode=1\nres=3\nrec_res=3\nup=1\nrec_up=1\n" TAIL(1, 1, 3, 3, 1, 1) S3(2, 2) ES(1, 1) SQ(2, 2) "rec_v=6\n");   // an older version than REC_V
    CHECK(r.v[K_MODE] == 0 && r.v[K_S3D] == 0 && !strcmp(r.file, FULL(0, 1, 3, 3, 1, 1) TAIL(1, 1, 3, 3, 1, 1) S3(0, 0) ES(1, 1) SQ(2, 2) RV),
          "(18h) windowed across a bump: s3d moved to 0 (%d), still windowed (%d)\n%s", r.v[K_S3D], r.v[K_MODE], r.file);
    // (18k) a Vivid player with every graphics key on Recommended: on Recommended (Colour is a display preference on the Mac), and picking
    // Recommended leaves the colour alone
    r = run_do(M_REC(1, 0), act_18k);
    CHECK(r.x[0] == P_REC && r.x[1] == -1, "(18k) Vivid on Recommended: shown as Recommended (%d), colour not pending (%d)", r.x[0], r.x[1]);
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
      // DM_SHADOW_SIZE: clamped to 512..4096 and made even (SHD-7), forced (no restart hint), latched
      static const char *const sz[6] = { "100", "9000", "3000", "8193", "3071", "abc" }; static const int want[6] = { 512, 4096, 3000, 4096, 3070, 512 };
      for (int i = 0; i < 6; i++) { l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, sz[i], 0);
          CHECK(l.s1 == want[i] && l.s2 == want[i] && l.fs == 1, "(21) DM_SHADOW_SIZE=%s: %d, forced, latched; got %d (forced %d) then %d", sz[i], want[i], l.s1, l.fs, l.s2); }
      l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, NULL, 0);
      CHECK(l.fs == 0 && l.fe == 0, "(21) neither switch set: nothing forced, so the restart hints follow the page; got forced %d / %d", l.fe, l.fs);
      l = run_latch(LATCH_FILE(0, 1, 2, 2), "1", NULL, 0);
      CHECK(l.fe == 1 && l.fs == 0, "(21) DM_ENV_SHADOWS set: environment shadows forced, the map not; got %d / %d", l.fe, l.fs);
      l = run_latch(LATCH_FILE(1, 1, 2, 2), NULL, NULL, 1);
      CHECK(l.e1 == 0 && l.s1 == 2100 && l.e2 == 1 && l.s2 == 4096,
            "(21) settings directory unknown: 0 / 2100 (the Switch look), not latched, so 1 / 4096 once it is known; got %d / %d, then %d / %d", l.e1, l.s1, l.e2, l.s2); }

    // ---- (22) the Frame Rate labels: the normal rate (dm_pace_hz) and the power saver's (dm_eco_hz), whole numbers within 0.1, else one decimal
    { static const struct { double hz; const char *hi, *lo; } k[] = { { 60, NULL, NULL }, { 120, NULL, NULL }, { 59.94, NULL, NULL }, { 240, NULL, NULL }, { 0, NULL, NULL },
          { 144, "72 fps", "36 fps" }, { 165, "82.5 fps", "33 fps" }, { 100, "100 fps", "33.3 fps" }, { 75, "75 fps", "37.5 fps" }, { 50, "50 fps", "25 fps" },
          { 90, "90 fps", "30 fps" }, { 48, "48 fps", "24 fps" }, { 119.88, NULL, NULL } };
      for (size_t i = 0; i < sizeof k / sizeof *k; i++) { char hi[48] = "", lo[48] = ""; int r = fps_labels(k[i].hz, "%s fps", hi, lo, sizeof hi);
          CHECK(k[i].hi ? r == 1 && !strcmp(hi, k[i].hi) && !strcmp(lo, k[i].lo) : r == 0, "(22) %.2f Hz: expect %s / %s, got %s%s / %s", k[i].hz, k[i].hi ? k[i].hi : "the layout's 60", k[i].lo ? k[i].lo : "30",
                r ? "" : "(none) ", hi, lo); }
      char hi[48], lo[48];
      CHECK(fps_labels(144, "%s 帧", hi, lo, sizeof hi) && !strcmp(hi, "72 帧") && !strcmp(lo, "36 帧"), "(22) the language's format: expect 72 帧 / 36 帧, got %s / %s", hi, lo);
      CHECK(fps_labels(144, "dmVideoFpsRate", hi, lo, sizeof hi) && !strcmp(hi, "72 fps"), "(22) a format without its placeholder (a missing entry) falls back to %%s fps: got %s", hi);
      CHECK(fps_labels(144, NULL, hi, lo, sizeof hi) && !strcmp(lo, "36 fps"), "(22) no format: %%s fps; got %s", lo); }

    // ---- (23) Frame Generation, the recommendation (rec_v 9): a chip with MetalFX Temporal and the interpolator takes both, others keep the conventional picture
    g_fe = 'M'; g_fgok = 1;
#define V8(up, rup, a) "mode=1\nrec_mode=1\nres=3\nrec_res=3\nup=" #up "\nrec_up=" #rup "\naa=" #a "\nrec_aa=1\naniso=3\nrec_aniso=3\ncolor=1\nrec_color=1\ns3d=0\nrec_s3d=0\nenvsh=1\nrec_envsh=1\nshq=2\nrec_shq=2\nrec_v=8\n"
    r = run(NULL);
    CHECK(r.v[K_UP] == 2 && r.v[K_FG] == 1 && strstr(r.file, "rec_up=2\n") && strstr(r.file, "rec_fg=1\n"), "(23a) first boot on a chip with both: Temporal and Frame Generation, got up %d fg %d\n%s", r.v[K_UP], r.v[K_FG], r.file);
    r = run(V8(1, 1, 1));
    CHECK(r.v[K_UP] == 2 && r.v[K_FG] == 1 && !strcmp(r.how, "update"), "(23b) a player on the old Recommended moves: up 2 fg 1, how=update; got %d %d '%s'\n%s", r.v[K_UP], r.v[K_FG], r.how, r.file);
    r = run(V8(1, 1, 0));
    CHECK(r.v[K_UP] == 1 && r.v[K_FG] == 0 && r.v[K_AA] == 0 && strstr(r.file, "rec_up=2\n") && strstr(r.file, "rec_fg=1\n"), "(23c) a player's own settings stay, real frames with them; only the recommendation moves; got up %d fg %d\n%s", r.v[K_UP], r.v[K_FG], r.file);
    r = run(V8(2, 1, 1));
    CHECK(r.v[K_UP] == 2 && r.v[K_FG] == 0, "(23d) a player who chose Temporal keeps the frames they had: fg 0, got %d", r.v[K_FG]);
    g_fgok = 0;
    r = run(NULL);
    CHECK(r.v[K_UP] == 1 && r.v[K_FG] == 0 && strstr(r.file, "rec_fg=0\n"), "(23e) a chip without them: the conventional picture, up 1 fg 0; got %d %d", r.v[K_UP], r.v[K_FG]);
    r = run(V8(1, 1, 1));
    CHECK(r.v[K_UP] == 1 && r.v[K_FG] == 0 && !strcmp(r.how, "update"), "(23f) the update changes nothing on a chip without them, got up %d fg %d '%s'", r.v[K_UP], r.v[K_FG], r.how);
    g_fgok = 1;
    r = run("mode=1\nrec_mode=1\nres=3\nrec_res=3\nup=1\nrec_up=2\naa=1\nrec_aa=1\naniso=3\nrec_aniso=3\ncolor=1\nrec_color=1\ns3d=0\nrec_s3d=0\nenvsh=1\nrec_envsh=1\nshq=2\nrec_shq=2\nfg=1\nrec_fg=1\nrec_v=9\n");
    CHECK(r.v[K_UP] == 1 && r.v[K_FG] == 0, "(23g) fg=1 without Temporal can't hold: fg 0, got %d", r.v[K_FG]);
    // (24) the rows that move together, and the presets
    r = run_do(NULL, act_fg);
    CHECK(r.x[0] == P_REC, "(24a) first boot reads Recommended, got %d", r.x[0]);
    CHECK(r.x[1] == 0 && r.x[2] == 1, "(24b) leaving Temporal turns it off: fg pending %d, up pending %d", r.x[1], r.x[2]);
    CHECK(r.x[3] == 1 && r.x[4] == 2 && r.x[5] == 0, "(24c) turning it on selects Temporal and the normal rate: fg %d up %d eco %d", r.x[3], r.x[4], r.x[5]);
    CHECK(r.x[6] == 0 && r.x[7] == 1 && r.x[8] == P_REC, "(24d) at the power saver's rate the row reads Off (%d), the setting stays (%d), and it is still Recommended (%d)", r.x[6], r.x[7], r.x[8]);
    CHECK(r.x[9] == 0 && r.x[10] == 0, "(24e) Performance and Quality use real frames: fg %d, %d", r.x[9], r.x[10]);
    CHECK(r.x[11] == 2 && r.x[12] == 1, "(24f) Recommended brings both back: up %d fg %d", r.x[11], r.x[12]);
    g_fgok = 0; g_fe = 'G';

    file_put(NULL); rmdir(g_dir);
    printf("t_video: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
