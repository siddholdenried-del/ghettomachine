/* SPDX-License-Identifier: GPL-3.0-only */
/* BongFX on the FM-1 (dsp/bongfx.c) against BongVerb's and BongDelay's own kernels (bongverb verb.c, bongdelay
 * delay.c: their repositories, private; give their paths). 60 s of a send that plays and stops, its settings moved
 * while it sounds (the reverb's four, the delay's four), the port and the kernel side by side:
 *   - every sample within -90 dB of the kernel's peak (the port's coefficients come from fastmath.h, not libm);
 *   - the sleep: silence in gives silence out once the tails have gone, and the first sample wakes it;
 *   - the delay's line lent and given back (bongdelay_restart): silence from an empty line, echoes again after;
 *   - the delay's time from milliseconds and back.
 * Twice: as built (the sleep on), and with -DBF_SLEEP=0 (never asleep: the arithmetic alone, within -90 dB)
 *   cc -O2 -std=c11 -ffp-contract=off -DPLAT_HOST -Iapp/dsp app/tests/bongfx_unit.c app/dsp/bongfx.c \
 *      BONGVERB/verb.c BONGDELAY/delay.c -lm -o /tmp/bongfx_unit && /tmp/bongfx_unit */
#ifndef BF_SLEEP
#define BF_SLEEP 1
#endif
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "bongfx.h"

void verb_init(float), verb_set(int, float), verb_tick(float, float *, float *);
void delay_init(float), delay_set(int, float), delay_tick(float, float *, float *);

static int fails, checks;
static void check(int ok, const char *what, double v)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s (%g)\n", what, v); }
}

#define N 256
static bongverb_t V;
static bongdelay_t D;
static int16_t ring[BD_RING];

int main(void)
{
    static const float vset[4] = {0.f, .49f, .16f, .44f}, dset[4] = {.27f, .46f, .52f, .21f};   /* presets.json */
    float in[N], pl[N], pr[N], ql[N], qr[N];
    double worst_v = 0, worst_d = 0, peak_v = 0, peak_d = 0;
    uint32_t rs = 7;
    verb_init(44100.f);
    delay_init(44100.f);
    bongverb_init(&V);
    bongdelay_init(&D, ring);
    for (int i = 0; i < 4; i++) {
        verb_set(i, vset[i]); bongverb_set(&V, i, vset[i]);
        delay_set(i, dset[i]); bongdelay_set(&D, i, dset[i]);
    }
    for (int b = 0; b < 60 * 44100 / N; b++) {
        const int sec = b * N / 44100;
        if (b % 1723 == 900) {            /* settings moved while it sounds */
            const float x = (float)((b / 1723) % 5) * .25f;
            const int id = (b / 1723) % 4;
            verb_set(id, x); bongverb_set(&V, id, x);
            delay_set(id, x); bongdelay_set(&D, id, x);
        }
        for (int i = 0; i < N; i++) {
            rs = rs * 1103515245u + 12345u;
            /* hits for 4 s in every 10, the rest silent (the tails, the sleep) */
            in[i] = (sec % 10 < 4 && (b % 20) < 2) ? .4f * ((float)(rs >> 16 & 0x7FFF) / 16384.f - 1.f) : 0.f;
        }
        for (int i = 0; i < N; i++) pl[i] = pr[i] = ql[i] = qr[i] = 0.f;
        bongverb_block(&V, in, pl, pr, 1.f, N);
        for (int i = 0; i < N; i++) {
            float a, c;
            verb_tick(in[i], &a, &c);
            worst_v = fmax(worst_v, fmax(fabs(a - pl[i]), fabs(c - pr[i])));
            peak_v = fmax(peak_v, fmax(fabs(a), fabs(c)));
        }
        bongdelay_block(&D, in, ql, qr, 1.f, N);
        for (int i = 0; i < N; i++) {
            float a, c;
            delay_tick(in[i], &a, &c);
            worst_d = fmax(worst_d, fmax(fabs(a - ql[i]), fabs(c - qr[i])));
            peak_d = fmax(peak_d, fabs(a));
            if (ql[i] != qr[i]) { check(0, "the delay is dual mono", ql[i] - qr[i]); break; }
        }
    }
    printf("BongVerb: peak %.4f, the port's largest difference %.1f dB under it\n", peak_v, 20 * log10(peak_v / (worst_v + 1e-30)));
    printf("BongDelay: peak %.4f, the port's largest difference %.1f dB under it\n", peak_d, 20 * log10(peak_d / (worst_d + 1e-30)));
#if BF_SLEEP
    /* with the sleep: asleep only in silence, and what is not sound kept going: within -90 dB still */
    check(peak_v > .01 && worst_v < peak_v * 3.2e-5, "BongVerb (asleep in the gaps) within -90 dB of its kernel", worst_v / peak_v);
    check(peak_d > .01 && worst_d < peak_d * 3.2e-5, "BongDelay (asleep in the gaps) within -90 dB of its kernel", worst_d / peak_d);
#else
    check(peak_v > .01 && worst_v < peak_v * 3.2e-5, "BongVerb within -90 dB of its kernel", worst_v / peak_v);
    check(peak_d > .01 && worst_d < peak_d * 3.2e-5, "BongDelay within -90 dB of its kernel", worst_d / peak_d);
#endif
    {   /* the sleep: 6 s of silence in -> asleep, silence out; one sample in wakes them */
        float z[N] = {0};
        for (int b = 0; b < 6 * 44100 / N; b++) {
            for (int i = 0; i < N; i++) pl[i] = pr[i] = 0.f;
            bongverb_block(&V, z, pl, pr, 1.f, N);
            bongdelay_block(&D, z, pl, pr, 1.f, N);
        }
        check(bongverb_asleep(&V), "BongVerb asleep after its tail", 0);
        check(bongdelay_asleep(&D), "BongDelay asleep after its tail", 0);
        z[3] = .5f;
        for (int i = 0; i < N; i++) pl[i] = pr[i] = 0.f;
        bongverb_block(&V, z, pl, pr, 1.f, N);
        bongdelay_block(&D, z, pl, pr, 1.f, N);
        check(!bongverb_asleep(&V) && !bongdelay_asleep(&D), "a sample in wakes them", 0);
    }
    {   /* the line lent (the caller zeroes it) and back: silence from it, then echoes of what comes */
        float z[N] = {0}, peak = 0;
        for (int i = 0; i < BD_RING; i++) ring[i] = 1234;   /* (what a recording left) */
        for (int i = 0; i < BD_RING; i++) ring[i] = 0;      /* fx_delay_zero */
        bongdelay_restart(&D);
        for (int b = 0; b < 200; b++) {
            for (int i = 0; i < N; i++) ql[i] = qr[i] = 0.f;
            bongdelay_block(&D, z, ql, qr, 1.f, N);
            for (int i = 0; i < N; i++) peak = fmaxf(peak, fabsf(ql[i]));
        }
        check(peak == 0.f, "an empty line gives back silence", peak);
        z[0] = .5f;
        peak = 0;
        for (int b = 0; b < 200; b++) {
            for (int i = 0; i < N; i++) ql[i] = qr[i] = 0.f;
            bongdelay_block(&D, z, ql, qr, 1.f, N);
            z[0] = 0.f;
            for (int i = 0; i < N; i++) peak = fmaxf(peak, fabsf(ql[i]));
        }
        check(peak > .01f, "echoes again after", peak);
    }
    {   /* BongVerb's lines sized for 44.1 kHz: what init laid out fits them */
        bongverb_t w;
        bongverb_init(&w);
        check(w.off[3] + w.len[3] <= BV_RING, "the tank lines fit BV_RING", w.off[3] + w.len[3]);
        check(w.ap_off[1] + w.ap_len[1] <= BV_AP, "the diffusers fit BV_AP", w.ap_off[1] + w.ap_len[1]);
        check(w.pre_len <= BV_PRE, "the pre-delay fits BV_PRE", w.pre_len);
        printf("BongVerb at 44.1 kHz: tank %d of %d, diffusers %d of %d, pre-delay %d of %d samples\n", w.off[3] + w.len[3],
               BV_RING, w.ap_off[1] + w.ap_len[1], BV_AP, w.pre_len, BV_PRE);
    }
    for (float ms = 45.f; ms <= 950.f; ms += 5.f) {   /* the time from ms, and back */
        bongdelay_set(&D, BD_TIME, bongdelay_time_value(ms));
        if (fabsf(bongdelay_ms(&D) - ms) > .05f) { check(0, "delay time round trip, ms", ms); break; }
    }
    checks++;
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
