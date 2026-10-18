/* Drumbox kits: the built-in ones, the 32 user slots, the files in the data folder, the favourites and the .mtdrum
 * sounds.
 *
 * Kit list (MPC's PRESET menu and KIT), in this order, so a number always means the same kit:
 *   the built-in kits (src/builtin_kits.h, made by mpc/gen.py from kits/: nb_tonic's five and four Patternarium presets)
 *   U01 .. U32, the user slots: <data>/USER/01.mtpreset .. 32.mtpreset (SAVE writes them; empty ones say "(empty)")
 *   every .mtpreset and .tkit in the data folder and its sub-folders one level down (not USER), sorted by path;
 *   those in Favoriten/ are listed as "FAV <name>"
 *   favourites added since the plugin was inserted (FAVORIT), at the end until it is inserted again
 * Up to MAX_KITS. A file's kit is named after the file ("Unmouth_Sphing_139.mtpreset" -> "Unmouth Sphing") and read
 * only when it is first picked (kit_ensure, on the thread that sets the parameter), so 600 presets cost a folder
 * listing when the plugin is inserted, not 600 file reads.
 *
 * Formats (all text):
 *   .mtpreset  Sonic Charge Microtonic preset ("MicroTonicPresetV3: { ... DrumPatches: { 1: { Key = Value ... } }
 *              Patterns: { a: { Length = 16  1: { Accents = ----#--- Triggers = #---#--- } ... } } }"), as
 *              Patternarium's download button saves it. The eight drum patches are the kit; pattern A, Tempo, StepRate
 *              and Mutes are kept for the preview only (Drumbox has no sequencer).
 *   .mtdrum    one Microtonic drum patch: the same Key = Value lines as one of a preset's DrumPatches.
 *   .tkit      nb_tonic kit (a Lua table: "-- Table: {2}" .. "{9}" hold voices 1-8 as ["key"]=value).
 * Units: Microtonic's own (Hz, ms, dB, %, -100..100) and nb_tonic's (Hz, s, linear level, -1..1); both are turned
 * into the knob values the parameters hold (the curves in voice.c). Where the two differ (EQ gain: Microtonic's +-40
 * is nb_tonic's +-20, kept on Microtonic's scale; MIX: nb_tonic's -1..1 crossfade is Microtonic's 100..0 %; MOD RATE
 * in decay mode: Microtonic gives the decay in ms, nb_tonic the rate in Hz, 1000 / ms) the conversion follows nb_tonic
 * and Scholl's importer (supercollider-microtonic/utonic2sc.lua). */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "drumbox.h"
#include "builtin_kits.h"

#define FAV_DIR "Favoriten"

void kit_init_values(float pv[NV][NVP]) {
    for (int v = 0; v < NV; v++)
        for (int k = 0; k < NVP; k++) pv[v][k] = PDEF[VK(v, k)].def;
}

/* "Unmouth Sphing (139)" -> "Unmouth Sphing": Patternarium puts the tempo in the name */
static void name_copy(char *dst, const char *src, size_t n) {
    snprintf(dst, n, "%s", src);
    size_t l = strlen(dst);
    if (l > 4 && dst[l - 1] == ')') {
        char *o = strrchr(dst, '(');
        if (o && o > dst && o[-1] == ' ') {
            int digits = 1;
            for (char *c = o + 1; c < dst + l - 1; c++) digits &= *c >= '0' && *c <= '9';
            if (digits) o[-1] = 0;
        }
    }
}

/* a file's name as a kit's: "Unmouth_Sphing_139.mtpreset" -> "Unmouth Sphing" (underscores, the tempo, the type) */
static void file_title(const char *path, const char *prefix, char *out, size_t n) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char t[256];
    snprintf(t, sizeof t, "%s", b);
    char *dot = strrchr(t, '.');
    if (dot) *dot = 0;
    for (char *c = t; *c; c++)
        if (*c == '_') *c = ' ';
    size_t l = strlen(t);
    char *sp = strrchr(t, ' ');
    if (sp && sp > t && l - (size_t)(sp - t) >= 3 && l - (size_t)(sp - t) <= 4) {   /* " 139": a tempo */
        int digits = 1;
        for (char *c = sp + 1; *c; c++) digits &= *c >= '0' && *c <= '9';
        if (digits) *sp = 0;
    }
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s%s", prefix, t);
    name_copy(out, tmp, n);
}

/* ---- .mtpreset / .mtdrum --------------------------------------------------------------------------------------- */

static int word_is(const char *v, const char *w) { return !strncasecmp(v, w, strlen(w)); }

static void mt_set(float *p, const char *key, const char *val, int *choke_own, int *rate_unit) {
    double x = strtod(val, NULL);
    int isinf_ = isinf(x);
    if (!strcmp(key, "OscWave")) p[VP_OSC_WAVE] = word_is(val, "Tri") ? W_TRI : word_is(val, "Saw") ? W_SAW : W_SINE;
    else if (!strcmp(key, "OscFreq")) p[VP_OSC_FREQ] = i_freq((float)x);
    else if (!strcmp(key, "OscAtk")) p[VP_OSC_ATK] = i_time((float)x);
    else if (!strcmp(key, "OscDcy")) p[VP_OSC_DCY] = i_time((float)x);
    else if (!strcmp(key, "ModMode")) p[VP_MOD_MODE] = word_is(val, "Sine") ? M_SINE : word_is(val, "Noise") ? M_NOISE : M_DECAY;
    else if (!strcmp(key, "ModAmt")) p[VP_MOD_AMT] = i_modamt((float)x);
    else if (!strcmp(key, "ModRate")) {   /* resolved below (mt_rate): by its unit when it has one, else by the mode */
        p[VP_MOD_RATE] = isinf_ ? -1.0f : (float)x;
        *rate_unit = strstr(val, "Hz") ? 2 : strstr(val, "ms") ? 1 : 0;
    }
    else if (!strcmp(key, "NFilMod")) p[VP_N_FILTER] = word_is(val, "BP") ? F_BP : word_is(val, "HP") ? F_HP : F_LP;
    else if (!strcmp(key, "NFilFrq")) p[VP_N_FREQ] = i_freq((float)x);
    else if (!strcmp(key, "NFilQ")) p[VP_N_Q] = i_q((float)x);
    else if (!strcmp(key, "NEnvMod")) p[VP_N_ENV] = word_is(val, "Lin") ? E_LIN : word_is(val, "Mod") ? E_CLAP : E_EXP;
    else if (!strcmp(key, "NEnvAtk")) p[VP_N_ATK] = i_time((float)x);
    else if (!strcmp(key, "NEnvDcy")) p[VP_N_DCY] = i_time((float)x);
    else if (!strcmp(key, "NStereo")) p[VP_N_STEREO] = word_is(val, "On") ? 1 : 0;
    else if (!strcmp(key, "Mix")) p[VP_MIX] = clampf((float)x, 0, 100);
    else if (!strcmp(key, "DistAmt")) p[VP_DIST] = clampf((float)x, 0, 100);
    else if (!strcmp(key, "EQFreq")) p[VP_EQ_FREQ] = i_freq((float)x);
    else if (!strcmp(key, "EQGain")) p[VP_EQ_GAIN] = i_eqgain((float)x);
    else if (!strcmp(key, "Level")) p[VP_LEVEL] = isinf_ && x < 0 ? 0.0f : i_db((float)x);
    else if (!strcmp(key, "Pan")) p[VP_PAN] = clampf((float)x, -100, 100);
    else if (!strcmp(key, "OscVel")) p[VP_OSC_VEL] = clampf((float)x, 0, 200);
    else if (!strcmp(key, "NVel")) p[VP_N_VEL] = clampf((float)x, 0, 200);
    else if (!strcmp(key, "ModVel")) p[VP_MOD_VEL] = clampf((float)x, 0, 200);
    else if (!strcmp(key, "Choke")) { if (*choke_own < 0) p[VP_CHOKE] = word_is(val, "On") ? 1 : 0; }
    else if (!strcmp(key, "DrumboxChoke")) { p[VP_CHOKE] = clampf((float)x, 0, 3); *choke_own = 1; }
}

/* MOD RATE: a value with its unit says what it is ("78.9 ms", "88 Hz": the .mtdrum / text style); a bare number
 * (MicroTonicPresetV3) is the decay time in ms in decay mode ("inf": none), else Hz. mt_set left it raw. */
static void mt_rate(float *p, int unit) {
    float r = p[VP_MOD_RATE], hz;
    int ms = unit ? unit == 1 : (int)p[VP_MOD_MODE] == M_DECAY;
    if (ms) hz = r < 0 ? 0.0f : r <= 0.05f ? 20000.0f : 1000.0f / r;
    else hz = r < 0 ? 20000.0f : r;
    p[VP_MOD_RATE] = i_modrate(hz);
}

static uint16_t steps_bits(const char *s, int *n) {   /* "#---#---" -> bit i = step i */
    uint16_t b = 0;
    int i = 0;
    for (; i < 16 && (s[i] == '#' || s[i] == '-'); i++)
        if (s[i] == '#') b |= (uint16_t)(1u << i);
    if (n) *n = i;
    return b;
}

/* one "Key = Value" (MicroTonicPresetV3) or "Key: Value" (.mtdrum, the text style) line -> key, value without its
 * quotes ("Sine", "52.18 Hz"); 0 for a header such as "DrumPatches: {" or "1: {" */
static int kv_split(char *s, char **key, char **val) {
    char *e = strstr(s, " = "), *v, *end;
    if (e) { end = e; v = e + 3; }
    else {
        size_t k = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
        if (!k || s[k] != ':' || s[k + 1] != ' ') return 0;
        end = s + k;
        v = s + k + 2;
    }
    while (*v == ' ') v++;
    if (*v == '{') return 0;   /* a header: the line is left as it is (its brace still counts) */
    *end = 0;
    if (*v == '"') v++;
    size_t l = strlen(v);
    while (l && (v[l - 1] == '"' || v[l - 1] == ' ' || v[l - 1] == ',')) v[--l] = 0;
    *key = s;
    *val = v;
    return 1;
}

static const char *next_line(const char *t, char *line, size_t cap) {
    size_t n = strcspn(t, "\r\n");
    snprintf(line, cap, "%.*s", (int)(n < cap ? n : cap - 1), t);
    t += n;
    while (*t == '\r' || *t == '\n') t++;
    return t;
}

int parse_mtpreset(const char *t, kit_t *k) {
    char line[512];
    enum { S_TOP, S_DP, S_PAT } sec = S_TOP;
    int depth = 0, sec_depth = 0, v = -1, nv = 0, choke_own = -1, in_a = 0, a_done = 0, pv_ = -1, rate_set[NV] = {0};
    int unit[NV] = {0};
    kit_init_values(k->pv);
    memset(&k->pat, 0, sizeof k->pat);
    k->pat.len = 16;
    k->pat.div = 16;
    if (strncasecmp(t, "MicroTonicPreset", 16)) return 0;
    while (*t) {
        t = next_line(t, line, sizeof line);
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char kvbuf[512], *key = NULL, *val = NULL;
        snprintf(kvbuf, sizeof kvbuf, "%s", s);
        int kv = kv_split(kvbuf, &key, &val);
        if (sec == S_TOP && depth == 1) {
            if (kv && !strcmp(key, "Name")) name_copy(k->name, val, sizeof k->name);
            else if (kv && !strcmp(key, "Tempo")) k->pat.tempo = (float)atof(val);
            else if (kv && !strcmp(key, "StepRate")) {
                const char *sl = strchr(val, '/');
                int d = sl ? atoi(sl + 1) : 16;
                k->pat.div = d == 8 || d == 16 || d == 32 ? d : 16;
            } else if (!strncmp(s, "Mutes:", 6)) {
                const char *c = strchr(s, '{');
                for (int i = 0; c && i < NV; i++) {
                    c += strspn(c, "{ ,");
                    if (!strncmp(c, "On", 2)) k->pat.mute |= (uint8_t)(1u << i);
                    c += strcspn(c, ",}");
                }
            } else if (!strncmp(s, "DrumPatches:", 12)) { sec = S_DP; sec_depth = depth; }
            else if (!strncmp(s, "Patterns:", 9)) { sec = S_PAT; sec_depth = depth; }
        } else if (sec == S_DP) {
            if (depth == sec_depth + 1 && *s >= '1' && *s <= '8' && s[1] == ':') {
                v = *s - '1';
                nv++;
                choke_own = -1;
            } else if (kv && v >= 0 && depth == sec_depth + 2) {
                mt_set(k->pv[v], key, val, &choke_own, &unit[v]);
                if (!strcmp(key, "ModRate")) rate_set[v] = 1;
            }
        } else if (sec == S_PAT && !a_done) {
            if (depth == sec_depth + 1 && s[0] == 'a' && s[1] == ':') in_a = 1;
            else if (in_a && depth == sec_depth + 2 && kv && !strcmp(key, "Length")) {
                int l = atoi(val);
                k->pat.len = l >= 1 && l <= 16 ? l : 16;
            } else if (in_a && depth == sec_depth + 2 && *s >= '1' && *s <= '8' && s[1] == ':') pv_ = *s - '1';
            else if (in_a && pv_ >= 0 && depth == sec_depth + 3) {
                if (kv && !strcmp(key, "Triggers")) { k->pat.trig[pv_] = steps_bits(val, NULL); k->pat.has = 1; }
                else if (kv && !strcmp(key, "Accents")) k->pat.acc[pv_] = steps_bits(val, NULL);
            }
        }
        for (char *c = s; *c; c++) {
            if (*c == '{') depth++;
            else if (*c == '}') {
                depth--;
                if (sec == S_DP && depth == sec_depth + 1) v = -1;
                if (sec == S_PAT && in_a && depth == sec_depth + 2) pv_ = -1;
                if (sec == S_PAT && in_a && depth == sec_depth + 1) { in_a = 0; a_done = 1; }
                if (sec != S_TOP && depth == sec_depth) sec = S_TOP;
            }
        }
    }
    for (int i = 0; i < NV; i++)
        if (rate_set[i]) mt_rate(k->pv[i], unit[i]);
    return nv > 0;
}

/* one drum patch ("MicrotonicDrumPatchV3: { OscWave: Sine  OscFreq: 52.18 Hz ... }"): every key line inside its
 * outer braces, in either style; values carry their units (Hz, ms, sm = semitones, dB, %; Mix "osc / noise") */
int parse_mtdrum(const char *t, float *p, char *name, int nlen) {
    char line[512];
    int depth = 0, n = 0, choke_own = -1, rate = 0, unit = 0;
    float pv[NV][NVP];
    kit_init_values(pv);
    memcpy(p, pv[0], sizeof(float) * NVP);
    while (*t) {
        t = next_line(t, line, sizeof line);
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char kvbuf[512], *key, *val;
        snprintf(kvbuf, sizeof kvbuf, "%s", s);
        if (depth == 1 && kv_split(kvbuf, &key, &val)) {
            if (!strcmp(key, "Name")) {
                if (name && *val) name_copy(name, val, nlen);
            } else {
                mt_set(p, key, val, &choke_own, &unit);
                if (!strcmp(key, "ModRate")) rate = 1;
                n++;
            }
        }
        for (char *c = s; *c; c++) depth += *c == '{' ? 1 : *c == '}' ? -1 : 0;
    }
    if (rate) mt_rate(p, unit);
    p[VP_CHOKE] = 0;   /* a single sound brings no group */
    return n >= 5;
}

/* ---- .tkit (nb_tonic) ------------------------------------------------------------------------------------------ */

static void tk_set(float *p, const char *key, double x, int *group) {
    float f = (float)x;
    if (!strcmp(key, "osc_wav")) p[VP_OSC_WAVE] = clampf(f - 1, 0, 2);
    else if (!strcmp(key, "osc_freq")) p[VP_OSC_FREQ] = i_freq(f);
    else if (!strcmp(key, "osc_attack")) p[VP_OSC_ATK] = i_time(f * 1000.0f);
    else if (!strcmp(key, "osc_decay")) p[VP_OSC_DCY] = i_time(f * 1000.0f);
    else if (!strcmp(key, "mod_mode")) p[VP_MOD_MODE] = clampf(f - 1, 0, 2);
    else if (!strcmp(key, "mod_amt")) p[VP_MOD_AMT] = i_modamt(f);
    else if (!strcmp(key, "mod_rate")) p[VP_MOD_RATE] = i_modrate(f);
    else if (!strcmp(key, "noise_mode")) p[VP_N_FILTER] = clampf(f - 1, 0, 2);
    else if (!strcmp(key, "noise_freq")) p[VP_N_FREQ] = i_freq(f);
    else if (!strcmp(key, "noise_q")) p[VP_N_Q] = i_q(f);
    else if (!strcmp(key, "noise_env")) p[VP_N_ENV] = clampf(f - 1, 0, 2);
    else if (!strcmp(key, "noise_attack")) p[VP_N_ATK] = i_time(f * 1000.0f);
    else if (!strcmp(key, "noise_decay")) p[VP_N_DCY] = i_time(f * 1000.0f);
    else if (!strcmp(key, "noise_stereo")) p[VP_N_STEREO] = clampf(f - 1, 0, 1);
    else if (!strcmp(key, "mix")) p[VP_MIX] = clampf((1.0f - f) * 50.0f, 0, 100);
    else if (!strcmp(key, "dist")) p[VP_DIST] = clampf(f * 100.0f, 0, 100);
    else if (!strcmp(key, "eq_freq")) p[VP_EQ_FREQ] = i_freq(f);
    else if (!strcmp(key, "eq_gain")) p[VP_EQ_GAIN] = i_eqgain(f * 2.0f);
    else if (!strcmp(key, "level")) p[VP_LEVEL] = f <= 0.000001f ? 0.0f : i_db(20.0f * log10f(f));
    else if (!strcmp(key, "pan")) p[VP_PAN] = clampf(f * 100.0f, -100, 100);
    else if (!strcmp(key, "osc_vel")) p[VP_OSC_VEL] = clampf(f * 100.0f, 0, 200);
    else if (!strcmp(key, "noise_vel")) p[VP_N_VEL] = clampf(f * 100.0f, 0, 200);
    else if (!strcmp(key, "mod_vel")) p[VP_MOD_VEL] = clampf(f * 100.0f, 0, 200);
    else if (!strcmp(key, "choke_group")) *group = (int)x;
}

int parse_tkit(const char *t, kit_t *k) {
    int group[NV], nv = 0;
    kit_init_values(k->pv);
    for (int v = 0; v < NV; v++) group[v] = 100 + v;   /* no group: its own */
    const char *s = t;
    while ((s = strstr(s, "-- Table: {"))) {
        int id = atoi(s + 11), v = id - 2;
        s += 11;
        const char *end = strstr(s, "-- Table: {");
        if (!end) end = s + strlen(s);
        if (v < 0 || v >= NV) { s = end; continue; }
        nv++;
        const char *q = s;
        while ((q = strstr(q, "[\"")) && q < end) {
            char key[48];
            q += 2;
            size_t kl = strcspn(q, "\"");
            snprintf(key, sizeof key, "%.*s", (int)(kl < 47 ? kl : 47), q);
            q += kl;
            const char *e = strchr(q, '=');
            if (!e || e >= end) break;
            tk_set(k->pv[v], key, strtod(e + 1, NULL), &group[v]);
            q = e + 1;
        }
        s = end;
    }
    /* choke groups: nb_tonic gives every voice one (1-6); only those shared by two or more voices choke anything */
    int next = 1;
    for (int v = 0; v < NV; v++) {
        int same = 0;
        for (int u = 0; u < NV; u++) same += group[u] == group[v];
        int g = 0;
        if (same > 1) {
            for (int u = 0; u < v; u++)
                if (group[u] == group[v]) g = (int)k->pv[u][VP_CHOKE];
            if (!g && next <= 3) g = next++;
        }
        k->pv[v][VP_CHOKE] = (float)g;
    }
    return nv > 0;
}

/* ---- files ----------------------------------------------------------------------------------------------------- */

static char *read_text(const char *path, long max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = NULL;
    if (n > 0 && n <= max && (b = malloc(n + 1))) {
        if (fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
        else b[n] = 0;
    }
    fclose(f);
    return b;
}

static int ends_with(const char *s, const char *x) {
    size_t a = strlen(s), b = strlen(x);
    return a >= b && !strcasecmp(s + a - b, x);
}

/* the file -> its kit's values and pattern (the name stays the list's) */
static int load_file(const char *path, kit_t *k) {
    char *t = read_text(path, 1 << 20);
    if (!t) return 0;
    char keep[32];
    snprintf(keep, sizeof keep, "%s", k->name);
    int ok = ends_with(path, ".tkit") ? parse_tkit(t, k) : parse_mtpreset(t, k);
    free(t);
    if (keep[0]) snprintf(k->name, sizeof k->name, "%s", keep);
    return ok;
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char **v; int n, cap; } list_t;

static void list_add(list_t *l, const char *s) {
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 64;
        char **nv = realloc(l->v, c * sizeof *nv);
        if (!nv) return;
        l->v = nv;
        l->cap = c;
    }
    char *d = strdup(s);
    if (d) l->v[l->n++] = d;
}

/* the files ending in ext in a folder and (at depth 0) its sub-folders but USER, full paths; no symlinks followed */
static void collect(const char *dir, int depth, const char *ext, const char *ext2, list_t *l) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p || lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth == 0 && strcasecmp(e->d_name, "USER")) collect(p, 1, ext, ext2, l);
        } else if (S_ISREG(st.st_mode) && (ends_with(e->d_name, ext) || (ext2 && ends_with(e->d_name, ext2)))) {
            list_add(l, p);
        }
    }
    closedir(d);
}

static void user_path(const db_t *D, int slot, char *buf, size_t n) {
    snprintf(buf, n, "%s/USER/%02d.mtpreset", D->dir[0] ? D->dir : ".", slot);
}

static void user_empty(kit_t *k, int slot) {
    memset(k, 0, sizeof *k);
    kit_init_values(k->pv);
    k->user = slot;
    k->loaded = 1;
    snprintf(k->name, sizeof k->name, "U%02d (empty)", slot);
}

static int in_fav_dir(const db_t *D, const char *path) {
    char f[700];
    snprintf(f, sizeof f, "%s/%s/", D->dir, FAV_DIR);
    return !strncasecmp(path, f, strlen(f));
}

void kits_load(db_t *D) {
    D->kits = calloc(MAX_KITS, sizeof(kit_t));
    if (!D->kits) return;
    int n = 0;
    for (int i = 0; i < NBUILTIN_KITS && n < MAX_KITS; i++) {
        kit_t *k = &D->kits[n];
        int ok = BUILTIN_KITS[i].tkit ? parse_tkit(BUILTIN_KITS[i].text, k) : parse_mtpreset(BUILTIN_KITS[i].text, k);
        if (!ok) continue;
        snprintf(k->name, sizeof k->name, "%s", BUILTIN_KITS[i].name);
        k->used = k->loaded = 1;
        n++;
    }
    D->nbuiltin = n;
    D->user0 = n;
    for (int s = 1; s <= NUSER; s++, n++) {   /* the slots are small and few: read now, for their names */
        char p[700];
        kit_t *k = &D->kits[n];
        user_empty(k, s);
        if (!D->dir[0]) continue;
        user_path(D, s, p, sizeof p);
        kit_t tmp;
        memset(&tmp, 0, sizeof tmp);
        if (load_file(p, &tmp)) {
            char nm[32];
            snprintf(nm, sizeof nm, "%s", tmp.name[0] ? tmp.name : "Drumbox");
            *k = tmp;
            k->user = s;
            k->used = k->loaded = 1;
            snprintf(k->name, sizeof k->name, "U%02d %.26s", s % 100, nm);
        }
    }
    if (D->dir[0]) {   /* the files: listed now, read when picked */
        list_t l = {0};
        collect(D->dir, 0, ".mtpreset", ".tkit", &l);
        if (l.n) qsort(l.v, l.n, sizeof *l.v, cmp_str);
        for (int i = 0; i < l.n; i++) {
            if (n < MAX_KITS) {
                kit_t *k = &D->kits[n++];
                memset(k, 0, sizeof *k);
                kit_init_values(k->pv);
                k->path = l.v[i];
                k->used = 1;
                file_title(k->path, in_fav_dir(D, k->path) ? "FAV " : "", k->name, sizeof k->name);
            } else free(l.v[i]);
        }
        free(l.v);
        /* the .mtdrum sounds */
        list_t d = {0};
        collect(D->dir, 0, ".mtdrum", NULL, &d);
        if (d.n) qsort(d.v, d.n, sizeof *d.v, cmp_str);
        if (d.n > MAX_DRUMS) {
            for (int i = MAX_DRUMS; i < d.n; i++) free(d.v[i]);
            d.n = MAX_DRUMS;
        }
        D->dpath = d.v;
        D->ndrums = d.n;
        D->dname = calloc(d.n ? d.n : 1, sizeof *D->dname);
        for (int i = 0; i < d.n && D->dname; i++) {
            char t[32];
            file_title(d.v[i], "", t, sizeof t);
            D->dname[i] = strdup(t);
        }
    }
    D->nkits = n;
    D->dcur = -1;
}

int kit_ensure(db_t *D, int n) {
    if (n < 0 || n >= D->nkits) return 0;
    kit_t *k = &D->kits[n];
    if (k->loaded) return k->used;
    k->loaded = 1;
    if (!k->path || !load_file(k->path, k)) {   /* unreadable: it stays in the list, marked, and loads the Init values */
        kit_init_values(k->pv);
        memset(&k->pat, 0, sizeof k->pat);
        char nm[32];
        snprintf(nm, sizeof nm, "%s", k->name);
        snprintf(k->name, sizeof k->name, "?? %.28s", nm);
        return 1;
    }
    return 1;
}

int drum_ensure(db_t *D, int n) {
    if (n < 0 || n >= D->ndrums) return 0;
    if (n == D->dcur) return 1;
    char *t = read_text(D->dpath[n], 1 << 18);
    if (!t) return 0;
    float p[NVP];
    int ok = parse_mtdrum(t, p, NULL, 0);
    free(t);
    if (!ok) return 0;
    memcpy(D->dpv, p, sizeof p);
    D->dcur = n;
    return 1;
}

void kits_free(db_t *D) {
    for (int i = 0; D->kits && i < D->nkits; i++) free(D->kits[i].path);
    free(D->kits);
    D->kits = NULL;
    for (int i = 0; i < D->ndrums; i++) {
        free(D->dpath[i]);
        if (D->dname) free(D->dname[i]);
    }
    free(D->dpath);
    free(D->dname);
    D->dpath = D->dname = NULL;
    D->ndrums = 0;
}

/* ---- writing a Microtonic preset ------------------------------------------------------------------------------- */

static float modrate_out(const float *p) {   /* Microtonic's ModRate: ms in decay mode */
    float hz = k_modrate(p[VP_MOD_RATE]);
    if ((int)p[VP_MOD_MODE] == M_DECAY) return hz <= 0.0f ? INFINITY : 1000.0f / hz;
    return hz;
}

static void steps_str(uint16_t b, int len, char *out) {
    for (int i = 0; i < 16; i++) out[i] = i < len && (b >> i & 1) ? '#' : '-';
    out[16] = 0;
}

/* name, the eight voices (pv as the parameters hold them) and, when there is one, pattern A -> path (via path.new) */
static int write_mtpreset(const char *path, const char *name, const float *pv, const pattern_t *pat) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    static const char *const WAVE[3] = {"Sine", "Triangle", "Saw"}, *const MODE[3] = {"Decay", "Sine", "Noise"},
                             *const FIL[3] = {"LP", "BP", "HP"}, *const ENV[3] = {"Exp", "Linear", "Mod"};
    fprintf(f, "MicroTonicPresetV3: {\n\tName = \"%s\"\n", name);
    if (pat && pat->has) {
        fprintf(f, "\tPattern = a\n\tStepRate = 1/%d\n", pat->div);
        if (pat->tempo > 0) fprintf(f, "\tTempo = %.6g\n", pat->tempo);
        fprintf(f, "\tMutes: {");
        for (int v = 0; v < NV; v++) fprintf(f, "%s %s", v ? "," : "", pat->mute >> v & 1 ? "On" : "Off");
        fprintf(f, " }\n");
    }
    fprintf(f, "\tDrumPatches: {\n");
    for (int v = 0; v < NV; v++) {
        const float *p = pv + VSTRIDE * v;
        fprintf(f, "\t\t%d: {\n", v + 1);
        fprintf(f, "\t\t\tChoke = %s\n\t\t\tDrumboxChoke = %d\n", p[VP_CHOKE] > 0 ? "On" : "Off", (int)p[VP_CHOKE]);
        fprintf(f, "\t\t\tDistAmt = %.6g\n", p[VP_DIST]);
        fprintf(f, "\t\t\tEQFreq = %.8g\n\t\t\tEQGain = %.6g\n", k_freq(p[VP_EQ_FREQ]), k_eqgain(p[VP_EQ_GAIN]));
        if (p[VP_LEVEL] <= 0.0f) fprintf(f, "\t\t\tLevel = -inf\n");
        else fprintf(f, "\t\t\tLevel = %.6g\n", k_db(p[VP_LEVEL]));
        fprintf(f, "\t\t\tMix = %.6g\n\t\t\tModAmt = %.6g\n", p[VP_MIX], k_modamt(p[VP_MOD_AMT]));
        fprintf(f, "\t\t\tModMode = %s\n", MODE[(int)p[VP_MOD_MODE] % 3]);
        float r = modrate_out(p);
        if (isinf(r)) fprintf(f, "\t\t\tModRate = inf\n");
        else fprintf(f, "\t\t\tModRate = %.8g\n", r);
        fprintf(f, "\t\t\tModVel = %.6g\n", p[VP_MOD_VEL]);
        fprintf(f, "\t\t\tNEnvAtk = %.8g\n\t\t\tNEnvDcy = %.8g\n", k_time(p[VP_N_ATK]), k_time(p[VP_N_DCY]));
        fprintf(f, "\t\t\tNEnvMod = %s\n", ENV[(int)p[VP_N_ENV] % 3]);
        fprintf(f, "\t\t\tNFilFrq = %.8g\n\t\t\tNFilMod = %s\n", k_freq(p[VP_N_FREQ]), FIL[(int)p[VP_N_FILTER] % 3]);
        fprintf(f, "\t\t\tNFilQ = %.8g\n\t\t\tNStereo = %s\n", k_q(p[VP_N_Q]), p[VP_N_STEREO] > 0 ? "On" : "Off");
        fprintf(f, "\t\t\tNVel = %.6g\n\t\t\tName = \"%s %d\"\n", p[VP_N_VEL], name, v + 1);
        fprintf(f, "\t\t\tOscAtk = %.8g\n\t\t\tOscDcy = %.8g\n", k_time(p[VP_OSC_ATK]), k_time(p[VP_OSC_DCY]));
        fprintf(f, "\t\t\tOscFreq = %.8g\n\t\t\tOscVel = %.6g\n", k_freq(p[VP_OSC_FREQ]), p[VP_OSC_VEL]);
        fprintf(f, "\t\t\tOscWave = %s\n\t\t\tOutput = A\n\t\t\tPan = %.6g\n\t\t}\n", WAVE[(int)p[VP_OSC_WAVE] % 3], p[VP_PAN]);
    }
    fprintf(f, "\t}\n");
    if (pat && pat->has) {
        char a[17], t[17];
        fprintf(f, "\tPatterns: {\n\t\ta: {\n\t\tLength = %d\n", pat->len);
        for (int v = 0; v < NV; v++) {
            steps_str(pat->acc[v], pat->len, a);
            steps_str(pat->trig[v], pat->len, t);
            fprintf(f, "\t\t\t%d: {\n\t\t\t\tAccents = %s\n\t\t\t\tFills = ----------------\n\t\t\t\tTriggers = %s\n\t\t\t}\n",
                    v + 1, a, t);
        }
        fprintf(f, "\t\t}\n\t}\n");
    }
    fprintf(f, "}\n");
    int bad = fflush(f) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path)) { remove(tmp); return -1; }
    return 0;
}

/* the kit loaded's name, without a user slot's "Unn " or a favourite's "FAV " */
static void base_name(const db_t *D, char *out, size_t n) {
    int cur = (int)D->pv[P_KIT];
    const char *nm = cur >= 0 && cur < D->nkits ? D->kits[cur].name : "Drumbox";
    if (cur >= D->user0 && cur < D->user0 + NUSER) nm = D->kits[cur].used && strlen(nm) > 4 ? nm + 4 : "Drumbox";
    else if (!strncmp(nm, "FAV ", 4)) nm += 4;
    else if (!strncmp(nm, "?? ", 3)) nm += 3;
    snprintf(out, n, "%s", nm);
}

/* SAVE: the voices as they are (and the loaded kit's pattern A, for the preview), as a Microtonic preset */
int kit_save_user(db_t *D, int slot) {
    if (slot < 1 || slot > NUSER || !D->kits) return -1;
    char path[700], dir[600], name[32];
    snprintf(dir, sizeof dir, "%s/USER", D->dir[0] ? D->dir : ".");
    if (mkdir(dir, 0755) && errno != EEXIST) return -1;
    user_path(D, slot, path, sizeof path);
    base_name(D, name, sizeof name);
    int cur = (int)D->pv[P_KIT];
    const pattern_t *pat = cur >= 0 && cur < D->nkits ? &D->kits[cur].pat : NULL;
    pattern_t keep;
    if (pat) keep = *pat;
    if (write_mtpreset(path, name, &D->pv[VK(0, 0)], pat ? &keep : NULL)) return -1;
    kit_t *k = &D->kits[D->user0 + slot - 1];
    free(k->path);
    memset(k, 0, sizeof *k);
    for (int v = 0; v < NV; v++)
        for (int j = 0; j < NVP; j++) k->pv[v][j] = D->pv[VK(v, j)];
    if (pat) k->pat = keep;
    k->user = slot;
    k->used = k->loaded = 1;
    snprintf(k->name, sizeof k->name, "U%02d %.26s", slot, name);
    return 0;
}

/* FAVORIT: the kit loaded, as it was picked, into <data>/Favoriten/: a file's own bytes (patterns and all), a built-in
 * kit or a user slot written out. It is added to the end of the list at once ("FAV <name>"); inserted again, the
 * plugin lists it in its place among the files. */
int kit_favorite(db_t *D) {
    int cur = (int)D->pv[P_KIT];
    if (!D->dir[0] || cur < 0 || cur >= D->nkits) { snprintf(D->info, sizeof D->info, "NO DATA FOLDER"); return -1; }
    kit_t *k = &D->kits[cur];
    if (k->path && in_fav_dir(D, k->path)) { snprintf(D->info, sizeof D->info, "ALREADY A FAVOURITE"); return -1; }
    char dir[600], path[1100], name[32];
    snprintf(dir, sizeof dir, "%s/%s", D->dir, FAV_DIR);
    if (mkdir(dir, 0755) && errno != EEXIST) { snprintf(D->info, sizeof D->info, "CAN'T WRITE TO THE CARD"); return -1; }
    base_name(D, name, sizeof name);
    if (k->path) {
        const char *b = strrchr(k->path, '/');
        snprintf(path, sizeof path, "%s/%s", dir, b ? b + 1 : k->path);
    } else {
        char clean[32];
        snprintf(clean, sizeof clean, "%s", name);
        for (char *c = clean; *c; c++)
            if (*c == '/' || *c == '\\' || *c == ':' || *c == '"' || *c == '*' || *c == '?' || *c == '<' || *c == '>' || *c == '|') *c = '_';
        snprintf(path, sizeof path, "%s/%s.mtpreset", dir, clean);
    }
    struct stat st;
    if (!stat(path, &st)) { snprintf(D->info, sizeof D->info, "ALREADY A FAVOURITE"); return -1; }
    int bad;
    if (k->path) {
        char *t = read_text(k->path, 1 << 20);
        bad = !t;
        if (t) {
            char tmp[1110];
            snprintf(tmp, sizeof tmp, "%s.new", path);
            FILE *f = fopen(tmp, "wb");
            size_t len = strlen(t);
            bad = !f || fwrite(t, 1, len, f) != len;
            if (f) bad |= fclose(f) != 0;
            if (!bad) bad = rename(tmp, path) != 0;
            if (bad) remove(tmp);
            free(t);
        }
    } else {
        pattern_t pat = k->pat;
        float pv[NV * NVP];
        for (int v = 0; v < NV; v++)
            for (int j = 0; j < NVP; j++) pv[VSTRIDE * v + j] = k->pv[v][j];
        bad = write_mtpreset(path, name, pv, &pat) != 0;
    }
    if (bad) { snprintf(D->info, sizeof D->info, "CAN'T WRITE TO THE CARD"); return -1; }
    if (D->nkits < MAX_KITS) {
        kit_t *f = &D->kits[D->nkits++];
        *f = *k;
        f->path = strdup(path);
        f->user = 0;
        snprintf(f->name, sizeof f->name, "FAV %.27s", name);
    }
    snprintf(D->info, sizeof D->info, "FAVOURITE: %.30s", name);
    return 0;
}
