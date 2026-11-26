/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's synth: the factory sounds' levels. Each preset plays one phrase from a fresh start (a MONO sound a
 * C2 and a C4, a POLY sound a C major triad at C3 and at C4, the louder; velocity 0.75, held 1 s) at track level 100 with its trim (dsp/synth_levels.inc)
 * and the loudest 100 ms is measured, K-weighted (BS.1770's filter), mono sum.
 *   check (the default): every preset within 1.5 dB of the target (some sounds have chance in them), unless its trim is at a limit (+6 dB, -12 dB)
 *   --write FILE: the trims that put each on the target (the median of the untrimmed presets) into FILE
 * Build (from the repository's top, after a host build for app/build/gen):
 *   cc -std=c99 -O2 -w -DPLAT_HOST -Iapp/build/gen -Iapp/dsp -Iapp app/tests/synth_levels.c app/dsp/synth.c -lm -o /tmp/sl */
#include "synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN 4410
#define TARGET (-16.3)
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;
static double bq(bq_t *f, double x) { double y = f->b0 * x + f->z1; f->z1 = f->b1 * x - f->a1 * y + f->z2; f->z2 = f->b2 * x - f->a2 * y; return y; }
static void kw(bq_t *s, bq_t *h)
{
    const double G = 3.999843853973347, fc = 1681.974450955533, Q = 0.7071752369554196;
    const double A = pow(10, G / 40), w = 2 * M_PI * fc / 44100.0, al = sin(w) / (2 * Q), c = cos(w);
    const double a0 = (A + 1) - (A - 1) * c + 2 * sqrt(A) * al;
    const double w2 = 2 * M_PI * 38.13547087602444 / 44100.0, al2 = sin(w2) / (2 * 0.5003270373238773), c2 = cos(w2), a02 = 1 + al2;
    memset(s, 0, sizeof *s); memset(h, 0, sizeof *h);
    s->b0 = A * ((A + 1) + (A - 1) * c + 2 * sqrt(A) * al) / a0; s->b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    s->b2 = A * ((A + 1) + (A - 1) * c - 2 * sqrt(A) * al) / a0; s->a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    s->a2 = ((A - 1) - (A + 1) * c - 2 * sqrt(A) * al) / a0; s->a2 = ((A + 1) - (A - 1) * c - 2 * sqrt(A) * al) / a0;
    h->b0 = (1 + c2) / 2 / a02; h->b1 = -(1 + c2) / a02; h->b2 = (1 + c2) / 2 / a02; h->a1 = -2 * c2 / a02; h->a2 = (1 - al2) / a02;
}
static double measure_at(int p, double gain, int up)
{
    static double sq[44100 * 2];
    int8_t v[SP_NUM];
    int mono = syn_preset(p, v);
    long k = 0;
    bq_t s, h;
    double acc = 0, best = 0;
    kw(&s, &h);
    syn_init(); syn_sound(1, v); syn_level(1, (float)(100.0 / 127.0 * gain)); syn_mixer(1, 64, 0, 0);
    for (int b = 0; b < 2400; b++) {
        float l[SYN_CTL], r[SYN_CTL], rv[SYN_CTL], dl[SYN_CTL];
        if (b == 8) {
            const int o = up ? (mono ? 24 : 12) : 0;   /* (a second look two octaves up, an octave for a chord) */
            if (mono) syn_note_on(1, 36 + o, .75f, 1378 * SYN_CTL, 1);
            else { syn_note_on(1, 48 + o, .75f, 1378 * SYN_CTL, 0); syn_note_on(1, 52 + o, .75f, 1378 * SYN_CTL, 0); syn_note_on(1, 55 + o, .75f, 1378 * SYN_CTL, 0); }
        }
        syn_prepare(); syn_half(0); syn_half(1); syn_mix(l, r, rv, dl);
        for (int j = 0; j < SYN_CTL; j++) { double y = bq(&h, bq(&s, .5 * ((double)l[j] + r[j]))); sq[k++] = y * y; }
    }
    for (long i = 0; i < k; i++) { acc += sq[i]; if (i >= WIN) acc -= sq[i - WIN]; if (acc / WIN > best) best = acc / WIN; }
    return best > 0 ? -0.691 + 10 * log10(best) : -200;
}
/* the louder of the two registers: a sound made for the top of the keys is not judged by its bottom */
static double measure(int p, double gain)
{
    const double a = measure_at(p, gain, 0), b = measure_at(p, gain, 1);
    return a > b ? a : b;
}
static int cmp(const void *a, const void *b) { double d = *(const double *)a - *(const double *)b; return d < 0 ? -1 : d > 0; }
int main(int argc, char **argv)
{
    int n = syn_presets(), fails = 0;
    const char *write = argc > 2 && !strcmp(argv[1], "--write") ? argv[2] : 0;
    static double raw[512], srt[512];
    if (write) {
        FILE *f;
        double target;
        for (int p = 0; p < n; p++) srt[p] = raw[p] = measure(p, 1.0);
        qsort(srt, (size_t)n, sizeof srt[0], cmp);
        target = TARGET;                  /* (the first set's median when its trims were first written: the level every
                                           * project's sounds were brought to, kept for the second set) */
        if (!(f = fopen(write, "w"))) return 2;
        fprintf(f, "/* GhettoMachine's synth: each factory sound's level trim, tenths of a dB (synth.c PRESET_TRIM). Written by\n"
                   " * tests/synth_levels.c --write (the loudest 100 ms of a note or a chord, K-weighted; the target the first set's median,\n"
                   " * %.1f LUFS at level 100; +6 / -12 dB at most); write it again after a preset's sound changes */\n", target);
        for (int p = 0; p < n; p++) {
            int t = (int)lround((target - raw[p]) * 10.0);
            if (t > 60) t = 60;               /* (a quiet sound comes up 6 dB at most: its own register may be louder) */
            if (t < -120) t = -120;
            fprintf(f, "%s%d,%s", p % 12 ? " " : "    ", t, p % 12 == 11 || p == n - 1 ? "\n" : "");
        }
        fclose(f);
        printf("target %.1f LUFS, %d presets\n", target, n);
        return 0;
    }
    for (int p = 0; p < n; p++) srt[p] = raw[p] = measure(p, pow(10, syn_preset_trim(p) / 200.0));
    qsort(srt, (size_t)n, sizeof srt[0], cmp);
    for (int p = 0; p < n; p++)
        if (fabs(raw[p] - TARGET) > 1.5 && syn_preset_trim(p) < 60 && syn_preset_trim(p) > -120) {
            printf("FAIL %d %s: %.1f LUFS, the target %.1f\n", p, syn_preset_name(p), raw[p], TARGET);
            fails++;
        }
    printf("%s: %d presets, median %.1f, quietest %.1f, loudest %.1f, %d off by over 1.5 dB\n", fails ? "FAIL" : "PASS", n, srt[n / 2], srt[0], srt[n - 1], fails);
    return fails != 0;
}
