/* Drumbox engine: parameters, MIDI, kits, user slots and the master section (VOLUME, low-pass).
 *
 * MIDI: notes BASE NOTE .. BASE NOTE + 7 (36-43, C1-G1, to start with: a Force's drum pads; or C0, G#1 = pads 9-16,
 * C2, C3) play voices 1-8, on any
 * channel; velocity reaches each voice through its OSC / NOISE / MOD VEL amounts. A voice is one-shot: note-offs do
 * nothing. A new hit fades the voice's (or its choke group's) last one out in 50 ms. CC 120 / 123 stop everything. */
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

static void seq_begin(db_t *D, const pattern_t *p);

static void load_kit(db_t *D, int n) {
    if (n < 0 || n >= D->nkits) return;
    kit_ensure(D, n);   /* normally done already where KIT was set (e_set_param): the file isn't read here */
    const kit_t *k = &D->kits[n];
    for (int v = 0; v < NV; v++)
        for (int j = 0; j < NVP; j++) D->pv[VK(v, j)] = k->pv[v][j];
    D->pv[P_KIT] = (float)n;
    if (k->user) D->pv[P_SLOT] = (float)k->user;
    /* PREVIEW: the kit's pattern A plays (twice), unless the Force is playing */
    if ((int)D->pv[P_PREVIEW] && !(D->tp_seen && D->tp_playing)) seq_begin(D, &k->pat);
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

/* PAD BROWSE: pads 9-15 (BASE NOTE + 8 .. + 14) while the Force is stopped. A kit picked here is read on the audio
 * thread (a few ms, only while nothing plays); the screen follows through mpc_engine_changed. */
static void pad_browse(db_t *D, int k) {
    int cur = (int)D->pv[P_KIT];
    if (k == 0 || k == 1) {
        int n = cur + (k ? 1 : -1);
        if (n < 0 || n >= D->nkits) return;
        kit_ensure(D, n);
        D->info[0] = 0;
        load_kit(D, n);
        D->changed |= 1;
    } else if (k == 2) kit_favorite(D);
    else if (k == 3) {
        if (cur >= 0 && cur < D->nkits) seq_begin(D, &D->kits[cur].pat);
    } else if (k == 4 || k == 5) {
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
    }
}

static void e_midi(void *inst, const uint8_t *m, int len) {
    db_t *D = inst;
    apply_kit(D);
    if (len < 2) return;
    int st = m[0] & 0xF0, d1 = m[1] & 0x7F, d2 = len > 2 ? m[2] & 0x7F : 0;
    if (st == 0x90 && d2 > 0) {
        int v = d1 - BASE_NOTE[(int)D->pv[P_BASE_NOTE] % 5];
        if (v >= NV && v < NV + 7 && (int)D->pv[P_PAD_BROWSE] && !(D->tp_seen && D->tp_playing)) {
            pad_browse(D, v - NV);
            return;
        }
        if (v < 0 || v >= NV) return;
        D->seq_on = 0;   /* playing the pads ends a preview */
        hit(D, v, (float)d2);
    } else if (st == 0xB0 && (d1 == 120 || d1 == 123)) {
        D->seq_on = 0;
        voice_kill(D);
    }
}

/* ---- the preview: pattern A, twice, at the Force's tempo (the preset's own without one) ------------------------- *
 * Accented steps play at velocity 127, the others at 96; the preset's mutes are kept. A kit without a pattern (a
 * .tkit, a built-in nb_tonic kit) plays its eight voices one after another, an eighth note apart. Only to hear a kit
 * while browsing: it never runs while the Force plays, and a pad, a new kit or PLAY on the Force stops it. */
static void seq_begin(db_t *D, const pattern_t *p) {
    pattern_t q = *p;
    if (!q.has) {
        memset(&q, 0, sizeof q);
        q.has = 1;
        q.len = 16;
        q.div = 16;
        for (int v = 0; v < NV; v++) q.trig[v] = (uint16_t)(1u << (2 * v));
        D->seq_total = 16;
    } else D->seq_total = 2 * q.len;
    double bpm = D->tp_bpm > 20.0 ? D->tp_bpm : q.tempo > 20.0f ? q.tempo : 120.0;
    D->seq_frames = (int)(SR * 60.0 / bpm * 4.0 / (q.div > 0 ? q.div : 16) + 0.5);
    D->seq = q;
    D->seq_step = 0;
    D->seq_left = 0;
    D->seq_on = 1;
}

static void seq_fire(db_t *D) {
    int s = D->seq_step % (D->seq.len > 0 ? D->seq.len : 16);
    for (int v = 0; v < NV; v++)
        if (!(D->seq.mute >> v & 1) && (D->seq.trig[v] >> s & 1)) hit(D, v, (D->seq.acc[v] >> s & 1) ? 127.0f : 96.0f);
    if (++D->seq_step >= D->seq_total) D->seq_on = 0;
    D->seq_left = D->seq_frames;
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
    if (D->seq_start) {   /* PLAY (PREVIEW): the loaded kit's pattern from the start */
        D->seq_start = 0;
        int k = (int)D->pv[P_KIT];
        if (k >= 0 && k < D->nkits) seq_begin(D, &D->kits[k].pat);
    }
    if (D->aud_drum) {   /* SOUNDS: the picked .mtdrum on its own voice */
        D->aud_drum = 0;
        if (D->dcur >= 0) voice_trigger(D, AUD, 110.0f);
    }
    for (int off = 0; off < frames;) {
        if (D->seq_on && D->seq_left <= 0) seq_fire(D);
        int n = frames - off < BLK ? frames - off : BLK;
        if (D->seq_on && D->seq_left < n) n = D->seq_left;
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
    ftz_off(fp);
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

static void set_state(db_t *D, const char *s) {
    char kv[256], kitname[40] = "";
    int kit = -1;
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
                else if (i == P_KIT) kit = atoi(eq);
                else if (i >= 0) set_value(D, i, eq);
            }
        }
        s += n;
        if (*s) s++;
    }
    /* the kit's number, or where that name is now (files added or removed since) */
    if (kit >= 0 && kit < D->nkits && (!kitname[0] || !strcmp(D->kits[kit].name, kitname))) D->pv[P_KIT] = (float)kit;
    else if (kitname[0]) {
        for (int k = 0; k < D->nkits; k++)
            if (!strcmp(D->kits[k].name, kitname)) { D->pv[P_KIT] = (float)k; break; }
    }
}

static int get_state(const db_t *D, char *buf, int len) {
    int k = (int)D->pv[P_KIT];
    int o = snprintf(buf, len, "kit=%d;kitname=%s;", k, k >= 0 && k < D->nkits ? D->kits[k].name : "");
    for (int i = 0; i < NP && o < len; i++)
        if (PDEF[i].kind == 0) o += snprintf(buf + o, len - o, "%s=%.4f;", PDEF[i].key, D->pv[i]);
        else if (PDEF[i].kind == 1 || i == P_SLOT) o += snprintf(buf + o, len - o, "%s=%d;", PDEF[i].key, (int)D->pv[i]);
    return o < len ? o : -1;
}

static void e_set_param(void *inst, const char *key, const char *val) {
    db_t *D = inst;
    if (!strcmp(key, "state")) {
        D->kit_pending = -1;   /* a restore says which kit it is, and every knob */
        set_state(D, val);
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
                snprintf(D->status, sizeof D->status, "SAVED TO U%02d", s);
            }
        }
        return;
    }
    if (i == P_PREVIEW_PLAY || i == P_FAV || i == P_DRUM_PLAY || i == P_DRUM_TAKE) {
        if (atof(val) < 0.5f) return;   /* the button's release */
        if (i == P_PREVIEW_PLAY) D->seq_start = 1;
        else if (i == P_FAV) kit_favorite(D);
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
    set_value(D, i, val);
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
    kits_load(D);
    if (!D->kits) { free(D); return NULL; }
    return D;
}

static void e_destroy(void *inst) {
    db_t *D = inst;
    if (!D) return;
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
    return 0;
}

/* The wrapper calls this once per host buffer, before rendering it (engine.h): the preview plays at the Force's
 * tempo, and stops (and doesn't start) while the Force plays. */
void mpc_engine_transport(void *inst, double bpm, double ppq, int playing) {
    db_t *D = inst;
    (void)ppq;
    if (playing && !(D->tp_seen && D->tp_playing)) D->seq_on = 0;
    D->tp_seen = 1;
    D->tp_playing = playing != 0;
    if (bpm > 20.0) D->tp_bpm = bpm;
}
