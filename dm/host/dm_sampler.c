// Whole-process sampling profiler: DM_SAMPLER=<ms>:<file> (bench.py profile, dm/tools/bench_why.py). Unlike DM_SAMPLE it sees every thread, the host's own (translation, present, display link,
// uploads) as well as the game's, by asking the system for their state: a thread that is waiting isn't touched, so nothing is woken and no signal arrives in the middle of a wait. A running
// thread is suspended for as long as it takes to copy its registers and walk its frame pointers, without a lock or an allocation in between, then symbolised after it runs again.
// The period is jittered by a quarter either way so that it can't lock onto the frame rate.
//
// File rows, tab separated, times on the event clock's base:
//   K <t>                 one sampling tick: every thread was looked at
//   T <tid> <name>        a thread's name, when first known
//   S <tid> <frames…>     a running thread, innermost frame first: main+0x… / sdk+0x… for the game, a symbol name for the host
//   C <t> <tid> <us>      a thread's total CPU time so far (every 16 ticks), exact where the samples are a statistic
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hle.h"

enum { PROF_DEPTH = 64, PROF_SYMS = 1 << 14, PROF_THREADS = 512, PROF_RUN = 128 };
extern uint64_t g_t0_ns;
int thr_name_of(pthread_t pt, char *out, size_t n);
static FILE *g_pf;

// Host addresses to names, cached (dladdr takes the loader's lock, so this runs only with every thread going again). The profiler's thread only
static struct { uint64_t pc; const char *name; } g_psym[PROF_SYMS];
static const char *host_sym(uint64_t a) {
    uint64_t h = (a * 0x9E3779B97F4A7C15ull) >> 50;
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = (unsigned)(h + i) & (PROF_SYMS - 1);
        if (g_psym[k].pc == a) return g_psym[k].name;
        if (!g_psym[k].pc) { Dl_info di; char b[32]; const char *n = dladdr((void *)a, &di) && di.dli_sname ? di.dli_sname : NULL;
            if (!n) { snprintf(b, sizeof b, "%#llx", (unsigned long long)a); n = b; }
            g_psym[k].pc = a; g_psym[k].name = strdup(n); return g_psym[k].name; }
    }
    return "?";
}
static void put_frame(uint64_t a) {
    module_t *m = module_of((void *)a);
    if (m) fprintf(g_pf, "\t%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)m->base));
    else fprintf(g_pf, "\t%s", host_sym(a));
}
// Registers and frame pointers of a suspended thread: no lock, no allocation, reads that can't fault. Returns the number of frames
static int walk(thread_act_t t, uint64_t *f) {
    arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT; int n = 0;
    if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS) return 0;
    uint64_t pc = arm_thread_state64_get_pc(st), lr = arm_thread_state64_get_lr(st), fp = arm_thread_state64_get_fp(st);
    f[n++] = pc; int lr_used = 0;
    while (n < PROF_DEPTH && fp && !(fp & 7)) {
        uint64_t fr[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof fr, (vm_address_t)fr, &got) != KERN_SUCCESS || got != sizeof fr) break;
        if (!lr_used) { lr_used = 1; if (lr && lr != fr[1] && n < PROF_DEPTH) f[n++] = lr; }   // a leaf function keeps its caller only in lr
        if (n >= PROF_DEPTH) break;
        if (!fr[1]) break;
        f[n++] = fr[1];
        if (fr[0] <= fp) break;
        fp = fr[0];
    }
    return n;
}
static void *sampler_main(void *arg) {
    pthread_setname_np("dm-sampler"); pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const uint64_t period = (uint64_t)(uintptr_t)arg; mach_port_t self = mach_thread_self();
    static struct { uint64_t tid; int named; } seen[PROF_THREADS]; static uint64_t run[PROF_RUN][PROF_DEPTH]; static int run_n[PROF_RUN]; static uint64_t run_tid[PROF_RUN];
    for (uint64_t tick = 0;; tick++) {
        thread_act_array_t th = NULL; mach_msg_type_number_t n = 0; int nrun = 0;
        if (task_threads(mach_task_self(), &th, &n) == KERN_SUCCESS) {
            fprintf(g_pf, "K\t%.4f\n", (now_ns() - g_t0_ns) / 1e9);
            for (mach_msg_type_number_t i = 0; i < n; i++) {
                thread_act_t t = th[i];
                if (t != self) {
                    thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT; thread_identifier_info_data_t ii; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
                    if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS && thread_info(t, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ic) == KERN_SUCCESS) {
                        int k = -1; for (int j = 0; j < PROF_THREADS; j++) { if (seen[j].tid == ii.thread_id) { k = j; break; } if (!seen[j].tid) { seen[j].tid = ii.thread_id; k = j; break; } }
                        if (k >= 0 && !seen[k].named) { char nm[96] = ""; pthread_t pt = pthread_from_mach_thread_np(t);
                            if (pt && !thr_name_of(pt, nm, sizeof nm)) pthread_getname_np(pt, nm, sizeof nm);
                            if (nm[0]) { seen[k].named = 1; fprintf(g_pf, "T\t%llu\t%s\n", (unsigned long long)ii.thread_id, nm); } }
                        if (tick % 16 == 0) fprintf(g_pf, "C\t%.4f\t%llu\t%llu\n", (now_ns() - g_t0_ns) / 1e9, (unsigned long long)ii.thread_id,
                                                    (unsigned long long)(bi.user_time.seconds + bi.system_time.seconds) * 1000000ull + (unsigned long long)(bi.user_time.microseconds + bi.system_time.microseconds));
                        if (bi.run_state == TH_STATE_RUNNING && !(bi.flags & TH_FLAGS_IDLE) && nrun < PROF_RUN && thread_suspend(t) == KERN_SUCCESS) {
                            run_n[nrun] = walk(t, run[nrun]); run_tid[nrun] = ii.thread_id; thread_resume(t); if (run_n[nrun] > 0) nrun++; }
                    }
                }
                mach_port_deallocate(mach_task_self(), t);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
        }
        for (int r = 0; r < nrun; r++) { fprintf(g_pf, "S\t%llu", (unsigned long long)run_tid[r]); for (int i = 0; i < run_n[r]; i++) put_frame(run[r][i]); fputc('\n', g_pf); }
        if (tick % 64 == 0) fflush(g_pf);
        uint64_t wait = period - period / 4 + arc4random_uniform((uint32_t)(period / 2 + 1));
        struct timespec ts = { (time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull) }; nanosleep(&ts, NULL);
    }
    return NULL;
}
void dm_sampler_start(void) {
    const char *e = getenv("DM_SAMPLER"); if (!e) return;
    char *q; double ms = strtod(e, &q); if (*q != ':' || ms <= 0 || !q[1]) { LOG("【红】DM_SAMPLER=%s: expected <ms>:<file>", e); return; }
    if (!(g_pf = fopen(q + 1, "w"))) { LOG("【红】DM_SAMPLER: cannot write %s", q + 1); return; }
    setvbuf(g_pf, NULL, _IOFBF, 1 << 20); fprintf(g_pf, "# dm_sampler 1 ms=%g\n", ms);
    pthread_t t; if (pthread_create(&t, NULL, sampler_main, (void *)(uintptr_t)(uint64_t)(ms * 1e6))) { LOG("【红】DM_SAMPLER: no thread"); return; }
    pthread_detach(t); LOG("DM_SAMPLER: every thread every %g ms → %s", ms, q + 1);
}
