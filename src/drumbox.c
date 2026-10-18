/* Drumbox engine: parameters, MIDI, kits, user slots, the master section (VOLUME, low-pass) and the sequencer.
 *
 * MIDI: each voice plays on its own note (NOTES page; to start with 36-43, C1-G1, a Force's drum pads; V1 NOTE on
 * MASTER sets all eight in a row), on any channel; velocity reaches each voice through its OSC / NOISE / MOD VEL
 * amounts. A voice is one-shot: note-offs do nothing. A new hit fades the voice's (or its choke group's) last one out
 * in 50 ms. CC 102-109 mute V1-V8 (64-127 muted, 0-63 plays), CC 110 picks the pattern (0-11 = A-L), CC 120 / 123
 * stop everything. The sequencer (SEQ ON) plays the patterns A-L with the Force's song
 * position; with SEQ OFF the Force's own sequencer plays the voices through their notes. */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drumbox.h"
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

/* the wrapper's engine interface (mpc-vst-plugins wrapper/engine.h) */
typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
} mpc_engine_t;
const mpc_engine_t *mpc_engine(void);
void mpc_engine_transport(void *inst, double bpm, double ppq, int playing);   /* optional (engine.h): tempo, PLAY */
int mpc_engine_changed(void *inst, char *key, int len);   /* optional (engine.h): what the pads changed */

static const int BASE_NOTE[5] = {24, 36, 44, 48, 60};   /* BASE NOTE's options (mpc/gen.py BASE_NOTES) */

static int find_key(const char *key) {
    for (int i = 0; i < NP; i++)
        if (!strcmp(PDEF[i].key, key)) return i;
    return -1;
}

/* ---- flush denormals while rendering (decaying filters would otherwise crawl) --------------------------------- */
typedef unsigned long fpstate_t;
static inline fpstate_t ftz_on(void) {
#if defined(__arm__)
    unsigned old, n;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(old));
    n = old | (1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(n));
    return old;
#elif defined(__aarch64__)
    unsigned long old, n;
    __asm__ volatile("mrs %0, fpcr" : "=r"(old));
    n = old | (1ul << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(n));
    return old;
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = _mm_getcsr();
    _mm_setcsr(old | 0x8040);
    return old;
#else
    return 0;
#endif
}
static inline void ftz_off(fpstate_t old) {
#if defined(__arm__)
    unsigned o = (unsigned)old;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(o));
#elif defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(old));
#elif defined(__x86_64__) || defined(__i386__)
    _mm_setcsr((unsigned)old);
#else
    (void)old;
#endif
}

/* ---- kits ------------------------------------------------------------------------------------------------------ */

static void preview_begin(db_t *D);

/* A kit, as HOLD says: OFF its sounds and its patterns A-L, KIT only its patterns (the sounds stay), PATTERN only its
 * sounds (the patterns stay, so the groove runs on with another kit). */
static void load_kit(db_t *D, int n) {
    if (n < 0 || n >= D->nkits) return;
    kit_ensure(D, n);   /* normally done already where KIT was set (e_set_param): the file isn't read here */
    const kit_t *k = &D->kits[n];
    int hold = (int)D->pv[P_HOLD];
    ex_push(D);   /* UNDO brings back what was there (a mutation, say) */
    if (hold != 1) D->snd_src = n;
    if (hold != 2) D->grv_src = n;
    D->sim_dirty = 1;
    if (hold != 1)
        for (int v = 0; v < NV; v++)
            for (int j = 0; j < NVP; j++) D->pv[VK(v, j)] = k->pv[v][j];
    if (hold != 2) {
        D->wp = k->ps;
        D->pv[P_PATTERN] = (float)(k->ps.cur % NPAT);
        D->pv[P_PAT_LEN] = (float)k->ps.p[k->ps.cur % NPAT].len;
        D->pv[P_SWING] = k->ps.swing;
        D->sq_queued = (int)D->pv[P_PATTERN];   /* the sequencer takes it at the end of the pattern playing */
    }
    D->pv[P_KIT] = (float)n;
    if (k->user) D->pv[P_SLOT] = (float)k->user;
    /* PREVIEW: the selected pattern plays (twice), unless the Force is playing */
    if ((int)D->pv[P_PREVIEW] && !(D->tp_seen && D->tp_playing)) preview_begin(D);
    else D->seq_on = 0;
}

/* A kit set waits for the next block, MIDI event, other parameter or state save. MPC's plugin host sets parameter 0
 * (KIT) to the other end of its range and straight back whenever it gets ready to play (on insert and on every STOP;
 * Percolator's Live II trace, 2026-10-10); loading at once would load the last kit and then this one again over every
 * knob moved since. Waiting, the pair ends on the kit already loaded and nothing happens. */
static void apply_kit(db_t *D) {
    if (D->kit_pending < 0) return;
    int n = D->kit_pending;
    D->kit_pending = -1;
    if (n != (int)D->pv[P_KIT]) load_kit(D, n);
}

/* ---- MIDI ------------------------------------------------------------------------------------------------------ */

static void hit(db_t *D, int v, float vel) {   /* a voice, and its choke group */
    int g = (int)D->pv[VK(v, VP_CHOKE)];
    if (g > 0)
        for (int u = 0; u < NV; u++)
            if (u != v && (int)D->pv[VK(u, VP_CHOKE)] == g) voice_choke(D, u);
    voice_trigger(D, v, vel);
}

/* MIDI CC (any channel): the lanes' mutes and the pattern, for a controller or a MIDI track's automation */
#define CC_MUTE 102     /* 102-109: V1-V8 */
#define CC_PATTERN 110

static int sel_pattern(const db_t *D) { return (int)D->pv[P_PATTERN] % NPAT; }
#define VNOTE(v) (P_V1_NOTE + 3 * (v))   /* v<n>_note, then its - and + */

/* PATTERN picked: at once while the sequencer is not playing, else at the end of the pattern playing */
static void pick_pattern(db_t *D, int i) {
    i = ((i % NPAT) + NPAT) % NPAT;
    D->pv[P_PATTERN] = (float)i;
    D->pv[P_PAT_LEN] = (float)D->wp.p[i].len;
    D->sq_queued = i;
    D->sim_dirty = 1;   /* similar grooves: to this one */
}

/* PAD BROWSE: pads 9-16 (BASE NOTE + 8 .. + 15): kit -/+, FAVORIT, PLAY, sound -/+, TAKE, next pattern. A kit picked here is read on the audio
 * thread (a few ms, only while nothing plays); the screen follows through mpc_engine_changed. */
static void pad_browse(db_t *D, int k) {
    if (k == 0 || k == 1) {
        int n = (int)D->pv[P_KIT] + (k ? 1 : -1);
        if (n < 0 || n >= D->nkits) return;
        kit_ensure(D, n);
        D->info[0] = 0;
        load_kit(D, n);
        D->changed |= 1;
    } else if (k == 2) kit_favorite(D);
    else if (k == 3) preview_begin(D);
    else if (k == 4 || k == 5) {
        int n = (int)D->pv[P_DRUM] + (k == 5 ? 1 : -1);
        if (n < 0 || n >= D->ndrums) return;
        D->pv[P_DRUM] = (float)n;
        D->info[0] = 0;
        if (drum_ensure(D, n)) voice_trigger(D, AUD, 110.0f);
        D->changed |= 2;
    } else if (k == 6 && D->dcur >= 0) {
        int v = (int)D->pv[P_DRUM_VOICE];
        for (int j = 0; j < NVP; j++)
            if (j != VP_CHOKE) D->pv[VK(v, j)] = D->dpv[j];
        snprintf(D->info, sizeof D->info, "V%d: %.36s", v + 1, D->dname && D->dname[D->dcur] ? D->dname[D->dcur] : "");
        D->changed |= 4;
        D->changed_k = 0;
    } else if (k == 7) {   /* the next pattern (A-L), heard at once */
        pick_pattern(D, sel_pattern(D) + 1);
        if (!(D->tp_seen && D->tp_playing)) preview_begin(D);
        D->changed |= 8;
    }
}

/* A note plays every voice whose NOTE it is (NOTES page; V1 NOTE on MASTER sets all eight in a row). With PAD BROWSE
 * on, a note no voice has, BASE NOTE + 8 .. + 15 (pads 9-16), browses while the Force is stopped; pad 16 (the next
 * pattern, taken at the end of the one playing) works while it plays too. */
static void e_midi(void *inst, const uint8_t *m, int len) {
    db_t *D = inst;
    apply_kit(D);
    if (len < 2) return;
    int st = m[0] & 0xF0, d1 = m[1] & 0x7F, d2 = len > 2 ? m[2] & 0x7F : 0;
    if (st == 0x90 && d2 > 0) {
        int played = 0;
        for (int v = 0; v < NV; v++)
            if ((int)D->pv[VNOTE(v)] == d1) {
                if (!played) D->seq_on = 0;   /* playing the pads ends a preview */
                hit(D, v, (float)d2);
                played = 1;
            }
        int b = d1 - BASE_NOTE[(int)D->pv[P_BASE_NOTE] % 5];
        int run = D->tp_seen && D->tp_playing;   /* while the Force plays only pad 16 (the next pattern, at the bar) */
        if (!played && b >= NV && b < NV + 8 && (int)D->pv[P_PAD_BROWSE] && (!run || b == NV + 7)) pad_browse(D, b - NV);
    } else if (st == 0xB0 && d1 >= CC_MUTE && d1 < CC_MUTE + NV) {   /* a lane's mute: 64-127 muted, 0-63 plays */
        uint8_t b = (uint8_t)(1u << (d1 - CC_MUTE));
        if (d2 >= 64) D->wp.mute |= b;
        else D->wp.mute &= (uint8_t)~b;
    } else if (st == 0xB0 && d1 == CC_PATTERN) {   /* the pattern: 0-11 = A-L, taken as a tap on A-L would */
        pick_pattern(D, d2 < NPAT ? d2 : NPAT - 1);
        D->changed |= 8;
        D->changed_p = 0;
    } else if (st == 0xB0 && (d1 == 120 || d1 == 123)) {
        D->seq_on = 0;
        voice_kill(D);
    }
}

/* ---- the preview: the selected pattern, twice, at the Force's tempo (the preset's own without one) -------------- *
 * Accented steps play at velocity 127, the others at 96; the mutes are kept. Patterns that are all empty (a .tkit, a
 * built-in nb_tonic kit) play the eight voices one after another, an eighth note apart. Only to hear a kit while
 * browsing: it never runs while the Force plays, and a pad, a new kit or PLAY on the Force stops it. */
static void preview_begin(db_t *D) {
    pattern_t q = D->wp.p[sel_pattern(D)];
    int div = D->wp.div > 0 ? D->wp.div : 16, empty = 1;
    for (int v = 0; v < NV; v++) empty &= !q.trig[v];
    if (empty) {
        memset(&q, 0, sizeof q);
        q.len = 16;
        div = 16;
        for (int v = 0; v < NV; v++) q.trig[v] = (uint16_t)(1u << (2 * v));
        D->seq_total = 16;
    } else D->seq_total = 2 * (q.len ? q.len : 16);
    double bpm = D->tp_bpm > 20.0 ? D->tp_bpm : D->wp.tempo > 20.0f ? D->wp.tempo : 120.0;
    D->seq_frames = (int)(SR * 60.0 / bpm * 4.0 / div + 0.5);
    D->seq = q;
    D->seq_step = 0;
    D->seq_left = 0;
    D->seq_on = 1;
}

static void preview_fire(db_t *D) {
    int s = D->seq_step % (D->seq.len > 0 ? D->seq.len : 16);
    for (int v = 0; v < NV; v++)
        if (!(D->wp.mute >> v & 1) && (D->seq.trig[v] >> s & 1)) hit(D, v, (D->seq.acc[v] >> s & 1) ? 127.0f : 96.0f);
    D->sq_step = s;
    if (++D->seq_step >= D->seq_total) { D->seq_on = 0; D->sq_step = -1; }
    D->seq_left = D->seq_frames;
}

/* ---- the sequencer: SEQ ON, while the Force plays --------------------------------------------------------------- *
 * It follows the song position the Force reports (mpc_engine_transport), so it starts, stops, loops and jumps with
 * it and every step lands on its sample. Steps are counted from the song's start in StepRate units (sixteenths, also
 * eighths or 32nds as the preset says); SWING delays every second step by up to half a step. A pattern runs its
 * LENGTH (1-16 steps) and then goes on to: a pattern picked meanwhile; with CHAIN ON the next one if this one is
 * chained (Microtonic's Chained, back to the first of its chain after the last); else itself. Accents play at
 * velocity 127, other steps at 96; muted voices (the grid's left column) don't play. */

static int chain_next(const db_t *D, int p) {
    if ((int)D->pv[P_CHAIN] && D->wp.p[p].chained && p + 1 < NPAT) return p + 1;
    if ((int)D->pv[P_CHAIN])   /* the end of a chain: back to its first pattern */
        while (p > 0 && D->wp.p[p - 1].chained) p--;
    return p;
}

static void sq_fire(db_t *D, long k) {
    int len = D->wp.p[D->sq_pat].len ? D->wp.p[D->sq_pat].len : 16;
    if (k < D->sq_base || k - D->sq_base > 4L * 16 || k != D->sq_last + 1) {   /* a start, a loop, a jump: line up again */
        D->sq_pat = D->sq_queued >= 0 ? D->sq_queued : sel_pattern(D);
        D->sq_queued = -1;
        len = D->wp.p[D->sq_pat].len ? D->wp.p[D->sq_pat].len : 16;
        D->sq_base = k - ((k % len) + len) % len;
    }
    while (k - D->sq_base >= len) {   /* the pattern's end: the next one */
        D->sq_base += len;
        D->sq_pat = D->sq_queued >= 0 ? D->sq_queued : chain_next(D, D->sq_pat);
        D->sq_queued = -1;
        if (D->sq_pat != sel_pattern(D)) { D->pv[P_PATTERN] = (float)D->sq_pat; D->pv[P_PAT_LEN] = (float)D->wp.p[D->sq_pat].len; D->changed |= 8; }
        len = D->wp.p[D->sq_pat].len ? D->wp.p[D->sq_pat].len : 16;
    }
    D->sq_last = k;
    int s = (int)(k - D->sq_base);
    const pattern_t *q = &D->wp.p[D->sq_pat];
    for (int v = 0; v < NV; v++)
        if (!(D->wp.mute >> v & 1) && (q->trig[v] >> s & 1)) hit(D, v, (q->acc[v] >> s & 1) ? 127.0f : 96.0f);
    D->sq_step = s;
}

/* ---- render ---------------------------------------------------------------------------------------------------- */

static inline float soft_ceiling(float x) {   /* instead of a hard clip */
    float ax = fabsf(x);
    if (ax > 0.9f) x = copysignf(0.9f + 0.1f * tanhf((ax - 0.9f) * 10.0f), x);
    return x;
}
static inline int16_t out_sample(float x) {
    int s = (int)lrintf(soft_ceiling(x) * 32767.0f);
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

#define BLK 64
static void e_render(void *inst, int16_t *out, int frames) {
    db_t *D = inst;
    apply_kit(D);
    fpstate_t fp = ftz_on();
    float vol = D->pv[P_VOLUME] <= 0.0f ? 0.0f : dbamp(k_db(D->pv[P_VOLUME]));
    float cut = D->pv[P_LPF_CUT], res = D->pv[P_LPF_RES];
    int lpf = cut < 99.5f || res > 0.5f;
    if (lpf && (cut != D->lpf_cut || res != D->lpf_res)) {   /* nb_tonic: RLPF(cutoff, rq 1 .. 0.05) */
        bq_lp(&D->lpf, k_freq(cut), 1.0f - 0.95f * res / 100.0f);
        D->lpf_cut = cut;
        D->lpf_res = res;
    }
    if (D->seq_start) {   /* PLAY (PREVIEW): the selected pattern from the start */
        D->seq_start = 0;
        preview_begin(D);
    }
    /* the sequencer: where this block lies in the song, in steps (swing: an odd step's time moves later) */
    int sq = (int)D->pv[P_SEQ] && D->tp_seen && D->tp_playing && D->tp_bpm > 20.0 && D->tp_ppq >= 0.0;
    double spq = (D->wp.div > 0 ? D->wp.div : 16) / 4.0, u0 = 0, du = 0, sw = 0.5 * D->pv[P_SWING] / 100.0;
    if (sq) {
        u0 = (D->tp_ppq + D->tp_frames * D->tp_bpm / 60.0 / SR) * spq;   /* steps at this block's first sample */
        du = D->tp_bpm / 60.0 / SR * spq;                                /* steps per sample */
        D->seq_on = 0;
    } else if (!(D->tp_seen && D->tp_playing) && D->sq_step >= 0 && !D->seq_on) D->sq_step = -1;
    /* the step to go on from: the last one fired; after a start or a jump, the ones from this block's first sample on
     * (a step that fell between two samples belongs to the later one) */
    long nk = 0;
    if (sq) {
        long from = (long)ceil(u0 - du - 1e-9) - 1;
        nk = D->sq_resync || D->sq_last < from - 4 ? from : D->sq_last;
        D->sq_resync = 0;
    }   /* the first step whose time may lie in this block */
    if (D->aud_drum) {   /* SOUNDS: the picked .mtdrum on its own voice */
        D->aud_drum = 0;
        if (D->dcur >= 0) voice_trigger(D, AUD, 110.0f);
    }
    for (int off = 0; off < frames;) {
        if (D->seq_on && D->seq_left <= 0) preview_fire(D);
        int n = frames - off < BLK ? frames - off : BLK;
        if (D->seq_on && D->seq_left < n) n = D->seq_left;
        if (sq) {   /* the steps due by now fire; the block ends at the next one */
            double at = u0 + off * du;
            for (;;) {
                long k = nk + 1;
                double t = k + ((k & 1) ? sw : 0.0);
                if (t > at + 1e-9) {
                    int f = (int)ceil((t - at) / du - 1e-9);
                    if (f < n) n = f > 0 ? f : 1;
                    break;
                }
                if (k > D->sq_last) sq_fire(D, k);
                nk = k;
            }
        }
        float L[BLK] = {0}, R[BLK] = {0};
        for (int v = 0; v <= NV; v++) voice_render(D, v, L, R, n);
        for (int i = 0; i < n; i++) {
            float l = L[i] * vol, r = R[i] * vol;
            if (lpf) {
                l = bq_run(&D->lpf, &D->lpfs[0], l);
                r = bq_run(&D->lpf, &D->lpfs[1], r);
            }
            out[2 * (off + i)] = out_sample(l);
            out[2 * (off + i) + 1] = out_sample(r);
        }
        if (D->seq_on) D->seq_left -= n;
        off += n;
    }
    D->tp_frames += frames;
    ftz_off(fp);
}

/* ---- the grid: st_N (voice row, step column), mute_N, ph_N; asked for every 10 ms, so they skip the key search -- */

/* "st4_6" (lane 4, step 6), "mute4_1", "ph6" + "_on" or not: kind 0, 1, 2 = st, mute, ph; n = 0-based tile (st: lane
 * x 16 + step) */
static int tile_key(const char *key, int *kind, int *n) {
    const char *c;
    int lane = 0;
    if (key[0] == 's' && key[1] == 't' && key[2] >= '1' && key[2] <= '0' + NV && key[3] == '_') { *kind = 0; lane = key[2] - '1'; c = key + 4; }
    else if (!strncmp(key, "mute", 4) && key[4] >= '1' && key[4] <= '0' + NV && key[5] == '_' && key[6] == '1') {
        *kind = 1;
        *n = key[4] - '1';
        c = key + 7;
        if (!*c) return 0;
        return strcmp(c, "_on") ? -1 : 1;
    } else if (key[0] == 'p' && key[1] == 'h' && key[2] == '_') { *kind = 2; c = key + 3; }
    else return -1;
    if (*c < '1' || *c > '9') return -1;
    char *e;
    long v = strtol(c, &e, 10);
    if (v < 1 || v > 16) return -1;
    *n = lane * 16 + (int)v - 1;
    if (!*e) return 0;
    return strcmp(e, "_on") ? -1 : 1;
}

static int tile_get(const db_t *D, int kind, int n, int on, char *buf, int len) {
    const pattern_t *q = &D->wp.p[sel_pattern(D)];
    if (kind == 0) {
        int v = n / 16, s = n % 16, t = q->trig[v] >> s & 1, a = q->acc[v] >> s & 1;
        if (on) return snprintf(buf, len, "%d", t);
        int L = q->len ? q->len : 16;
        return snprintf(buf, len, "%s", s >= L ? "-" : t && a ? "A" : "");   /* past LENGTH: "-" */
    }
    if (kind == 1) return on ? snprintf(buf, len, "%d", !(D->wp.mute >> n & 1)) : snprintf(buf, len, "V%d", n + 1);
    if (on) {   /* the step playing: the sequencer's, or the preview's */
        int playing = ((int)D->pv[P_SEQ] && D->tp_seen && D->tp_playing) || D->seq_on;
        return snprintf(buf, len, "%d", playing && D->sq_step == n && (!(int)D->pv[P_SEQ] || D->sq_pat == sel_pattern(D) || D->seq_on));
    }
    return snprintf(buf, len, "%s", n % 4 ? "" : n == 0 ? "1" : n == 4 ? "5" : n == 8 ? "9" : "13");
}

static void tile_tap(db_t *D, int kind, int n) {
    if (kind == 0) {   /* off -> on -> accent -> off */
        pattern_t *q = &D->wp.p[sel_pattern(D)];
        int v = n / 16;
        uint16_t b = (uint16_t)(1u << (n % 16));
        if (!(q->trig[v] & b)) { q->trig[v] |= b; q->acc[v] &= (uint16_t)~b; }
        else if (!(q->acc[v] & b)) q->acc[v] |= b;
        else { q->trig[v] &= (uint16_t)~b; q->acc[v] &= (uint16_t)~b; }
    } else if (kind == 1) D->wp.mute ^= (uint8_t)(1u << n);
}

/* EXPLORE's tiles: a similar sound (the kit's name) or groove (the name and the pattern); lit: the one taken */
static int ex_tile(db_t *D, int groove, int i, int on, char *buf, int len) {
    if (i < 0 || i >= NSIM) return -1;
    if (!on) ex_poll(D);   /* the lists, when the index has grown */
    int n = groove ? D->nsim_grv : D->nsim_snd, reading = __atomic_load_n(&D->ix_count, __ATOMIC_ACQUIRE) < D->ix_n;
    if (i >= n) {
        if (on) return snprintf(buf, len, "0");
        if (i == 0 && groove && !reading) return snprintf(buf, len, "(EMPTY PATTERN)");
        return snprintf(buf, len, "%s", i == 0 && reading ? "READING THE PRESETS ..." : "");
    }
    int k = groove ? D->sim_grv[i] : D->sim_snd[i];
    if (on) return snprintf(buf, len, "%d", k == (groove ? D->grv_src : D->snd_src));
    if (groove) return snprintf(buf, len, "%.30s  %c", D->kits[k].name, 'A' + D->sim_grv_pat[i]);
    return snprintf(buf, len, "%.31s", D->kits[k].name);
}

static int ex_status(db_t *D, char *buf, int len) {
    if (D->ex_info[0]) return snprintf(buf, len, "%s", D->ex_info);
    int c = __atomic_load_n(&D->ix_count, __ATOMIC_ACQUIRE);
    if (c < D->ix_n) return snprintf(buf, len, "READING %d / %d PRESETS", c, D->ix_n);
    return snprintf(buf, len, "%d PRESETS  -  UNDO: %d", D->nkits, D->nundo);
}

/* ---- parameters ------------------------------------------------------------------------------------------------ */

static void set_value(db_t *D, int i, const char *val) {
    const pdef_t *d = &PDEF[i];
    float x = (float)atof(val);
    if (d->kind == 0 || d->kind == 2) D->pv[i] = clampf(x, d->min, d->max);
    else if (d->kind == 1) {
        int k = atoi(val);
        D->pv[i] = (float)(k < 0 ? 0 : k >= d->nopts ? d->nopts - 1 : k);
    }
}

/* the patterns in the state: per pattern LL C then 8 x TTTT, 8 x AAAA (hex), then mutes, step rate, tempo */
static int pats_put(const patset_t *ps, char *b, int len) {
    int o = snprintf(b, len, "pats=");
    for (int i = 0; i < NPAT && o < len; i++) {
        o += snprintf(b + o, len - o, "%02x%x", ps->p[i].len, ps->p[i].chained ? 1 : 0);
        for (int v = 0; v < NV && o < len; v++) o += snprintf(b + o, len - o, "%04x", ps->p[i].trig[v]);
        for (int v = 0; v < NV && o < len; v++) o += snprintf(b + o, len - o, "%04x", ps->p[i].acc[v]);
    }
    if (o < len) o += snprintf(b + o, len - o, ";pmute=%d;pdiv=%d;ptempo=%.2f;", ps->mute, ps->div, ps->tempo);
    return o;
}

static unsigned hexn(const char *c, int n) {
    unsigned x = 0;
    for (int i = 0; i < n; i++) {
        int d = c[i] >= '0' && c[i] <= '9' ? c[i] - '0' : c[i] >= 'a' && c[i] <= 'f' ? c[i] - 'a' + 10 : 0;
        x = x << 4 | (unsigned)d;
    }
    return x;
}

static void pats_get(patset_t *ps, const char *c) {
    const int per = 3 + 2 * NV * 4;
    if ((int)strlen(c) < NPAT * per) return;
    for (int i = 0; i < NPAT; i++, c += per) {
        pattern_t *q = &ps->p[i];
        int l = (int)hexn(c, 2);
        q->len = (uint8_t)(l >= 1 && l <= 16 ? l : 16);
        q->chained = c[2] == '1';
        for (int v = 0; v < NV; v++) {
            q->trig[v] = (uint16_t)hexn(c + 3 + 4 * v, 4);
            q->acc[v] = (uint16_t)hexn(c + 3 + 4 * (NV + v), 4);
        }
    }
    ps->has = 1;
}

static void set_state(db_t *D, const char *s) {
    char kv[1024], kitname[40] = "";
    int kit = -1, notes = 0;
    while (*s) {
        size_t n = strcspn(s, ";");
        if (n && n < sizeof kv) {
            memcpy(kv, s, n);
            kv[n] = 0;
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq++ = 0;
                int i = find_key(kv);
                if (!strcmp(kv, "kitname")) snprintf(kitname, sizeof kitname, "%s", eq);
                else if (!strcmp(kv, "pats")) pats_get(&D->wp, eq);
                else if (!strcmp(kv, "pmute")) D->wp.mute = (uint8_t)atoi(eq);
                else if (!strcmp(kv, "pdiv")) { int d = atoi(eq); D->wp.div = d == 8 || d == 32 ? d : 16; }
                else if (!strcmp(kv, "ptempo")) D->wp.tempo = (float)atof(eq);
                else if (i == P_KIT) kit = atoi(eq);
                else if (i >= 0) {
                    set_value(D, i, eq);
                    notes |= i == P_V1_NOTE;
                }
            }
        }
        s += n;
        if (*s) s++;
    }
    if (!notes)   /* a state from before 0.3.0: the notes follow BASE NOTE, as they did */
        for (int v = 0; v < NV; v++) D->pv[VNOTE(v)] = (float)(BASE_NOTE[(int)D->pv[P_BASE_NOTE] % 5] + v);
    D->pv[P_PAT_LEN] = (float)D->wp.p[sel_pattern(D)].len;
    D->sq_queued = sel_pattern(D);
    D->sim_dirty = 1;
    /* the kit's number, or where that name is now (files added or removed since) */
    if (kit >= 0 && kit < D->nkits && (!kitname[0] || !strcmp(D->kits[kit].name, kitname))) D->pv[P_KIT] = (float)kit;
    else if (kitname[0]) {
        for (int k = 0; k < D->nkits; k++)
            if (!strcmp(D->kits[k].name, kitname)) { D->pv[P_KIT] = (float)k; break; }
    }
    D->snd_src = D->grv_src = (int)D->pv[P_KIT];
}

static int get_state(const db_t *D, char *buf, int len) {
    int k = (int)D->pv[P_KIT];
    int o = snprintf(buf, len, "kit=%d;kitname=%s;", k, k >= 0 && k < D->nkits ? D->kits[k].name : "");
    for (int i = 0; i < NP && o < len; i++)
        if (PDEF[i].kind == 0) o += snprintf(buf + o, len - o, "%s=%.4f;", PDEF[i].key, D->pv[i]);
        else if (PDEF[i].kind == 1 || i == P_SLOT) o += snprintf(buf + o, len - o, "%s=%d;", PDEF[i].key, (int)D->pv[i]);
    if (o < len) o += pats_put(&D->wp, buf + o, len - o);
    return o < len ? o : -1;
}

static void e_set_param(void *inst, const char *key, const char *val) {
    db_t *D = inst;
    if (!strcmp(key, "state")) {
        D->kit_pending = -1;   /* a restore says which kit it is, and every knob */
        set_state(D, val);
        return;
    }
    int tk, tn, t = tile_key(key, &tk, &tn);
    if (t == 0) {   /* a tap on the grid */
        apply_kit(D);
        tile_tap(D, tk, tn);
        return;
    }
    if (key[0] == 's' && (key[1] == 's' || key[1] == 'g') && key[2] == '_') {   /* a similar sound / groove */
        int n = atoi(key + 3);
        if (key[3] < '1' || key[3] > '9' || n < 1 || n > NSIM) return;
        apply_kit(D);
        if (ex_take(D, key[1] == 'g', n - 1)) {
            if (key[1] == 'g') { D->changed |= 8; D->changed_p = 0; }
            else { D->changed |= 32; D->changed_m = 0; }
            if (!(D->tp_seen && D->tp_playing) && (int)D->pv[P_PREVIEW]) D->seq_start = 1;   /* heard at once */
        }
        return;
    }
    int i = find_key(key);
    if (i < 0) return;
    if (i == P_KIT) {   /* loaded at the next block (apply_kit); setting the kit already loaded changes nothing */
        int n = atoi(val);
        D->kit_pending = n < 0 ? 0 : n >= D->nkits ? D->nkits - 1 : n;
        kit_ensure(D, D->kit_pending);   /* its file is read here, on the thread that set it, not in the audio block */
        D->info[0] = 0;
        return;
    }
    if (i == P_DRUM) {   /* SOUNDS: read the .mtdrum and play it */
        int n = atoi(val);
        if (D->ndrums <= 0) return;
        n = n < 0 ? 0 : n >= D->ndrums ? D->ndrums - 1 : n;
        D->pv[P_DRUM] = (float)n;
        D->info[0] = 0;
        if (drum_ensure(D, n)) D->aud_drum = 1;
        else snprintf(D->info, sizeof D->info, "CAN'T READ THIS .MTDRUM");
        return;
    }
    apply_kit(D);   /* a kit first, then this on top of it, in the order they came */
    if (i == P_SAVE) {
        if (atof(val) >= 0.5f) {
            int s = (int)D->pv[P_SLOT];
            if (kit_save_user(D, s)) snprintf(D->status, sizeof D->status, "U%02d: CAN'T WRITE TO THE CARD", s);
            else {
                D->pv[P_KIT] = (float)(D->user0 + s - 1);
                ex_kit_changed(D, D->user0 + s - 1);
                snprintf(D->status, sizeof D->status, "SAVED TO U%02d", s);
            }
        }
        return;
    }
    if (i == P_SEARCH || i == P_MUT_SOUND || i == P_MUT_GROOVE || i == P_UNDO) {
        if (atof(val) < 0.5f) return;
        if (i == P_SEARCH) { ex_find(D); return; }
        if (i == P_UNDO) {
            if (!ex_undo(D)) { snprintf(D->ex_info, sizeof D->ex_info, "NOTHING TO UNDO"); return; }
            D->changed |= 8 | 32;
            D->changed_m = D->changed_p = 0;
            D->sq_queued = sel_pattern(D);
        } else {
            ex_push(D);
            if (i == P_MUT_SOUND) { ex_mutate_sound(D); D->changed |= 32; D->changed_m = 0; }
            else ex_mutate_groove(D);
            ex_find(D);   /* what is near the mutation */
        }
        D->ex_info[0] = 0;
        if (!(D->tp_seen && D->tp_playing) && (int)D->pv[P_PREVIEW]) D->seq_start = 1;
        return;
    }
    if (i == P_PREVIEW_PLAY || i == P_FAV || i == P_DRUM_PLAY || i == P_DRUM_TAKE) {
        if (atof(val) < 0.5f) return;   /* the button's release */
        if (i == P_PREVIEW_PLAY) D->seq_start = 1;
        else if (i == P_FAV) { if (!kit_favorite(D)) ex_kit_changed(D, D->nkits - 1); }
        else if (i == P_DRUM_PLAY) D->aud_drum = D->dcur >= 0;
        else if (D->dcur >= 0) {   /* TAKE: the sound into the chosen voice (its choke group stays) */
            int v = (int)D->pv[P_DRUM_VOICE];
            for (int j = 0; j < NVP; j++)
                if (j != VP_CHOKE) D->pv[VK(v, j)] = D->dpv[j];
            snprintf(D->info, sizeof D->info, "V%d: %.36s", v + 1, D->dname && D->dname[D->dcur] ? D->dname[D->dcur] : "");
            D->changed |= 4;   /* the screen shows the voice's new values */
            D->changed_k = 0;
        }
        return;
    }
    if (i == P_PATTERN) {
        int n = atoi(val);
        pick_pattern(D, n < 0 ? 0 : n >= NPAT ? NPAT - 1 : n);
        if (!(D->tp_seen && D->tp_playing) && (int)D->pv[P_PREVIEW]) D->seq_start = 1;   /* heard at once */
        return;
    }
    set_value(D, i, val);
    if (i == P_PAT_LEN) D->wp.p[sel_pattern(D)].len = (uint8_t)D->pv[P_PAT_LEN];
    if (i == P_BASE_NOTE) {   /* V1 NOTE on MASTER: all eight voices in a row from there */
        for (int v = 0; v < NV; v++) D->pv[VNOTE(v)] = (float)(BASE_NOTE[(int)D->pv[P_BASE_NOTE] % 5] + v);
        D->changed |= 16;
        D->changed_n = 0;
    }
    if (i == P_SEQ && !(int)D->pv[P_SEQ]) D->sq_step = -1;
    if (i == P_SLOT) D->status[0] = 0;
    if (i == P_PREVIEW && !(int)D->pv[P_PREVIEW]) D->seq_on = 0;
}

/* ---- what the screen reads -------------------------------------------------------------------------------------- */

static int fmt_hz(char *buf, int len, float hz) {
    return hz < 1000.0f ? snprintf(buf, len, "%.0f Hz", hz) : snprintf(buf, len, "%.2f kHz", hz / 1000.0f);
}
static int fmt_ms(char *buf, int len, float ms) {
    return ms < 10.0f ? snprintf(buf, len, "%.1f ms", ms) : ms < 1000.0f ? snprintf(buf, len, "%.0f ms", ms)
                                                          : snprintf(buf, len, "%.2f s", ms / 1000.0f);
}
static int fmt_db(char *buf, int len, float knob) {
    return knob <= 0.0f ? snprintf(buf, len, "-inf dB") : snprintf(buf, len, "%+.1f dB", k_db(knob));
}

static int voice_display(const db_t *D, int v, int k, char *buf, int len) {
    const float x = D->pv[VK(v, k)];
    switch (k) {
    case VP_OSC_FREQ: case VP_N_FREQ: case VP_EQ_FREQ: return fmt_hz(buf, len, k_freq(x));
    case VP_OSC_ATK: case VP_OSC_DCY: case VP_N_ATK: case VP_N_DCY: return fmt_ms(buf, len, k_time(x));
    case VP_MOD_AMT: return snprintf(buf, len, "%+.1f st", k_modamt(x));
    case VP_MOD_RATE: {
        float hz = k_modrate(x);
        if ((int)D->pv[VK(v, VP_MOD_MODE)] == M_DECAY)   /* Microtonic shows the decay time */
            return hz <= 0.0f ? snprintf(buf, len, "INF") : fmt_ms(buf, len, 1000.0f / hz);
        return hz < 10.0f ? snprintf(buf, len, "%.2f Hz", hz) : fmt_hz(buf, len, hz);
    }
    case VP_N_Q: return snprintf(buf, len, x >= 60.0f ? "%.0f" : "%.2f", k_q(x));
    case VP_MIX: return snprintf(buf, len, "%.0f / %.0f", x, 100.0f - x);   /* osc / noise */
    case VP_EQ_GAIN: return snprintf(buf, len, "%+.1f dB", k_eqgain(x));
    case VP_LEVEL: return fmt_db(buf, len, x);
    case VP_PAN: return fabsf(x) < 0.5f ? snprintf(buf, len, "C") : snprintf(buf, len, "%s %.0f", x < 0 ? "L" : "R", fabsf(x));
    }
    return 0;
}

static int e_get_param(void *inst, const char *key, char *buf, int len) {
    db_t *D = inst;
    int tk, tn, t = tile_key(key, &tk, &tn);
    if (t >= 0) return tile_get(D, tk, tn, t, buf, len);
    if (key[0] == 's' && (key[1] == 's' || key[1] == 'g') && key[2] == '_' && key[3] >= '1' && key[3] <= '9')
        return ex_tile(D, key[1] == 'g', atoi(key + 3) - 1, strstr(key, "_on") != NULL, buf, len);
    if (!strcmp(key, "ex_info")) return ex_status(D, buf, len);
    if (!strcmp(key, "state")) {
        apply_kit(D);
        return get_state(D, buf, len);
    }
    if (!strcmp(key, "kit_count")) return snprintf(buf, len, "%d", D->nkits);
    int cur = D->kit_pending >= 0 ? D->kit_pending : (int)D->pv[P_KIT];   /* as set, loaded or not */
    if (!strcmp(key, "kit")) return snprintf(buf, len, "%d", cur);
    if (!strcmp(key, "kit_name") || !strncmp(key, "kit_name_at:", 12)) {
        int n = key[8] ? atoi(key + 12) : cur;
        return n >= 0 && n < D->nkits ? snprintf(buf, len, "%s", D->kits[n].name) : 0;
    }
    if (!strcmp(key, "info")) return snprintf(buf, len, "%s", D->info);
    if (!strcmp(key, "drum_name")) {
        if (D->ndrums <= 0) return snprintf(buf, len, "NO .MTDRUM IN THE FOLDER");
        int n = (int)D->pv[P_DRUM];
        return snprintf(buf, len, "%d/%d  %s", n + 1, D->ndrums, D->dname && D->dname[n] ? D->dname[n] : "");
    }
    if (!strcmp(key, "slot_name")) {
        if (D->status[0]) return snprintf(buf, len, "%s", D->status);
        int s = (int)D->pv[P_SLOT];
        const kit_t *k = s >= 1 && s <= NUSER && D->user0 + s - 1 < D->nkits ? &D->kits[D->user0 + s - 1] : NULL;
        return snprintf(buf, len, "%s", k ? k->name : "");
    }
    int i = find_key(key);
    if (i >= 0) {
        if (PDEF[i].kind == 1 || PDEF[i].kind == 2) return snprintf(buf, len, "%d", (int)D->pv[i]);
        if (PDEF[i].kind == 0) return snprintf(buf, len, "%.3f", D->pv[i]);
        return snprintf(buf, len, "0");
    }
    size_t kl = strlen(key);
    if (kl <= 8 || strcmp(key + kl - 8, "_display")) return 0;
    char base[48];
    snprintf(base, sizeof base, "%.*s", (int)(kl - 8), key);
    i = find_key(base);
    if (i < 0) return 0;
    if (i == P_VOLUME) return fmt_db(buf, len, D->pv[i]);
    if (i == P_SWING) return snprintf(buf, len, "%.0f %%", D->pv[i]);
    if (i == P_LPF_CUT) return D->pv[i] >= 99.5f ? snprintf(buf, len, "OPEN") : fmt_hz(buf, len, k_freq(D->pv[i]));
    if (i >= P_V1_OSC_WAVE && i < P_V1_OSC_WAVE + NV * VSTRIDE)
        return voice_display(D, (i - P_V1_OSC_WAVE) / VSTRIDE, (i - P_V1_OSC_WAVE) % VSTRIDE, buf, len);
    return 0;
}

/* ---- life ------------------------------------------------------------------------------------------------------ */

static void *e_create(const char *dir) {
    db_t *D = calloc(1, sizeof *D);
    if (!D) return NULL;
    snprintf(D->dir, sizeof D->dir, "%s", dir ? dir : "");
    for (int i = 0; i < NP; i++) D->pv[i] = PDEF[i].def;
    D->kit_pending = -1;
    D->seed = 0x2545f491u;
    D->lpf_cut = D->lpf_res = -1.0f;
    D->sq_queued = -1;
    D->sq_last = LONG_MIN / 2;
    D->sq_step = -1;
    patset_init(&D->wp);
    kits_load(D);
    if (!D->kits) { free(D); return NULL; }
    D->wp = D->kits[0].ps;   /* the first kit's patterns, with its sounds (the defaults) */
    ex_start(D);
    return D;
}

static void e_destroy(void *inst) {
    db_t *D = inst;
    if (!D) return;
    ex_stop(D);
    kits_free(D);
    free(D);
}

static const mpc_engine_t ENGINE = {e_create, e_destroy, e_midi, e_set_param, e_get_param, e_render};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }

/* After each block the wrapper asks which parameters changed here (the pads' browsing), one at a time, and tells MPC
 * (engine.h), so the screen follows: KIT, SOUND, and after a TAKE the voice's controls. */
int mpc_engine_changed(void *inst, char *key, int len) {
    db_t *D = inst;
    if (D->changed & 1) { D->changed &= ~1; return snprintf(key, len, "kit") > 0; }
    if (D->changed & 2) { D->changed &= ~2; return snprintf(key, len, "drum") > 0; }
    if (D->changed & 4) {   /* TAKE: the voice's controls, one per call */
        int v = (int)D->pv[P_DRUM_VOICE];
        if (D->changed_k < NVP) return snprintf(key, len, "%s", PDEF[VK(v, D->changed_k++)].key) > 0;
        D->changed &= ~4;
        D->changed_k = 0;
    }
    if (D->changed & 8) {   /* a chain, pad 16, a groove taken: PATTERN, LENGTH, SWING */
        static const char *const K[3] = {"pattern", "pat_len", "swing"};
        if (D->changed_p < 3) return snprintf(key, len, "%s", K[D->changed_p++]) > 0;
        D->changed &= ~8;
        D->changed_p = 0;
    }
    if (D->changed & 32) {   /* a sound taken, mutated or undone: every voice control */
        if (D->changed_m < NV * NVP) return snprintf(key, len, "%s", PDEF[VK(0, 0) + D->changed_m++].key) > 0;
        D->changed &= ~32;
        D->changed_m = 0;
    }
    if (D->changed & 16) {   /* BASE NOTE set the eight notes */
        if (D->changed_n < NV) return snprintf(key, len, "v%d_note", ++D->changed_n) > 0;
        D->changed &= ~16;
        D->changed_n = 0;
    }
    return 0;
}

/* The wrapper calls this once per host buffer, before rendering it (engine.h): the preview plays at the Force's
 * tempo, and stops (and doesn't start) while the Force plays. */
void mpc_engine_transport(void *inst, double bpm, double ppq, int playing) {
    db_t *D = inst;
    int was = D->tp_seen && D->tp_playing;
    if (playing && !was) D->seq_on = 0;
    /* where the song should be by now; anything else (a start, a loop, a locate) lines the sequencer up again */
    double expect = D->tp_ppq + D->tp_frames * (D->tp_bpm > 20.0 ? D->tp_bpm : 120.0) / 60.0 / SR;
    if (!playing || !was || ppq < 0.0 || fabs(ppq - expect) > 0.02) { D->sq_last = LONG_MIN / 2; D->sq_resync = 1; }
    D->tp_ppq = ppq;
    D->tp_frames = 0;
    D->tp_seen = 1;
    D->tp_playing = playing != 0;
    if (bpm > 20.0) D->tp_bpm = bpm;
}
