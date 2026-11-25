/* SPDX-License-Identifier: GPL-3.0-only */
#include "shells.h"
#include "kit_math.h"

/* Time constants in milliseconds. All coefficients are computed at a strike.
 * FLOOR is -118 dBFS. The envelope bound, never an individual zero crossing or
 * the load of other voices, determines when a drum rests. */
#define FLOOR 0.00000125f
enum { RESTRIKE = 24 };
static const unsigned body_outputs[] = {0, 3, 4, 5};

static float fall(float ms)
{
    return 1.f - 1.f / (ms * (KIT_RATE * .001f));
}

/* A short contact pulse and damped low-passed noise excite every tom.
 * A fixed seed gives the three membranes the same mallet, independently
 * restarted at their own strike times. The resonators ring on after it. */
enum { MALLET_FRAMES = 128 };
static inline float tom_mallet(BodyVoice *v)
{
    float noise = kit_noise(&v->mallet_noise);
    v->mallet_lp += .62f * (noise - v->mallet_lp);
    float pressure = v->age < 18 ? .24f * (1.f - (float)v->age / 18.f) : 0.f;
    float excitation = v->mallet_level * (v->mallet_lp + pressure);
    v->mallet_level *= .9715f;
    return excitation;
}

static void excite(Membrane *m, float hz, float level, float decay_ms,
                   float fast_hz, float fast_ms, float slow_hz, float slow_ms)
{
    m->phase = 0;
    m->base = (float)kit_increment(hz);
    m->fast = (float)kit_increment(fast_hz);
    m->slow = (float)kit_increment(slow_hz);
    m->fast_rate = fall(fast_ms);
    m->slow_rate = fall(slow_ms);
    m->level = level;
    m->level_rate = fall(decay_ms);
}

static inline float membrane_wave(Membrane *m)
{
    m->phase += (uint32_t)(m->base + m->fast + m->slow);
    /* Stop multiplying sub-audible pitch offsets (one phase unit = 10 μHz). */
    if (m->fast > 1.f) m->fast *= m->fast_rate;
    if (m->slow > 1.f) m->slow *= m->slow_rate;
    return kit_sine(m->phase);
}

static inline float start_sample(float sample, float carry, unsigned age)
{
    if (age >= RESTRIKE) return sample;
    float mix = (float)age * (1.f / RESTRIKE);
    return carry + mix * (sample - carry);
}

static float bound(float x, float lo, float hi)
{
    return x >= lo ? (x <= hi ? x : hi) : lo;
}

/* Interpolate exact equal-temperament semitones once per hit. The maximum
 * endpoint (36 st) starts at eight times the selected settled frequency. */
static float drop_ratio(float semitones)
{
    static const float ratio[13] = {
        1.f, 1.05946309f, 1.12246205f, 1.18920712f, 1.25992105f, 1.33483985f,
        1.41421356f, 1.49830708f, 1.58740105f, 1.68179283f, 1.78179744f, 1.88774863f, 2.f
    };
    unsigned whole = (unsigned)semitones;
    unsigned note = whole % 12u;
    float part = semitones - (float)whole;
    return (float)(1u << (whole / 12u)) * (ratio[note] + part*(ratio[note+1] - ratio[note]));
}

static void kick_hit(BodyVoice *v, struct KickImpact *impact, float velocity, const KickPatch *p)
{
    float f = bound(p->tune_hz, 30.f, 80.f);
    float t60 = bound(p->decay_ms, 100.f, 2400.f);
    float ratio = drop_ratio(bound(p->drop_st, 0.f, 36.f));
    float drop_ms = bound(p->drop_ms, 2.f, 120.f);
    float drive = kit_unit(p->drive);
    /* An accented kick bites: more of its click and a little more body drive, set here once. A soft one loses
       some click. The normal hit is the patch as written. */
    const float hard = kit_hard(velocity), soft = kit_soft(velocity);
    drive = kit_unit(drive + .22f * hard);
    excite(&v->head, f, velocity*.96f/(1.f+.10f*drive), t60*(1.f/6.90775528f),
           f*(ratio-1.f), drop_ms, 0.f, 1.f);
    v->shape = drive;
    v->stick = kit_unit(kit_unit(p->click) * (1.f + .55f * hard - .50f * soft) + .10f * hard);
    v->carry = v->last;
    v->age = 0;
    /* The head is under pressure briefly before free decay begins. */
    impact->hold = (unsigned)(drive * (6.f + .65f*drop_ms) * (KIT_RATE*.001f));
    impact->click_level = v->stick;
    impact->filtered_noise = 0.f;
    impact->pressure = 1.f;
    impact->seed = 0xe3a12877u;
}

static void kick_render(BodyVoice *v, struct KickImpact *impact, float *out, unsigned n)
{
    Membrane *m = &v->head;
    for (unsigned i=0; i<n && m->level>FLOOR; ++i) {
        float x = membrane_wave(m);
        float body = x;
        if (v->shape > 0.f) {
            /* The same phase produces a controlled pair of neighboring
             * harmonics. Membrane pressure gives the front more edge than
             * the ringing tail, with no extra oscillator or lookup. */
            float square = x*x;
            float octave = 2.f*square - 1.f;
            float third = x*(3.f - 4.f*square);
            body += v->shape*((.26f + .23f*impact->pressure)*octave
                            + (.20f + .26f*impact->pressure)*third);
            if (impact->pressure > 1e-6f) impact->pressure *= .99948f; /* 44 ms */
        }
        if (v->stick > 0.f && v->age < 200u) {
            /* A short contact pulse drives filtered noise and a damped
             * synthetic beater resonance. Only the initial 4.5 ms run. */
            impact->filtered_noise += .27f*(kit_noise(&impact->seed) - impact->filtered_noise);
            float knock = kit_sine(v->age * kit_increment(1180.f));
            float edge = .75f*knock + .75f*impact->filtered_noise + (v->age < 8u ? .25f : 0.f);
            body += impact->click_level*edge;
            impact->click_level *= .971f;
        }
        v->last = start_sample(body*m->level, v->carry, v->age);
        out[i] += v->last;
        if (v->age >= impact->hold) m->level *= m->level_rate;
        ++v->age;
    }
    if (m->level <= FLOOR) v->last = 0.f;
}

float kit_tom_pitch_hz(int pad, TomPatch controls)
{
    static const float hz[] = {97.f, 142.f, 204.f};
    if (pad < 4 || pad > 6) return 0.f;
    float tune = kit_unit(controls.tune) - .5f;
    float pitch_scale = 1.f + tune * (tune < 0.f ? .58578644f : .82842712f);
    return hz[pad-4] * (controls.model == 1 ? 1.05f : 1.f) * pitch_scale;
}

static void tom_hit(BodyVoice *v, unsigned tom, float vel, const KitPatch *p)
{
    static const float ring_ms[] = {151.f, 132.f, 115.f};
    int pad = (int)tom + 4;
    TomPatch param = kit_tom_controls(p, pad);
    int hard = param.model == 1;
    float duration = kit_unit(param.decay) - .5f;
    float decay_scale = 1.f + duration * (duration < 0.f ? 1.f : 2.f);
    float f = kit_tom_pitch_hz(pad, param);
    float fast = f * (hard ? .67f : .16f);
    float slow = f * (hard ? .21f : .062f);
    excite(&v->head, f, .435f*vel, ring_ms[tom] * (hard ? .83f : 1.f) * decay_scale,
           fast, hard ? 12.f : 2.4f, slow, hard ? 35.f : 68.f);
    v->overtone_phase = 0;
    v->overtone_inc = (float)kit_increment(f * (hard ? 1.49f : 1.57f));
    v->overtone = hard ? .29f : .15f;
    v->overtone_rate = fall(hard ? 31.f : 19.f);
    v->shape = hard ? .57f : .06f;
    v->stick = kit_unit(param.attack) * 1.45f * (hard ? 1.f : .48f);
    v->mallet_noise = UINT32_C(0x82beade9);
    v->mallet_lp = 0.f;
    v->mallet_level = .76f;
    v->carry = v->last;
    v->age = 0;
}

static void tom_render(BodyVoice *v, float *out, unsigned frames)
{
    Membrane *m = &v->head;
    for (unsigned i = 0; i < frames && m->level > FLOOR; ++i) {
        float fundamental = membrane_wave(m);
        float body = fundamental * (1.f + v->shape * (1.f - fundamental * fundamental));
        /* Let the driven edge settle into a rounder tail. A small residual
         * harmonic stays in the hard voice, including after its stick ends. */
        if (v->shape > .038f) v->shape *= .99965f;
        if (v->overtone > FLOOR) {
            body += v->overtone * kit_sine(v->overtone_phase);
            v->overtone_phase += (uint32_t)v->overtone_inc;
            v->overtone *= v->overtone_rate;
        }
        if (v->age < MALLET_FRAMES && v->stick > 0.f) body += v->stick * tom_mallet(v);
        float y = start_sample(body * m->level, v->carry, v->age);
        v->last = y;
        out[i] += y;
        ++v->age;
        m->level *= m->level_rate;
    }
    if (m->level <= FLOOR) v->last = 0.f;
}

static void snare_hit(SnareVoice *v, float vel, const KitPatch *p)
{
    int hard = p->snare_preset == 1;
    int dry = p->snare_preset == 2;
    float tune = kit_unit(p->snare_tune);
    float f = dry ? (140.f + 124.f*tune)*1.04f : hard ? 104.f + 90.f*tune : 124.f + 104.f*tune;
    float body = kit_unit(p->snare_body);
    float wire = kit_unit(p->snare_wire);
    float snappy = kit_unit(p->snare_snappy);
    /* Decay: the whole drum's length, its body and wires together; 0 leaves both as Body and Wire set them. */
    float dk = p->snare_decay < -1.f ? -1.f : p->snare_decay > 1.f ? 1.f : p->snare_decay;
    float len = dk < 0.f ? 1.f + .75f * dk : 1.f + 2.f * dk;
    /* Round's upper resonator dies early; Hard's paired oscillators ring together. */
    float ring = (dry ? 18.f + 104.f*body*body : hard ? 11.f + 55.f*body*body : 10.f + 66.f*body*body) * len;
    float upper = dry ? 2.32f : hard ? 1.44f : 1.97f;
    float lift = hard ? .47f : dry ? .22f : .035f;
    excite(v->head, f, vel * .375f, ring, f * lift, hard ? 9.f : 5.f, 0.f, 1.f);
    excite(v->head + 1, f * upper, vel * (dry ? .11f + .20f * vel * vel : hard ? .15f : .30f),
           ring * (dry ? .58f : hard ? .84f : .41f), f * upper * lift, 7.f, 0.f, 1.f);
    /* The 909-style body is prominent around the working setting; the upper
     * travel of Snappy retains a full, bright noise burst for live accents. */
    float snappy2 = snappy*snappy;
    float wire_gain = hard ? .07f + 1.75f*snappy2*snappy2*snappy2 : dry ? 1.13f : 1.06f;
    /* A hard snare snaps: more wire, a brighter wire band and more of the head's edge. A soft one is rounder. */
    const float hit_hard = kit_hard(vel), hit_soft = kit_soft(vel);
    wire_gain *= 1.f + .45f * hit_hard - .30f * hit_soft;
    v->wire = vel * snappy * wire_gain;
    /* The Hard pitched component dies before its quieter high-passed wires. */
    v->wire_rate = fall((dry ? 13.f + 146.f*wire*wire : hard ? 20.f + 287.f*wire*wire : 9.f + 100.f*wire*wire) * len);
    /* Two moving averages give the independently decaying snappy band. */
    v->wire_hp = hard ? .145f : dry ? .13f + .065f * vel : .083f;
    v->wire_lp = hard ? .67f : dry ? .32f + .42f * vel : .58f;
    v->wire_lp = bound(v->wire_lp + .14f * hit_hard - .14f * hit_soft, .12f, .93f);
    v->edge = (dry ? .25f + .8f * vel * vel : hard ? .65f : .12f) * (1.f + .60f * hit_hard);
    v->edge_rate = fall(dry ? 17.f : 8.f);
    v->low = v->band = 0.f;
    v->carry = v->last;
    v->age = 0;
}

static inline int snare_active(const SnareVoice *v)
{
    return v->head[0].level > FLOOR || v->head[1].level > FLOOR || v->wire > FLOOR;
}

static void snare_render(SnareVoice *v, float *out, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i) {
        if (!snare_active(v)) {
            v->last = 0.f;
            break;
        }
        float sample = 0.f;
        for (int k = 0; k < 2; ++k) {
            Membrane *m = v->head + k;
            if (m->level > FLOOR) {
                float w = membrane_wave(m);
                sample += m->level * w * (1.f + v->edge * (1.f - w * w));
                m->level *= m->level_rate;
            }
        }
        if (v->wire > FLOOR) {
            float noise = kit_noise(&v->noise);
            v->low += v->wire_hp * (noise - v->low);
            v->band += v->wire_lp * ((noise - v->low) - v->band);
            sample += v->wire * v->band;
            v->wire *= v->wire_rate;
        }
        if (v->edge > FLOOR) v->edge *= v->edge_rate;
        v->last = start_sample(sample, v->carry, v->age);
        out[i] += v->last;
        ++v->age;
    }
}

static void clap_hit(ClapVoice *v, float vel, const KitPatch *p)
{
    int hard = p->clap_preset == 1;
    int dry = p->clap_preset == 2;
    float spread = kit_unit(p->clap_spread);
    float tone = kit_unit(p->clap_tone);
    float tail = kit_unit(p->clap_tail);
    float spacing_ms = dry ? 4.f + spread*24.f : (hard ? 5.1f : 4.6f) + spread*15.f;
    v->cluster = dry ? 0u : hard ? 2u : 1u;
    v->count = 4;
    v->spacing = (unsigned)(spacing_ms * (KIT_RATE * .001f));
    v->age = v->next = v->fired = 0;
    v->velocity = vel;
    v->tooth = 0.f;
    v->tooth_rate = fall(hard ? 2.3f : dry ? 2.f : 2.1f);
    v->wash = 0.f;
    v->wash_rise = 0.f;
    v->wash_rate = fall(dry ? 13.f + 136.f*tail*tail : 10.f + (hard ? 109.f : 81.f)*tail*tail);
    if (dry) {
        v->low_rate = .057f + .12f*tone;
        v->high_rate = .17f + .49f*tone;
    } else {
        /* A single damped bandpass feeds both the hands and their diffuse tail.
         * At this low fraction of Fs, 2*pi*f/Fs is the SVF integrator step. */
        float hz = (hard ? 735.f : 690.f) + (hard ? 1120.f : 940.f)*tone;
        v->low_rate = (6.2831853f / KIT_RATE)*hz;
        v->high_rate = .81f;
    }
    v->wash_gain = dry ? .62f : hard ? .92f : .78f;
    v->low = v->band = v->air = 0.f;
    v->carry = v->last;
}

static inline int clap_active(const ClapVoice *v)
{
    return v->fired != v->count || v->wash > FLOOR || v->tooth > FLOOR;
}

static void clap_render(ClapVoice *v, float *out, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i) {
        if (!clap_active(v)) {
            v->last = 0.f;
            break;
        }
        if (v->fired < v->count && v->age == v->next) {
            ++v->fired;
            v->next += v->cluster && v->fired == 3 ? v->spacing*3u/4u : v->spacing;
            v->tooth = v->velocity * (.78f + .12f * v->fired);
            if (v->fired == v->count) {
                if (v->cluster) v->tooth = v->velocity * .60f;
                v->wash = v->velocity * v->wash_gain;
                v->wash_rise = 1.f;
            }
        }
        float noise = kit_noise(&v->noise);
        if (v->cluster) {
            v->low += v->low_rate * v->band;
            v->band += v->low_rate * (noise - v->low - v->high_rate*v->band);
        } else {
            v->low += v->low_rate * (noise - v->low);
            v->band += v->high_rate * ((noise - v->low) - v->band);
        }
        v->air += (v->cluster == 2u ? .14f : .32f) * (v->band - v->air);
        float wash = v->cluster == 2u ? v->air : .60f * v->band + .40f * v->air;
        float sample = (v->tooth * v->band + v->wash * (1.f - v->wash_rise) *
                       wash) * 1.18f;
        if (v->tooth > FLOOR) v->tooth *= v->tooth_rate;
        if (v->wash > FLOOR) v->wash *= v->wash_rate;
        if (v->wash_rise > FLOOR) v->wash_rise *= .993f;
        v->last = start_sample(sample, v->carry, v->age);
        out[i] += v->last;
        ++v->age;
    }
}

void shells_init(Shells *s)
{
    *s = (Shells){0};
    s->snare.noise = UINT32_C(0x94f34a2b);
    s->clap.noise = UINT32_C(0xa5b36c31);
}

void shells_hit(Shells *s, int pad, float velocity, const KitPatch *p)
{
    float vel = kit_unit(velocity);
    if (vel <= 0.f) return;
    if (pad == 1) kick_hit(s->body, &s->kick_impact, vel, &p->kick);
    else if (pad == 2) snare_hit(&s->snare, vel, p);
    else if (pad == 3) clap_hit(&s->clap, vel, p);
    else if (pad >= 4 && pad <= 6) tom_hit(&s->body[pad - 3], (unsigned)(pad - 4), vel, p);
}

unsigned shells_active_mask(const Shells *s)
{
    unsigned mask = (unsigned)snare_active(&s->snare) << 1 |
                    (unsigned)clap_active(&s->clap) << 2;
    for (unsigned i = 0; i < 4; ++i)
        if (s->body[i].head.level > FLOOR) mask |= 1u << body_outputs[i];
    return mask;
}

void shells_render_routed(Shells *s, float *lanes[6], unsigned frames)
{
    if (s->body[0].head.level > FLOOR) kick_render(&s->body[0], &s->kick_impact, lanes[0], frames);
    for (unsigned i = 1; i < 4; ++i)
        if (s->body[i].head.level > FLOOR) tom_render(s->body + i, lanes[body_outputs[i]], frames);
    snare_render(&s->snare, lanes[1], frames);
    clap_render(&s->clap, lanes[2], frames);
}

void shells_render(Shells *s, float *mono, unsigned frames)
{
    float *lanes[6] = {mono, mono, mono, mono, mono, mono};
    shells_render_routed(s, lanes, frames);
}
