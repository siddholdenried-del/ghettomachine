/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine: the drum track's three kits (see drums.h). */
#include "drums.h"
#include "drum909.h"
#include "drum808.h"
#include "drum606.h"
#include "drum707.h"

static drum909_t d909;
static drum808_t d808;
static drum606_t d606;
static drum707_t d707;
static x0x_param_t desc606[D6_NUM + 1][D6P_NUM];
static float k8_l[DRUMS_MAX_BLOCK], k8_r[DRUMS_MAX_BLOCK], k8_rev[DRUMS_MAX_BLOCK], k8_dly[DRUMS_MAX_BLOCK];

/* pad -> the engine's own track */
static const uint8_t PAD808[DRUMS_NPADS] = {D8_BD, D8_SD, D8_LT, D8_MT, D8_HT, D8_RS, D8_CP,
                                            D8_CH, D8_OH, D8_CY, D8_CB};
static const char *const NAME909[DRUMS_NPADS + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP",
                                                     "CH", "OH", "CR", "RD", "KIT"};
static const char *const NAME808[DRUMS_NPADS + 1] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP",
                                                     "CH", "OH", "CY", "CB", "KIT"};

static const int8_t PAD606[DRUMS_NPADS] = {D6_BD, D6_SD, D6_LT, -1, D6_HT, -1, D6_CP,
                                           D6_CH, D6_OH, D6_CY, -1};
static const char *const NAME606[DRUMS_NPADS + 1] = {"BD", "SD", "LT", "--", "HT", "--", "CP",
                                                     "CH", "OH", "CY", "--", "KIT"};
static const char *const DIST606[] = {"DIODE", "CLIP", "SAT", "BFZ", "PDIST", "FOLD", "CRUSH"};
static const char *const CHOKE606[] = {"OFF", "CH>OH", "BOTH"};
static const char *const POT606[] = {"LEVEL", "TUNE", "DECY", "DRIVE", "DIST", "REV", "DLY", "", ""};
static const uint8_t COUNT606[D6_NUM + 1] = {8, 9, 7, 7, 7, 7, 7, 8, 2};

const char *drums_kit_name(int kit)
{
    return kit == KIT_707 ? "707" : kit == KIT_606 ? "606" : kit == KIT_808 ? "808" : "909";
}

static int voice(int kit, int pad)
{
    if (pad == DRUMS_PAD_KIT)
        return kit == KIT_707 ? D7_KIT : kit == KIT_606 ? D6_KIT : kit == KIT_808 ? D8_KIT : DR_KIT;
    if (pad < 0 || pad >= DRUMS_NPADS)
        return -1;
    return kit == KIT_606 ? PAD606[pad] : kit == KIT_808 ? PAD808[pad] : pad;
}

void drums_init(void)
{
    drum909_init(&d909);
    drum808_init(&d808);
    drum606_init(&d606);
    drum707_init(&d707);
    for (int v = 0; v <= D6_KIT; v++)
        for (int i = 0; i < COUNT606[v]; i++) {
            const int choke = v == D6_KIT && i == D6K_CHOKE;
            const int dist = v != D6_KIT && i == D6P_DIST;
            const char *name = v == D6_KIT ? (choke ? "CHOKE" : "ACC") :
                (i == D6P_X1 ? (v == D6_BD ? "ATCK" : v == D6_SD ? "SNAP" : "NOISE") :
                 i == D6P_X2 ? "TONE" : POT606[i]);
            desc606[v][i] = (x0x_param_t){name, choke ? 2 : dist ? D6_DIST_TYPES - 1 : 127,
                                         (uint8_t)drum606_default(v, i), choke ? CHOKE606 : dist ? DIST606 : 0};
        }
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
    else if (kit == KIT_606) {
        if (vel > 0.0f) {
            const int raw = (int)(vel * 127.0f + .5f);
            drum606_trigger(&d606, v, raw < 1 ? 1 : raw);
        }
    } else if (kit == KIT_707) {
        if (vel > 0.0f)
            drum707_trigger(&d707, v, vel);
    } else
        drum909_trigger(&d909, v, vel);
}

void drums_cut_pad(int pad)
{
    if (pad < 0 || pad >= DRUMS_NPADS)
        return;
    drum909_cut(&d909, pad);
    drum707_cut(&d707, pad);
    drum808_cut(&d808, PAD808[pad]);
    if (PAD606[pad] >= 0)
        drum606_cut(&d606, PAD606[pad]);
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
    if (kit == KIT_707 || drum707_active(&d707))
        drum707_render_st(&d707, l, r, rev, dly, n);
    if (kit == KIT_606 || drum606_active(&d606)) {
        /* The 606 is mono; use the now-unused 808 scratch buffer for its dry
         * bus. The engine adds into the existing sends, preserving other tails. */
        for (i = 0; i < n; i++)
            k8_l[i] = 0.0f;
        drum606_render(&d606, k8_l, rev, dly, n);
        for (i = 0; i < n; i++) {
            l[i] += k8_l[i];
            r[i] += k8_l[i];
        }
    }
}

int drums_active(int kit) { return kit == KIT_707 ? drum707_active(&d707) : kit == KIT_606 ? drum606_active(&d606) : kit == KIT_808 ? drum808_active(&d808) : drum909_active(&d909); }

int drums_nparams(int kit, int pad)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_707 ? drum707_nparams(v) : kit == KIT_606 ? COUNT606[v] : kit == KIT_808 ? drum808_nparams(v) : drum909_nparams(v);
}

const x0x_param_t *drums_param(int kit, int pad, int i)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_707 ? drum707_param(v, i) : kit == KIT_606 ? (i >= 0 && i < COUNT606[v] ? &desc606[v][i] : 0) : kit == KIT_808 ? drum808_param(v, i) : drum909_param(v, i);
}

int drums_get(int kit, int pad, int i)
{
    int v = voice(kit, pad);
    if (v < 0)
        return 0;
    return kit == KIT_707 ? drum707_get(&d707, v, i) : kit == KIT_606 ? (i >= 0 && i < COUNT606[v] ? drum606_get(&d606, v, i) : 0) : kit == KIT_808 ? drum808_get(&d808, v, i) : drum909_get(&d909, v, i);
}

void drums_set(int kit, int pad, int i, int value)
{
    int v = voice(kit, pad);
    if (v < 0)
        return;
    if (kit == KIT_707)
        drum707_set(&d707, v, i, value);
    else if (kit == KIT_808)
        drum808_set(&d808, v, i, value);
    else if (kit == KIT_606) {
        if (i >= 0 && i < COUNT606[v])
            drum606_set(&d606, v, i, value);
    } else
        drum909_set(&d909, v, i, value);
}

const char *drums_pad_name(int kit, int pad)
{
    if (pad < 0 || pad > DRUMS_NPADS)
        return "";
    return kit == KIT_606 ? NAME606[pad] : kit == KIT_808 ? NAME808[pad] : NAME909[pad];
}

int drums_has_pad(int kit, int pad)
{
    return kit >= 0 && kit < KIT_NUM && pad >= 0 && pad < DRUMS_NPADS &&
           voice(kit, pad) >= 0;
}

int drums_model(int base, const uint8_t *map, int pad)
{
    int requested = map && pad >= 0 && pad < DRUMS_NPADS ? map[pad] - 1 : -1;
    return drums_has_pad(requested, pad) ? requested : base;
}

static void drums_cut_voice(int kit, int pad)
{
    const int v = voice(kit, pad);
    if (v < 0 || pad >= DRUMS_NPADS)
        return;
    switch (kit) {
    case KIT_909: drum909_cut(&d909, v); break;
    case KIT_808: drum808_cut(&d808, v); break;
    case KIT_606: drum606_cut(&d606, v); break;
    case KIT_707: drum707_cut(&d707, v); break;
    }
}

void drums_trigger_mapped(int base, const uint8_t *map, int pad, float vel)
{
    const int model = drums_model(base, map, pad);
    if (vel > 0.0f && (pad == 7 || pad == 8)) {
        const int peer = pad == 7 ? 8 : 7;
        const int other = drums_model(base, map, peer);
        // Compare source engine's choke direction without changing its normal
        // same-engine algorithm or any factory preset's signal path.
        const int mode = model == KIT_606 ? drum606_get(&d606, D6_KIT, D6K_CHOKE)
                       : model == KIT_808 ? d808.choke : model == KIT_909 ? 2 : d707.choke;
        if (other != model && (mode == 2 || (mode == 1 && pad == 7)))
            drums_cut_voice(other, peer);
    }
    drums_trigger(model, pad, vel);
}
