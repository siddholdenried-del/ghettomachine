#include "freestanding.h"
#include "metal.h"
#include "kit_math.h"

enum { CLOSED, OPEN, CRASH, RIDE, RISER, DROP, VOICES };
enum { CHOKE_FRAMES = 160, BELL_BLEND = 64, PIECE = 32 };
/* Envelope retirement below -119 dB, independent of CPU or active voice count. */
static const float quiet = 0.000001f;
static const float tuning[2][6] = {
    {218.3f, 301.7f, 379.1f, 518.9f, 627.1f, 811.3f},
    {331.7f, 453.1f, 591.7f, 751.9f, 1003.1f, 1279.7f}
};
typedef struct { float body, bright, edge, air; } Taps;

void metal_init(Metal *m)
{
    V3_CLEAR(m, 0, sizeof *m);
    for (unsigned p = 0; p < 2; p++) {
        for (unsigned i = 0; i < 6; i++) {
            m->bank[p].step[i] = kit_increment(tuning[p][i]);
            m->bank[p].phase[i] = 0x81282ef3u * (i + 1u);
        }
    }
}

/* Each articulation mixes the existing filter taps independently. Changing
   one pad's brightness leaves the shared oscillator bank and its filters alone. */
static void metal_controls(MetalVoice *v, int lane, MetalPatch param)
{
    static const float rate[VOICES] = {
        .99843738f, .9999035f, .9999496f, .99995465f, .99973326f, .99993331f
    };
    float brightness = 2.f * kit_unit(param.brightness) - 1.f;
    float time = 2.f * kit_unit(param.decay) - 1.f;
    float stretch = 1.f + time * (time < 0.f ? .60f : .50f);
    v->decay_rate = 1.f - (1.f - rate[lane]) / stretch;
    switch (lane) {
    case CLOSED:
        v->body_weight = .91f * (1.f - .72f*brightness);
        v->bright_weight = .80f * (1.f + .75f*brightness);
        v->edge_weight = .14f * (1.f + .75f*brightness);
        break;
    case OPEN:
        v->body_weight = .39f * (1.f - .82f*brightness);
        v->bright_weight = .77f * (1.f + .45f*brightness);
        v->edge_weight = .60f * (1.f + .75f*brightness);
        break;
    case CRASH:
        v->body_weight = .94f * (1.f - .69f*brightness);
        v->bright_weight = .83f * (1.f + .59f*brightness);
        v->edge_weight = .94f * (1.f + .55f*brightness);
        break;
    case RIDE:
        v->body_weight = .45f * (1.f - .82f*brightness);
        v->bright_weight = .57f * (1.f + .59f*brightness);
        v->edge_weight = .57f * (1.f + .75f*brightness);
        break;
    case RISER:
    case DROP:
        /* Brightness moves the sweep ceiling. Riser length follows the
           decay control; Drop's filter and amplitude fall together. */
        v->bright_weight = (lane == RISER ? .82f : .83f) *
                           (1.f + brightness * (brightness < 0.f ? .75f : .15f));
        if (lane == RISER) {
            v->rise_frames = (unsigned)(56448.f * stretch + .5f);
            v->sweep_rate = 1.f / (float)v->rise_frames;
        } else v->sweep_rate = 1.f - (1.f - .99992442f) / stretch;
        break;
    }
}

void metal_hit(Metal *m, int pad, float velocity, const KitPatch *patch)
{
    velocity = kit_unit(velocity);
    if (pad < 7 || pad > 9 || velocity < quiet) return;
    int mode = patch->metal_mode;
    if (mode < 0 || mode > 3) mode = 0;
    int lane = pad == 9 ? CRASH + mode : pad - 7;
    MetalPatch param = kit_metal_controls(patch, pad);
    unsigned p = param.profile == 1;
    MetalBank *b = &m->bank[p];
    if (lane == CLOSED) {
        for (unsigned j = 0; j < 2; j++) {
            MetalVoice *oh = &m->bank[j].voice[OPEN];
            if ((m->bank[j].active & (1u << OPEN)) && !oh->choke)
                oh->choke = CHOKE_FRAMES;
        }
    }
    if (!b->active) {
        /* The six free-running phases advance through skipped time in six
           multiplies. Filters start at rest after the last inaudible tail. */
        uint32_t gap = m->frame - b->next_frame;
        for (unsigned i = 0; i < 6; i++) b->phase[i] += gap * b->step[i];
        b->next_frame = m->frame;
        b->low = b->air_low = b->air_high_low = 0;
        b->mid1 = b->mid2 = b->high1 = b->high2 = 0;
        b->lp_p1 = b->lp_p2 = b->lp_n1 = b->lp_n2 = 0;
    }
    MetalVoice *v = &b->voice[lane];
    int sounding = !!(b->active & (1u << lane));
    if (!sounding) V3_CLEAR(v, 0, sizeof *v);
    metal_controls(v, lane, param);
    if (!sounding) v->cutoff = lane == DROP ? .852f + (v->bright_weight - .83f) : .023f;
    b->active |= 1u << lane;
    v->level = v->accent = velocity;
    v->age = v->choke = 0;
    v->movement = 1.0f;
    v->dark = 1.0f;                       /* (bright again at the strike; the ring's low-pass keeps its state) */
    if (lane == RIDE) {
        /* Re-striking does not abruptly replace the ringing bell. The two
           copies have the same poles: collapse an unfinished crossfade into
           one old resonator before spending 64 frames on the next strike. */
        if (sounding) {
            float old = (float)v->bell_blend * (1.0f / BELL_BLEND);
            v->old_ring1 = v->ring1 + old * (v->old_ring1 - v->ring1);
            v->old_ring2 = v->ring2 + old * (v->old_ring2 - v->ring2);
            v->bell_blend = BELL_BLEND;
        }
        v->ring1 = 0;
        v->ring2 = -velocity * (p ? .35364f : .29230f);
    }
}

/* Stateless, seekable air excitation. Skipping an idle bank or subdividing a
   block leaves the subsequent air sample at the same point in the sequence. */
static float air_at(uint32_t frame, unsigned profile)
{
    uint32_t x = (frame ^ (profile * 0x632be5abu)) * 0x9e3779b9u;
    x = (x ^ (x >> 15)) * 0x85ebca6bu;
    return (float)((x ^ (x >> 13)) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/* Finite slopes replace v1's binary edges. The source is continuous through
   wrap and each polarity transition occupies one sixth of the cycle. Pair
   products supply sum/difference partials without hard logical switches. */
static inline float pulse_mix(const uint32_t phase[6])
{
    float pulse[6];
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t q = phase[i] + 0x40000000u;
        uint32_t fold = q ^ (0u - (q >> 31));
        /* Clamp the Q23 triangle before its only floating conversion. Its
           24-bit phase index converts to float exactly; scaling by three
           then converting rounds identically to the original ramp. */
        uint32_t tri = 3u * (fold >> 7);
        if (tri < 16777216u) tri = 16777216u;
        if (tri > 33554432u) tri = 33554432u;
        pulse[i] = (float)tri * (1.0f / 8388608.0f) - 3.0f;
    }
    return .075f*(pulse[0]+pulse[1]+pulse[2]+pulse[3]+pulse[4]+pulse[5])
         + .26f*pulse[0]*pulse[5] - .21f*pulse[1]*pulse[3] + .22f*pulse[2]*pulse[4];
}

/* (0.5) The source band-limited: the pulses (their slopes' and their products' partials past 16 kHz, some folded
   back from past Nyquist) and the noise (white to 22 kHz: the hats' fizz) through a 2-pole low-pass, Butterworth at
   13 kHz by the bilinear transform: flat to 10 kHz, -9 dB at 16, -28 at 20. Before the resonators and the bands, so
   the six voices share it; their mixes of the taps are unchanged. */
#define LP_B0 .380423011f
#define LP_A1 .330721804f
#define LP_A2 .190970239f
static inline float lp13k(float x, float *s1, float *s2)
{
    float y = LP_B0 * x + *s1;
    *s1 = 2.0f * LP_B0 * x - LP_A1 * y + *s2;
    *s2 = LP_B0 * x - LP_A2 * y;
    return y;
}

static void source(MetalBank *b, unsigned p, Taps *tap, unsigned count)
{
    /* Two broad resonances and two air bands are shared before envelopes.
       The higher profile raises both plate resonances along with the pulses. */
    const float mid0 = p ? .17963914f : .15875684f;
    const float mid1 = p ? 1.27911605f : 1.39551425f;
    const float mid2 = p ? -.64072173f : -.68248631f;
    const float top0 = p ? .28765588f : .27577891f;
    const float top1 = p ? .52804666f : .69905788f;
    const float top2 = p ? -.42468825f : -.44844218f;
    for (unsigned n = 0; n < count; n++) {
        for (unsigned i = 0; i < 6; i++) b->phase[i] += b->step[i];
        tap[n].air = pulse_mix(b->phase);
    }
    for (unsigned n = 0; n < count; n++) {
        float pulse = lp13k(tap[n].air, &b->lp_p1, &b->lp_p2);
        float noise = lp13k(air_at(b->next_frame++, p), &b->lp_n1, &b->lp_n2);
        b->low += .19f * (pulse - b->low);
        float exciter = .77f*(pulse-b->low) + .69f*noise;
        b->air_low += .38f*(noise-b->air_low);
        float hiss = noise-b->air_low;
        b->air_high_low += .51f*(hiss-b->air_high_low);
        float edge = hiss-b->air_high_low;
        float mid = mid0*exciter + b->mid1;
        b->mid1 = mid1*mid + b->mid2;
        b->mid2 = mid2*mid - mid0*exciter;
        float top = top0*exciter + b->high1;
        b->high1 = top1*top + b->high2;
        b->high2 = top2*top - top0*exciter;
        tap[n].body = .97f*mid + .35f*top;
        tap[n].bright = .81f*top + .25f*hiss;
        tap[n].edge = .95f*edge + .14f*top;
        tap[n].air = .24f*pulse + .76f*noise;
    }
#ifdef METAL_TEST_COUNTERS
    b->rendered_frames += count;
#endif
}

/* (0.5) A ringing voice's darkening: a one-pole low-pass on it, open at the strike (no filtering), closing as dark
   falls toward its floor (the coefficient: 0.62 about 7 kHz, 0.70 about 8.5 kHz). Highs die first, as on a real
   cymbal; the strike itself keeps its edge. */
#define DARK_HAT .70f
#define DARK_CRASH .62f
#define DARK_RIDE .70f

static int render_hat(MetalVoice *v, int lane, const Taps *tap, float *out, unsigned n)
{
    const float settle = lane == CLOSED ? .9983816f : .9996977f; /* contact or open sizzle */
    for (unsigned i = 0; i < n; i++) {
        v->gain += (lane == CLOSED ? .095f : .012f) * (v->level - v->gain);
        v->level *= v->decay_rate;
        float g = v->gain;
        if (v->choke) g *= (float)v->choke * (1.0f / CHOKE_FRAMES);
        float timbre = lane == CLOSED
            ? v->body_weight*tap[i].bright + (v->edge_weight+v->bright_weight*v->movement)*tap[i].edge
            : v->body_weight*tap[i].body + v->bright_weight*tap[i].bright + v->edge_weight*v->movement*tap[i].edge;
        float s = g * timbre * (lane == CLOSED ? .51f : .61f);
        if (lane == OPEN) {               /* (0.5) the open hat's ring darkens: highs first, as a real one's */
            v->tone += (DARK_HAT + (1.0f - DARK_HAT) * v->dark) * (s - v->tone);
            v->dark *= .99993521f;        /* (0.35 s) */
            s = v->tone;
        }
        out[i] += s;
        if (v->movement > 1e-8f) v->movement *= settle;
        if ((v->choke && --v->choke == 0) || v->gain < quiet) return 0;
    }
    return 1;
}

static int render_cymbal(MetalVoice *v, int lane, unsigned p, const Taps *tap, float *out, unsigned n)
{
    const float bell_a = p ? 1.87043584f : 1.91231757f;
    /* Parallel plate mode: unity-peak bandpass, Q=3.5, 7.10/7.60 kHz.
       The state belongs to Crash only. Its original wash carries through. */
    const float plate_b = p ? .11204494f : .10801554f;
    const float plate_a1 = p ? .83262206f : .94643701f;
    const float plate_a2 = p ? -.77591013f : -.78396891f;
    for (unsigned i = 0; i < n; i++) {
        if (lane == CRASH) {
            float wash = .78f*v->gain*(v->body_weight*tap[i].body + (.19f+v->bright_weight*v->movement)*tap[i].bright)
                       + v->edge_weight*v->accent_gain*tap[i].edge;
            float drive = plate_b * wash;
            float plate = drive + v->filter1;
            v->filter1 = plate_a1 * plate + v->filter2;
            v->filter2 = plate_a2 * plate - drive;
            v->tone += (DARK_CRASH + (1.0f - DARK_CRASH) * v->dark) * (.789f*(wash + (.56f + .63f*v->movement)*plate) - v->tone);
            v->dark *= .99998110f;        /* (1.2 s) */
            out[i] += v->tone;
        } else {
            float bell = bell_a * v->ring1 - .9996512f * v->ring2;
            v->ring2 = v->ring1;
            v->ring1 = bell;
            /* The stick resonator is below -460 dB at seven seconds. An
               extended ride can ring for another three seconds in the wash. */
            if (++v->age == 7u*KIT_RATE) v->ring1 = v->ring2 = 0.f;
            if (v->bell_blend) {
                float old = bell_a * v->old_ring1 - .9996512f * v->old_ring2;
                v->old_ring2 = v->old_ring1;
                v->old_ring1 = old;
                bell += (old - bell) * ((float)v->bell_blend * (1.0f / BELL_BLEND));
                v->bell_blend--;
            }
            v->tone += (DARK_RIDE + (1.0f - DARK_RIDE) * v->dark)
                     * (v->gain*(v->body_weight*tap[i].body + v->bright_weight*tap[i].bright)
                        + .165f*bell + .84f*v->accent_gain*(v->edge_weight*tap[i].edge + .48f*tap[i].body) - v->tone);
            v->dark *= .99998866f;        /* (2 s) */
            out[i] += v->tone;
        }
        v->gain += (lane == CRASH ? .0029f : .022f) * (v->level-v->gain);
        if (lane == CRASH && v->movement>1e-8f) v->movement *= .9998740f;
        v->level *= v->decay_rate;
        if (v->accent > 1e-8f) {
            v->accent_gain += .08f * (v->accent - v->accent_gain);
            v->accent *= lane == CRASH ? .9994333f : .9990556f; /* 40/24 ms */
        } else {
            v->accent = v->accent_gain = 0;
        }
        if (v->gain < quiet) return 0;
    }
    return 1;
}

static int render_fx(MetalVoice *v, int lane, const Taps *tap, float *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        float g, cutoff;
        if (lane == RISER) {
            float pos = 1.0f;
            if (v->age < v->rise_frames) pos = (float)++v->age * v->sweep_rate;
            else v->level *= v->decay_rate;
            float curve = pos * pos;
            g = v->level * (.006f + .994f * curve);
            cutoff = .023f + v->bright_weight * curve;
        } else {
            v->level *= v->decay_rate;
            v->movement *= v->sweep_rate;
            g = v->level;
            cutoff = .022f + v->bright_weight * v->movement;
        }
        /* Smoothing carries the previous level and cutoff through a retrigger,
           including a second hit at the crest of a nearly completed riser. */
        v->filter1 += v->cutoff * (tap[i].air - v->filter1);
        v->filter2 += v->cutoff * (v->filter1 - v->filter2);
        out[i] += .96f * v->gain * v->filter2;
        v->gain += .055f * (g - v->gain);
        v->cutoff += .055f * (cutoff - v->cutoff);
        if (v->level < quiet) return 0;
    }
    return 1;
}

void metal_render_routed(Metal *m, float *lanes[3], unsigned n)
{
    for (unsigned p = 0; p < 2; p++) {
        MetalBank *b = &m->bank[p];
        for (unsigned off = 0; off < n && b->active; off += PIECE) {
            unsigned len = n - off < PIECE ? n - off : PIECE;
            Taps tap[PIECE];
            source(b, p, tap, len);
            for (int lane = 0; lane < VOICES; lane++) {
                if (!(b->active & (1u << lane))) continue;
                MetalVoice *v = &b->voice[lane];
                float *lane_out = lanes[lane <= OPEN ? lane : 2] + off;
                int alive;
                if (lane <= OPEN) alive = render_hat(v, lane, tap, lane_out, len);
                else if (lane <= RIDE) alive = render_cymbal(v, lane, p, tap, lane_out, len);
                else alive = render_fx(v, lane, tap, lane_out, len);
                if (!alive) b->active &= ~(1u << lane);
            }
        }
    }
    m->frame += n;
}

void metal_render(Metal *m, float *mono, unsigned n)
{
    float *lanes[3] = {mono, mono, mono};
    metal_render_routed(m, lanes, n);
}
