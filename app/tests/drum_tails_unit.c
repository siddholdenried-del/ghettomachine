/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine 0.5 (CPU): the drums' cheaper tails keep the sound.
 *   the 909: a voice rests once it has stayed under -90 dBFS (drum909.c d9_rest): against every hit rendered to its
 *            timer's end (D9_REST=0), nothing over -90 dBFS differs, and the kit is at rest sooner
 *   the 808's cymbal: lite once its body has shut (drum808.c cy_lite_run): against the whole circuit throughout
 *            (D8_CY_LITE=0), the same samples until then, and after it the same level within 0.1 dB in every tenth of
 *            a second; at Tune's ends, at the longest Decay and on an accent too
 * Two builds of this file, the reference first:
 *   S="app/dsp/drums.c app/dsp/drum808.c app/dsp/drum909.c"; F="-O2 -std=c99 -ffp-contract=off -w -DPLAT_HOST -Iapp/dsp -Iapp/build/gen"
 *   cc $F -DD9_REST=0 -DD8_CY_LITE=0 app/tests/drum_tails_unit.c $S -lm -o /tmp/dt_ref && /tmp/dt_ref write /tmp/dt_ref.raw
 *   cc $F app/tests/drum_tails_unit.c $S -lm -o /tmp/dt && /tmp/dt check /tmp/dt_ref.raw */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drums.h"

#define N 256
#define BLOCKS 520                                /* 3 s */
#define CASES (DRUMS_NPADS + 5)
static float L[N], R[N], RV[N], DL[N];
static float take[BLOCKS * N], ref[BLOCKS * N];
static int rested_at;                             /* the block the kit was first at rest in */
static int fails, checks;

static void check(int ok, const char *what, int c, double got)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL case %d: %s (%g)\n", c, what, got);
    }
}

/* cases 0..10: the 909's pads, a hit each. 11..15: the 808's cymbal: plain, Tune low and high, Decay long, an accent */
static void render(int c)
{
    int kit = c < DRUMS_NPADS ? KIT_909 : KIT_808, pad = c < DRUMS_NPADS ? c : 9, b, i;
    drums_init();
    if (c == DRUMS_NPADS + 1) drums_set(KIT_808, pad, 2, 10);     /* (the cymbal's knobs: Level, Decay, Tune) */
    if (c == DRUMS_NPADS + 2) drums_set(KIT_808, pad, 2, 118);
    if (c == DRUMS_NPADS + 3) drums_set(KIT_808, pad, 1, 127);
    drums_trigger(kit, pad, c == DRUMS_NPADS + 4 ? 1.0f : DRUMS_VEL_NORMAL);
    rested_at = -1;
    for (b = 0; b < BLOCKS; b++) {
        drums_render(kit, L, R, RV, DL, N);
        for (i = 0; i < N; i++)
            take[b * N + i] = L[i];
        if (rested_at < 0 && !drums_active(kit))
            rested_at = b;
    }
}

int main(int argc, char **argv)
{
    int c, i, w;
    FILE *f;
    if (argc < 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "check")))
        return 2;
    f = fopen(argv[2], argv[1][0] == 'w' ? "wb" : "rb");
    if (!f)
        return 2;
    for (c = 0; c < CASES; c++) {
        int ref_rest = 0;
        render(c);
        if (argv[1][0] == 'w') {
            fwrite(take, sizeof take, 1, f);
            fwrite(&rested_at, sizeof rested_at, 1, f);
            continue;
        }
        if (fread(ref, sizeof ref, 1, f) != 1 || fread(&ref_rest, sizeof ref_rest, 1, f) != 1)
            return 2;
        if (c < DRUMS_NPADS) {                    /* the 909 */
            float worst = 0.0f;
            for (i = 0; i < BLOCKS * N; i++) {
                float d = fabsf(take[i] - ref[i]);
                if (d > worst)
                    worst = d;
            }
            check(worst <= 3.2e-5f, "a 909 hit differs over -90 dBFS", c, worst);
            check(rested_at >= 0 && rested_at <= ref_rest, "the 909 voice rests later than its timer", c, rested_at);
        } else {                                  /* the 808's cymbal */
            int first = -1;
            for (i = 0; i < BLOCKS * N && first < 0; i++)
                if (take[i] != ref[i])
                    first = i;
            check(first < 0 || first > 2205, "the cymbal differs in its first 50 ms", c, first);
            for (w = 0; w + 4410 <= BLOCKS * N; w += 4410) {
                double pa = 0.0, pb = 0.0, pd = 0.0;
                for (i = w; i < w + 4410; i++) {
                    pa += (double)ref[i] * ref[i];
                    pb += (double)take[i] * take[i];
                    pd += (double)(take[i] - ref[i]) * (take[i] - ref[i]);
                }
                if (pa < 4410.0 * 1e-9)           /* (under -90 dB: the tail's end) */
                    continue;
                check(fabs(10.0 * log10(pb / pa)) <= 0.1, "the lite cymbal's level is off by over 0.1 dB", c, 10.0 * log10(pb / pa));
                /* (the samples themselves: 20 dB close at Tune's centre; at its ends the stand-in's phase is off) */
                check(pd <= pa * (c == DRUMS_NPADS + 1 || c == DRUMS_NPADS + 2 ? 0.1 : 0.01), "the lite cymbal's samples are far off", c,
                      10.0 * log10(pd / pa));
            }
            check(abs(rested_at - ref_rest) <= 2, "the cymbal ends elsewhere", c, rested_at - ref_rest);
        }
    }
    fclose(f);
    if (argv[1][0] == 'c')
        printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
