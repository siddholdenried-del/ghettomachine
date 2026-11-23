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

    v->mode = p->mode >= 0 && p->mode < 6 ? p->mode : 0;
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
    v->phase = v->mod_phase = 0u;
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
        v->pole1 += k * (signal - .30f*v->pole2 - v->pole1);
        v->pole2 += k * (v->pole1 - v->pole2);
        float sample = v->carry*v->attack + v->amplitude * v->level * (1.0f-v->attack) * v->pole2;
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
