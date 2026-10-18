/* EXPLORE: presets with a similar sound or a similar groove, MUTATE and UNDO.
 *
 * The index: when the plugin is inserted, a thread of its own (low priority, a pause after each file) reads every
 * preset file once and keeps a copy of its eight voices and twelve patterns. It writes only its own entries (ix,
 * ix_ok); the kit list the audio side uses is never touched by it. Built-in kits and user slots are in memory and
 * indexed at once.
 *
 * Similar sound: the eight voices compared knob by knob (each scaled to its range, the ones that shape a sound most
 * weigh most: pitch, decays, mix, noise filter). Similar groove: the selected pattern against each of the other
 * presets' twelve, voice by voice: a hit one has and the other hasn't costs 1, half when the other has one a step
 * away; an accent where both hit costs a quarter. Kick, snare and the hi-hats (V1, V5, V7, V8: their usual places
 * in Patternarium's presets) weigh more. The lists are found again when a kit is picked, a mutation is made or
 * SEARCH HERE is tapped, not when a tile is tapped: the six alternatives stay where they are to compare.
 *
 * MUTATE SOUND moves every knob of the voices chosen (ALL or one) by a random amount, AMOUNT % of a quarter of its
 * range at most (level and pan less), and switches an option now and then. MUTATE GROOVE edits the selected pattern
 * row by row: hits move a step, drop out, appear where the preset's other patterns have them (or on the beat),
 * accents flip; the number of edits grows with AMOUNT and with the row's hits, and the row keeps roughly its density.
 * Every change here (and a similar sound or groove taken) can be undone, sixteen steps back. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "drumbox.h"

/* ---- the index thread ------------------------------------------------------------------------------------------ */

static void feat_from_kit(feat_t *f, const kit_t *k) {
    memcpy(f->pv, k->pv, sizeof f->pv);
    f->ps = k->ps;
}

static void *ix_main(void *arg) {
    db_t *D = arg;
    if (nice(10) == -1) { /* stays at the normal priority */ }
    kit_t *tmp = malloc(sizeof *tmp);
    for (int k = 0; tmp && k < D->ix_n && !__atomic_load_n(&D->ix_stop, __ATOMIC_ACQUIRE); k++) {
        if (!D->ix_path[k] || __atomic_load_n(&D->ix_ok[k], __ATOMIC_ACQUIRE)) continue;
        int ok = kit_read_file(D->ix_path[k], tmp);
        if (ok) feat_from_kit(&D->ix[k], tmp);
        __atomic_store_n(&D->ix_ok[k], ok ? 1 : 2, __ATOMIC_RELEASE);
        __atomic_add_fetch(&D->ix_count, 1, __ATOMIC_RELEASE);
        usleep(300);   /* the card and the CPU stay free for MPC */
    }
    free(tmp);
    return NULL;
}

void ex_start(db_t *D) {
    D->snd_src = D->grv_src = 0;
    D->mseed = (uint32_t)time(NULL) * 2654435761u | 1u;
    D->ix = calloc(MAX_KITS, sizeof *D->ix);
    D->ix_ok = calloc(MAX_KITS, 1);
    D->ix_path = calloc(MAX_KITS, sizeof *D->ix_path);
    if (!D->ix || !D->ix_ok || !D->ix_path) return;
    int files = 0;
    for (int k = 0; k < D->nkits; k++) {
        if (D->kits[k].path) {
            D->ix_path[k] = strdup(D->kits[k].path);
            files += D->ix_path[k] != NULL;
        } else if (D->kits[k].loaded && D->kits[k].used) {   /* built in, a user slot: in memory */
            feat_from_kit(&D->ix[k], &D->kits[k]);
            D->ix_ok[k] = 1;
            D->ix_count++;
        } else {
            D->ix_ok[k] = 2;   /* an empty slot */
            D->ix_count++;
        }
    }
    D->ix_n = D->nkits;
    D->sim_dirty = 1;
    if (files && !pthread_create(&D->ix_th, NULL, ix_main, D)) D->ix_running = 1;
}

void ex_stop(db_t *D) {
    if (D->ix_running) {
        __atomic_store_n(&D->ix_stop, 1, __ATOMIC_RELEASE);
        pthread_join(D->ix_th, NULL);
        D->ix_running = 0;
    }
    for (int k = 0; D->ix_path && k < MAX_KITS; k++) free(D->ix_path[k]);
    free(D->ix_path);
    free(D->ix_ok);
    free(D->ix);
    D->ix_path = NULL;
    D->ix_ok = NULL;
    D->ix = NULL;
}

/* SAVE wrote a slot, FAVORIT added a kit: its entry from the list (both are in memory, not files the thread reads) */
void ex_kit_changed(db_t *D, int k) {
    if (!D->ix || k < 0 || k >= MAX_KITS || (k < D->ix_n && D->ix_path[k])) return;
    feat_from_kit(&D->ix[k], &D->kits[k]);
    if (!__atomic_exchange_n(&D->ix_ok[k], 1, __ATOMIC_ACQ_REL)) __atomic_add_fetch(&D->ix_count, 1, __ATOMIC_RELEASE);
}

/* ---- distances -------------------------------------------------------------------------------------------------- */

static const float W_SND[NVP] = {   /* per voice parameter, in VP_ order */
    [VP_OSC_WAVE] = 0.5f, [VP_OSC_FREQ] = 2.0f, [VP_OSC_ATK] = 0.3f, [VP_OSC_DCY] = 1.5f, [VP_MOD_MODE] = 0.5f,
    [VP_MOD_AMT] = 1.0f, [VP_MOD_RATE] = 0.7f, [VP_N_FILTER] = 0.5f, [VP_N_FREQ] = 1.5f, [VP_N_Q] = 0.5f,
    [VP_N_ENV] = 0.5f, [VP_N_ATK] = 0.3f, [VP_N_DCY] = 1.5f, [VP_N_STEREO] = 0.1f, [VP_MIX] = 1.5f, [VP_DIST] = 0.7f,
    [VP_EQ_FREQ] = 0.4f, [VP_EQ_GAIN] = 0.4f, [VP_LEVEL] = 0.6f, [VP_PAN] = 0.1f,
};
static const float W_VOICE[NV] = {1.5f, 1.0f, 1.0f, 1.0f, 1.5f, 1.0f, 1.3f, 1.3f};   /* kick, snare, hats more */

/* per parameter: its weight over its range (a knob), or its weight (an option: 0 or 1 apart); in that order */
static float SC[NVP];
static int NK, NO, KJ[NVP], OJ[NVP];
static void snd_init(void) {
    if (NK + NO) return;
    for (int j = 0; j < NVP; j++) {
        if (W_SND[j] == 0.0f) continue;
        const pdef_t *p = &PDEF[VK(0, j)];
        if (p->kind == 1) OJ[NO++] = j;
        else KJ[NK++] = j;
        SC[j] = p->kind == 1 ? W_SND[j] : W_SND[j] / (p->max > p->min ? p->max - p->min : 1.0f);
    }
}

/* cur: D->pv from VK(0, 0); stops once past `stop` (it can't be among the nearest then) */
static float snd_dist(const float *cur, const float pv[NV][NVP], float stop) {
    float d = 0;
    for (int v = 0; v < NV && d < stop; v++) {
        const float *a = cur + VSTRIDE * v, *b = pv[v];
        for (int i = 0; i < NK; i++) d += SC[KJ[i]] * fabsf(a[KJ[i]] - b[KJ[i]]);
        for (int i = 0; i < NO; i++) d += SC[OJ[i]] * ((int)a[OJ[i]] != (int)b[OJ[i]]);
    }
    return d;
}

static uint16_t len_mask(int len) { return (uint16_t)(len >= 16 || len <= 0 ? 0xFFFF : (1u << len) - 1); }

static uint16_t near_bits(uint16_t b, uint16_t m) { return (uint16_t)(((b << 1) | (b >> 1)) & m); }

/* a pattern's steps as heard over 16 steps: a shorter one repeats (an 8-step pattern twice, 12 steps then 4 again) */
static uint16_t tile16(uint16_t b, int len) {
    if (len <= 0 || len >= 16) return b;
    uint16_t o = 0;
    for (int s = 0; s < 16; s++) o |= (uint16_t)((b >> (s % len) & 1) << s);
    return o;
}

static float grv_dist(const pattern_t *a, const pattern_t *b, float stop) {
    int la = a->len ? a->len : 16, lb = b->len ? b->len : 16;
    float d = la == lb ? 0.0f : 1.0f;   /* a different length: a little apart even when the steps agree */
    for (int v = 0; v < NV && d < stop; v++) {
        uint16_t x = tile16(a->trig[v], la), y = tile16(b->trig[v], lb), m = 0xFFFF;
        uint16_t only_x = x & ~y, only_y = y & ~x;
        float c = 0;
        c += __builtin_popcount(only_x & ~near_bits(y, m)) + 0.5f * __builtin_popcount(only_x & near_bits(y, m));
        c += __builtin_popcount(only_y & ~near_bits(x, m)) + 0.5f * __builtin_popcount(only_y & near_bits(x, m));
        c += 0.25f * __builtin_popcount((tile16(a->acc[v], la) ^ tile16(b->acc[v], lb)) & x & y);
        d += W_VOICE[v] * c;
    }
    return d;
}

static int pat_empty(const pattern_t *p) {
    for (int v = 0; v < NV; v++)
        if (p->trig[v] & len_mask(p->len)) return 0;
    return 1;
}

static void keep_best(int *ids, int *pats, float *ds, int *n, int k, int pat, float d) {
    if (*n == NSIM && d >= ds[NSIM - 1]) return;
    int i = *n < NSIM ? (*n)++ : NSIM - 1;
    while (i > 0 && ds[i - 1] > d) {
        ids[i] = ids[i - 1];
        ds[i] = ds[i - 1];
        if (pats) pats[i] = pats[i - 1];
        i--;
    }
    ids[i] = k;
    ds[i] = d;
    if (pats) pats[i] = pat;
}

void ex_find(db_t *D) {
    D->sim_dirty = 0;
    D->nsim_snd = D->nsim_grv = 0;
    if (!D->ix) return;
    D->ix_last = __atomic_load_n(&D->ix_count, __ATOMIC_ACQUIRE);
    float ds[NSIM], dg[NSIM];
    snd_init();
    const pattern_t *cur = &D->wp.p[(int)D->pv[P_PATTERN] % NPAT];
    int groove = !pat_empty(cur);
    for (int k = 0; k < D->nkits && k < MAX_KITS; k++) {
        if (__atomic_load_n(&D->ix_ok[k], __ATOMIC_ACQUIRE) != 1) continue;
        const feat_t *f = &D->ix[k];
        if (k != D->snd_src) {
            float d = snd_dist(&D->pv[VK(0, 0)], f->pv, D->nsim_snd == NSIM ? ds[NSIM - 1] : 1e30f);
            if (d > 1e-4f) keep_best(D->sim_snd, NULL, ds, &D->nsim_snd, k, 0, d);
        }
        if (groove && k != D->grv_src) {
            float best = D->nsim_grv == NSIM ? dg[NSIM - 1] : 1e30f;
            int bp = -1;
            for (int i = 0; i < NPAT; i++) {
                if (pat_empty(&f->ps.p[i])) continue;
                float d = grv_dist(cur, &f->ps.p[i], best);
                if (d < best) { best = d; bp = i; }
            }
            if (bp >= 0 && best > 1e-4f) keep_best(D->sim_grv, D->sim_grv_pat, dg, &D->nsim_grv, k, bp, best);
        }
    }
}

/* the index grew: find the lists again once a quarter more is read, and when it is complete */
void ex_poll(db_t *D) {
    if (!D->ix) return;
    int c = __atomic_load_n(&D->ix_count, __ATOMIC_ACQUIRE);
    if (D->sim_dirty || (c != D->ix_last && (c >= D->ix_n || c - D->ix_last >= D->ix_n / 4 + 1))) ex_find(D);
}

/* ---- UNDO ---------------------------------------------------------------------------------------------------- */

void ex_push(db_t *D) {
    undo_t *u = &D->undo[D->undo_top];
    memcpy(u->vp, &D->pv[VK(0, 0)], sizeof u->vp);
    u->wp = D->wp;
    u->pattern = D->pv[P_PATTERN];
    u->swing = D->pv[P_SWING];
    u->snd_src = D->snd_src;
    u->grv_src = D->grv_src;
    D->undo_top = (D->undo_top + 1) % NUNDO;
    if (D->nundo < NUNDO) D->nundo++;
}

int ex_undo(db_t *D) {
    if (!D->nundo) return 0;
    D->undo_top = (D->undo_top + NUNDO - 1) % NUNDO;
    D->nundo--;
    const undo_t *u = &D->undo[D->undo_top];
    memcpy(&D->pv[VK(0, 0)], u->vp, sizeof u->vp);
    D->wp = u->wp;
    D->pv[P_PATTERN] = u->pattern;
    D->pv[P_PAT_LEN] = (float)D->wp.p[(int)u->pattern % NPAT].len;
    D->pv[P_SWING] = u->swing;
    D->snd_src = u->snd_src;
    D->grv_src = u->grv_src;
    return 1;
}

/* ---- MUTATE ------------------------------------------------------------------------------------------------------ */

static uint32_t mr(db_t *D) {   /* xorshift32 */
    uint32_t x = D->mseed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return D->mseed = x;
}
static float mf(db_t *D) { return (mr(D) >> 8) * (1.0f / 16777216.0f); }       /* 0 .. 1 */
static float mg(db_t *D) { return (mf(D) + mf(D) + mf(D) - 1.5f) * 2.0f; }    /* about -3 .. 3, sd 1 */

static void targets(const db_t *D, int *from, int *to) {
    int t = (int)D->pv[P_MUT_TARGET];   /* 0 ALL, 1..8 a voice */
    *from = t ? t - 1 : 0;
    *to = t ? t : NV;
}

void ex_mutate_sound(db_t *D) {
    float a = D->pv[P_MUT_AMT] / 100.0f;
    int v0, v1;
    targets(D, &v0, &v1);
    for (int v = v0; v < v1; v++)
        for (int j = 0; j < NVP; j++) {
            if (j == VP_OSC_VEL || j == VP_N_VEL || j == VP_MOD_VEL || j == VP_CHOKE) continue;
            const pdef_t *p = &PDEF[VK(v, j)];
            float *x = &D->pv[VK(v, j)];
            if (p->kind == 1) {
                if (p->nopts > 1 && mf(D) < a * 0.15f) *x = (float)(((int)*x + 1 + (int)(mr(D) % (uint32_t)(p->nopts - 1))) % p->nopts);
                continue;
            }
            float s = j == VP_LEVEL ? 0.06f : j == VP_PAN ? 0.1f : 0.25f;
            *x = clampf(*x + mg(D) * a * s * (p->max - p->min), p->min, p->max);
        }
    D->snd_src = -1;
}

void ex_mutate_groove(db_t *D) {
    float a = D->pv[P_MUT_AMT] / 100.0f;
    int sel = (int)D->pv[P_PATTERN] % NPAT, v0, v1;
    targets(D, &v0, &v1);
    pattern_t *q = &D->wp.p[sel];
    int L = q->len ? q->len : 16;
    uint16_t m = len_mask(L);
    for (int v = v0; v < v1; v++) {
        /* where this voice plays in the preset's other patterns, and how often */
        float w[16], ref = 0;
        int npat = 0, nacc = 0, nhit = 0;
        for (int s = 0; s < 16; s++) w[s] = s % 4 == 0 ? 0.4f : s % 2 == 0 ? 0.2f : 0.1f;
        for (int i = 0; i < NPAT; i++) {
            const pattern_t *o = &D->wp.p[i];
            uint16_t om = len_mask(o->len);
            if (pat_empty(o)) continue;
            npat++;
            ref += __builtin_popcount(o->trig[v] & om);
            nhit += __builtin_popcount(o->trig[v] & om);
            nacc += __builtin_popcount(o->acc[v] & o->trig[v] & om);
            for (int s = 0; s < 16; s++)
                if (o->trig[v] >> s & 1) w[s] += 1.0f;
        }
        ref = npat ? ref / npat : 0;
        int hits = __builtin_popcount(q->trig[v] & m);
        if (!hits && ref < 0.5f && v1 - v0 > 1) continue;   /* a voice the preset hardly uses: left alone with ALL */
        float pacc = nhit ? (float)nacc / nhit : 0.25f;
        int edits = (int)(a * (2.0f + hits) * 1.2f + mf(D));
        for (int e = 0; e < edits; e++) {
            hits = __builtin_popcount(q->trig[v] & m);
            float r = mf(D);
            if (hits > ref * 1.5f + 1) r = 0.6f + 0.25f * mf(D);   /* too busy: drop one */
            else if (hits < ref * 0.5f) r = 0.35f + 0.25f * mf(D);  /* too sparse: add one */
            if (hits && r < 0.35f) {   /* move a hit a step (now and then two) */
                int s = -1, n = (int)(mr(D) % (uint32_t)hits);
                for (int i = 0; i < L; i++)
                    if ((q->trig[v] >> i & 1) && n-- == 0) { s = i; break; }
                int dir = (mr(D) & 1 ? 1 : -1) * (mf(D) < 0.3f ? 2 : 1), t = ((s + dir) % L + L) % L;
                if (s >= 0 && !(q->trig[v] >> t & 1)) {
                    int acc = q->acc[v] >> s & 1;
                    q->trig[v] &= (uint16_t)~(1u << s);
                    q->acc[v] &= (uint16_t)~(1u << s);
                    q->trig[v] |= (uint16_t)(1u << t);
                    if (acc) q->acc[v] |= (uint16_t)(1u << t);
                }
            } else if (r < 0.6f || !hits) {   /* add one where it is likely */
                float tot = 0;
                for (int s = 0; s < L; s++)
                    if (!(q->trig[v] >> s & 1)) tot += w[s];
                if (tot <= 0) continue;
                float x = mf(D) * tot;
                for (int s = 0; s < L; s++) {
                    if (q->trig[v] >> s & 1) continue;
                    if ((x -= w[s]) <= 0) {
                        q->trig[v] |= (uint16_t)(1u << s);
                        if (mf(D) < pacc) q->acc[v] |= (uint16_t)(1u << s);
                        break;
                    }
                }
            } else if (r < 0.85f) {   /* drop one, the less likely ones first */
                int s = -1;
                float lo = 1e9f;
                for (int i = 0; i < L; i++)
                    if ((q->trig[v] >> i & 1) && w[i] * (0.5f + mf(D)) < lo) { lo = w[i] * (0.5f + mf(D)); s = i; }
                if (s >= 0) {
                    q->trig[v] &= (uint16_t)~(1u << s);
                    q->acc[v] &= (uint16_t)~(1u << s);
                }
            } else {   /* flip an accent */
                int n = (int)(mr(D) % (uint32_t)hits);
                for (int i = 0; i < L; i++)
                    if ((q->trig[v] >> i & 1) && n-- == 0) { q->acc[v] ^= (uint16_t)(1u << i); break; }
            }
        }
    }
    D->wp.has = 1;
    D->grv_src = -1;
}

/* ---- a tile tapped ---------------------------------------------------------------------------------------------- */

int ex_take(db_t *D, int groove, int i) {
    if (i < 0 || i >= (groove ? D->nsim_grv : D->nsim_snd)) return 0;
    int k = groove ? D->sim_grv[i] : D->sim_snd[i];
    if (__atomic_load_n(&D->ix_ok[k], __ATOMIC_ACQUIRE) != 1) return 0;
    const feat_t *f = &D->ix[k];
    ex_push(D);
    if (groove) {   /* its patterns, at the most similar one (the sounds stay) */
        D->wp = f->ps;
        D->grv_src = k;
        int p = D->sim_grv_pat[i];
        D->pv[P_PATTERN] = (float)p;
        D->pv[P_PAT_LEN] = (float)D->wp.p[p].len;
        D->pv[P_SWING] = f->ps.swing;
        D->sq_queued = p;
    } else {   /* its sounds (the patterns stay) */
        for (int v = 0; v < NV; v++)
            for (int j = 0; j < NVP; j++) D->pv[VK(v, j)] = f->pv[v][j];
        D->snd_src = k;
    }
    return 1;
}
