/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's DrumEngine: its factory sounds' levels against the stock kits' (v3/trial.c says why).
 * One hit of each pad's recipe from a fresh start at the sequencer's normal velocity (88/127), 1.5 s, through the
 * DrumEngine as the drum track renders it (trial_render, DRY, level 100) and the stock 808 and 909 through
 * drums_render: the loudest 100 ms, K-weighted (BS.1770's filter; LUFS), stereo.
 *   check (the default): every recipe within 0.5 dB of its pad's target, and no sample over +12 dBFS on the track
 *   --write FILE: the trims that put each recipe on its target into FILE (v3/levels.inc): the trims built in,
 *                 corrected by what was measured with them
 *   --csv: the table
 * Build (from the repository's top):
 *   F="-O2 -std=c11 -ffp-contract=off -w -DPLAT_HOST -Iapp -Iapp/dsp -Iapp/v3 -Iapp/build/gen"
 *   V="app/v3/kit.c app/v3/kit_math.c app/v3/shells.c app/v3/metal.c app/v3/perc.c app/v3/drum_bus.c app/v3/macro.c
 *      app/v3/presets.c app/v3/trial.c app/dsp/drums.c app/dsp/drum808.c app/dsp/drum909.c"
 *   cc $F app/tests/drumengine_levels.c $V -lm -o /tmp/de_levels && /tmp/de_levels */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3/trial.h"
#include "v3/presets.h"
#include "dsp/drums.h"

uint32_t plat_ticks24(void) { return 0; }

/* the targets, by pad (1..11): the 808's and 909's own pads at level 100, averaged (LUFS over 100 ms) */
static const double TARGET[12] = {0, -1.7, -6.5, -12.2, -7.0, -7.0, -7.0, -20.9, -15.8, -13.5, -13.5, -13.5};

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;
static void bq_shelf(bq_t *f)             /* +4 dB high shelf (BS.1770 stage 1), at 44.1 kHz */
{
    const double G = 3.999843853973347, fc = 1681.974450955533, Q = 0.7071752369554196;
    const double A = pow(10, G / 40), w = 2 * M_PI * fc / 44100.0, al = sin(w) / (2 * Q), c = cos(w);
    const double a0 = (A + 1) - (A - 1) * c + 2 * sqrt(A) * al;
    f->b0 = A * ((A + 1) + (A - 1) * c + 2 * sqrt(A) * al) / a0;
    f->b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    f->b2 = A * ((A + 1) + (A - 1) * c - 2 * sqrt(A) * al) / a0;
    f->a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    f->a2 = ((A + 1) - (A - 1) * c - 2 * sqrt(A) * al) / a0;
    f->z1 = f->z2 = 0;
}
static void bq_hp(bq_t *f)                /* the 38 Hz high-pass (stage 2) */
{
    const double fc = 38.13547087602444, Q = 0.5003270373238773, w = 2 * M_PI * fc / 44100.0;
    const double al = sin(w) / (2 * Q), c = cos(w), a0 = 1 + al;
    f->b0 = (1 + c) / 2 / a0; f->b1 = -(1 + c) / a0; f->b2 = (1 + c) / 2 / a0;
    f->a1 = -2 * c / a0; f->a2 = (1 - al) / a0; f->z1 = f->z2 = 0;
}
static double bq_run(bq_t *f, double x)
{
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#define BL 256
#define NB 260
#define WIN 4410
typedef struct { bq_t k[2][2]; double sq[NB * BL]; long n; double peak; } meter_t;
static meter_t M;
static float L[BL], R[BL], RV[BL], DL[BL];
static void m_init(void)
{
    for (int c = 0; c < 2; c++) { bq_shelf(&M.k[c][0]); bq_hp(&M.k[c][1]); }
    M.n = 0; M.peak = 0;
}
static void m_add(void)
{
    for (int i = 0; i < BL; i++) {
        double a = bq_run(&M.k[0][1], bq_run(&M.k[0][0], L[i])), b = bq_run(&M.k[1][1], bq_run(&M.k[1][0], R[i]));
        M.sq[M.n++] = a * a + b * b;
        if (fabs(L[i]) > M.peak) M.peak = fabs(L[i]);
        if (fabs(R[i]) > M.peak) M.peak = fabs(R[i]);
    }
}
static double m_lufs(void)
{
    double s = 0, best = 0;
    for (long i = 0; i < M.n; i++) {
        s += M.sq[i];
        if (i >= WIN) s -= M.sq[i - WIN];
        if (s / WIN > best) best = s / WIN;
    }
    return best > 0 ? -0.691 + 10 * log10(best) : -200;
}

int main(int argc, char **argv)
{
    const float vel = 88.0f / 127.0f;
    const char *write = 0;
    int csv = 0, fails = 0, checks = 0;
    static double meas[12][8];
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--write") && a + 1 < argc) write = argv[++a];
        else if (!strcmp(argv[a], "--csv")) csv = 1;
    }
    if (csv) printf("engine,pad,recipe,name,peak_dbfs,lufs100,target\n");
    for (int pad = 1; pad <= 11; pad++)
        for (int r = 0; r < kit_preset_count(pad); r++) {
            trial_init();
            if (r) trial_recipe_turn((unsigned)pad, r);
            if (trial_recipe((unsigned)pad) != r) { printf("FAIL pad %d recipe %d not recalled\n", pad, r); return 1; }
            trial_hit((unsigned)pad, vel);
            m_init();
            for (int b = 0; b < NB; b++) { trial_render(L, R, RV, DL, BL, 1, 0, 0); m_add(); }
            meas[pad][r] = m_lufs();
            if (csv)
                printf("DrumEngine,%d,%d,%s,%.2f,%.2f,%.1f\n", pad, r, kit_preset_name(pad, r), 20 * log10(M.peak), meas[pad][r], TARGET[pad]);
            if (!write) {
                checks += 2;
                if (fabs(meas[pad][r] - TARGET[pad]) > 0.5) {
                    printf("FAIL pad %d %s: %.2f LUFS, its target %.1f\n", pad, kit_preset_name(pad, r), meas[pad][r], TARGET[pad]);
                    fails++;
                }
                if (M.peak > pow(10, 12 / 20.0)) {
                    printf("FAIL pad %d %s: peak %.2f dBFS\n", pad, kit_preset_name(pad, r), 20 * log10(M.peak));
                    fails++;
                }
            }
        }
    if (csv)
        for (int kit = 0; kit < 2; kit++)
            for (int pad = 0; pad < DRUMS_NPADS; pad++) {
                drums_init();
                drums_trigger(kit, pad, vel);
                m_init();
                for (int b = 0; b < NB; b++) { drums_render(kit, L, R, RV, DL, BL); m_add(); }
                printf("%s,%d,0,%s,%.2f,%.2f,\n", drums_kit_name(kit), pad + 1, drums_pad_name(kit, pad), 20 * log10(M.peak), m_lufs());
            }
    if (write) {
        FILE *f = fopen(write, "w");
        if (!f) return 2;
        fprintf(f, "/* GhettoMachine's DrumEngine: each factory recipe's level trim, tenths of a dB (trial.c DE_TRIM). Written by\n"
                   " * tests/drumengine_levels.c --write from measurement (the loudest 100 ms of a hit, K-weighted, against its\n"
                   " * pad's target); write it again after a recipe's sound changes */\n");
        for (int pad = 1; pad <= 11; pad++) {
            fprintf(f, "    {");
            for (int r = 0; r < 8; r++) {
                int t = 0;
                if (r < kit_preset_count(pad)) t = trial_trim_db10((unsigned)pad, (unsigned)r) + (int)lround((TARGET[pad] - meas[pad][r]) * 10.0);
                if (t > 300) t = 300;
                if (t < -300) t = -300;
                fprintf(f, "%s%d", r ? ", " : "", t);
            }
            fprintf(f, "}%s   /* pad %d */\n", pad < 11 ? "," : "", pad);
        }
        fclose(f);
        return 0;
    }
    if (!csv) printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
