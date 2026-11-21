/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../dsp/drum606.h"
#include "../dsp/drums.h"

enum { N = 128, BLOCKS = 10 };
static float left[N], right[N], rev[N], dly[N];

static double sum(const float *samples)
{
    double energy = 0;
    for (int i = 0; i < N; i++) {
        assert(isfinite(samples[i]));
        energy += (double)samples[i] * samples[i];
    }
    return energy;
}

static double pad_energy(int pad)
{
    drums_init();
    drums_trigger(KIT_606, pad, DRUMS_VEL_NORMAL);
    double energy = 0;
    for (int i = 0; i < BLOCKS; i++) {
        drums_render(KIT_606, left, right, rev, dly, N);
        assert(!memcmp(left, right, sizeof left));
        energy += sum(left);
    }
    return energy;
}

static double hat_tail(int choke)
{
    /* Setting 2 makes the hats choke in both directions. */
    drum606_t d;
    drum606_init(&d);
    drum606_set(&d, D6_KIT, D6K_CHOKE, choke);
    drum606_set(&d, D6_CH, D6P_LEVEL, 0);
    drum606_trigger(&d, D6_OH, 110);
    for (int i = 0; i < 6; i++) {
        memset(left, 0, sizeof left); memset(rev, 0, sizeof rev); memset(dly, 0, sizeof dly);
        drum606_render(&d, left, rev, dly, N);
    }
    drum606_trigger(&d, D6_CH, 110);
    double energy = 0;
    for (int i = 0; i < 8; i++) {
        memset(left, 0, sizeof left); memset(rev, 0, sizeof rev); memset(dly, 0, sizeof dly);
        drum606_render(&d, left, rev, dly, N);
        if (i >= 4) energy += sum(left);
    }
    return energy;
}

int main(void)
{
    const int pads[] = {0, 1, 2, 4, 6, 7, 8, 9};
    assert(KIT_909 == 0 && KIT_808 == 1 && KIT_606 == 2 && KIT_NUM >= 3);   /* (the 707 came after: KIT_NUM 4) */
    for (unsigned i = 0; i < sizeof pads / sizeof pads[0]; i++) {
        int pad = pads[i];
        assert(drums_nparams(KIT_606, pad) >= 7);
        assert(pad_energy(pad) > 1e-6);
        for (int p = 0; p < drums_nparams(KIT_606, pad); p++) {
            const x0x_param_t *desc = drums_param(KIT_606, pad, p);
            assert(desc && desc->def == drums_get(KIT_606, pad, p));
            drums_set(KIT_606, pad, p, 255);
            assert(drums_get(KIT_606, pad, p) == desc->max);
        }
    }
    assert(pad_energy(3) == 0 && pad_energy(5) == 0 && pad_energy(10) == 0);
    assert(hat_tail(0) > 1e-6 && hat_tail(2) < hat_tail(0) * .05);
    puts("606 eight pads, three intentional gaps, parameter descriptors, mono balance, and closed-hat choke passed");
}
