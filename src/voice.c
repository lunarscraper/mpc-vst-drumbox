/* Drumbox voices: nb_tonic's SynthDef (lib/nb_tonic.sc, sonocircuit, after supertonic by Zack Scholl), written out
 * sample by sample in C. Each line of the SynthDef has its counterpart here, in the same order:
 *
 *   pitch mod   decay: Decay.ar(Impulse), 60 dB in 1 / (2 x rate); sine: SinOsc(rate, pi); noise: LFNoise0(4 x rate)
 *               lagged by 1 / (4 x rate); scaled by MOD AMT semitones (not halved as in nb_tonic) and the velocity
 * Checked against Microtonic's own stems (VT-78 In The Air, 2026-10-25): nb_tonic's +5 Hz, MOD AMT / 2, noise decay
 * x 1.4, noise -9 dB, EQ / 2 and the noise replacement in NOISE mode all moved Drumbox away from Microtonic; gone.
 *   oscillator  sine / triangle / saw (x 0.5) at FREQ + the pitch mod (no +5 Hz offset and no noise replacement in
 *               NOISE mode: measured against Microtonic's own stems, a voice plays at its FREQ, and NOISE mode with
 *               MOD AMT 0 is a clean tone); envelope [0.0001, 1, 0.9, 0.0001] over [attack, decay x decayer, decay],
 *               exponential; velocity; soft limit above full scale only (a sine at full level stays a sine)
 *   noise       +1 / -1 random (nb_tonic's noise buffer), two sources for stereo; LP / BP / HP (RBJ, Q from FILTER Q);
 *               at high Q it fades to a sine at the filter's frequency; envelopes (decay as set, not x 1.4):
 *               exp [0.001, 1, 0.0001], linear [0.0001, 1, 0.9, 0.0001], clap (Decay(Impulse) bursts during the attack,
 *               then exponential); Splay; velocity; softclip (no -9 dB trim)
 *   mix         XFade2 (equal power): MIX 100 % = oscillator only, 0 % = noise only (Microtonic's convention)
 *   end         distortion (dry / softclipped +12..24 dB, then -0..6 dB), peak EQ (EQ GAIN in full), 20 Hz
 *               high-pass, LEVEL - 6 dB,
 *               Balance2 pan
 * A new hit of a voice (or of its choke group) fades the one still sounding out in 50 ms, as nb_tonic's
 * release(0.05). */
#include <math.h>
#include <string.h>
#include "drumbox.h"

#define OUT_GAIN 0.5f   /* calibration of the whole engine (mpc/render.c measures the built-in kits) */

float db_sintab[SIN_N + 1];
__attribute__((constructor)) static void sintab_init(void) {
    for (int i = 0; i <= SIN_N; i++) db_sintab[i] = (float)sin(2.0 * M_PI * i / SIN_N);
}

/* ---- knob curves ---------------------------------------------------------------------------------------------- */

float k_freq(float k) { return 20.0f * powf(1000.0f, clampf(k, 0, 100) / 100.0f); }
float i_freq(float hz) { return clampf(logf(fmaxf(hz, 20.0f) / 20.0f) / logf(1000.0f) * 100.0f, 0, 100); }
float k_time(float k) { float x = clampf(k, 0, 100) / 100.0f; return 10000.0f * x * x * x; }
float i_time(float ms) { return clampf(cbrtf(fmaxf(ms, 0.0f) / 10000.0f) * 100.0f, 0, 100); }
float k_modamt(float k) { float x = clampf(k, 0, 100) / 50.0f - 1.0f; return 96.0f * x * fabsf(x); }
float i_modamt(float st) { float x = sqrtf(fminf(fabsf(st), 96.0f) / 96.0f); return (st < 0 ? -x : x) * 50.0f + 50.0f; }
float k_modrate(float k) { return k <= 0.0f ? 0.0f : 0.1f * powf(200000.0f, clampf(k, 0, 100) / 100.0f); }
float i_modrate(float hz) { return hz <= 0.1f ? 0.0f : clampf(logf(hz / 0.1f) / logf(200000.0f) * 100.0f, 0.01f, 100); }
float k_q(float k) { return 0.1f * powf(100000.0f, clampf(k, 0, 100) / 100.0f); }
float i_q(float q) { return clampf(logf(fmaxf(q, 0.1f) / 0.1f) / logf(100000.0f) * 100.0f, 0, 100); }
float k_db(float k) { return -60.0f + 0.72f * clampf(k, 0, 100); }
float i_db(float db) { return clampf((db + 60.0f) / 0.72f, 0, 100); }
float k_eqgain(float k) { return -40.0f + 0.8f * clampf(k, 0, 100); }
float i_eqgain(float db) { return clampf((db + 40.0f) / 0.8f, 0, 100); }

/* ---- envelopes ------------------------------------------------------------------------------------------------- */

static void env_seg(env_t *e) {
    while (e->seg < e->nseg) {
        int n = (int)lrintf(e->tm[e->seg] * SR);
        float a = e->lv[e->seg], b = e->lv[e->seg + 1];
        if (n > 0) {
            e->left = n;
            e->y = a;
            if (e->lin) e->add = (b - a) / n;
            else e->mul = powf(b / a, 1.0f / n);
            return;
        }
        e->seg++;
        e->y = b;
    }
    e->done = 1;
}

static void env_start(env_t *e, const float *lv, const float *tm, int nseg, int lin) {
    memset(e, 0, sizeof *e);
    memcpy(e->lv, lv, sizeof(float) * (nseg + 1));
    memcpy(e->tm, tm, sizeof(float) * nseg);
    e->nseg = nseg;
    e->lin = lin;
    env_seg(e);
}

static inline float env_next(env_t *e) {
    if (e->done) return 0.0f;
    float y = e->y;
    if (e->lin) e->y += e->add;
    else e->y *= e->mul;
    if (--e->left <= 0) {
        e->y = e->lv[++e->seg];
        env_seg(e);
    }
    return y;
}

/* ---- a hit ----------------------------------------------------------------------------------------------------- */

static float velf(float pct, float v) {   /* nb_tonic: vel.range(1 - amount, 1) with vel 0..2 */
    return fmaxf(0.0f, 1.0f + pct / 100.0f * (v - 0.5f));
}

void voice_choke(db_t *D, int v) {
    inst_t *a = &D->v[v].in[0];
    if (!a->active) return;
    D->v[v].in[1] = *a;   /* the one still fading from before goes */
    D->v[v].in[1].fade_step = 1.0f / (0.05f * SR);
    a->active = 0;
}

void voice_kill(db_t *D) {
    for (int v = 0; v <= NV; v++)
        for (int i = 0; i < NINST; i++) D->v[v].in[i].active = 0;
}

void voice_trigger(db_t *D, int v, float vel) {
    const float *p = v == AUD ? D->dpv : &D->pv[VK(v, 0)];
    voice_choke(D, v);
    inst_t *I = &D->v[v].in[0];
    memset(I, 0, sizeof *I);
    I->active = 1;
    I->fade = 1.0f;
    I->rng = (D->seed = D->seed * 1664525u + 1013904223u) | 1u;
    float vv = clampf(vel / 127.0f, 0.0f, 1.0f);

    float dist = p[VP_DIST] / 100.0f;
    float decayer = 0.05f * cosf(dist * PI_F * 0.5f) + dist * 0.3f * sinf(dist * PI_F * 0.5f);   /* SelectX */
    float oatk = k_time(p[VP_OSC_ATK]) * 0.001f, odcy = k_time(p[VP_OSC_DCY]) * 0.001f;
    float natk = k_time(p[VP_N_ATK]) * 0.001f, ndcy = k_time(p[VP_N_DCY]) * 0.001f;

    /* pitch modulation */
    I->mmode = (int)p[VP_MOD_MODE];
    float rate = k_modrate(p[VP_MOD_RATE]);
    I->amt = k_modamt(p[VP_MOD_AMT]) * velf(p[VP_MOD_VEL], vv);
    I->pm = 1.0f;
    I->pmul = rate > 0.0f ? powf(0.001f, 2.0f * rate * INV_SR) : 1.0f;   /* 60 dB in 1 / (2 rate) */
    I->mph = 0.5f;                                                         /* SinOsc(rate, pi) */
    I->minc = rate * INV_SR;
    I->rnd_period = rate > 0.0f ? (int)fmaxf(1.0f, SR / (4.0f * rate)) : 0x7fffffff;
    I->rnd_left = 0;
    I->lag_c = rate > 0.0f ? powf(0.001f, 4.0f * rate * INV_SR) : 0.0f;
    I->rnd_y = 0.0f;

    /* oscillator */
    I->wave = (int)p[VP_OSC_WAVE];
    I->f0 = k_freq(p[VP_OSC_FREQ]);
    I->ph = I->wave == W_SAW ? 0.5f : 0.0f;
    {
        float lv[4] = {0.0001f, 1.0f, 0.9f, 0.0001f}, tm[3] = {oatk, odcy * decayer, odcy};
        env_start(&I->eo, lv, tm, 3, 0);
    }
    I->ovel = velf(p[VP_OSC_VEL], vv);

    /* noise */
    I->stereo = (int)p[VP_N_STEREO];
    I->nenv = (int)p[VP_N_ENV];
    float nf = k_freq(p[VP_N_FREQ]), q = k_q(p[VP_N_Q]);
    switch ((int)p[VP_N_FILTER]) {
    case F_LP: bq_lp(&I->nf, nf, clampf(1.0f / q, 0.5f, 3.0f)); break;
    case F_BP: bq_bp(&I->nf, nf, clampf(2.0f / q, 0.1f, 6.0f)); break;
    default: bq_hp(&I->nf, nf, clampf(1.0f / q, 0.5f, 3.0f)); break;
    }
    float xf = clampf(0.1092f * logf(q) + 0.0343f, 0.0f, 1.0f);
    I->xf_f = cosf(xf * PI_F * 0.5f);
    I->xf_s = sinf(xf * PI_F * 0.5f);
    I->sin_inc = clampf(nf, 0.0f, 0.49f * SR) * INV_SR;
    if (I->nenv == E_EXP) {
        float lv[3] = {0.001f, 1.0f, 0.0001f}, tm[2] = {natk, ndcy};
        env_start(&I->en, lv, tm, 2, 0);
    } else if (I->nenv == E_LIN) {
        float lv[4] = {0.0001f, 1.0f, 0.9f, 0.0001f}, tm[3] = {natk, ndcy * decayer, ndcy * (1.0f - decayer)};
        env_start(&I->en, lv, tm, 3, 1);
    } else {
        float lv[4] = {0.001f, 0.001f, 1.0f, 0.0001f}, tm[3] = {natk, 0.001f, ndcy};
        env_start(&I->en, lv, tm, 3, 0);
        /* the clap: Decay(Impulse(f), 1 / f) x 0.85 + 0.15 while Trig(attack + 1 ms); f from the attack (Scholl's fit,
         * in ms) */
        float cf = 4311.0f / (natk * 1000.0f + 28.4f) + 11.44f;
        I->clap_period = (int)fmaxf(1.0f, SR / cf);
        I->clap_left = 0;
        I->clap_count = (int)((natk + 0.001f) * SR);
        I->clap_mul = powf(0.001f, 1.0f / I->clap_period);
        I->clap_y = 0.0f;
    }
    I->nvel = velf(p[VP_N_VEL], vv);

    /* mix and the end */
    float pan = 1.0f - 2.0f * p[VP_MIX] / 100.0f;
    I->g_osc = cosf((pan + 1.0f) * PI_F * 0.25f);
    I->g_noise = sinf((pan + 1.0f) * PI_F * 0.25f);
    I->dist = dist;
    I->dist_pre = dbamp(12.0f + 12.0f * dist);
    I->dist_post = dbamp(-6.0f * dist);
    float eqg = k_eqgain(p[VP_EQ_GAIN]);   /* Microtonic's dB as they are (Scholl halved them; Microtonic's stems say no) */
    bq_peak(&I->eq, k_freq(p[VP_EQ_FREQ]), 1.0f, eqg);
    I->use_osc = I->g_osc > 1e-5f;
    I->use_noise = I->g_noise > 1e-5f;
    I->use_eq = fabsf(eqg) > 0.01f;
    bq_hp(&I->hp, 20.0f, 1.41421356f);
    float lvl = p[VP_LEVEL] <= 0.0f ? 0.0f : dbamp(k_db(p[VP_LEVEL])) * 0.5f * OUT_GAIN;
    float pp = clampf(p[VP_PAN] / 100.0f, -1.0f, 1.0f);
    I->gl = lvl * cosf((pp + 1.0f) * PI_F * 0.25f);
    I->gr = lvl * sinf((pp + 1.0f) * PI_F * 0.25f);
}

/* ---- render ---------------------------------------------------------------------------------------------------- */

static int inst_render(inst_t *I, float *L, float *R, int n) {
    for (int i = 0; i < n; i++) {
        /* pitch modulation, in semitones */
        float m = 0.0f;
        if (I->amt == 0.0f) ;
        else if (I->mmode == M_DECAY) {
            m = I->pm;
            I->pm *= I->pmul;
        } else if (I->mmode == M_SINE) {
            m = sin_t(I->mph);
            I->mph += I->minc;
            I->mph -= (int)I->mph;
        } else {
            if (--I->rnd_left <= 0) {
                I->rnd_left = I->rnd_period;
                I->rnd = rnd_bi(&I->rng);
            }
            I->rnd_y = I->rnd + I->lag_c * (I->rnd_y - I->rnd);
            m = I->rnd_y;
        }
        float a = rnd_sign(&I->rng), b = rnd_sign(&I->rng);

        /* oscillator */
        float eo = env_next(&I->eo), o = 0.0f;
        if (!I->use_osc) ;
        else {
            float f = m != 0.0f ? I->f0 * fexp2(m * I->amt * (1.0f / 12.0f)) : I->f0, inc = clampf(f * INV_SR, 0.0f, 0.49f);
            float ph = I->ph;
            if (I->wave == W_SINE) o = sin_t(ph);
            else if (I->wave == W_TRI) o = 0.5f * (ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
            else {   /* saw with polyBLEP */
                float s = 2.0f * ph - 1.0f;
                if (ph < inc) { float t = ph / inc; s -= t + t - t * t - 1.0f; }
                else if (ph > 1.0f - inc) { float t = (ph - 1.0f) / inc; s -= t * t + t + t + 1.0f; }
                o = 0.5f * s;
            }
            ph += inc;
            I->ph = ph - floorf(ph);
        }
        if (I->use_osc) o = 2.0f * softclip(0.5f * o * eo * I->ovel);   /* linear up to full scale */

        /* noise */
        float en = env_next(&I->en);
        if (I->nenv == E_CLAP && I->clap_count > 0) {
            if (--I->clap_left <= 0) {
                I->clap_left = I->clap_period;
                I->clap_y += 1.0f;
            }
            en += I->clap_y * 0.85f + 0.15f;
            I->clap_y *= I->clap_mul;
            I->clap_count--;
        }
        float nl = 0.0f, nr = 0.0f;
        if (I->use_noise) {
            float sn = 0.0f;
            if (I->xf_s > 0.0f) {   /* high Q: the filter fades to a sine at its frequency */
                sn = sin_t(I->sin_ph) * I->xf_s;
                I->sin_ph += I->sin_inc;
                I->sin_ph -= (int)I->sin_ph;
            }
            nl = bq_run(&I->nf, &I->nfs[0], a);
            nr = I->stereo ? bq_run(&I->nf, &I->nfs[1], b) : nl;
            float g = en * 0.70710678f;
            nl = softclip((nl * I->xf_f + sn) * g * I->nvel);
            nr = I->stereo || sn != 0.0f ? softclip((nr * I->xf_f + sn) * g * I->nvel) : nl;
        }

        /* mix, distortion, EQ, high-pass, level and pan */
        float sl = o * I->g_osc + nl * I->g_noise, sr = o * I->g_osc + nr * I->g_noise;
        if (I->dist > 0.0f) {
            sl = (sl * (1.0f - I->dist) + softclip(sl * I->dist_pre) * I->dist) * I->dist_post;
            sr = (sr * (1.0f - I->dist) + softclip(sr * I->dist_pre) * I->dist) * I->dist_post;
        }
        if (I->use_eq) {
            sl = bq_run(&I->eq, &I->eqs[0], sl);
            sr = bq_run(&I->eq, &I->eqs[1], sr);
        }
        sl = bq_run(&I->hp, &I->hps[0], sl);
        sr = bq_run(&I->hp, &I->hps[1], sr);
        float fd = I->fade;
        if (I->fade_step > 0.0f) {
            I->fade -= I->fade_step;
            if (I->fade <= 0.0f) return 0;
        }
        L[i] += sl * I->gl * fd;
        R[i] += sr * I->gr * fd;
        if (I->eo.done && I->en.done && I->clap_count <= 0) return 0;
    }
    return 1;
}

void voice_render(db_t *D, int v, float *L, float *R, int n) {
    for (int k = 0; k < NINST; k++) {
        inst_t *I = &D->v[v].in[k];
        if (I->active) I->active = inst_render(I, L, R, n);
    }
}
