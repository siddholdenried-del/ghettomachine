/* SPDX-License-Identifier: GPL-3.0-only */
/* Mixed kits at the engines' level (dsp/drums.c): a pad's model from the other kit, the four buses, a cut pad, the hats'
 * choke across kits, and the kits that were cut (the 606, the 707 and its two extra voices: no pads, no sound, a pad
 * that names one falls back to its kit's own). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../dsp/drums.h"

static float a[4][128], b[4][128], c[4][128];
static void render(int base, float dest[4][128])
{
    drums_render(base, dest[0], dest[1], dest[2], dest[3], 128);
}
static float energy(float buf[4][128])
{
    float x = 0;
    for (int bus = 0; bus < 4; bus++)
        for (int i = 0; i < 128; i++) x += buf[bus][i] * buf[bus][i];
    return x;
}

int main(void)
{
    uint8_t map[DRUMS_NPADS] = {0};
    map[7] = KIT_808 + 1;
    map[3] = KIT_606 + 1; /* a cut kit's override must not silence the 909's MT */
    map[5] = DRUM_MODEL_707_CB + 1;
    assert(drums_model(KIT_909, map, 3) == KIT_909);
    assert(drums_model(KIT_909, map, 5) == KIT_909);
    assert(drums_model(KIT_909, map, 7) == KIT_808);
    assert(drums_model(KIT_909, map, 0) == KIT_909);
    assert(drums_kit_present(KIT_909) && drums_kit_present(KIT_808));
    assert(!drums_kit_present(KIT_606) && !drums_kit_present(KIT_707) && !drums_kit_present(DRUM_MODEL_707_TB));
    for (int pad = 0; pad <= DRUMS_NPADS; pad++) {
        assert(!drums_has_pad(KIT_606, pad) && !drums_has_pad(KIT_707, pad) && !drums_has_pad(DRUM_MODEL_707_CB, pad));
        assert(drums_nparams(KIT_606, pad) == 0 && drums_param(KIT_707, pad, 0) == 0);
    }

    /* the four buses: the mixed kit's sum is the two kits' own */
    drums_init();
    drums_trigger_mapped(KIT_909, map, 0, .75f);
    drums_trigger_mapped(KIT_909, map, 7, .75f);
    render(KIT_909, a);
    drums_init();
    drums_trigger(KIT_909, 0, .75f); render(KIT_909, b);
    drums_init();
    drums_trigger(KIT_808, 7, .75f); render(KIT_808, c);
    assert(energy(b) > .001f && energy(c) > .001f);
    for(int bus=0; bus<4; ++bus)
        for(int i=0; i<128; ++i)
            assert(fabsf(a[bus][i] - (b[bus][i] + c[bus][i])) < 0.000001f);

    /* a cut pad: only its own voice goes */
    drums_init();
    drums_trigger_mapped(KIT_909, map, 0, .8f);
    drums_trigger_mapped(KIT_909, map, 7, .8f);
    render(KIT_909, a); drums_cut_pad(7); render(KIT_909, a);
    drums_init();
    drums_trigger(KIT_909, 0, .8f); render(KIT_909, b); render(KIT_909, b);
    assert(memcmp(a, b, sizeof a) == 0);

    /* the 909's closed hat on an 808 kit chokes the 808's open hat (the 909's hats always choke) */
    memset(map, 0, sizeof map);
    map[7] = KIT_909 + 1;
    drums_init();
    drums_trigger_mapped(KIT_808, map, 8, .8f); render(KIT_808, a);
    assert(energy(a) > .001f);
    drums_trigger_mapped(KIT_808, map, 7, .8f);
    drums_cut_pad(7); render(KIT_808, a);
    assert(energy(a) == 0.0f);

    /* a cut kit makes no sound and takes no knob */
    drums_init();
    drums_trigger(KIT_606, 0, 1.0f);
    drums_trigger(KIT_707, 7, 1.0f);
    drums_trigger(DRUM_MODEL_707_CB, 0, 1.0f);
    drums_set(KIT_606, 0, 0, 100);
    render(KIT_909, a);
    assert(energy(a) == 0.0f && !drums_active(KIT_606) && !drums_active(KIT_707));
    assert(drums_get(KIT_606, 0, 0) == 0);

    puts("Mixed DSP: four-bus sum, pad cut isolation, cross-kit hat choke, the cut kits silent and without pads");
    return 0;
}
