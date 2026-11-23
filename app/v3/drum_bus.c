#include "freestanding.h"
/* SPDX-License-Identifier: GPL-3.0-only */
/* The board's drum909.c defines the shared x0x curve tables. */
#ifndef FM1_EXTERNAL_DRUM_TABLES
#define FM1_EXTERNAL_DRUM_TABLES 1
#endif
#include "drum_bus.h"
#include "kit.h"
#include "macro.h"

BusMix bus_default_mix(void)
{
    static const float reverb[BUS_PADS] = {.10f,.65f,.68f,.38f,.43f,.48f,.16f,.30f,.45f,.46f,.48f};
    static const float delay[BUS_PADS]  = {.06f,.27f,.34f,.22f,.30f,.38f,.12f,.23f,.30f,.56f,.61f};
    BusMix m = {0};
    m.bus.level = 1;
    m.bus.reverb_send = .34f;
    m.bus.delay_send = .22f;
    m.bus.comp_on = m.bus.transient_on = m.bus.limiter_on = 1;
    m.bus.threshold_db = -17.f;
    m.bus.ratio = 2.5f;
    m.bus.attack_ms = 14.f;
    m.bus.release_ms = 115.f;
    m.bus.makeup_db = 3.7f; /* level-matched on the factory electro pattern */
    m.bus.transient_attack = .30f;
    m.bus.transient_sustain = -.10f;
    m.bus.ceiling_db = -5.f;
    m.bus.limiter_release_ms = 90.f;
    /* GhettoMachine's DrumEngine mix: the toms spread low to high, left to right, the hats to the right and the
       cymbal to the left, the two percussion pads apart (BongDrums' 909 Warehouse kit's pans) */
    static const float pan[BUS_PADS] = {0,0,.06f,-.36f,0,.36f,.18f,.22f,-.24f,-.30f,.30f};
    for (unsigned i = 0; i < BUS_PADS; ++i)
        m.pad[i] = (BusPad){1,pan[i],reverb[i],delay[i]};
    return m;
}

void drum_bus_init(DrumBus *b)
{
    V3_CLEAR(b, 0, sizeof *b);
    b->track_gain = 1.f;
}

static void balance(float pan, float level, float *l, float *r)
{
    if (!V3_FINITE(pan)) pan = 0;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    int pot = (int)(64.f + pan * (pan < 0.f ? 64.f : 63.f) + .5f);
    x0x_pan_gains(pot, l, r);
    float gain = kit_unit(level);
    *l *= gain;
    *r *= gain;
}

static float setting(float x, float low, float high, float fallback)
{
    if (!V3_FINITE(x)) return fallback;
    return x < low ? low : x > high ? high : x;
}

/* One-pole 1/e time constant. All ranges are at least 8.8 samples. */
static float slew(float ms)
{
    return 1.f / (1.f + ms * (KIT_RATE * .001f));
}

static float stereo_peak(float l, float r)
{
    float a = V3_ABS(l), b = V3_ABS(r);
    return a > b ? a : b;
}

/* Positive reduction, before make-up or the transient shaper. */
static float reduction_db(float gain)
{
    return gain < 1.f ? -6.02059991f * fm_log2f(gain) : 0.f;
}

/* A stereo-linked feed-forward peak detector and a hard knee. For steady
   inputs above threshold, output dB changes by input dB divided by ratio. */
static float compress(DrumBus *b, const BusPatch *p, float *l, float *r, unsigned n)
{
    float threshold = fm_db2lin(setting(p->threshold_db,-36,0,-17));
    float ratio = setting(p->ratio,1,12,2.5f);
    float slope = 1.f/ratio - 1.f;
    float attack = slew(setting(p->attack_ms,.2f,60,14));
    float release = slew(setting(p->release_ms,20,500,115));
    float makeup = fm_db2lin(setting(p->makeup_db,0,12,0));
    float inv_threshold = 1.f/threshold;
    float env = b->comp_env, lowest = 1.f;
    for (unsigned i=0; i<n; ++i) {
        float peak = stereo_peak(l[i],r[i]);
        env += (peak > env ? attack : release) * (peak-env);
        float gain = 1.f;
        if (env > threshold && ratio > 1.f) {
            gain = fm_exp2f(slope * fm_log2f(env*inv_threshold));
            if (gain < lowest) lowest = gain;
        }
        l[i] *= gain*makeup;
        r[i] *= gain*makeup;
    }
    b->comp_env = env > 1.e-14f ? env : 0.f;
    return lowest;
}

/* A fast envelope opens before a slower envelope on a strike. Their
   relative separation assigns gain to the attack. The remainder shapes
   the body and decay, independent of absolute hit level. */
static void transients(DrumBus *b, const BusPatch *p, float *l, float *r, unsigned n)
{
    float attack = setting(p->transient_attack,-1,1,0);
    float sustain = setting(p->transient_sustain,-1,1,0);
    float attack_gain = attack*(attack < 0.f ? .82f : 1.0f);
    float body_gain = sustain*(sustain < 0.f ? .82f : .85f);
    float fast = b->fast_env, slow = b->slow_env;
    for (unsigned i=0; i<n; ++i) {
        float peak = stereo_peak(l[i],r[i]);
        fast += (peak>fast ? .01244f : .00118f)*(peak-fast);  /* 1.8 / 19 ms */
        slow += (peak>slow ? .000985f : .000206f)*(peak-slow); /* 23 / 110 ms */
        float onset = fast > slow ? (fast-slow)/(fast+1.e-9f) : 0.f;
        float gain = 1.f + body_gain + onset*(attack_gain-body_gain);
        l[i] *= gain;
        r[i] *= gain;
    }
    b->fast_env = fast>1.e-14f ? fast : 0.f;
    b->slow_env = slow>1.e-14f ? slow : 0.f;
}

static float render_block(DrumBus *b, const BusMix *m, float *lanes[BUS_PADS],
                         unsigned offset, float *l, float *r, float *rev, float *dly, unsigned n)
{
    V3_CLEAR(l, 0, n * sizeof *l);
    V3_CLEAR(r, 0, n * sizeof *r);
    V3_CLEAR(rev, 0, n * sizeof *rev);
    V3_CLEAR(dly, 0, n * sizeof *dly);
    const float master_rv = .75f * b->track_gain * kit_unit(m->bus.reverb_send);
    const float master_dl = .75f * b->track_gain * kit_unit(m->bus.delay_send);
    for (unsigned p = 0; p < BUS_PADS; ++p) {
        if (!lanes[p]) continue;
        const BusPad *v = &m->pad[p];
        float level = kit_unit(v->level);
        if (level == 0) continue;
        float lg, rg;
        balance(v->pan,level,&lg,&rg);
        const float *src = lanes[p] + offset;
        float rv = level * master_rv * kit_unit(v->reverb);
        float dl = level * master_dl * kit_unit(v->delay);
        if (rv != 0 || dl != 0) {
            for (unsigned i = 0; i < n; ++i) {
                l[i] += lg*src[i]; r[i] += rg*src[i];
                rev[i] += rv*src[i]; dly[i] += dl*src[i];
            }
        } else {
            for (unsigned i = 0; i < n; ++i) {
                l[i] += lg*src[i]; r[i] += rg*src[i];
            }
        }
    }
    float lg, rg;
    balance(m->bus.pan,m->bus.level,&lg,&rg);
    for (unsigned i = 0; i < n; ++i) {
        l[i] *= .75f*lg*b->track_gain;
        r[i] *= .75f*rg*b->track_gain;
    }
    if (!m->dry_only && b->saturation)
        v3_saturate(l, r, n, b->saturation);
    float lowest = 1.f;
    if (!m->dry_only && m->bus.comp_on) lowest = compress(b,&m->bus,l,r,n);
    else b->comp_env = 0;
    if (!m->dry_only && m->bus.transient_on) transients(b,&m->bus,l,r,n);
    else b->fast_env = b->slow_env = 0;
    return lowest;
}

void drum_bus_render(DrumBus *b, const BusMix *mix, float *lanes[BUS_PADS],
                     float *left, float *right, float *rev, float *dly, unsigned n)
{
    float lowest = 1.f;
    for (unsigned at = 0; at < n;) {
        unsigned part = n-at > BUS_FRAMES ? BUS_FRAMES : n-at;
        float g = render_block(b,mix,lanes,at,left+at,right+at,rev+at,dly+at,part);
        if (g < lowest) lowest = g;
        at += part;
    }
    b->comp_gr_db = reduction_db(lowest);
}

void drum_bus_limit_final(DrumBus *b, const BusMix *m, float *l, float *r, unsigned n)
{
    if (m->dry_only || !m->bus.limiter_on) {
        b->limiter_reduction = 0.f;
        b->limiter_gr_db = 0.f;
        return;
    }
    float ceiling = fm_db2lin(setting(m->bus.ceiling_db,-18,0,-5));
    float release = slew(setting(m->bus.limiter_release_ms,10,400,90));
    /* Store the distance from unity, which decays accurately even at a
       400 ms release. Adding tiny steps to a near-unity gain stalls. */
    float reduction = b->limiter_reduction, lowest = 1.f;
    for (unsigned i=0; i<n; ++i) {
        float peak = stereo_peak(l[i],r[i]);
        reduction *= 1.f-release;
        if (reduction < 1.e-8f) reduction = 0;
        float gain = 1.f-reduction;
        if (peak*gain > ceiling) {
            gain = ceiling/peak;
            reduction = 1.f-gain;
        }
        if (gain < lowest) lowest = gain;
        /* The linked gain enforces the ceiling. Clamp the last rounding
           bit so no digital sample exceeds the configured magnitude. */
        l[i] = fm_clampf(l[i]*gain,-ceiling,ceiling);
        r[i] = fm_clampf(r[i]*gain,-ceiling,ceiling);
    }
    b->limiter_reduction = reduction;
    b->limiter_gr_db = reduction_db(lowest);
}
