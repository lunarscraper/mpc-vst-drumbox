/* Drumbox: an eight-voice drum synth for MPC / Force, after nb_tonic (sonocircuit, MIT), which is a port of
 * supertonic (Zack Scholl, MIT), itself inspired by Sonic Charge Microtonic. It loads Microtonic presets (.mtpreset,
 * e.g. from Patternarium), Microtonic drum patches (.mtdrum, one voice) and nb_tonic kits (.tkit). No sequencer: the
 * voices are played by MIDI notes; a preset's pattern A only plays to preview it.
 *
 * Per voice: an oscillator (sine / triangle / saw) with a pitch modulation (decay / sine / noise), filtered noise
 * (LP / BP / HP, three envelope shapes: exp, linear, clap), their mix, distortion, a peak EQ, level and pan, three
 * velocity amounts and a choke group. The master: VOLUME and a resonant low-pass. All written for this repo in C from
 * nb_tonic's SuperCollider SynthDef (lib/nb_tonic.sc); no upstream code is compiled in. */
#pragma once
#include <stdint.h>
#include "dsp.h"
#include "params_def.h"

#define NV 8              /* voices */
#define MAX_KITS 2048
#define MAX_DRUMS 4096    /* .mtdrum files listed */
#define NUSER 32          /* the user slots: <data>/USER/NN.mtpreset */
#define AUD NV            /* the audition voice (SOUNDS page), after the eight */
#define NINST 2           /* per voice: the one playing and the one fading out (choke, retrigger) */

/* a voice's parameters, in PDEF order from P_V1_OSC_WAVE (VSTRIDE apart) */
enum {
    VP_OSC_WAVE, VP_OSC_FREQ, VP_OSC_ATK, VP_OSC_DCY, VP_MOD_MODE, VP_MOD_AMT, VP_MOD_RATE,
    VP_N_FILTER, VP_N_FREQ, VP_N_Q, VP_N_ENV, VP_N_ATK, VP_N_DCY, VP_N_STEREO,
    VP_MIX, VP_DIST, VP_EQ_FREQ, VP_EQ_GAIN, VP_LEVEL, VP_PAN, VP_OSC_VEL, VP_N_VEL, VP_MOD_VEL, VP_CHOKE,
    NVP
};
_Static_assert(NVP == VSTRIDE, "per-voice parameter block");
#define VK(v, k) (P_V1_OSC_WAVE + VSTRIDE * (v) + (k))

enum { W_SINE, W_TRI, W_SAW };
enum { M_DECAY, M_SINE, M_NOISE };
enum { F_LP, F_BP, F_HP };
enum { E_EXP, E_LIN, E_CLAP };

/* ---- knob curves (0..100 on screen) <-> real units; kits.c and the display use the same ones ------------- */
float k_freq(float k);       /* 20 Hz .. 20 kHz, log */
float k_time(float k);       /* 0 .. 10 000 ms, k^3 */
float k_modamt(float k);     /* -96 .. +96 semitones, finer near 0 */
float k_modrate(float k);    /* 0, then 0.1 Hz .. 20 kHz, log */
float k_q(float k);          /* 0.1 .. 10 000, log */
float k_db(float k);         /* LEVEL / VOLUME: -60 .. +12 dB (0 = silent) */
float k_eqgain(float k);     /* -40 .. +40 dB (Microtonic's scale; the EQ applies half) */
float i_freq(float hz);
float i_time(float ms);
float i_modamt(float st);
float i_modrate(float hz);
float i_q(float q);
float i_db(float db);
float i_eqgain(float db);

#define NPAT 12           /* patterns A-L, as in Microtonic */

/* one pattern: 1..16 steps per voice, triggers and accents, and whether it chains on to the next (Microtonic) */
typedef struct {
    uint16_t trig[NV], acc[NV];
    uint8_t len, chained;
} pattern_t;

/* a kit's patterns A-L, as a preset has them */
typedef struct {
    pattern_t p[NPAT];
    int has;               /* any step set in any pattern */
    int cur;               /* the preset's selected pattern ("Pattern = a") */
    int div;               /* StepRate's 1/<div> (16 = sixteenths, also 8, 32) */
    float tempo, swing;    /* the preset's own (tempo BPM, 0: none; swing %) */
    uint8_t mute;          /* bit v: voice v muted */
} patset_t;

typedef struct {
    char name[32];
    int user;              /* 1..32: a user slot (empty while !used) */
    int used;
    int loaded;            /* a file's kit is read when it is first picked (kit_ensure) */
    char *path;            /* the file it comes from (NULL: built in) */
    float pv[NV][NVP];     /* knob 0..100 or option index, as the parameters hold them */
    patset_t ps;
} kit_t;

/* an exponential / linear breakpoint envelope (SuperCollider Env: levels, times, curve) */
typedef struct {
    float lv[4], tm[3];
    int nseg, seg, left, lin, done;
    float y, mul, add;
} env_t;

typedef struct {
    int active;
    int ns;                 /* samples since the hit */
    /* oscillator */
    int wave, mmode, noisy_osc;
    float f0, ph, amt, mph, minc, pm, pmul, rnd, rnd_t, lag_c, rnd_y;
    int rnd_left, rnd_period;
    biquad_t osclp; bq_state_t osclp_s;
    env_t eo;
    float ovel;
    /* noise */
    int stereo, nenv;
    biquad_t nf; bq_state_t nfs[2];
    float sin_ph, sin_inc, xf_f, xf_s;
    env_t en;
    int clap_left, clap_period, clap_count;
    float clap_y, clap_mul;
    float nvel;
    uint32_t rng;
    /* end of the voice */
    float g_osc, g_noise, dist, dist_pre, dist_post;
    int use_osc, use_noise, use_eq;   /* skip a path that adds nothing (MIX at an end, EQ GAIN at 0 dB) */
    biquad_t eq, hp; bq_state_t eqs[2], hps[2];
    float gl, gr;           /* level x pan */
    float fade, fade_step;  /* 1, or falling to 0 over 50 ms when choked / retriggered */
} inst_t;

typedef struct {
    inst_t in[NINST];
} voice_t;

typedef struct {
    float pv[NP];
    voice_t v[NV + 1];             /* the eight, and the audition voice (AUD) */
    kit_t *kits;
    int nkits, nbuiltin, user0;    /* user slots at user0 .. user0+NUSER-1 */
    int kit_pending;
    char dir[512];
    char status[48], info[48];
    /* .mtdrum files (SOUNDS page): listed when the plugin is inserted, read when picked */
    char **dpath, **dname;
    int ndrums, dcur;              /* dcur: the one in dpv (-1: none) */
    float dpv[NVP];
    int aud_drum;                  /* play the audition voice at the next block */
    /* the patterns being played and edited (the kit's, copied when it loads; edits stay here and in the project) */
    patset_t wp;
    /* the preview (the Force stopped): the selected pattern, twice, at the host's tempo */
    int seq_on, seq_step, seq_total, seq_left, seq_frames, seq_start;
    pattern_t seq;
    /* the sequencer (SEQ ON, the Force playing): follows the song position */
    int sq_pat, sq_queued, sq_resync;         /* pattern playing; one picked to follow at the end of it (-1 none) */
    long sq_base, sq_last;         /* absolute step where sq_pat started; the last step fired */
    int sq_step;                   /* step of sq_pat playing (the lit column), -1 none */
    int tp_seen, tp_playing;
    double tp_bpm, tp_ppq;
    long tp_frames;                /* frames rendered since the host last told the position */
    int changed, changed_k, changed_n;   /* what changed by itself, for the screen (mpc_engine_changed): 1 KIT, 2 SOUND,
                                          * 4 TAKE, 8 PATTERN, 16 the notes */
    biquad_t lpf; bq_state_t lpfs[2];
    float lpf_cut, lpf_res;        /* the coefficients' settings */
    uint32_t seed;
} db_t;

/* voice.c */
void voice_trigger(db_t *D, int v, float vel);   /* v = AUD plays dpv */
void voice_choke(db_t *D, int v);
void voice_kill(db_t *D);
void voice_render(db_t *D, int v, float *L, float *R, int n);

/* kits.c */
void kits_load(db_t *D);
void kits_free(db_t *D);
int kit_ensure(db_t *D, int n);              /* read kit n's file if not yet; 1 = usable */
int kit_save_user(db_t *D, int slot);        /* the voices as they are -> USER/NN.mtpreset; 0 = ok */
int kit_favorite(db_t *D);                   /* the loaded kit -> Favoriten/; 0 = ok (D->info says what happened) */
int drum_ensure(db_t *D, int n);             /* read .mtdrum n into dpv; 1 = ok */
void kit_init_values(float pv[NV][NVP]);
void patset_init(patset_t *ps);
int parse_mtpreset(const char *text, kit_t *k);
int parse_mtdrum(const char *text, float *p, char *name, int nlen);
int parse_tkit(const char *text, kit_t *k);
