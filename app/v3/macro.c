/* SPDX-License-Identifier: GPL-3.0-only */
#include "macro.h"

static unsigned pot7(unsigned v) { return v > 127u ? 127u : v; }

/* Independent lower/upper travel. The detent is an exact factory snapshot. */
static float curve(unsigned k, float low, float middle, float high)
{
    if (k <= 64u) return low + ((float)k/64.f)*(middle-low);
    return middle + ((float)(k-64u)/63.f)*(high-middle);
}

void v3_macro_patch(BusPatch *p,const unsigned char pot[4])
{
    unsigned comp=pot7(pot[V3_COMP]), tr=pot7(pot[V3_TRANS]), lim=pot7(pot[V3_LIMIT]);
    p->comp_on = comp != 0;
    p->threshold_db = curve(comp,-6.f,-13.f,-17.f);
    p->ratio = curve(comp,1.f,2.5f,5.f);
    float rest=(127.f-(float)comp)*(1.f/127.f);
    float rest3=rest*rest*rest;
    p->attack_ms=.2f+10.8f*rest3*rest3;
    p->release_ms = curve(comp,145.f,72.f,30.f);
    /* Static makeup fitted to the 130 BPM factory kit at eight positions.
       Audio never drives this curve, so rests and hits retain their contrast. */
    if (comp <= 64u) {
        float x=(float)comp*(1.f/64.f);
        p->makeup_db=x*x*(1.f+1.77f*x);
    } else {
        float x=(float)(comp-64u)*(1.f/63.f);
        p->makeup_db=2.77f+x*(4.42f-.96f*x);
    }

    p->transient_on = tr != 64u;
    if (tr <= 64u) {
        float x=(64.f-(float)tr)*(1.f/64.f);
        p->transient_attack=-.78f*x;
        p->transient_sustain=.24f*x;
    } else if (tr <= V3_TRANS_DEFAULT) {
        float x=(float)(tr-64u)/(V3_TRANS_DEFAULT-64.f);
        p->transient_attack=.30f*x;
        p->transient_sustain=-.10f*x;
    } else {
        float x=(float)(tr-V3_TRANS_DEFAULT)/(127.f-V3_TRANS_DEFAULT);
        p->transient_attack=.30f+.20f*x;
        p->transient_sustain=-.10f-.25f*x;
    }

    p->limiter_on = lim != 0;
    p->ceiling_db = curve(lim,-.6f,-5.f,-8.5f);
    p->limiter_release_ms = curve(lim,145.f,90.f,48.f);
}

/* Normalized symmetric soft clip. Its slope is (1-z*z)^2 inside the
   knee and zero on the plateaus; both slope and curvature join at +/-1. */
static inline float soft(float z)
{
    if (z > 1.f) return 8.f/15.f;
    if (z < -1.f) return -8.f/15.f;
    float z2=z*z;
    return z*(1.f+z2*(-2.f/3.f+z2*(1.f/5.f)));
}

static inline float saturate_sample(float s,float drive,float dry,float wet,float bound)
{
    /* The dry branch preserves some attack even at full travel.
       Clamping only that branch also bounds exceptional hot sums. */
    float bounded=s>bound ? bound : s< -bound ? -bound : s;
    return dry*bounded + wet*soft(s*drive);
}

void v3_saturate(float *l,float *r,unsigned n,unsigned knob)
{
    knob=pot7(knob);
    if (!knob || !n) return;
    float u=(float)knob*(1.f/127.f);
    float drive=.53f + 1.02f*u;
    float mix=.85f*u;
    /* Match the wet and dry branches at a fixed 0.36 input amplitude.
       One division per block; the stereo sample loop is polynomial only. */
    float wet=mix*(.36f/soft(.36f*drive));
    float dry=1.f-mix;
    float bound=8.f-5.f*u;
    for (unsigned i=0;i<n;++i) {
        l[i]=saturate_sample(l[i],drive,dry,wet,bound);
        r[i]=saturate_sample(r[i],drive,dry,wet,bound);
    }
}
