// Frame recorder: every frame the screen is given, generated or real, in full and in order, with what it was. For judging motion and animation: unlike screenshots or DM_MV_DUMP it
// never stalls the game, needs no screen-recording permission and works in background test runs. Included by nvn_mtl.m; the present thread unless a comment says otherwise.
//
// DM_FRAME_REC=<first second>:<seconds>:<path prefix>[:x,y,w,h]   seconds from the first present; the optional rectangle keeps only that part of the picture
// writes <prefix>.raw (RGBA, frames one after the other) and <prefix>.tsv (index, ticket, game frame, gen or real, the time the system planned to show it). dm/tools/motion_scan.py reads them.
//
// The copy goes in the present's own command buffer (fr_encode). When the writer falls behind, the frame is counted as dropped rather than waited for: a stall would change the motion being measured
enum { FR_POOL = 16 };
static struct { int on; double first, secs, t0; const char *prefix; NSUInteger x, y, w, h, row; int has_rect, inited, over; __strong id<MTLBuffer> buf[FR_POOL]; _Atomic int busy[FR_POOL]; FILE *raw, *idx; __strong dispatch_queue_t q; uint64_t n, dropped; } g_fr;
static void fr_parse(void) {
    static int done; if (done) return; done = 1;
    const char *e = getenv("DM_FRAME_REC"); if (!e) return;
    char *q; g_fr.first = strtod(e, &q); if (*q != ':') return; g_fr.secs = strtod(q + 1, &q); if (*q != ':') return;
    static char path[1024]; strlcpy(path, q + 1, sizeof path);
    int rx, ry, rw, rh; char *c = strrchr(path, ':'); if (c && sscanf(c + 1, "%d,%d,%d,%d", &rx, &ry, &rw, &rh) == 4) { *c = 0; g_fr.x = rx; g_fr.y = ry; g_fr.w = rw; g_fr.h = rh; g_fr.has_rect = 1; }
    g_fr.prefix = path; g_fr.on = 1;
}
// In the present's command buffer, after its own work. i = the pool buffer the feedback handler hands to fr_done, or -1
static int fr_encode(CBUF cb, id<MTLTexture> src, uint64_t ticket, uint64_t frame, int gen, double target_ts) {
    fr_parse(); if (!g_fr.on || g_fr.over || !src || src.textureType != MTLTextureType2D) return -1;
    double now = CACurrentMediaTime(); if (g_fr.t0 == 0) g_fr.t0 = now;
    double at = now - g_fr.t0; if (at < g_fr.first) return -1;
    if (at >= g_fr.first + g_fr.secs) { g_fr.over = 1; MLOG("frame recorder: %llu frames, %llu dropped", (unsigned long long)g_fr.n, (unsigned long long)g_fr.dropped); DM_EV("frame_rec", "frames:u dropped:u", (unsigned long long)g_fr.n, (unsigned long long)g_fr.dropped); dispatch_async(g_fr.q, ^{ if (g_fr.raw) fclose(g_fr.raw); if (g_fr.idx) fclose(g_fr.idx); g_fr.raw = g_fr.idx = NULL; }); return -1; }
    if (!g_fr.inited) {
        g_fr.inited = 1;
        if (!g_fr.has_rect) { g_fr.x = g_fr.y = 0; g_fr.w = src.width; g_fr.h = src.height; }
        g_fr.row = g_fr.w * 4; char p[1100];
        snprintf(p, sizeof p, "%s.raw", g_fr.prefix); g_fr.raw = fopen(p, "wb"); snprintf(p, sizeof p, "%s.tsv", g_fr.prefix); g_fr.idx = fopen(p, "w");
        if (!g_fr.raw || !g_fr.idx) { MLOG("【红】frame recorder: cannot write %s", g_fr.prefix); g_fr.over = 1; return -1; }
        fprintf(g_fr.idx, "# w %lu h %lu format %lu\nindex\tticket\tframe\tkind\ttarget_s\n", (unsigned long)g_fr.w, (unsigned long)g_fr.h, (unsigned long)src.pixelFormat);
        for (int i = 0; i < FR_POOL; i++) { g_fr.buf[i] = [g_dev newBufferWithLength:g_fr.row * g_fr.h options:MTLResourceStorageModeShared]; res_add(g_fr.buf[i]); }
        res_commit(); g_fr.q = dispatch_queue_create("dm.framerec", DISPATCH_QUEUE_SERIAL);
        MLOG("frame recorder: %lux%lu of %lux%lu from %.1f s for %.1f s", (unsigned long)g_fr.w, (unsigned long)g_fr.h, (unsigned long)src.width, (unsigned long)src.height, g_fr.first, g_fr.secs);
    }
    int i = -1; for (int k = 0; k < FR_POOL; k++) { int z = 0; if (atomic_compare_exchange_strong(&g_fr.busy[k], &z, 1)) { i = k; break; } }
    if (i < 0) { g_fr.dropped++; return -1; }
    CENC e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    enc_b_copy_texture_buffer(e, src, 0, 0, MTLOriginMake(g_fr.x, g_fr.y, 0), MTLSizeMake(g_fr.w, g_fr.h, 1), g_fr.buf[i], 0, g_fr.row, g_fr.row * g_fr.h);
    m4_end(e);
    uint64_t n = g_fr.n++; id<MTLBuffer> b = g_fr.buf[i]; int k = i;
    dispatch_async(g_fr.q, ^{ if (g_fr.idx) fprintf(g_fr.idx, "%llu\t%llu\t%llu\t%s\t%.6f\n", (unsigned long long)n, (unsigned long long)ticket, (unsigned long long)frame, gen ? "gen" : "real", target_ts); (void)b; (void)k; });
    return i;
}
// The present's feedback handler (any thread): the copy is done, so the frame can go to the file and the buffer back to the pool
static void fr_done(int i) {
    if (i < 0) return;
    id<MTLBuffer> b = g_fr.buf[i];
    dispatch_async(g_fr.q, ^{ if (g_fr.raw) fwrite(b.contents, 1, g_fr.row * g_fr.h, g_fr.raw); atomic_store(&g_fr.busy[i], 0); });
}
