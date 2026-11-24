/* SPDX-License-Identifier: GPL-3.0-only */
/* The DrumEngine's metal (v3/metal.c: the hats and the cymbal's four modes) heard and measured on the host: each
 * genre kit's closed hat, open hat and cymbal (CRASH, RIDE, RISER, DROP), one hit at velocity 100/127 through
 * metal_render (the voice alone: no trims, no bus), into one audition WAV (a voice, a gap, the next) and a table:
 *   - its band energies, as a share of the whole (0-2, 2-6, 6-12, 12-16, 16-22.05 kHz) over the first 300 ms;
 *   - its spectral centroid at 0-50 ms, 300 ms and 1 s (how it darkens as it rings);
 *   - its cost: metal_render's instructions a sample (with --bench).
 * Build (from the repository's top):
 *   F="-O2 -std=c11 -ffp-contract=off -w -DPLAT_HOST -Iapp -Iapp/dsp -Iapp/v3 -Iapp/build/gen"
 *   cc $F app/tests/metal_audition.c app/v3/metal.c app/v3/kit_math.c app/v3/presets.c app/v3/kit.c app/v3/shells.c \
 *      app/v3/perc.c -lm -o /tmp/metal && /tmp/metal OUT.wav [--genre N] [--bench] */
#include "metal.h"
#include "presets.h"
#include "kit_patch.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define SECS 3
#define N (SR * SECS)
static float buf[N];

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                double ur = re[i + k], ui = im[i + k], vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

/* the power spectrum of 2048 samples from `at` (Hann), summed into pw[1024] */
static void spec(const float *x, int at, double *pw)
{
    static double re[2048], im[2048];
    for (int i = 0; i < 2048; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / 2047.0);
        re[i] = at + i < N ? x[at + i] * w : 0; im[i] = 0;
    }
    fft(re, im, 2048);
    for (int k = 0; k < 1024; k++) pw[k] += re[k] * re[k] + im[k] * im[k];
}

static double centroid(const float *x, int at)
{
    double pw[1024] = {0}, s = 0, w = 0;
    spec(x, at, pw);
    for (int k = 1; k < 1024; k++) { s += pw[k] * k; w += pw[k]; }
    return w > 0 ? s / w * SR / 2048.0 : 0;
}

static void put_u(FILE *f, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++, v >>= 8) fputc(v & 255, f); }

int main(int argc, char **argv)
{
    static const char *const VOICE[6] = {"CLOSED HAT", "OPEN HAT", "CRASH", "RIDE", "RISER", "DROP"};
    const char *out = argc > 1 ? argv[1] : "metal.wav";
    int only = -1, bench = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--genre") && i + 1 < argc) only = atoi(argv[++i]);
        if (!strcmp(argv[i], "--bench")) bench = 1;
    }
    FILE *w = fopen(out, "wb");
    if (!w) return 1;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 16, 1, w); put_u(w, 16, 4); put_u(w, 1, 2); put_u(w, 1, 2); put_u(w, SR, 4);
    put_u(w, SR * 2, 4); put_u(w, 2, 2); put_u(w, 16, 2); fwrite("data\0\0\0\0", 8, 1, w);
    long frames = 0;
    double tot[5] = {0}, cen[3] = {0};
    int count = 0;
    printf("%-10s %-10s %6s %6s %6s %6s %6s | %6s %6s %6s Hz | peak\n", "kit", "voice", "0-2k", "2-6k", "6-12k",
           "12-16k", "16k+", "c0", "c300", "c1s");
    for (int g = 0; g < kit_genre_count(); g++) {
        if (only >= 0 && g != only) continue;
        for (int vo = 0; vo < 6; vo++) {
            KitPatch p = kit_default_patch();
            Metal m;
            int pad = vo < 2 ? 7 + vo : 9, len = vo == 0 ? SR / 2 : vo == 1 ? SR * 3 / 2 : N;
            kit_genre_apply(&p, g);
            if (vo >= 2) p.metal_mode = vo - 2;
            metal_init(&m);
            memset(buf, 0, sizeof buf);
            metal_hit(&m, pad, 100.0f / 127.0f, &p);
            for (int at = 0; at < len; at += 256) metal_render(&m, buf + at, len - at < 256 ? len - at : 256);
            double pw[1024] = {0}, band[5] = {0}, all = 0, peak = 0;
            for (int at = 0; at + 2048 <= SR * 3 / 10; at += 1024) spec(buf, at, pw);
            for (int k = 1; k < 1024; k++) {
                double hz = k * (double)SR / 2048;
                band[hz < 2000 ? 0 : hz < 6000 ? 1 : hz < 12000 ? 2 : hz < 16000 ? 3 : 4] += pw[k];
                all += pw[k];
            }
            for (int i = 0; i < len; i++) peak = fabs(buf[i]) > peak ? fabs(buf[i]) : peak;
            double c0 = centroid(buf, 0), c1 = centroid(buf, SR * 3 / 10), c2 = len > SR ? centroid(buf, SR) : 0;
            printf("%-10s %-10s", kit_genre_name(g), VOICE[vo]);
            for (int b = 0; b < 5; b++) { printf(" %5.1f%%", 100 * band[b] / all); tot[b] += band[b] / all; }
            printf(" | %6.0f %6.0f %6.0f    | %.2f\n", c0, c1, c2, peak);
            cen[0] += c0; cen[1] += c1; cen[2] += c2; count++;
            for (int i = 0; i < len + SR / 4; i++) {   /* the voice, then a quarter second's gap */
                double v = i < len ? buf[i] * 0.7 : 0;
                int s = (int)lrint((v > 1 ? 1 : v < -1 ? -1 : v) * 32767);
                put_u(w, (uint32_t)(uint16_t)s, 2);
                frames++;
            }
        }
    }
    printf("%-21s", "mean");
    for (int b = 0; b < 5; b++) printf(" %5.1f%%", 100 * tot[b] / count);
    printf(" | %6.0f %6.0f %6.0f\n", cen[0] / count, cen[1] / count, cen[2] / count);
    fseek(w, 4, SEEK_SET); put_u(w, (uint32_t)(36 + frames * 2), 4);
    fseek(w, 40, SEEK_SET); put_u(w, (uint32_t)(frames * 2), 4);
    fclose(w);
    if (bench) {                          /* a crash and both hats ringing, 60 s of samples: run under time -l */
        KitPatch p = kit_default_patch();
        Metal m;
        metal_init(&m);
        for (int s = 0; s < 60 * SR / 256; s++) {
            if (s % 40 == 0) { metal_hit(&m, 9, .8f, &p); metal_hit(&m, 8, .8f, &p); }
            if (s % 10 == 0) metal_hit(&m, 7, .8f, &p);
            static float b[256];
            memset(b, 0, sizeof b);
            metal_render(&m, b, 256);
        }
        printf("bench: 60 s rendered\n");
    }
    return 0;
}
