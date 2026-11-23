/* SPDX-License-Identifier: GPL-3.0-only */
/* BongFX on the FM-1: BongVerb and BongDelay (bongfx.h says what differs from their repositories). */
#define FM_ALWAYS_INLINE
#include "bongfx.h"
#include "fastmath.h"

#define BF_FLOOR 1.0e-6f                  /* -120 dBFS: silence, for the sleep */
#ifndef BF_SLEEP
#define BF_SLEEP 1                        /* (0: never asleep: tests/bongfx_unit.c's parity with the kernels) */
#endif
#define BV_SLEEP 22050u                   /* in and out under it for 0.5 s: the tank has emptied */
#define BD_SLEEP 48000u                   /* ... for 1.09 s: longer than the ring (1.04 s), so it holds nothing more */

static float bf_clip(float f, float lo, float hi) { return f < lo ? lo : f > hi ? hi : f; }
static int bf_finite(float x) { return __builtin_isfinite(x); }

/* ======================================================================= BongVerb (bongverb verb.c, 100717b) === */
/* Motion Plate: an original half-rate mono-send / stereo four-line FDN; slowly moving diffusion reads (the idea after
 * Mutable Instruments Clouds; none of its code, coefficients or topology) */
static float approach(float a, float b, float k) { return a + (b - a) * k; }

static void bv_recalc(bongverb_t *v)
{
    const float mode = v->param[0], d = v->param[1], damp = v->param[3];
    const float rt = (mode > .5f ? .50f : .35f) * fm_powf(mode > .5f ? 24.f : 20.f, d);
    const float cutoff = (mode > .5f ? 4100.f : 5800.f) * fm_powf(.11f, damp);
    const float step = 1.f / v->rate;
    v->mode_target = mode;
    v->delay_target = v->rate * .100f * v->param[2];
    v->damp_target = fm_expf(-6.28318530718f * cutoff * step);
    for (int i = 0; i < BV_LINES; i++) {
        const float g = fm_expf(-6.90775527898f * (float)v->len[i] * step / rt);
        v->gain_target[i] = bf_clip(g, 0.f, .985f);
    }
}

void bongverb_init(bongverb_t *v)
{
    uint8_t *p = (uint8_t *)v;
    for (unsigned i = 0; i < sizeof *v; i++)
        p[i] = 0;
    v->rate = 44100.f * .5f;
    v->coef = 1.f - fm_expf(-1.f / (.013f * v->rate));
    v->motion_phase[0] = 0x11A00000u;
    v->motion_phase[1] = 0x839F0000u;
    v->motion_inc[0] = 582791u;           /* (uint32_t)(2^32 .187 BV_MOTION_BLOCK / 22050), worked out in double */
    v->motion_inc[1] = 844579u;           /* ... .271 */
    v->motion[0] = .92f;
    v->motion[1] = .92f;
    {
        static const float ms[BV_LINES] = {29.6f, 35.1f, 43.4f, 51.7f};
        int total = 0;
        for (int i = 0; i < BV_LINES; i++) {
            v->off[i] = total;
            v->len[i] = ((int)(v->rate * .001f * ms[i])) | 1;
            total += v->len[i];
        }
    }
    v->ap_len[0] = ((int)(v->rate * .0047f)) | 1;
    v->ap_len[1] = ((int)(v->rate * .0093f)) | 1;
    v->ap_off[1] = v->ap_len[0];
    v->pre_len = (int)(v->rate * .123f) + 4;
    v->param[0] = 0.f;
    v->param[1] = .58f;
    v->param[2] = 0.f;
    v->param[3] = .45f;
    bv_recalc(v);
    v->mode = v->mode_target;
    v->delay = v->delay_target;
    v->damp = v->damp_target;
    for (int i = 0; i < BV_LINES; i++)
        v->gain[i] = v->gain_target[i];
}

void bongverb_set(bongverb_t *v, int id, float value)
{
    if (id < 0 || id >= 4 || !bf_finite(value))
        return;
    value = bf_clip(value, 0.f, 1.f);
    if (id == 0)
        value = value >= .5f ? 1.f : 0.f;
    if (v->param[id] == value)
        return;
    v->param[id] = value;
    bv_recalc(v);
}

static inline float bv_pred(const bongverb_t *v, float delay)
{
    const int whole = (int)delay;
    const float frac = delay - (float)whole;
    const int a = v->pre_pos - whole;
    const int i = a < 0 ? a + v->pre_len : a, j = i == 0 ? v->pre_len - 1 : i - 1;
    return v->pre[i] + frac * (v->pre[j] - v->pre[i]);
}

static inline float bv_allpass(bongverb_t *v, int which, float in, float gain)
{
    const int k = v->ap_off[which] + v->ap_pos[which];
    const float old = v->diff[k];
    const float y = old - gain * in;
    v->diff[k] = in + gain * y;
    if (++v->ap_pos[which] >= v->ap_len[which])
        v->ap_pos[which] = 0;
    return y;
}

/* One inexpensive smooth-ish modulation update per 16 internal samples, then linear ramps. The two LFOs do not lock
 * phase; modulation is strictly in the wet tank. The excursion is at most a fraction of 0.1 ms, not audible chorus. */
static inline void bv_step_motion(bongverb_t *v, float mode)
{
    if (!v->motion_clock) {
        const float depth = .37f + .35f * mode;
        for (int j = 0; j < 2; j++) {
            const uint32_t u = v->motion_phase[j] >> 16;
            const float t = (float)u * (1.f / 32768.f) - 1.f;
            const float tri = 1.f - 2.f * fm_fabsf(t);
            const float curved = tri * (1.5f - .5f * tri * tri);
            const float target = .92f + depth * curved;
            v->motion_step[j] = (target - v->motion[j]) * (1.f / BV_MOTION_BLOCK);
            v->motion_phase[j] += v->motion_inc[j];
        }
        v->motion_clock = BV_MOTION_BLOCK;
    }
    --v->motion_clock;
    v->motion[0] += v->motion_step[0];
    v->motion[1] += v->motion_step[1];
}

static inline float bv_tank_read(const bongverb_t *v, int i)
{
    const int j = i >> 1;
    const float offset = v->motion[j];
    const int whole = (int)offset, len = v->len[i];
    int a = v->pos[i] + whole, b;
    if (a >= len)
        a -= len;
    b = a + 1;
    if (b >= len)
        b = 0;
    const float *line = v->ring + v->off[i];
    return approach(line[a], line[b], offset - (float)whole);
}

static inline void bv_core(bongverb_t *v, float in, float *left, float *right)
{
    const float k = v->coef;
    v->mode = approach(v->mode, v->mode_target, k);
    /* Delay movement is intentionally slew limited: switching a control never resets the tail. */
    v->delay += bf_clip((v->delay_target - v->delay) * k, -.65f, .65f);
    v->damp = approach(v->damp, v->damp_target, k);
    const float mode = v->mode;
    bv_step_motion(v, mode);
    v->pre[v->pre_pos] = in;
    const float src = bv_pred(v, v->delay);
    const float early_l = mode * bv_pred(v, v->delay + v->rate * .013f) * .086f;
    const float early_r = mode * bv_pred(v, v->delay + v->rate * .022f) * .076f;
    if (++v->pre_pos >= v->pre_len)
        v->pre_pos = 0;
    const float a = bv_allpass(v, 0, src, .68f - .09f * mode), b = bv_allpass(v, 1, a, -.62f + .13f * mode);
    float f[BV_LINES], d[BV_LINES];
    for (int i = 0; i < BV_LINES; i++) {
        d[i] = (i & 1) ? bv_tank_read(v, i) : v->ring[v->off[i] + v->pos[i]];
        v->gain[i] = approach(v->gain[i], v->gain_target[i], k);
        const float lp = approach(d[i], v->lp[i], v->damp);
        v->lp[i] = fm_fabsf(lp) < 1.e-20f ? 0.f : lp;
        f[i] = v->lp[i] * v->gain[i];
    }
    const float sum = .5f * (f[0] + f[1] + f[2] + f[3]);   /* orthogonal 4x4 Householder reflection */
    const float inj = .33f * b;
    for (int i = 0; i < BV_LINES; i++) {
        v->ring[v->off[i] + v->pos[i]] = f[i] - sum + ((i & 1) ? -inj : inj);
        if (++v->pos[i] >= v->len[i])
            v->pos[i] = 0;
    }
    const float mid = .23f * (d[0] + d[1] + d[2] - d[3]);
    const float side = (.22f - .065f * mode) * (d[0] - d[1] + d[2] + d[3]);
    *left = mid + side + early_l;
    *right = mid - side + early_r;
}

static inline void bv_tick(bongverb_t *v, float in, float *left, float *right)
{
    const float input = bf_finite(in) ? bf_clip(in, -4.f, 4.f) : 0.f;
    if (!v->half) {
        v->input = input;
        *left = .5f * (v->old_l + v->new_l);
        *right = .5f * (v->old_r + v->new_r);
    } else {
        *left = v->new_l;
        *right = v->new_r;
        v->old_l = v->new_l;
        v->old_r = v->new_r;
        bv_core(v, .5f * (input + v->input), &v->new_l, &v->new_r);
    }
    v->half ^= 1;
}

int bongverb_asleep(const bongverb_t *v) { return v->quiet >= BV_SLEEP; }

/* asleep for n samples: what the kernel would have done that is not sound, done (its settings' glides, the motion,
 * the lines' places), so it wakes where the kernel would be; the sound itself would have been silence */
static void bv_skip(bongverb_t *v, int n)
{
    const int m = v->half ? (n + 1) / 2 : n / 2;   /* the core's runs among the n ticks */
    const float k = v->coef;
    int i, s;
    v->half ^= n & 1;
    for (s = 0; s < m; s++) {
        v->mode = approach(v->mode, v->mode_target, k);
        v->delay += bf_clip((v->delay_target - v->delay) * k, -.65f, .65f);
        v->damp = approach(v->damp, v->damp_target, k);
        bv_step_motion(v, v->mode);
        for (i = 0; i < BV_LINES; i++)
            v->gain[i] = approach(v->gain[i], v->gain_target[i], k);
    }
    if (m <= 0)
        return;
    for (i = 0; i < BV_LINES; i++)
        v->pos[i] = (v->pos[i] + m) % v->len[i];
    for (i = 0; i < 2; i++)
        v->ap_pos[i] = (v->ap_pos[i] + m) % v->ap_len[i];
    v->pre_pos = (v->pre_pos + m) % v->pre_len;
}

void bongverb_block(bongverb_t *v, const float *in, float *l, float *r, float gain, int n)
{
    float pk_in = 0.f, pk_out = 0.f;
    int i;
    for (i = 0; i < n; i++)
        pk_in = fm_maxf(pk_in, fm_fabsf(in[i]));
    if (BF_SLEEP && pk_in < BF_FLOOR && v->quiet >= BV_SLEEP) {
        bv_skip(v, n);                    /* asleep: the same silence, unrun */
        return;
    }
    for (i = 0; i < n; i++) {
        float a, b;
        bv_tick(v, in[i], &a, &b);
        pk_out = fm_maxf(pk_out, fm_maxf(fm_fabsf(a), fm_fabsf(b)));
        l[i] += gain * a;
        r[i] += gain * b;
    }
    v->quiet = pk_in < BF_FLOOR && pk_out < BF_FLOOR ? (v->quiet < 0x40000000u ? v->quiet + (uint32_t)n : v->quiet) : 0u;
}

/* ===================================================================== BongDelay (bongdelay delay.c, 017d298) === */
/* Clock Echo: one half-rate 16-bit line; an effective bucket clock shapes the bandwidth (the principle after Jatin
 * Chowdhury's BBDDelay; none of its code or coefficients). Wet only, dual mono */
static float bd_lp(const bongdelay_t *d, float cutoff)
{
    const float w = 6.28318531f * cutoff * d->inv_hz;
    return w / (1.f + w);
}

/* The cubic has unit slope around zero and rolls off at a finite ceiling. */
static inline float bd_soft(float x)
{
    x = bf_clip(x, -1.5f, 1.5f);
    return x * (1.f - (4.f / 27.f) * x * x);
}

static void bd_retune(bongdelay_t *d)
{
    const float seconds = d->lag * d->inv_hz;
    /* Longer times represent slower bucket clocks. Never darken below this floor, and let TONE tune the top of that
     * clock-dependent range. */
    const float capacity = .28f + .72f * (.165f / (seconds + .120f));
    d->alpha = bd_lp(d, (1160.f + 8460.f * d->tone * d->tone) * capacity);
    d->motion = d->hz * (.000025f + (.00037f + .00012f * seconds) * d->drive * d->drive);
    d->input_gain = 1.f + 3.2f * d->drive * d->drive;
    d->loop_gain = 1.f + 2.15f * d->drive * d->drive;
}

void bongdelay_set(bongdelay_t *d, int id, float value)
{
    if (!bf_finite(value))
        return;
    const float x = bf_clip(value, 0.f, 1.f);
    switch (id) {
    case 0: d->lag_to = (.045f + .905f * x * x) * d->hz; if (!d->started) d->lag = d->lag_to; break;
    case 1: d->regen_to = 1.18f * x * x; if (!d->started) d->regen = d->regen_to; break;
    case 2: d->tone_to = x; if (!d->started) d->tone = x; break;
    case 3: d->drive_to = x; if (!d->started) d->drive = x; break;
    }
}

float bongdelay_time_value(float ms)
{
    const float s = bf_clip(ms * .001f, .045f, .950f);
    return fm_sqrtf((s - .045f) * (1.f / .905f));
}

float bongdelay_ms(const bongdelay_t *d) { return d->lag_to * d->inv_hz * 1000.f; }

void bongdelay_init(bongdelay_t *d, int16_t *ring)
{
    uint8_t *p = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++)
        p[i] = 0;
    d->ring = ring;
    for (int i = 0; i < BD_RING; i++)
        ring[i] = 0;
    d->hz = .5f * 44100.f;
    d->inv_hz = 1.f / d->hz;
    d->dc_coef = bd_lp(d, 42.f);
    d->smooth = d->inv_hz / .014f;
    d->lag_coef = d->inv_hz / .035f;
    d->drift = UINT32_C(0x40000000);
    d->drift_delta = (uint32_t)(4294967296.f * .54f * d->inv_hz);
    bongdelay_set(d, 0, .53f);
    bongdelay_set(d, 1, .62f);
    bongdelay_set(d, 2, .60f);
    bongdelay_set(d, 3, .25f);
}

void bongdelay_restart(bongdelay_t *d)
{
    d->at = d->phase = d->control = 0;
    d->prev_in = d->prev_wet = d->wet = 0.f;
    d->in_a = d->in_b = d->read_a = d->read_b = d->dc = 0.f;
    d->drift = UINT32_C(0x40000000);
    d->quiet = BD_SLEEP;                  /* (an empty line: asleep until something comes) */
}

static inline void bd_tick(bongdelay_t *d, float in, float *left, float *right)
{
    if (!bf_finite(in))
        in = 0;
    in = bf_clip(in, -2.f, 2.f);
    d->started = 1;
    if (!d->phase) {
        d->prev_in = in;
        *left = *right = .5f * (d->prev_wet + d->wet);
    } else {
        d->lag += bf_clip((d->lag_to - d->lag) * d->lag_coef, -.86f, .86f);
        d->regen += (d->regen_to - d->regen) * d->smooth;
        d->tone += (d->tone_to - d->tone) * d->smooth;
        d->drive += (d->drive_to - d->drive) * d->smooth;
        if (d->control == 0)
            bd_retune(d);
        d->control = (d->control + 1) & (BD_CONTROL - 1);
        const float u = (float)(d->drift >> 8) * (1.f / 16777216.f);
        const float tri = 1.f - 4.f * fm_fabsf(u - .5f);
        const float wobble = tri * (1.5f - .5f * tri * tri);
        d->drift += d->drift_delta;
        const float effective = bf_clip(d->lag + d->motion * wobble, 1.f, (float)(BD_RING - 2));
        const unsigned whole = (unsigned)effective;
        int a = (int)d->at - (int)whole;
        if (a < 0)
            a += BD_RING;
        const int b = a ? a - 1 : BD_RING - 1;
        const float sample = d->ring[a];
        const float tap = (sample + (effective - (float)whole) * ((float)d->ring[b] - sample)) * (1.f / 32768.f);
        d->read_a += d->alpha * (tap - d->read_a);
        d->read_b += d->alpha * (d->read_a - d->read_b);
        d->dc += d->dc_coef * (d->read_b - d->dc);
        const float heard = bf_clip(d->read_b - d->dc, -1.f, 1.f);
        d->in_a += .52f * (.5f * (d->prev_in + in) - d->in_a);
        d->in_b += .72f * (d->in_a - d->in_b);
        /* Drive increases first-pass signal and softens loud recirculations. Normalizing the loop saturator keeps its
         * small-signal gain at one; REGEN, rather than DRIVE, still decides whether a tail builds. */
        const float feedback = bd_soft(heard * d->loop_gain) / d->loop_gain;
        const float w = bd_soft(bd_soft(d->in_b * d->input_gain) + d->regen * feedback);
        const float pcm = bf_clip(w * 32767.f, -32767.f, 32767.f);
        d->ring[d->at] = (int16_t)(pcm + (pcm < 0 ? -.5f : .5f));
        if (++d->at == BD_RING)
            d->at = 0;
        d->prev_wet = d->wet;
        d->wet = heard;
        *left = *right = d->prev_wet;
    }
    d->phase ^= 1;
}

int bongdelay_asleep(const bongdelay_t *d) { return d->quiet >= BD_SLEEP; }

static void bd_skip(bongdelay_t *d, int n)  /* (as bv_skip: the glides, the control's retuning, the drift, the place) */
{
    const int m = d->phase ? (n + 1) / 2 : n / 2;
    d->phase ^= (unsigned)(n & 1);
    for (int s = 0; s < m; s++) {
        d->lag += bf_clip((d->lag_to - d->lag) * d->lag_coef, -.86f, .86f);
        d->regen += (d->regen_to - d->regen) * d->smooth;
        d->tone += (d->tone_to - d->tone) * d->smooth;
        d->drive += (d->drive_to - d->drive) * d->smooth;
        if (d->control == 0)
            bd_retune(d);
        d->control = (d->control + 1) & (BD_CONTROL - 1);
        d->drift += d->drift_delta;
    }
    if (m > 0)
        d->at = (d->at + (unsigned)m) % BD_RING;
}

void bongdelay_block(bongdelay_t *d, const float *in, float *l, float *r, float gain, int n)
{
    float pk_in = 0.f, pk_out = 0.f;
    int i;
    for (i = 0; i < n; i++)
        pk_in = fm_maxf(pk_in, fm_fabsf(in[i]));
    if (BF_SLEEP && pk_in < BF_FLOOR && d->quiet >= BD_SLEEP) {
        bd_skip(d, n);
        return;
    }
    for (i = 0; i < n; i++) {
        float a, b;
        bd_tick(d, in[i], &a, &b);
        pk_out = fm_maxf(pk_out, fm_fabsf(a));
        l[i] += gain * a;
        r[i] += gain * b;
    }
    d->quiet = pk_in < BF_FLOOR && pk_out < BF_FLOOR ? (d->quiet < 0x40000000u ? d->quiet + (uint32_t)n : d->quiet) : 0u;
}
