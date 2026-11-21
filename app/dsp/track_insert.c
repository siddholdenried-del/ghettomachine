#include "track_insert.h"
#include <stddef.h>

static const char *const names[TI_MODES] = {"OFF", "PHASER", "TREM / PAN", "CHORUS", "FLANGER", "COMB", "ENV FILTER",
                                           "FREQ SHIFT", "MICRO PITCH", "RESONATOR"};
static const char *const labels[TI_MODES][4] = {
    {"", "", "", ""}, {"RATE", "DEPTH", "FEEDBACK", "MIX"}, {"RATE", "DEPTH", "PAN", "MIX"},
    {"RATE", "DEPTH", "WIDTH", "MIX"}, {"RATE", "DEPTH", "FEEDBACK", "MIX"}, {"TUNE", "FEEDBACK", "DAMP", "MIX"},
    {"CUTOFF", "SENSE", "RESONANCE", "MIX"}, {"SHIFT", "FINE", "STEREO", "MIX"},
    {"DETUNE", "DELAY", "WIDTH", "MIX"}, {"TUNE", "DECAY", "COLOR", "MIX"}
};
static const uint8_t defaults[TI_MODES][4] = {
    {0,0,0,0},{32,80,42,65},{38,88,64,110},{37,68,90,52},{30,75,52,61},{48,54,48,64},
    {46,78,50,101},{69,64,64,100},{32,54,94,52},{45,55,54,65}
};

const char *track_insert_name(int mode) { return names[(unsigned)mode < TI_MODES ? mode : TI_OFF]; }
const char *track_insert_label(int mode, int k) { return labels[(unsigned)mode < TI_MODES ? mode : TI_OFF][k & 3]; }
void track_insert_default(track_insert_param_t *p, int mode)
{
    unsigned k;
    p->mode = (uint8_t)((unsigned)mode < TI_MODES ? mode : TI_OFF);
    for (k = 0; k < 4; k++) p->v[k] = defaults[p->mode][k];
}
void track_insert_bind(track_insert_t *s, int16_t l[TI_DELAY], int16_t r[TI_DELAY])
{
    s->ring[0] = l; s->ring[1] = r;
    track_insert_reset(s);
}
void track_insert_reset(track_insert_t *s)
{
    unsigned i;
    for (i = 0; i < offsetof(track_insert_t, ring); i++) ((uint8_t *)s)[i] = 0;
    s->osc_c[0] = s->osc_c[1] = 1.f;
}

static float unit(float p)
{
    if (p >= 1.f) return p-1.f;
    if (p < 0.f) return p+1.f;
    return p;
}
static float triangle(float p) { return p < 0.5f ? 4.f*p - 1.f : 3.f - 4.f*p; }
static float absf(float x) { return x < 0.f ? -x : x; }
static float sat(float x) { return x > 2.f ? 2.f : x < -2.f ? -2.f : x; }

/* Polynomial sine used only for block-rate filter and oscillator coefficients. */
static float sine(float phase)
{
    float x = phase > .5f ? (phase-1.f)*2.f : phase*2.f;
    float y = 4.f*x*(1.f-absf(x));
    return y + .225f*(y*absf(y)-y);
}
static float sine_cycle(float phase) { return sine(unit(phase)); }

#define WRAP(i) ((unsigned)(i) & (TI_DELAY-1))
_Static_assert((TI_DELAY & (TI_DELAY-1)) == 0, "power-of-two delay ring");
static float delay(const track_insert_t *s, unsigned ch, float samples)
{
    unsigned i = (unsigned)samples;
    float f = samples-i;
    float a = i <= s->hist ? s->ring[ch][WRAP(s->pos-i)]*(1.f/16384.f) : 0.f;
    float b = i < s->hist ? s->ring[ch][WRAP(s->pos-i-1)]*(1.f/16384.f) : 0.f;
    return a+(b-a)*f;
}
static int16_t quant(float v)
{
    return v >= 1.999f ? 32751 : v <= -1.999f ? -32751 : (int16_t)(16384.f*v);
}
static void advance(track_insert_t *s)
{
    s->pos = (uint16_t)WRAP(s->pos+1);
    if (s->hist < TI_DELAY-1) s->hist++;
}

/* Hamming-windowed 63-tap Hilbert transformer, 31 samples delay on I.
 * At low frequencies Q amplitude rolls off; above 2 kHz it rejects the
 * mirror well. Odd symmetry costs 16 multiply-accumulates per channel. */
static const float hilbert[16] = {
    0.635117279f, 0.207729893f, 0.119964559f, 0.080853325f, 0.058114101f, 0.042976540f, 0.032090542f, 0.023907136f, 0.017613517f, 0.012740423f, 0.008993817f, 0.006174526f, 0.004136725f, 0.002765274f, 0.001962901f, 0.001642890f
};
static void shift(track_insert_t *s, unsigned ch, float x, float sr, float cr, float *out)
{
    float i = 0.f, q = 0.f, os = s->osc_s[ch], oc = s->osc_c[ch];
    unsigned k;
    s->ring[ch][s->pos] = quant(x);
    if (s->hist >= 62) {
        i = s->ring[ch][WRAP(s->pos-31)]*(1.f/16384.f);
        for (k = 0; k < 16; k++) {
            unsigned d = 2*k+1;
            q += hilbert[k]*(s->ring[ch][WRAP(s->pos-31-d)]-s->ring[ch][WRAP(s->pos-31+d)]);
        }
        q *= 1.f/16384.f;
    }
    *out = i*oc - q*os;
    s->osc_s[ch] = os*cr + oc*sr;
    s->osc_c[ch] = oc*cr - os*sr;
}

static float reson(track_insert_t *s, unsigned ch, unsigned k, float x, float coeff, float decay)
{
    float *z = s->reson[ch][k];
    float y = coeff*z[0]-decay*decay*z[1] + (1.f-decay)*x;
    z[1] = z[0];
    z[0] = sat(y);
    return z[0];
}

void track_insert_process(track_insert_t *s, const track_insert_param_t *p, float *l, float *r, unsigned n)
{
    const float a = p->v[0] / 127.f, b = p->v[1] / 127.f, c = p->v[2] / 127.f, wet = p->v[3] / 127.f;
    const float dp = (0.08f + 10.f*a*a) / 44100.f;
    int mode = p->mode;
    float sr[2] = {0.f, 0.f}, cr[2] = {1.f, 1.f}, rst[2] = {0.f,0.f}, pole = 0.f;
    unsigned i, ch;
    if (mode <= TI_OFF || mode >= TI_MODES || !wet)
        return;
    if (mode == TI_SHIFT) {
        /* Signed coarse ±900Hz, fine ±25Hz, stereo offset ±15Hz. */
        const float coarse = (p->v[0]-64)*(900.f/64.f), fine = (p->v[1]-64)*(25.f/64.f);
        for (ch = 0; ch < 2; ch++) {
            float w = (coarse+fine+(ch ? 1.f : -1.f)*(p->v[2]-64)*(15.f/64.f))*(1.f/44100.f);
            w *= 6.28318530718f;
            sr[ch] = w*(1.f-w*w*(1.f/6.f));
            /* Compute the cosine from the sine so approximation error doesn't grow gain. */
            cr[ch] = 1.f - .5f*sr[ch]*sr[ch] - .125f*sr[ch]*sr[ch]*sr[ch]*sr[ch];
            {
                float norm = 1.5f - .5f*(s->osc_s[ch]*s->osc_s[ch]+s->osc_c[ch]*s->osc_c[ch]);
                s->osc_s[ch] *= norm; s->osc_c[ch] *= norm;
            }
        }
    } else if (mode == TI_RESON) {
        float tune = 75.f+1500.f*a*a, ratio = 1.25f+.75f*c;
        pole = .96f+.0391f*b;
        rst[0] = 2.f*pole*sine_cycle(.25f+tune/44100.f);
        rst[1] = 2.f*pole*sine_cycle(.25f+tune*ratio/44100.f);
    }
    for (i = 0; i < n; i++) {
        float mod = triangle(s->phase), mod_r = triangle(unit(s->phase+.25f));
        float in[2] = {l[i],r[i]}, out[2] = {in[0],in[1]};
        s->phase = unit(s->phase+dp);
        switch (mode) {
        case TI_PHASE:
            for (ch = 0; ch < 2; ch++) {
                const float w = .18f + .155f * b * (ch ? mod_r : mod);
                const float g = (w-1.f)/(w+1.f);
                float x = in[ch] + .68f*c*s->fb[ch];
                unsigned k;
                for (k = 0; k < 4; k++) {
                    float y = g*x + s->ap[ch][k];
                    s->ap[ch][k] = x - g*y;
                    x = y;
                }
                s->fb[ch] = sat(x);
                out[ch] = .5f * (in[ch] + x);
            }
            break;
        case TI_TREM: {
            const float common = 1.f - b*(1.f-c)*(.5f+.5f*mod);
            out[0] *= common * (1.f - b*c*(.5f+.5f*mod));
            out[1] *= common * (1.f - b*c*(.5f-.5f*mod));
            break;
        }
        case TI_CHORUS:
        case TI_FLANGE:
        case TI_COMB:
            for (ch = 0; ch < 2; ch++) {
                float m = ch ? mod_r : mod, d, fb = 0.f, y;
                if (mode == TI_CHORUS)
                    d = 625.f+245.f*b*(mod+(m-mod)*c);
                else if (mode == TI_FLANGE) {
                    d = 130.f+110.f*b*m;
                    fb = .84f*c;
                } else {
                    d = 22.f + 620.f*(1.f-a)*(1.f-a);
                    fb = .92f*b;
                }
                y = delay(s,ch,d);
                if (mode == TI_COMB) {
                    s->lp[ch] += (.7f-.65f*c)*(y-s->lp[ch]);
                    s->ring[ch][s->pos] = quant(in[ch]+fb*s->lp[ch]);
                } else
                    s->ring[ch][s->pos] = quant(in[ch]+fb*y);
                out[ch] = mode == TI_CHORUS ? y : .55f*in[ch]+.65f*y;
            }
            advance(s);
            break;
        case TI_ENV: {
            float amp = .5f*(absf(in[0])+absf(in[1]));
            float cutoff;
            s->env += (amp > s->env ? .018f : .0015f)*(amp-s->env);
            cutoff = .019f + .28f*a*a + 1.8f*b*s->env;
            if (cutoff > .54f) cutoff = .54f;
            for (ch = 0; ch < 2; ch++) {
                s->lp[ch] += cutoff*s->band[ch];
                s->band[ch] += cutoff*(in[ch]-s->lp[ch]-(1.25f-.92f*c)*s->band[ch]);
                out[ch] = s->lp[ch];
            }
            break;
        }
        case TI_SHIFT:
            for (ch = 0; ch < 2; ch++) shift(s,ch,in[ch],sr[ch],cr[ch],&out[ch]);
            advance(s);
            break;
        case TI_DOUBLE: {
            float shift_rate = a*.0095f, sweep = 590.f;
            float base = 114.f+300.f*b;
            for (ch = 0; ch < 2; ch++) {
                float phi = s->rate[ch], phi2 = unit(phi+.5f);
                float w1 = 1.f-absf(2.f*phi-1.f), w2 = 1.f-w1;
                out[ch] = w1*delay(s,ch,base+phi*sweep) + w2*delay(s,ch,base+phi2*sweep);
                s->rate[ch] = unit(phi+(ch ? 1.f : -1.f)*shift_rate/sweep);
                s->ring[ch][s->pos] = quant(in[ch]);
            }
            {
                float mid = .5f*(out[0]+out[1]);
                out[0] = mid+c*(out[0]-mid); out[1] = mid+c*(out[1]-mid);
            }
            advance(s);
            break;
        }
        case TI_RESON:
            for (ch = 0; ch < 2; ch++) {
                float one = reson(s,ch,0,in[ch],rst[0],pole), two = reson(s,ch,1,in[ch],rst[1],pole);
                out[ch] = .44f*one + (.13f+.24f*c)*two;
            }
            break;
        default: break;
        }
        l[i] = in[0] + wet*(out[0]-in[0]);
        r[i] = in[1] + wet*(out[1]-in[1]);
    }
}
