/* SPDX-License-Identifier: GPL-3.0-only */
/* From X0X v1.2.2 (Charles Vestal, GPL-3.0); GhettoMachine's changes are in master.h's head. */
/* X0X master: bus compressor (+ PUMP), resonant multimode filter, volume, limiter.
 *
 * Compressor: feed-forward, peak detector, gain computed in dB with a 6 dB soft knee
 * (Giannoulis / Massberg / Reiss's "smooth decoupled" design: the gain reduction itself is
 * smoothed, attack and release independent), makeup gain and a dry / wet mix for parallel
 * compression. At RATIO 1:1 with PUMP at 0 the stage is bypassed sample for sample.
 * PUMP: a 909 / 808 kick ducks the whole mix by its depth, rising in 3 ms and swelling back
 * with the release: the sidechain pump of these styles, keyed by the sequencer rather than by
 * audio, so it needs no kick on the master and has no detector lag.
 *
 * Filter: Zavalishin's TPT state-variable filter (stable at any cutoff), the coefficient
 * glides so a fast sweep does not zipper (ReBirth's PCF idea: sweep the whole mix).
 * Limiter: instant-attack peak follower, ~150 ms release, then a tanh knee above -1 dBFS. */
#define FM_ALWAYS_INLINE                  /* (code in RAM here: fastmath.h, gm_ram.h) */
#include "master.h"
#include "fastmath.h"
#include "gm_ram.h"

#define FS 44100.0f

static const char *const RATIO_N[] = {"1:1", "1.5:1", "2:1", "3:1", "4:1", "6:1", "8:1", "12:1", "20:1", "INF"};
static const float RATIO_V[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 20.0f, 1e6f};
static const char *const SRC_N[] = {"909", "808", "BOTH"};
static const char *const MODE_N[] = {"OFF", "LP", "BP", "HP"};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const x0x_param_t MST_P[MST_NPARAMS] = {
    {"THRESH", 127, 95, 0},           /* -48 .. 0 dB: 95 = -12 dB */
    {"RATIO", 9, 0, RATIO_N},         /* 1:1 = off */
    {"ATTACK", 127, 60, 0},           /* 0.1 .. 100 ms, exponential: 60 = ~2.6 ms */
    {"RELEAS", 127, 60, 0},           /* 10 .. 1500 ms, exponential: 60 = ~110 ms */
    {"MAKEUP", 127, 0, 0},            /* 0 .. 24 dB */
    {"MIX", 127, 127, 0},             /* dry .. wet */
    {"PUMP", 127, 0, 0},              /* 0 .. 24 dB of kick-keyed ducking */
    {"PUMP BY", 2, 0, SRC_N},
    {"FILTER", 3, 0, MODE_N},
    {"CUTOFF", 127, 127, 0},
    {"RESO", 127, 20, 0},
    {"LIMIT", 1, 1, ONOFF_N},
    {"COMP", 127, 0, 0},              /* X0X: one knob over THRESH, RATIO, MAKEUP (0 = off) */
    {"FILTER", 127, 64, 0},           /* X0X: one knob over MODE, CUTOFF (64 = off) */
    {"GAIN", 24, 0, 0},               /* GhettoMachine: 0 .. +12 dB into the limiter, 0.5 dB a step */
    {"CEILING", 60, 3, 0},            /* 0 .. -6 dB, 0.1 dB a step: 3 = -0.3 dB */
    {"RELEASE", 127, 75, 0},          /* 10 .. 1000 ms, exponential: 75 = ~150 ms (X0X's) */
};

int master_nparams(void) { return MST_NPARAMS; }
const x0x_param_t *master_param(int i) { return (i >= 0 && i < MST_NPARAMS) ? &MST_P[i] : 0; }

static float thresh_db(int p) { return -48.0f + 48.0f * (float)p / 127.0f; }
static float attack_ms(int p) { return 0.1f * fm_exp2f((float)p * (9.966f / 127.0f)); }     /* 0.1 .. 100 */
static float release_ms(int p) { return 10.0f * fm_exp2f((float)p * (7.229f / 127.0f)); }   /* 10 .. 1500 */
static float makeup_db(int p) { return 24.0f * (float)p / 127.0f; }
static float pump_db(int p) { return 24.0f * (float)p / 127.0f; }
static float lgain_db(int p) { return 0.5f * (float)p; }
static float ceil_db(int p) { return -0.1f * (float)p; }
static float lrel_ms(int p) { return 10.0f * fm_exp2f((float)p * (6.644f / 127.0f)); }   /* 10 .. 1000 */

/* PUMP's duck rises over this: 12 ms lets the kick's attack through and then ducks. At 3 ms it
 * dropped the whole mix 4 dB inside a cycle of the bass: measured, the fast gain movement at the
 * kick was 6 dB louder than at 10 ms, and it was heard as a click on every kick. */
#ifndef MASTER_PUMP_RISE_MS
#define MASTER_PUMP_RISE_MS 12.0f
#endif

/* one-pole coefficient reaching 1 - 1/e of a step in t milliseconds */
static float coef_ms(float t) { return fm_expf(-1000.0f / (t * FS)); }

static void comp_coefs(master_t *m)
{
    float ratio = RATIO_V[m->pot[MST_RATIO] % 10];
    m->thr_db = thresh_db(m->pot[MST_THRESH]);
    m->slope = 1.0f - 1.0f / ratio;
    m->knee = 6.0f;
    m->a_att = coef_ms(attack_ms(m->pot[MST_ATTACK]));
    m->a_rel = coef_ms(release_ms(m->pot[MST_RELEASE]));
    m->a_det = coef_ms(8.0f);
    m->makeup = fm_db2lin(makeup_db(m->pot[MST_MAKEUP]));
    m->mix = (float)m->pot[MST_MIX] / 127.0f;
    m->pump_db = pump_db(m->pot[MST_PUMP]);
    m->a_pump = coef_ms(MASTER_PUMP_RISE_MS);
    {
        uint8_t was = m->comp_on;
        m->comp_on = m->pot[MST_RATIO] != 0 || m->pot[MST_PUMP] != 0;
        if (m->comp_on && !was) {                        /* switched on: the ramp starts at the makeup */
            m->g_lin = m->makeup;
            m->g_step = 0.0f;
            m->cr = 0;
        }
    }
}

static void filt_coefs(master_t *m)
{
    float fc = 30.0f * fm_exp2f((float)m->pot[MST_CUTOFF] * (9.23f / 127.0f));   /* 30 Hz .. 18 kHz */
    float q = 0.5f + (float)m->pot[MST_RESO] * (11.5f / 127.0f);
    m->g_t = fm_tanf(FM_PI * fm_minf(fc, 19000.0f) / FS);
    m->k = 1.0f / q;
    m->a1 = 1.0f / (1.0f + m->g * (m->g + m->k));       /* the resonance may have changed */
}

/* the limiter: GAIN, CEILING, RELEASE */
static void lim_coefs(master_t *m)
{
    m->lgain = fm_db2lin(lgain_db(m->pot[MST_LGAIN]));
    m->ceil_c = fm_db2lin(ceil_db(m->pot[MST_CEIL]));
    m->lim_thr = m->ceil_c * 0.94406f;                /* -0.5 dB under the ceiling */
    m->knee_k = m->ceil_c * 0.96605f;                 /* -0.3 dB */
    m->env_dec = fm_expf(-1000.0f / (lrel_ms(m->pot[MST_LREL]) * FS));
    m->inv_dec = 1.0f / m->env_dec;
}

void master_bypass(master_t *m, int on) { m->byp = (uint8_t)(on != 0); }

void master_init(master_t *m)
{
    int i;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < (int)sizeof *m; i++)
        z[i] = 0;
    for (i = 0; i < MST_NPARAMS; i++)
        m->pot[i] = MST_P[i].def;
    m->gain = 1.0f;
    m->vol_cur = -1.0f;                               /* unset: the first block takes the volume as it is */
    m->lg_cur = -1.0f;                                /* (and GAIN) */
    comp_coefs(m);
    lim_coefs(m);
    m->g = m->g_t = 0.0f;
    filt_coefs(m);
    m->g = m->g_t;
    m->a1 = 1.0f / (1.0f + m->g * (m->g + m->k));
}

/* COMP x: the threshold falls (-4 .. -32 dB), the ratio climbs (1.5:1 .. 6:1), makeup follows (0.85 of
 * the reduction at the threshold, at most 24 dB); 3/4 is the factory's -24 dB, 4:1, +16 dB */
void master_comp1_pots(int v, uint8_t *thresh, uint8_t *ratio, uint8_t *makeup)
{
    static const float R[6] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};
    float x = (float)v * (1.0f / 127.0f), thr = -4.0f - 28.0f * x, mk;
    int ri = v ? 1 + (int)(x * 4.0f + 0.5f) : 0;
    mk = ri ? fm_minf(-thr * (1.0f - 1.0f / R[ri]) * 0.85f, 24.0f) : 0.0f;
    *thresh = (uint8_t)((thr + 48.0f) * (127.0f / 48.0f) + 0.5f);
    *ratio = (uint8_t)ri;
    *makeup = (uint8_t)(mk * (127.0f / 24.0f) + 0.5f);
}
void master_djf_pots(int v, uint8_t *mode, uint8_t *cutoff)
{
    if (v < 60) {                                     /* low-pass: right of 0, the cutoff falls to the left */
        *mode = 1;
        *cutoff = (uint8_t)(8 + v * 119 / 59);
    } else if (v > 68) {                              /* high-pass: the cutoff rises to the right */
        *mode = 3;
        *cutoff = (uint8_t)((v - 69) * 120 / 58);
    } else {
        *mode = 0;
        *cutoff = 127;
    }
}

void master_set(master_t *m, int i, int v)
{
    if (i < 0 || i >= MST_NPARAMS)
        return;
    if (i == MST_COMP1) {                             /* the macros: their pots, then as those */
        m->pot[i] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        master_comp1_pots(m->pot[i], &m->pot[MST_THRESH], &m->pot[MST_RATIO], &m->pot[MST_MAKEUP]);
        comp_coefs(m);
        return;
    }
    if (i == MST_DJF) {
        m->pot[i] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        master_djf_pots(m->pot[i], &m->pot[MST_MODE], &m->pot[MST_CUTOFF]);
        filt_coefs(m);
        return;
    }
    m->pot[i] = (uint8_t)(v < 0 ? 0 : v > MST_P[i].max ? MST_P[i].max : v);
    if (i <= MST_PUMPSRC)
        comp_coefs(m);
    else if (i >= MST_LGAIN)
        lim_coefs(m);
    else
        filt_coefs(m);
}

int master_get(const master_t *m, int i) { return (i >= 0 && i < MST_NPARAMS) ? m->pot[i] : 0; }

void master_key(master_t *m, int kit, float vel)
{
    int src = m->pot[MST_PUMPSRC];
    if (!m->pot[MST_PUMP] || (src == PUMP_909 && kit != 0) || (src == PUMP_808 && kit != 1))
        return;
    m->pump_tgt = m->pump_db * (0.5f + 0.5f * vel);   /* an accented kick pumps deeper */
}

/* static curve: gain reduction (dB, >= 0) for a level (dB), soft knee */
GM_INLINE float gr_of(const master_t *m, float lv)
{
    float over = lv - m->thr_db, h = 0.5f * m->knee;
    if (over <= -h)
        return 0.0f;
    if (over >= h)
        return m->slope * over;
    over += h;
    return m->slope * over * over / (2.0f * m->knee);
}

void master_process(master_t *m, float *x, int n, float volume)
{
    int i, mode = m->pot[MST_MODE];
    float k = m->k, grmax = 0.0f;
    for (i = 0; i < n; i++) {
        float s = x[i];
        if (m->comp_on) {
            float lv, want, total, wet;
            /* the level: mean square over ~8 ms, x2 so a sine reads its peak. Read sample by sample
             * (|x|), the reduction chased every cycle of a bass note and grabbed each kick within a
             * millisecond: audio-rate gain movement, heard as clicks */
            m->ms = fm_flush(m->a_det * m->ms + (1.0f - m->a_det) * s * s);
            if (m->cr == 0) {                                /* the static curve, every MST_CR samples */
                lv = 2.0f * m->ms;
                m->want = (lv > 1e-12f && m->slope > 0.0f) ? gr_of(m, 3.0103f * fm_log2f(lv)) : 0.0f;
            }
            want = m->want;
            m->gr = want > m->gr ? m->a_att * m->gr + (1.0f - m->a_att) * want
                                 : m->a_rel * m->gr + (1.0f - m->a_rel) * want;
            if (m->pump_tgt > m->pump) {                     /* PUMP: rise to the kick's depth ... */
                m->pump = m->a_pump * m->pump + (1.0f - m->a_pump) * m->pump_tgt;
                if (m->pump_tgt - m->pump < 0.05f)
                    m->pump_tgt = 0.0f;                      /* ... then let go: the release swells back */
            } else {
                m->pump = fm_flush(m->a_rel * m->pump);
            }
            total = m->gr + m->pump;
            if (total > grmax)
                grmax = total;
            if (m->cr == 0) {                                /* the gain, linear, ramped over MST_CR samples */
                m->g_step = (m->makeup * fm_db2lin(-total) - m->g_lin) * (1.0f / MST_CR);
                m->cr = MST_CR;
            }
            m->cr--;
            m->g_lin += m->g_step;
            wet = s * m->g_lin;
            s = s + (wet - s) * m->mix;
        }
        if (mode) {
            float g = m->g, a1;
            if (g != m->g_t) {                               /* the cutoff moving: its coefficient too */
                g = m->g += (m->g_t - m->g) * 0.0625f;
                if (fm_fabsf(m->g_t - g) < 1e-6f * m->g_t)
                    g = m->g = m->g_t;
                m->a1 = 1.0f / (1.0f + g * (g + k));
            }
            a1 = m->a1;
            float v3 = s - m->ic2;
            float v1 = a1 * m->ic1 + g * a1 * v3;
            float v2 = m->ic2 + g * v1;
            m->ic1 = fm_flush(2.0f * v1 - m->ic1);
            m->ic2 = fm_flush(2.0f * v2 - m->ic2);
            s = mode == 1 ? v2 : mode == 2 ? v1 : s - k * v1 - v2;
        }
        s *= volume;
        if (m->pot[MST_LIMIT]) {
            /* look ahead MST_LA samples: a peak is seen when it enters, the gain eases down over the
             * MST_LA samples it takes to come out, and is held for them. (It used to halve the
             * distance every sample: a step in the waveform, a click on every limited kick.) */
            float a = fm_fabsf(s), d;
            if (a >= m->env) {
                m->env = a;
                m->hold = MST_LA;
                /* a divide only for a new peak over the ceiling. The compiler computes the divide
                 * before the test, so it must be safe on its own: a clamp INSIDE the branch is folded
                 * away (a > 0.8 there) and 0.2-beta crashed here on 1 / 0 (firmware/hal/fm1_irq.h). */
                float r = 1.0f / fm_maxf(a, 0.8f);
                m->inv_env = a > 0.8f ? r : 0.0f;
            } else if (m->hold) {
                m->hold--;
            } else {
                m->env *= 0.99985f;
                m->inv_env *= 1.0001500225f;                 /* 1 / 0.99985 */
            }
            {
                float want = m->env > 0.8f ? 0.8f * m->inv_env : 1.0f;
                m->gain += (want - m->gain) * (want < m->gain ? (5.0f / MST_LA) : 0.002f);
            }
            d = m->la[m->la_pos];
            m->la[m->la_pos] = s;
            m->la_pos = (m->la_pos + 1) % MST_LA;
            s = d * m->gain;
            if (s > 0.89f || s < -0.89f)
                s = s > 0.0f ? 0.89f + 0.11f * fm_tanhf((s - 0.89f) * 9.0f)
                             : -0.89f - 0.11f * fm_tanhf((-s - 0.89f) * 9.0f);
        } else {
            s = fm_clampf(s, -1.0f, 1.0f);
        }
        x[i] = s;
    }
    m->gr_view = grmax > m->gr_view ? grmax : m->gr_view * 0.93f;   /* the meter: peak, falling */
}

/* the limiter's last stage: a soft knee over k rounding toward the ceiling c (w = c - k wide; iw = 1 / w, worked out
 * once a block), its slope 1 where it starts (X0X's was over 0.89 toward 1.0: master_process keeps it) */
GM_INLINE float lim_knee(float s, float k, float w, float iw)
{
    if (s > k)
        return k + w * fm_tanhf((s - k) * iw);
    if (s < -k)
        return -k - w * fm_tanhf((-s - k) * iw);
    return s;
}

/* GhettoMachine 0.5 (CPU): master_pre_st, master_post_st and master_volume_st run from RAM (gm_ram.h), with their
 * running state in locals for the block: xl and xr may alias the struct for all the compiler knows, so it loaded and
 * stored every field of it each sample. The same arithmetic, in the same order. */
GM_RAM void master_pre_st(master_t *m, float *xl, float *xr, int n)
{
    int i, mode = m->pot[MST_MODE], comp = m->comp_on && !m->byp;
    float k = m->k, grmax = 0.0f;
    if (!comp && !mode) {
        m->gr_view *= 0.93f;
        return;
    }
    const float a_det = m->a_det, a_att = m->a_att, a_rel = m->a_rel, a_pump = m->a_pump, slope = m->slope;
    const float makeup = m->makeup, mix = m->mix, g_t = m->g_t;
    float ms = m->ms, want = m->want, gr = m->gr, pump = m->pump, pump_tgt = m->pump_tgt;
    float g_lin = m->g_lin, g_step = m->g_step;
    int cr = m->cr;
    float g = m->g, a1 = m->a1, ic1 = m->ic1, ic2 = m->ic2, ic1r = m->ic1r, ic2r = m->ic2r;
    for (i = 0; i < n; i++) {
        float s = xl[i], sr = xr[i];
        if (comp) {
            float lv, total, wet;
            /* the level: mean square over ~8 ms, x2 so a sine reads its peak. Read sample by sample
             * (|x|), the reduction chased every cycle of a bass note and grabbed each kick within a
             * millisecond: audio-rate gain movement, heard as clicks */
            /* both sides' power, each term as master_process forms it, so l == r gives its value */
            ms = fm_flush(a_det * ms + 0.5f * ((1.0f - a_det) * s * s + (1.0f - a_det) * sr * sr));
            if (cr == 0) {                                   /* the static curve, every MST_CR samples */
                lv = 2.0f * ms;
                want = (lv > 1e-12f && slope > 0.0f) ? gr_of(m, 3.0103f * fm_log2f(lv)) : 0.0f;
            }
            gr = want > gr ? a_att * gr + (1.0f - a_att) * want
                           : a_rel * gr + (1.0f - a_rel) * want;
            if (pump_tgt > pump) {                           /* PUMP: rise to the kick's depth ... */
                pump = a_pump * pump + (1.0f - a_pump) * pump_tgt;
                if (pump_tgt - pump < 0.05f)
                    pump_tgt = 0.0f;                         /* ... then let go: the release swells back */
            } else {
                pump = fm_flush(a_rel * pump);
            }
            total = gr + pump;
            if (total > grmax)
                grmax = total;
            if (cr == 0) {                                   /* the gain, linear, ramped over MST_CR samples */
                g_step = (makeup * fm_db2lin(-total) - g_lin) * (1.0f / MST_CR);
                cr = MST_CR;
            }
            cr--;
            g_lin += g_step;
            wet = s * g_lin;
            s = s + (wet - s) * mix;
            wet = sr * g_lin;
            sr = sr + (wet - sr) * mix;
        }
        if (mode) {
            if (g != g_t) {                                  /* the cutoff moving: its coefficient too */
                g += (g_t - g) * 0.0625f;
                if (fm_fabsf(g_t - g) < 1e-6f * g_t)
                    g = g_t;
                a1 = 1.0f / (1.0f + g * (g + k));
            }
            float v3 = s - ic2;
            float v1 = a1 * ic1 + g * a1 * v3;
            float v2 = ic2 + g * v1;
            ic1 = fm_flush(2.0f * v1 - ic1);
            ic2 = fm_flush(2.0f * v2 - ic2);
            s = mode == 1 ? v2 : mode == 2 ? v1 : s - k * v1 - v2;
            v3 = sr - ic2r;
            v1 = a1 * ic1r + g * a1 * v3;
            v2 = ic2r + g * v1;
            ic1r = fm_flush(2.0f * v1 - ic1r);
            ic2r = fm_flush(2.0f * v2 - ic2r);
            sr = mode == 1 ? v2 : mode == 2 ? v1 : sr - k * v1 - v2;
        }
        xl[i] = s;
        xr[i] = sr;
    }
    m->ms = ms;
    m->want = want;
    m->gr = gr;
    m->pump = pump;
    m->pump_tgt = pump_tgt;
    m->g_lin = g_lin;
    m->g_step = g_step;
    m->cr = (uint8_t)cr;
    m->g = g;
    m->a1 = a1;
    m->ic1 = ic1;
    m->ic2 = ic2;
    m->ic1r = ic1r;
    m->ic2r = ic2r;
    m->gr_view = grmax > m->gr_view ? grmax : m->gr_view * 0.93f;   /* the meter: peak, falling */
}

_Static_assert((MST_LA & (MST_LA - 1)) == 0, "MST_LA: a power of two (the look-ahead's ring wraps by a mask)");
GM_RAM void master_post_st(master_t *m, float *xl, float *xr, int n)
{
    int i;
    float lt = m->byp ? 1.0f : m->lgain, lc = m->lg_cur < 0.0f ? lt : m->lg_cur, gmin = 1.0f;
    if (m->pot[MST_LIMIT]) {
        const float thr = m->lim_thr, kk = m->knee_k, kw = m->ceil_c - m->knee_k, kiw = 1.0f / kw;
        const float env_dec = m->env_dec, inv_dec = m->inv_dec;
        float env = m->env, inv_env = m->inv_env, gain = m->gain, *la = m->la, *la_r = m->la_r;
        int hold = m->hold, pos = m->la_pos;
        for (i = 0; i < n; i++) {
            float s = xl[i], sr = xr[i];
            if (lc != lt) {                          /* GAIN glides (~10 ms): no zipper */
                lc += (lt - lc) * 0.0022651f;
                if (fm_fabsf(lt - lc) < 1e-5f)
                    lc = lt;
            }
            s *= lc;
            sr *= lc;
            /* look ahead MST_LA samples: a peak is seen when it enters, the gain eases down over the
             * MST_LA samples it takes to come out, and is held for them. (It used to halve the
             * distance every sample: a step in the waveform, a click on every limited kick.) */
            float a = fm_maxf(fm_fabsf(s), fm_fabsf(sr)), d, dr;
            if (a >= env) {
                env = a;
                hold = MST_LA;
                /* a divide only for a new peak over the threshold. The compiler computes the divide
                 * before the test, so it must be safe on its own: a clamp INSIDE the branch is folded
                 * away (a > thr there) and 0.2-beta crashed here on 1 / 0 (firmware/hal/fm1_irq.h). */
                float r = 1.0f / fm_maxf(a, thr);
                inv_env = a > thr ? r : 0.0f;
            } else if (hold) {
                hold--;
            } else {
                env *= env_dec;
                inv_env *= inv_dec;
            }
            {
                float want = env > thr ? thr * inv_env : 1.0f;
                gain += (want - gain) * (want < gain ? (5.0f / MST_LA) : 0.002f);
            }
            if (gain < gmin)
                gmin = gain;
            d = la[pos];
            dr = la_r[pos];
            la[pos] = s;
            la_r[pos] = sr;
            pos = (pos + 1) & (MST_LA - 1);
            xl[i] = lim_knee(d * gain, kk, kw, kiw);
            xr[i] = lim_knee(dr * gain, kk, kw, kiw);
        }
        m->env = env;
        m->inv_env = inv_env;
        m->gain = gain;
        m->hold = hold;
        m->la_pos = pos;
    } else {
        for (i = 0; i < n; i++) {
            float s = xl[i], sr = xr[i];
            if (lc != lt) {                          /* GAIN glides (~10 ms): no zipper */
                lc += (lt - lc) * 0.0022651f;
                if (fm_fabsf(lt - lc) < 1e-5f)
                    lc = lt;
            }
            xl[i] = fm_clampf(s * lc, -1.0f, 1.0f);
            xr[i] = fm_clampf(sr * lc, -1.0f, 1.0f);
        }
    }
    m->lg_cur = lc;
    {
        float lg = gmin < 0.9999f ? -6.0206f * fm_log2f(gmin) : 0.0f;
        m->lgr_view = lg > m->lgr_view ? lg : m->lgr_view * 0.93f;
    }
}

GM_RAM void master_volume_st(master_t *m, float *xl, float *xr, int n, float volume)
{
    int i;
    float vc = m->vol_cur < 0.0f ? volume : m->vol_cur;
    if (vc == volume && volume == 1.0f)
        return;
    for (i = 0; i < n; i++) {
        if (vc != volume) {                          /* the MASTER knob glides (~10 ms): no zipper */
            vc += (volume - vc) * 0.0022651f;
            if (fm_fabsf(volume - vc) < 1e-5f)
                vc = volume;
        }
        xl[i] *= vc;
        xr[i] *= vc;
    }
    m->vol_cur = vc;
}

void master_process_st(master_t *m, float *xl, float *xr, int n, float volume)
{
    master_pre_st(m, xl, xr, n);
    master_post_st(m, xl, xr, n);
    master_volume_st(m, xl, xr, n, volume);
}

static void put_num(char *b, float v, int decimals, const char *unit)
{
    int neg = v < 0.0f, n = 0, i;
    char t[16];
    uint32_t scale = decimals ? 10u : 1u, u = (uint32_t)((neg ? -v : v) * (float)scale + 0.5f);
    if (decimals) {
        t[n++] = (char)('0' + u % 10u);
        t[n++] = '.';
        u /= 10u;
    }
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    i = 0;
    if (neg)
        b[i++] = '-';
    while (n)
        b[i++] = t[--n];
    while (*unit)
        b[i++] = *unit++;
    b[i] = 0;
}

void master_format(const master_t *m, int i, char *b)
{
    int p = m->pot[i];
    switch (i) {
    case MST_THRESH: put_num(b, thresh_db(p), thresh_db(p) > -10.0f ? 1 : 0, "dB"); return;
    case MST_ATTACK: { float t = attack_ms(p); put_num(b, t, t < 10.0f ? 1 : 0, "ms"); return; }
    case MST_RELEASE: put_num(b, release_ms(p), 0, "ms"); return;
    case MST_LGAIN: b[0] = '+'; put_num(b + (p ? 1 : 0), lgain_db(p), 1, "dB"); return;
    case MST_CEIL: put_num(b, ceil_db(p), 1, "dB"); return;
    case MST_LREL: put_num(b, lrel_ms(p), 0, "ms"); return;
    case MST_MAKEUP: put_num(b, makeup_db(p), 1, "dB"); return;
    case MST_PUMP: put_num(b, pump_db(p), 1, "dB"); return;
    case MST_MIX: put_num(b, (float)p * (100.0f / 127.0f), 0, "%"); return;
    case MST_COMP1:
        if (!p) {
            b[0] = 'O'; b[1] = 'F'; b[2] = 'F'; b[3] = 0;
        } else
            put_num(b, (float)p * (100.0f / 127.0f), 0, "%");
        return;
    case MST_DJF: {                                   /* OFF, LP 40, HP 20: how far each way */
        int a = p < 60 ? (60 - p) * 100 / 60 : p > 68 ? (p - 68) * 100 / 59 : 0;
        b[0] = a ? (p < 60 ? 'L' : 'H') : 'O';
        b[1] = a ? 'P' : 'F';
        b[2] = a ? ' ' : 'F';
        b[3] = 0;
        if (a)
            put_num(b + 3, (float)a, 0, "%");
        return;
    }
    case MST_CUTOFF: {
        float fc = 30.0f * fm_exp2f((float)p * (9.23f / 127.0f));
        if (fc >= 1000.0f)
            put_num(b, fc / 1000.0f, 1, "k");
        else
            put_num(b, fc, 0, "Hz");
        return;
    }
    default: {
        const x0x_param_t *d = &MST_P[i];
        if (d->names) {
            const char *s = d->names[p];
            while ((*b = *s++) != 0)
                b++;
        } else
            put_num(b, (float)p, 0, "");
        return;
    }
    }
}
