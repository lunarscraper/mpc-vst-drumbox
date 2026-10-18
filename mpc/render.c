/* Drumbox offline renderer (development): plays each kit's eight voices alone, then a two-bar groove, writes a stereo
 * WAV per kit and prints each voice's peak and length. Built natively, without the wrapper:
 *   cc -O2 -Isrc mpc/render.c src/drumbox.c src/voice.c src/kits.c -lm -o build/render
 *   build/render <data dir or ""> <out dir> [kit numbers...]          (no kit numbers: every kit) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
} mpc_engine_t;
const mpc_engine_t *mpc_engine(void);

static const mpc_engine_t *E;
static void *I;
static int16_t *buf;
static int pos, cap;

static void note(int n, int v) {
    uint8_t m[3] = {0x90, (uint8_t)n, (uint8_t)v};
    E->midi(I, m, 3);
}
static void run(int frames) {
    while (frames > 0) {
        int n = frames < 128 ? frames : 128;
        if (pos + 128 > cap) { cap = cap ? cap * 2 : 1 << 20; buf = realloc(buf, cap * 2 * sizeof *buf); }
        E->render(I, buf + 2 * pos, n);
        pos += n;
        frames -= n;
    }
}
static void stats(int from, int to, float *peak_db, float *len_s) {
    int pk = 1, last = from;
    for (int i = 2 * from; i < 2 * to; i++) if (abs(buf[i]) > pk) pk = abs(buf[i]);
    for (int i = from; i < to; i++) if (abs(buf[2 * i]) > pk / 100 || abs(buf[2 * i + 1]) > pk / 100) last = i;
    *peak_db = 20.0f * log10f(pk / 32768.0f);
    *len_s = (last - from) / 44100.0f;
}
static void wav(const char *path, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t d = frames * 4, r = 36 + d, sr = 44100, br = sr * 4, sixteen = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&r, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f);
    fwrite(buf, 4, frames, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "render <data dir> <out dir> [kits...]\n"); return 1; }
    E = mpc_engine();
    I = E->create(argv[1][0] ? argv[1] : NULL);
    char s[256];
    E->get_param(I, "kit_count", s, sizeof s);
    int nk = atoi(s), *list = malloc(sizeof(int) * (nk + argc)), nl = 0;
    if (argc > 3) for (int i = 3; i < argc; i++) list[nl++] = atoi(argv[i]);
    else for (int i = 0; i < nk; i++) list[nl++] = i;
    printf("%d kits\n", nk);
    for (int li = 0; li < nl; li++) {
        int k = list[li];
        char v[16], name[64] = "";
        snprintf(v, sizeof v, "%d", k);
        E->set_param(I, "kit", v);
        run(1);
        E->get_param(I, "kit_name", name, sizeof name);
        pos = 0;
        run(4410);
        printf("%3d %-24s", k, name);
        for (int vo = 0; vo < 8; vo++) {   /* each voice alone */
            int at = pos;
            note(36 + vo, 110);
            run(44100 + 22050);
            float pk, ln;
            stats(at, pos, &pk, &ln);
            printf(" V%d %5.1f/%4.2f", vo + 1, pk, ln);
        }
        int at = pos;
        /* a groove at 125 BPM: kick, snare-ish 5, hats 7/8, the rest sprinkled */
        static const char *const PAT[8] = {"x...x...x...x...", "..x.......x.....", "......x.......x.", "...x.......x..x.",
                                           "....x.......x...", ".......x.......x", "x.x.x.x.x.x.x.x.", "..x...x...x...x."};
        for (int bar = 0; bar < 2; bar++)
            for (int st = 0; st < 16; st++) {
                for (int vo = 0; vo < 8; vo++) if (PAT[vo][st] == 'x') note(36 + vo, (st & 3) ? 90 : 120);
                run(5292);
            }
        run(44100);
        float pk, ln;
        stats(at, pos, &pk, &ln);
        double rms = 0;
        for (int i = 2 * at; i < 2 * pos; i++) rms += (double)buf[i] * buf[i];
        rms = sqrt(rms / (2.0 * (pos - at))) / 32768.0;
        printf("  | groove %5.1f dB rms %5.1f\n", pk, 20 * log10(rms + 1e-9));
        char path[512];
        snprintf(path, sizeof path, "%s/kit_%03d.wav", argv[2], k);
        wav(path, pos);
    }
    E->destroy(I);
    free(buf);
    free(list);
    return 0;
}
