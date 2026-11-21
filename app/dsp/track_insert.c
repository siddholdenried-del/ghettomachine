#include "track_insert.h"
#include <string.h>

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
void track_insert_reset(track_insert_t *s) { memset(s, 0, sizeof *s); }

static float triangle(float p) { return p < 0.5f ? 4.f*p - 1.f : 3.f - 4.f*p; }
static float sat(float x) { return x > 2.f ? 2.f : x < -2.f ? -2.f : x; }

void track_insert_process(track_insert_t *s, const track_insert_param_t *p, float *l, float *r, unsigned n)
{
    const float a = p->v[0] / 127.f, b = p->v[1] / 127.f, c = p->v[2] / 127.f, wet = p->v[3] / 127.f;
    const float dp = (0.08f + 10.f*a*a) / 44100.f;
    unsigned i;
    if (!p->mode || !wet)
        return;
    for (i = 0; i < n; i++) {
        float mod = triangle(s->phase), mod_r = triangle(s->phase < .75f ? s->phase+.25f : s->phase-.75f);
        float in[2] = {l[i],r[i]}, out[2] = {in[0],in[1]};
        unsigned ch;
        s->phase += dp;
        if (s->phase >= 1.f) s->phase -= 1.f;
        if (p->mode == TI_PHASE) {
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
        } else if (p->mode == TI_TREM) {
            const float common = 1.f - b*(1.f-c)*(.5f+.5f*mod);
            out[0] *= common * (1.f - b*c*(.5f+.5f*mod));
            out[1] *= common * (1.f - b*c*(.5f-.5f*mod));
        }
        l[i] = in[0] + wet*(out[0]-in[0]);
        r[i] = in[1] + wet*(out[1]-in[1]);
    }
}
