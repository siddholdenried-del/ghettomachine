#include "freestanding.h"
#include "perc.h"
#include "kit_math.h"

/* SY-1-inspired signal paths. All controls are captured at the strike.
   The original operation guide describes Width as the span of a closing
   filter and D as an upward glide set by strike strength. */
void perc_init(PercVoice *voice)
{
    *voice = (PercVoice){0};
    voice->noise = 0x6e5ad2f3u;
}

static float smoothing(float seconds)
{
    return 1.0f - 1.0f / (1.0f + (float)KIT_RATE * seconds);
}

/* Coefficient of a backward-Euler RC pole. All values lie in [0,1). */
static float lowpass_coefficient(float hz)
{
    float k = hz * (6.2831853f / KIT_RATE);
    return k / (1.0f + k);
}

void perc_hit(PercVoice *v, float velocity, const PercPatch *p)
{
    float strength = kit_unit(velocity);
    if (strength == 0.0f) return;
    float tune = kit_unit(p->tune);
    float decay = kit_unit(p->decay);
    float width = kit_unit(p->width);
    float sweep = 2.0f * kit_unit((p->sweep + 1.0f) * .5f) - 1.0f;
    float speed = kit_unit(p->speed);

    v->mode = PERC_MODEL(p->mode) < 9 ? PERC_MODEL(p->mode) : 0;
    float hz = 46.0f + 1120.0f * tune * tune;
    float interval = sweep >= 0.0f ? 1.0f + 2.4f*sweep : 1.0f + .76f*sweep;
    float glide = .009f + .37f * (1.0f-speed) * (1.0f-speed);
    if (v->mode == 3) {
        interval = 1.0f + 2.45f * strength * strength;
        glide = .062f;
    }
    float increment = (float)kit_increment(hz);
    v->pitch_end = increment * interval;
    v->pitch_offset = increment - v->pitch_end;
    v->pitch_decay = smoothing(glide);
    if (v->mode >= 6) {
        /* The electro models (G ZAP, H SYNC, I RING): Sweep is up to four octaves. Down (negative) the voice starts
           that far above Tune and falls onto it, a laser; up, it starts on Tune and climbs. Speed is how fast. */
        float a = sweep < 0.0f ? -sweep : sweep, span = 1.0f + a * (3.0f + a * (4.0f + a * 8.0f));   /* 1 .. 16 */
        if (sweep < 0.0f) {
            v->pitch_end = increment;
            v->pitch_offset = increment * (span - 1.0f);
        } else {
            v->pitch_end = increment * span;
            v->pitch_offset = increment - v->pitch_end;
        }
        v->pitch_decay = smoothing(.004f + .30f * (1.0f-speed) * (1.0f-speed));
    }

    /* A tune-tracking resting corner and a velocity-sensitive, decaying
       excursion. The two poles stay stable throughout the excursion. */
    float cutoff = 210.0f + hz * 2.55f;
    v->cutoff_min = lowpass_coefficient(cutoff);
    v->cutoff_offset = lowpass_coefficient(cutoff + 14500.0f*width*(.4f+.6f*strength))
                     - v->cutoff_min;
    v->cutoff_decay = smoothing(.016f + .082f*decay);
    v->release = smoothing(.008f + .32f*decay*decay);
    v->level = 1.0f;
    v->amplitude = .59f * strength;
    v->attack = 1.0f;
    v->carry = v->last;
    v->pole1 = v->pole2 = 0.0f;
    v->phase = v->mod_phase = v->sync_phase = 0u;
    {   /* RESO and the filter's kind: any of them puts the state-variable filter in. 1/Q from 1.6 down to 0.07; the
           level comes down as the peak goes up, so a ringing filter is no louder than a plain one */
        float reso = (float)PERC_RESO(p->mode) * .001f;
        if (reso > 1.0f) reso = 1.0f;
        v->filter = (reso > 0.0f || PERC_FILTER(p->mode)) ? 1 + PERC_FILTER(p->mode) : 0;
        v->damp = 1.6f - 1.53f * reso * (2.0f - reso);
        v->ic1 = v->ic2 = 0.0f;
        if (v->filter)
            v->amplitude *= 1.0f - .55f * reso;
    }
}

static float triangle(uint32_t phase)
{
    float folded = (float)(phase >> 16) * (1.0f/32768.0f) - 1.0f;
    return 1.0f - 2.0f*V3_ABS(folded);
}

static float source(PercVoice *v, float increment)
{
    /* The modulation branches are genuine instantaneous-frequency FM.
       At the highest Tune and upward sweep, the worst signed step is
       under 0.46 cycles/sample, within the signed 32-bit conversion. */
    switch (v->mode) {
    case 0:
        v->phase += (uint32_t)increment;
        return triangle(v->phase + 0x40000000u);
    case 1:
    case 4: {
        int sheet = v->mode == 4;
        v->mod_phase += (uint32_t)(increment * (sheet ? 2.39f : 1.417f));
        float modulator = kit_sine(v->mod_phase);
        int32_t step = (int32_t)(increment * (1.0f + (sheet ? 3.9f : 2.65f) * modulator));
        v->phase += (uint32_t)step;
        float carrier = kit_sine(v->phase);
        return sheet ? .60f*carrier + .40f*kit_noise(&v->noise) : carrier;
    }
    case 2:
    case 3: {
        int swept = v->mode == 3;
        v->phase += (uint32_t)increment;
        v->mod_phase += (uint32_t)(increment * (swept ? 1.57f : 2.756f));
        float body = kit_sine(v->phase);
        float overtone = kit_sine(v->mod_phase);
        return swept ? .80f*body + .20f*overtone : .64f*body + .36f*overtone;
    }
    case 6: {                             /* G ZAP: a saw with a little sub, for the filter to bite on */
        v->phase += (uint32_t)increment;
        return .78f * ((float)(v->phase >> 8) * (2.0f/16777216.0f) - 1.0f) + .22f * kit_sine(v->phase >> 1);
    }
    case 7: {                             /* H SYNC: a sine at Tune restarts a sine swept above it: the hard-sync tear */
        uint32_t before = v->sync_phase;
        v->sync_phase += (uint32_t)v->pitch_end < (uint32_t)increment ? (uint32_t)v->pitch_end : (uint32_t)increment;
        v->phase += (uint32_t)increment > (uint32_t)v->pitch_end ? (uint32_t)increment : (uint32_t)v->pitch_end;
        if (v->sync_phase < before)
            v->phase = 0u;
        return kit_sine(v->phase);
    }
    case 8: {                             /* I RING: the swept sine times a fixed one, an inharmonic blip */
        v->phase += (uint32_t)increment;
        v->mod_phase += (uint32_t)(v->pitch_end * 1.483f);
        return kit_sine(v->phase) * kit_sine(v->mod_phase);
    }
    default:
        return kit_noise(&v->noise);
    }
}

void perc_render(PercVoice *v, float *mono, unsigned n)
{
    if (!v->level) return;
    for (unsigned i = 0; i < n; ++i) {
        float increment = v->pitch_end + v->pitch_offset;
        if (v->mode != 5)
            v->pitch_offset = V3_ABS(v->pitch_offset) > 1e-6f ? v->pitch_offset*v->pitch_decay : 0.0f;
        float signal = source(v, increment);
        float k = v->cutoff_min + v->cutoff_offset;
        v->cutoff_offset = v->cutoff_offset > 1e-10f ? v->cutoff_offset*v->cutoff_decay : 0.0f;
        float out;
        if (!v->filter) {
            v->pole1 += k * (signal - .30f*v->pole2 - v->pole1);
            v->pole2 += k * (v->pole1 - v->pole2);
            out = v->pole2;
        } else {
            /* A zero-delay state-variable filter at the same swept corner (g from the pole's coefficient: k/(1-k)
               is the corner's angle a sample, half of it the integrators' gain). Stable at any corner and Q. */
            float g = .5f * k / (1.0f - k);
            if (g > 3.0f) g = 3.0f;
            float a1 = 1.0f / (1.0f + g * (g + v->damp)), a2 = g * a1;
            float v3 = signal - v->ic2;
            float bp = a1 * v->ic1 + a2 * v3, lp = v->ic2 + a2 * v->ic1 + g * a2 * v3;
            v->ic1 = 2.0f * bp - v->ic1;
            v->ic2 = 2.0f * lp - v->ic2;
            if (v->ic1 > 4.0f) v->ic1 = 4.0f; else if (v->ic1 < -4.0f) v->ic1 = -4.0f;   /* (a ring held in bounds) */
            out = v->filter == 1 ? lp : v->filter == 2 ? bp * v->damp * 1.4f + bp * .6f : signal - v->damp * bp - lp;
        }
        float sample = v->carry*v->attack + v->amplitude * v->level * (1.0f-v->attack) * out;
        mono[i] += sample;
        v->last = sample;
        v->attack = v->attack > 1e-8f ? v->attack*.955f : 0.0f;
        v->level *= v->release;
        if (v->level < .000008f) {
            v->level = v->last = 0.0f;
            return;
        }
    }
}
