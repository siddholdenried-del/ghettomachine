/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../dsp/drums.h"
#include "../dsp/drum707.h"

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
    map[7] = KIT_707 + 1;
    map[8] = KIT_808 + 1;
    map[3] = KIT_606 + 1; /* unavailable override must not silence 909 MT */
    assert(drums_model(KIT_909, map, 3) == KIT_909);
    assert(drums_model(KIT_909, map, 7) == KIT_707);
    assert(drums_model(KIT_909, map, 0) == KIT_909);
    assert(!drums_has_pad(KIT_606, 3));

    drums_init();
    drums_trigger_mapped(KIT_909, map, 0, .75f);
    drums_trigger_mapped(KIT_909, map, 7, .75f);
    render(KIT_909, a);
    drums_init();
    drums_trigger(KIT_909, 0, .75f); render(KIT_909, b);
    drums_init();
    drums_trigger(KIT_707, 7, .75f); render(KIT_707, c);
    assert(energy(b) > .001f && energy(c) > .001f);
    for(int bus=0; bus<4; ++bus)
        for(int i=0; i<128; ++i)
            assert(fabsf(a[bus][i] - (b[bus][i] + c[bus][i])) < 0.000001f);

    drums_init();
    drums_trigger_mapped(KIT_909, map, 0, .8f);
    drums_trigger_mapped(KIT_909, map, 7, .8f);
    render(KIT_909, a); drums_cut_pad(7); render(KIT_909, a);
    drums_init();
    drums_trigger(KIT_909, 0, .8f); render(KIT_909, b); render(KIT_909, b);
    assert(memcmp(a, b, sizeof a) == 0);

    // 707 closed hat chokes an independently allocated 808 open hat only
    // when the incoming source's kit toggle is enabled.
    drums_init();
    drums_set(KIT_707,DRUMS_PAD_KIT,D7K_CHOKE,1);
    drums_trigger_mapped(KIT_909,map,8,.8f); render(KIT_909,a);
    assert(energy(a) > .001f);
    drums_trigger_mapped(KIT_909,map,7,.8f);
    drums_cut_pad(7); render(KIT_909,a);
    assert(energy(a)==0.0f);
    drums_init();
    drums_set(KIT_707,DRUMS_PAD_KIT,D7K_CHOKE,0);
    drums_trigger_mapped(KIT_909,map,8,.8f); render(KIT_909,a);
    drums_trigger_mapped(KIT_909,map,7,.8f);
    drums_cut_pad(7); render(KIT_909,a);
    assert(energy(a) > .001f);
    /* 707 auxiliaries replace voices, not lanes. Resolve invalid duplicate
     * model bytes without granting two lanes ownership of the same voice. */
    memset(map,0,sizeof map);
    map[0] = DRUM_MODEL_707_CB + 1;
    map[7] = DRUM_MODEL_707_TB + 1;
    map[8] = DRUM_MODEL_707_CB + 1;
    assert(drums_model(KIT_909,map,8)==KIT_909);
    assert(drums_model(KIT_909,map,0)==DRUM_MODEL_707_CB);
    assert(drums_model(KIT_909,map,7)==DRUM_MODEL_707_TB);

    drums_init();
    drums_set(DRUM_MODEL_707_CB,0,D7P_PAN,127);
    drums_set(DRUM_MODEL_707_CB,0,D7P_REV,90);
    drums_set(DRUM_MODEL_707_CB,0,D7P_DLY,70);
    drums_trigger_mapped(KIT_909,map,0,.8f);
    render(KIT_909,a);
    for(int i=0;i<128;i++) assert(a[0][i]==0.f);
    assert(energy(a)>.01f && a[2][20]!=0.f && a[3][20]!=0.f);
    drums_cut_mapped(KIT_909,map,0); render(KIT_909,a);
    assert(energy(a)==0.0f);

    /* Tambourine on a hat's physical lane must not choke the open hat. */
    drums_init();
    drums_trigger_mapped(KIT_909,map,8,.8f); render(KIT_909,a);
    drums_trigger_mapped(KIT_909,map,7,.8f);
    drums_cut_mapped(KIT_909,map,7); render(KIT_909,a);
    assert(energy(a)>.001f);

    drums_init();
    drums_trigger_mapped(KIT_909,map,0,.8f);
    drums_trigger_mapped(KIT_909,map,7,.8f);
    render(KIT_909,a);
    drums_cut_mapped(KIT_909,map,0); render(KIT_909,a);
    drums_init();
    drums_trigger_mapped(KIT_909,map,7,.8f); render(KIT_909,b); render(KIT_909,b);
    assert(memcmp(a,b,sizeof a)==0);
    puts("Mixed DSP: four-bus sum, pad cut isolation, invalid override fallback, cross-model choke toggle");
}
