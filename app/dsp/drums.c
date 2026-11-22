/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine: the drum track's two kits (see drums.h). */
#include "drums.h"
#include "drum909.h"
#include "drum808.h"

static drum909_t d909;
static drum808_t d808;
static float k8_l[DRUMS_MAX_BLOCK], k8_r[DRUMS_MAX_BLOCK], k8_rev[DRUMS_MAX_BLOCK], k8_dly[DRUMS_MAX_BLOCK];

/* pad -> the engine's own track */
static const uint8_t PAD808[DRUMS_NPADS] = {D8_BD, D8_SD, D8_LT, D8_MT, D8_HT, D8_RS, D8_CP,
                                            D8_CH, D8_OH, D8_CY, D8_CB};
static const char *const NAME909[DRUMS_NPADS + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP",
                                                     "CH", "OH", "CR", "RD", "KIT"};
static const char *const NAME808[DRUMS_NPADS + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP",
                                                     "CH", "OH", "CY", "CB", "KIT"};

int drums_kit_present(int kit) { return kit == KIT_909 || kit == KIT_808; }

const char *drums_kit_name(int kit)
{
    return kit == KIT_707 ? "707" : kit == KIT_606 ? "606" : kit == KIT_808 ? "808" : "909";
}

const char *drums_model_name(int model)
{
    return model == DRUM_MODEL_707_CB ? "707 COWBELL" :
           model == DRUM_MODEL_707_TB ? "707 TAMBOURINE" : drums_kit_name(model);
}

static int voice(int kit, int pad)        /* (-1: no such pad, or a kit that was cut) */
{
    if (!drums_kit_present(kit))
        return -1;
    if (pad == DRUMS_PAD_KIT)
        return kit == KIT_808 ? D8_KIT : DR_KIT;
    if (pad < 0 || pad >= DRUMS_NPADS)
        return -1;
    return kit == KIT_808 ? PAD808[pad] : pad;
}

void drums_init(void)
{
    drum909_init(&d909);
    drum808_init(&d808);
}

void drums_trigger(int kit, int pad, float vel)
{
    int v = voice(kit, pad);
    if (v < 0 || pad == DRUMS_PAD_KIT)
        return;
    if (vel > 1.0f)
        vel = 1.0f;
    if (kit == KIT_808)                  /* the 808's unaccented hit sits at D8_VEL_NORMAL (as X0X maps it) */
        drum808_trigger(&d808, v, vel >= 0.999f ? 1.0f : vel * (D8_VEL_NORMAL / DRUMS_VEL_NORMAL));
    else
        drum909_trigger(&d909, v, vel);
}

void drums_cut_pad(int pad)
{
    if (pad < 0 || pad >= DRUMS_NPADS)
        return;
    drum909_cut(&d909, pad);
    drum808_cut(&d808, PAD808[pad]);
}

void drums_render(int kit, float *l, float *r, float *rev, float *dly, int n)
{
    int i;
    if (n > DRUMS_MAX_BLOCK)
        n = DRUMS_MAX_BLOCK;
    for (i = 0; i < n; i++)
        l[i] = r[i] = rev[i] = dly[i] = 0.0f;
    if (kit == KIT_909 || drum909_active(&d909))
        drum909_render_st(&d909, l, r, rev, dly, n);
    if (kit == KIT_808 || drum808_active(&d808)) {
        for (i = 0; i < n; i++)
            k8_l[i] = k8_r[i] = k8_rev[i] = k8_dly[i] = 0.0f;
        drum808_render_st(&d808, k8_l, k8_r, k8_rev, k8_dly, n);
        for (i = 0; i < n; i++) {
            l[i] += k8_l[i];
            r[i] += k8_r[i];
            rev[i] += k8_rev[i];
            dly[i] += k8_dly[i];
        }
    }
}

int drums_active(int kit) { return kit == KIT_808 ? drum808_active(&d808) : kit == KIT_909 ? drum909_active(&d909) : 0; }

static int governor_pad(int pad)
{
    return pad >= DR_LT && pad < DRUMS_NPADS;   /* never truncate kick or snare */
}

int drums_governor_tail_active(int pad)
{
    if (!governor_pad(pad)) return 0;
    return drum909_governor_tail_active(&d909, pad) ||
           drum808_governor_tail_active(&d808, PAD808[pad]);
}

int drums_governor_release_tail(int pad)
{
    if (!governor_pad(pad)) return 0;
    /* A factory-to-factory change can leave the preceding kit ringing. */
    int a = drum909_governor_release_tail(&d909, pad);
    int b = drum808_governor_release_tail(&d808, PAD808[pad]);
    return a || b;
}

int drums_nparams(int kit, int pad)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_808 ? drum808_nparams(v) : drum909_nparams(v);
}

const x0x_param_t *drums_param(int kit, int pad, int i)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_808 ? drum808_param(v, i) : drum909_param(v, i);
}

int drums_get(int kit, int pad, int i)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_808 ? drum808_get(&d808, v, i) : drum909_get(&d909, v, i);
}

void drums_set(int kit, int pad, int i, int value)
{
    int v = voice(kit, pad);
    if (v < 0)
        return;
    if (kit == KIT_808)
        drum808_set(&d808, v, i, value);
    else
        drum909_set(&d909, v, i, value);
}

const char *drums_pad_name(int kit, int pad)
{
    if (pad < 0 || pad > DRUMS_NPADS)
        return "";
    return kit == KIT_808 ? NAME808[pad] : NAME909[pad];
}

int drums_has_pad(int kit, int pad)
{
    return kit >= 0 && kit < DRUM_MODEL_NUM && pad >= 0 && pad < DRUMS_NPADS &&
           voice(kit, pad) >= 0;
}

int drums_model(int base, const uint8_t *map, int pad)
{
    int requested = map && pad >= 0 && pad < DRUMS_NPADS ? map[pad] - 1 : -1;
    if (!drums_has_pad(requested, pad)) return base;
    return requested;
}

static void drums_cut_voice(int kit, int pad)
{
    const int v = voice(kit, pad);
    if (v < 0 || pad >= DRUMS_NPADS)
        return;
    switch (kit) {
    case KIT_909: drum909_cut(&d909, v); break;
    case KIT_808: drum808_cut(&d808, v); break;
    }
}

void drums_cut_mapped(int base, const uint8_t *map, int pad)
{
    const int model = drums_model(base, map, pad);
    drums_cut_pad(pad);
    if (model >= KIT_NUM) drums_cut_voice(model, pad);
}

void drums_trigger_mapped(int base, const uint8_t *map, int pad, float vel)
{
    const int model = drums_model(base, map, pad);
    if (vel > 0.0f && model < KIT_NUM && (pad == 7 || pad == 8)) {
        const int peer = pad == 7 ? 8 : 7;
        const int other = drums_model(base, map, peer);
        // Compare source engine's choke direction without changing its normal
        // same-engine algorithm or any factory preset's signal path.
        const int mode = model == KIT_808 ? d808.choke : 2;
        if (other < KIT_NUM && other != model && (mode == 2 || (mode == 1 && pad == 7)))
            drums_cut_voice(other, peer);
    }
    drums_trigger(model, pad, vel);
}
