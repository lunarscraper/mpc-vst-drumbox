/* Drumbox kits: the built-in ones, the 32 user slots and the files in the data folder.
 *
 * Kit list (MPC's PRESET menu and KIT), in this order, so a number always means the same kit:
 *   the built-in kits (src/builtin_kits.h, made by mpc/gen.py from kits/: nb_tonic's five and four Patternarium presets)
 *   U01 .. U32, the user slots: <data>/USER/01.mtpreset .. 32.mtpreset (SAVE writes them; empty ones say "(empty)")
 *   every .mtpreset and .tkit in the data folder and its sub-folders one level down (not USER), sorted by path
 *
 * Formats (both text):
 *   .mtpreset  Sonic Charge Microtonic preset ("MicroTonicPresetV3: { ... DrumPatches: { 1: { Key = Value ... } } }"),
 *              as Patternarium's download button saves it. Only the eight drum patches are read; patterns, tempo and
 *              mutes are not (Drumbox has no sequencer).
 *   .tkit      nb_tonic kit (a Lua table: "-- Table: {2}" .. "{9}" hold voices 1-8 as ["key"]=value).
 * Units: Microtonic's own (Hz, ms, dB, %, -100..100) and nb_tonic's (Hz, s, linear level, -1..1); both are turned
 * into the knob values the parameters hold (the curves in voice.c). Where the two differ (EQ gain: Microtonic's +-40
 * is nb_tonic's +-20, kept on Microtonic's scale; MIX: nb_tonic's -1..1 crossfade is Microtonic's 100..0 %; MOD RATE in decay mode: Microtonic
 * gives the decay in ms, nb_tonic the rate in Hz, 1000 / ms) the conversion follows nb_tonic and Scholl's importer
 * (supercollider-microtonic/utonic2sc.lua). */
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

void kit_init_values(float pv[NV][NVP]) {
    for (int v = 0; v < NV; v++)
        for (int k = 0; k < NVP; k++) pv[v][k] = PDEF[VK(v, k)].def;
}

static void name_copy(char *dst, const char *src, size_t n) {
    snprintf(dst, n, "%s", src);
    /* "Unmouth Sphing (139)": Patternarium puts the tempo in the name; Drumbox has none */
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

/* ---- .mtpreset ------------------------------------------------------------------------------------------------- */

static int word_is(const char *v, const char *w) { return !strncasecmp(v, w, strlen(w)); }

static void mt_set(float *p, const char *key, const char *val, int *choke_own) {
    double x = strtod(val, NULL);
    int isinf_ = isinf(x);
    if (!strcmp(key, "OscWave")) p[VP_OSC_WAVE] = word_is(val, "Tri") ? W_TRI : word_is(val, "Saw") ? W_SAW : W_SINE;
    else if (!strcmp(key, "OscFreq")) p[VP_OSC_FREQ] = i_freq((float)x);
    else if (!strcmp(key, "OscAtk")) p[VP_OSC_ATK] = i_time((float)x);
    else if (!strcmp(key, "OscDcy")) p[VP_OSC_DCY] = i_time((float)x);
    else if (!strcmp(key, "ModMode")) p[VP_MOD_MODE] = word_is(val, "Sine") ? M_SINE : word_is(val, "Noise") ? M_NOISE : M_DECAY;
    else if (!strcmp(key, "ModAmt")) p[VP_MOD_AMT] = i_modamt((float)x);
    else if (!strcmp(key, "ModRate")) p[VP_MOD_RATE] = isinf_ ? -1.0f : (float)x;   /* resolved below, by the mode */
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

int parse_mtpreset(const char *t, kit_t *k) {
    char line[512];
    int depth = 0, dp = 0, dp_depth = 0, v = -1, nv = 0, rate_raw_set[NV] = {0}, choke_own = -1;
    float raw_rate[NV];
    kit_init_values(k->pv);
    if (strncmp(t, "MicroTonicPreset", 16)) return 0;
    while (*t) {
        size_t n = strcspn(t, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), t);
        t += n;
        while (*t == '\r' || *t == '\n') t++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "Patterns:", 9)) break;
        char *eq = strstr(s, " = ");
        if (dp && depth == dp_depth + 1 && *s >= '1' && *s <= '8' && s[1] == ':') {
            v = *s - '1';
            nv++;
            choke_own = -1;
        } else if (eq && v >= 0 && depth == dp_depth + 2) {
            *eq = 0;
            char *val = eq + 3;
            if (*val == '"') val++;
            mt_set(k->pv[v], s, val, &choke_own);
            if (!strcmp(s, "ModRate")) { raw_rate[v] = k->pv[v][VP_MOD_RATE]; rate_raw_set[v] = 1; }
        } else if (eq && !dp && depth == 1 && !strncmp(s, "Name = ", 7)) {
            char *q = s + 7;
            if (*q == '"') q++;
            q[strcspn(q, "\"")] = 0;
            name_copy(k->name, q, sizeof k->name);
        } else if (!strncmp(s, "DrumPatches:", 12)) {
            dp = 1;
            dp_depth = depth;
        }
        for (char *c = s; *c; c++) {
            if (*c == '{') depth++;
            else if (*c == '}') {
                depth--;
                if (dp && depth == dp_depth + 1) v = -1;
                if (dp && depth == dp_depth) dp = 0;
            }
        }
    }
    /* MOD RATE: in decay mode Microtonic stores the decay time in ms ("inf": none), else Hz */
    for (int i = 0; i < NV; i++) {
        if (!rate_raw_set[i]) continue;
        float r = raw_rate[i], hz;
        if ((int)k->pv[i][VP_MOD_MODE] == M_DECAY) hz = r < 0 ? 0.0f : r <= 0.05f ? 20000.0f : 1000.0f / r;
        else hz = r < 0 ? 20000.0f : r;
        k->pv[i][VP_MOD_RATE] = i_modrate(hz);
    }
    return nv > 0;
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

static int load_file(const char *path, kit_t *k) {
    char *t = read_text(path, 1 << 20);
    if (!t) return 0;
    int ok = ends_with(path, ".tkit") ? parse_tkit(t, k) : parse_mtpreset(t, k);
    free(t);
    if (ok && (!k->name[0] || ends_with(path, ".tkit"))) {   /* a .tkit has no name inside: its file's */
        const char *b = strrchr(path, '/');
        b = b ? b + 1 : path;
        snprintf(k->name, sizeof k->name, "%.*s", (int)(strcspn(b, ".") < 31 ? strcspn(b, ".") : 31), b);
    }
    return ok;
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* the kit files in a folder (and, at depth 0, its sub-folders but USER), full paths, malloc'd */
static void collect(const char *dir, int depth, char ***list, int *n, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p || lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth == 0 && strcasecmp(e->d_name, "USER")) collect(p, 1, list, n, cap);
        } else if (S_ISREG(st.st_mode) && (ends_with(e->d_name, ".mtpreset") || ends_with(e->d_name, ".tkit"))) {
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 64;
                char **nl = realloc(*list, *cap * sizeof **list);
                if (!nl) break;
                *list = nl;
            }
            (*list)[(*n)++] = strdup(p);
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
    snprintf(k->name, sizeof k->name, "U%02d (empty)", slot);
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
        k->used = 1;
        n++;
    }
    D->nbuiltin = n;
    D->user0 = n;
    for (int s = 1; s <= NUSER; s++, n++) {
        char p[700];
        kit_t *k = &D->kits[n];
        user_empty(k, s);
        if (!D->dir[0]) continue;
        user_path(D, s, p, sizeof p);
        kit_t tmp;
        memset(&tmp, 0, sizeof tmp);
        if (load_file(p, &tmp)) {
            *k = tmp;
            k->user = s;
            k->used = 1;
            char nm[32];
            snprintf(nm, sizeof nm, "%s", tmp.name);
            snprintf(k->name, sizeof k->name, "U%02d %.26s", s % 100, nm);
        }
    }
    if (D->dir[0]) {
        char **list = NULL;
        int nf = 0, cap = 0;
        collect(D->dir, 0, &list, &nf, &cap);
        if (nf) qsort(list, nf, sizeof *list, cmp_str);
        for (int i = 0; i < nf; i++) {
            if (n < MAX_KITS) {
                kit_t *k = &D->kits[n];
                memset(k, 0, sizeof *k);
                if (load_file(list[i], k)) {
                    k->used = 1;
                    n++;
                }
            }
            free(list[i]);
        }
        free(list);
    }
    D->nkits = n;
}

void kits_free(db_t *D) {
    free(D->kits);
    D->kits = NULL;
}

/* ---- SAVE: the voices as they are, as a Microtonic preset -------------------------------------------------------- */

static float modrate_out(const float *p) {   /* Microtonic's ModRate: ms in decay mode */
    float hz = k_modrate(p[VP_MOD_RATE]);
    if ((int)p[VP_MOD_MODE] == M_DECAY) return hz <= 0.0f ? INFINITY : 1000.0f / hz;
    return hz;
}

int kit_save_user(db_t *D, int slot) {
    if (slot < 1 || slot > NUSER || !D->kits) return -1;
    char path[700], dir[600];
    snprintf(dir, sizeof dir, "%s/USER", D->dir[0] ? D->dir : ".");
    if (mkdir(dir, 0755) && errno != EEXIST) return -1;
    user_path(D, slot, path, sizeof path);
    char tmp[720];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    /* the name: the kit loaded, without a user slot's "Unn " */
    int cur = (int)D->pv[P_KIT];
    const char *nm = cur >= 0 && cur < D->nkits ? D->kits[cur].name : "Drumbox";
    if (cur >= D->user0 && cur < D->user0 + NUSER) nm = D->kits[cur].used ? nm + 4 : "Drumbox";
    char name[32];
    snprintf(name, sizeof name, "%s", nm);
    static const char *const WAVE[3] = {"Sine", "Triangle", "Saw"}, *const MODE[3] = {"Decay", "Sine", "Noise"},
                             *const FIL[3] = {"LP", "BP", "HP"}, *const ENV[3] = {"Exp", "Linear", "Mod"};
    fprintf(f, "MicroTonicPresetV3: {\n\tName = \"%s\"\n\tDrumPatches: {\n", name);
    for (int v = 0; v < NV; v++) {
        const float *p = &D->pv[VK(v, 0)];
        float lvl = p[VP_LEVEL];
        fprintf(f, "\t\t%d: {\n", v + 1);
        fprintf(f, "\t\t\tChoke = %s\n\t\t\tDrumboxChoke = %d\n", p[VP_CHOKE] > 0 ? "On" : "Off", (int)p[VP_CHOKE]);
        fprintf(f, "\t\t\tDistAmt = %.6g\n", p[VP_DIST]);
        fprintf(f, "\t\t\tEQFreq = %.8g\n\t\t\tEQGain = %.6g\n", k_freq(p[VP_EQ_FREQ]), k_eqgain(p[VP_EQ_GAIN]));
        if (lvl <= 0.0f) fprintf(f, "\t\t\tLevel = -inf\n");
        else fprintf(f, "\t\t\tLevel = %.6g\n", k_db(lvl));
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
    fprintf(f, "\t}\n}\n");
    int bad = fflush(f) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path)) { remove(tmp); return -1; }
    kit_t *k = &D->kits[D->user0 + slot - 1];
    memset(k, 0, sizeof *k);
    for (int v = 0; v < NV; v++)
        for (int j = 0; j < NVP; j++) k->pv[v][j] = D->pv[VK(v, j)];
    k->user = slot;
    k->used = 1;
    snprintf(k->name, sizeof k->name, "U%02d %.26s", slot, name);
    return 0;
}
