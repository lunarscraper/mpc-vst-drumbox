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

static void load_kit(db_t *D, int n) {
    if (n < 0 || n >= D->nkits) return;
    const kit_t *k = &D->kits[n];
    for (int v = 0; v < NV; v++)
        for (int j = 0; j < NVP; j++) D->pv[VK(v, j)] = k->pv[v][j];
    D->pv[P_KIT] = (float)n;
    if (k->user) D->pv[P_SLOT] = (float)k->user;
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

static void e_midi(void *inst, const uint8_t *m, int len) {
    db_t *D = inst;
    apply_kit(D);
    if (len < 2) return;
    int st = m[0] & 0xF0, d1 = m[1] & 0x7F, d2 = len > 2 ? m[2] & 0x7F : 0;
    if (st == 0x90 && d2 > 0) {
        int v = d1 - BASE_NOTE[(int)D->pv[P_BASE_NOTE] % 5];
        if (v < 0 || v >= NV) return;
        int g = (int)D->pv[VK(v, VP_CHOKE)];
        if (g > 0)
            for (int u = 0; u < NV; u++)
                if (u != v && (int)D->pv[VK(u, VP_CHOKE)] == g) voice_choke(D, u);
        voice_trigger(D, v, (float)d2);
    } else if (st == 0xB0 && (d1 == 120 || d1 == 123)) {
        voice_kill(D);
    }
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
    for (int off = 0; off < frames; off += BLK) {
        int n = frames - off < BLK ? frames - off : BLK;
        float L[BLK] = {0}, R[BLK] = {0};
        for (int v = 0; v < NV; v++) voice_render(D, v, L, R, n);
        for (int i = 0; i < n; i++) {
            float l = L[i] * vol, r = R[i] * vol;
            if (lpf) {
                l = bq_run(&D->lpf, &D->lpfs[0], l);
                r = bq_run(&D->lpf, &D->lpfs[1], r);
            }
            out[2 * (off + i)] = out_sample(l);
            out[2 * (off + i) + 1] = out_sample(r);
        }
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
    set_value(D, i, val);
    if (i == P_SLOT) D->status[0] = 0;
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
