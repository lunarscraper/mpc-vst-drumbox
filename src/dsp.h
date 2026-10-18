/* Drumbox: small DSP building blocks (SuperCollider's UGens where the SynthDef uses them). */
#pragma once
#include <math.h>
#include <stdint.h>

#define SR 44100.0f
#define INV_SR (1.0f / 44100.0f)
#define PI_F 3.14159265358979f
#define TWO_PI_F 6.28318530717959f

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* SuperCollider's softclip: linear to 0.5, then (|x| - 0.25) / x */
static inline float softclip(float x) {
    float a = fabsf(x);
    return a <= 0.5f ? x : (a - 0.25f) / x;
}

static inline float dbamp(float db) { return powf(10.0f, db * 0.05f); }

/* 2^x, |x| < ~30 (exponent bits and a cubic: error < 0.01 %) */
static inline float fexp2(float x) {
    if (x < -30.0f) return 0.0f;
    if (x > 30.0f) x = 30.0f;
    float fl = floorf(x), f = x - fl;
    union { float f; int32_t i; } u;
    u.i = (int32_t)(fl + 127.0f) << 23;
    return u.f * (1.0f + f * (0.6951786f + f * (0.2261587f + f * 0.0782630f)));
}

/* sine of a phase in turns (0..1), from a table (filled once when the library loads: voice.c) */
#define SIN_N 4096
extern float db_sintab[SIN_N + 1];
static inline float sin_t(float ph) {
    ph -= floorf(ph);
    float x = ph * SIN_N;
    int i = (int)x;
    float f = x - i;
    return db_sintab[i] + (db_sintab[i + 1] - db_sintab[i]) * f;
}

/* xorshift: 32 random bits */
static inline uint32_t rnd32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
/* nb_tonic's noise buffer holds +1 / -1 at random: the same here, sample by sample */
static inline float rnd_sign(uint32_t *s) { return (rnd32(s) & 0x80000000u) ? -1.0f : 1.0f; }
static inline float rnd_bi(uint32_t *s) { return (int32_t)rnd32(s) * (1.0f / 2147483648.0f); }

/* RBJ biquads, as SuperCollider's BLowPass / BBandPass / BHiPass / BPeakEQ compute them (direct form I) */
typedef struct { float b0, b1, b2, a1, a2; } biquad_t;
typedef struct { float x1, x2, y1, y2; } bq_state_t;

static inline float bq_run(const biquad_t *c, bq_state_t *s, float x) {
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

static inline void bq_norm(biquad_t *c, float b0, float b1, float b2, float a0, float a1, float a2) {
    float r = 1.0f / a0;
    c->b0 = b0 * r; c->b1 = b1 * r; c->b2 = b2 * r; c->a1 = a1 * r; c->a2 = a2 * r;
}
static inline float bq_w0(float f) { return TWO_PI_F * clampf(f, 10.0f, 0.45f * SR) * INV_SR; }

static inline void bq_lp(biquad_t *c, float f, float rq) {   /* BLowPass(freq, rq) */
    float w = bq_w0(f), cs = cosf(w), al = sinf(w) * 0.5f * rq;
    bq_norm(c, (1 - cs) * 0.5f, 1 - cs, (1 - cs) * 0.5f, 1 + al, -2 * cs, 1 - al);
}
static inline void bq_hp(biquad_t *c, float f, float rq) {   /* BHiPass(freq, rq) */
    float w = bq_w0(f), cs = cosf(w), al = sinf(w) * 0.5f * rq;
    bq_norm(c, (1 + cs) * 0.5f, -(1 + cs), (1 + cs) * 0.5f, 1 + al, -2 * cs, 1 - al);
}
static inline void bq_bp(biquad_t *c, float f, float bw) {   /* BBandPass(freq, bw in octaves): 0 dB peak */
    float w = bq_w0(f), sn = sinf(w), cs = cosf(w);
    float al = sn * sinhf(0.5f * logf(2.0f) * bw * w / sn);
    bq_norm(c, al, 0, -al, 1 + al, -2 * cs, 1 - al);
}
static inline void bq_peak(biquad_t *c, float f, float rq, float db) {   /* BPeakEQ(freq, rq, db) */
    float w = bq_w0(f), sn = sinf(w), cs = cosf(w), A = powf(10.0f, db / 40.0f), al = sn * 0.5f * rq;
    bq_norm(c, 1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}
