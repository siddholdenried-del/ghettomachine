/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Changed for GhettoMachine (19 Nov 2026), in analog_osc (WAVE 0..4) only: OSC 2 also moves by the host's P_INTV semitones
 * (-12..12) and takes the unison fine tune (m->fine) as OSC 1 does; and a sub oscillator, a square an octave below
 * OSC 1 at the host's P_SUB level (0..127), joins the mix before the drive and the filter (its phase in ph[2]).
 * Changed again (20 Nov 2026, the synth update): analog_render_r (WAVE 0..4 with OSC 2's own wave, FM, RING, the
 * SHAPER or the matrix's OSC 2 pitch, P_INTV now -24..24) and the SHAPER in analog_render_x (SYNC, SUB; P_O2 on SYNC's
 * OSC 2 too). With none of them the loops are as they were. And (21 Nov 2026, 0.5) the helpers FE_INLINE and the render
 * loops FE_RAM (dsp.c): run from RAM on GhettoMachine's FM-1; and analog_osc leaves OSC 2 out of its loop while MIX is
 * at 0 (the same samples: it was multiplied by nothing). */
/* ANALOG: two band-limited oscillators (saw / square / tri / sine / PWM),
 * noise, drive and a trapezoidal state-variable filter: low-pass, or (1.5, TYPE) band-pass / high-pass.
 * WAVE SYNC and SUB (#104, appended: WAVE 0..4 play as they always did) are two more oscillator pairs (analog_osc_x):
 *   SYNC  OSC 2 a saw hard-synced to OSC 1 (a saw): it restarts with OSC 1's every cycle, band-limited at the restart
 *         (polyBLEP scaled by the step the restart makes). DTN (shown SYNC) sets OSC 2 above OSC 1, 3/16 semitone a
 *         step (127: +23.8 semitones), and the SHP routings sweep it (ENV DEST / LFO DEST SHP, the matrix's SHP: 3/16
 *         semitone a step of shape, as PWM takes them for its width): the sync sweep. MIX between the two as for
 *         every pair (OSC 2 alone at 127)
 *   SUB   OSC 1 a saw, OSC 2 a square an octave below it (detuned by DTN in cents as before): MIX (shown SUB) is the
 *         sub oscillator's level */
static const char *const N_ANALOG_WAVE[] = {"SAW", "SQR", "TRI", "SIN", "PWM", "SYNC", "SUB"};
enum { AW_SYNC = 5, AW_SUB = 6 };
static const param_desc_t ANALOG_SYNC = {"SYNC", F_INT, 0, 127, 10, 0, 0};   /* DTN's range and default */
static const param_desc_t ANALOG_SUB = {"SUB", F_PCT, 0, 127, 64, 0, 0};      /* MIX's */

/* the filter's TYPE (1.5, #104: track parameter P_FTYPE, EDIT > FILTER; ANALOG's 8 values are full): the SVF's
 * outputs (dsp.c tsvf_lp, written out here with the taps: Simper's trapezoidal state-variable filter), 0 LP the
 * low-pass as always (the same steps, bit for bit), 1 BP its band-pass v1, 2 HP in - k v1 - v2. At the cutoff the three peak alike (1/k, k the damping
 * RES sets: 2 at RES 0, ~0.15 at 127), so the soft knee after them and the level stay as LP's */
FE_INLINE int32_t analog_tap(const tsvf_t *c, int32_t k, int32_t in, int32_t *ic1, int32_t *ic2, uint32_t ft)
{
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3) >> 13);
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    if (!ft)
        return v2;                                    /* LP: tsvf_lp's (the same steps) */
    return ft == 1u ? v1 : in - ((k * v1) >> 12) - v2;
}
#define ANALOG_K(res) (8192 - (res) * 7600 / 127)     /* tsvf_coef's damping, Q12 */

static const param_desc_t *analog_desc(const track_t *t, uint32_t k)
{
    if (k == 1u && t->p[P_E0] == AW_SYNC)
        return &ANALOG_SYNC;
    if (k == 2u && t->p[P_E0] == AW_SUB)
        return &ANALOG_SUB;
    return 0;
}

static void analog_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[1] = v->ph[0] + 0x40000000u;
    v->s[0] = v->s[1] = 0;                            /* filter */
    if (!v->s[2])
        v->s[2] = 0x1234567 + (int32_t)v->age;        /* noise state */
    v->s[3] = 0;                                      /* SYNC: no restart pending */
}

/* GhettoMachine's synth update (20 Nov 2026): the SHAPER, OSC 2's own wave, FM and RING.
 * The SHAPER (P_SHT type, P_SHA amount, 0 off: the loops as they were; P_BIAS; P_SHPOS 0 before the drive and the
 * filter, 1 after the filter), a voice's state in s[4..7]:
 *   FOLD     Felucca's PHASE engine's sine folder (eng_phase.c FOLD, MIT): sin(pi/2 x gain), the gain 0.64x (a soft
 *            saturation) .. ~8x, the part above 0.64x falling with the pitch above A4 (1/f: the top notes fold less)
 *   CLIP     a hard clip at 1x .. 16x
 *   RECTIFY  the wave blended with its full-wave rectified self (an octave up, the bias moving where it folds)
 *   CRUSH    fewer bits (up to 13 off) and a sample hold (up to 15 samples)
 * BIAS (-64..63) adds up to half the full scale before it; a DC blocker (~14 Hz, PHASE's) after it */
typedef struct {
    int32_t type, g, bias, x1, hp, hold, cnt, per, sh;
} shp_t;
#define SHP_G0 163u                       /* FOLD at 1: 0.64x, Q8 (PHASE's PD_FOLD_G0) */
#define SHP_REF ((uint32_t)((440ull << 32) / FS))
FE_INLINE void shp_begin(shp_t *h, const int16_t *p, const voice_t *v, uint32_t inc)
{
    uint32_t a = (uint32_t)clamp(p[P_SHA], 0, 127);
    h->type = p[P_SHT];
    h->bias = p[P_BIAS] * 256;
    h->x1 = v->s[4];
    h->hp = v->s[5];
    h->hold = v->s[6];
    h->cnt = v->s[7];
    h->g = 0;
    h->per = 1;
    h->sh = 0;
    switch (h->type) {
    case 0: {
        uint32_t ext = a * ((2048u - SHP_G0) << 4) / 127u;
        if (inc > SHP_REF)
            ext = ext * (SHP_REF >> 12) / (inc >> 12);
        h->g = (int32_t)(SHP_G0 + (ext >> 4));
        break;
    }
    case 1: h->g = 256 + (int32_t)(a * (4096u - 256u) / 127u); break;
    case 2: h->g = (int32_t)(a * 258u); break;
    default:
        h->sh = (int32_t)(a * 13u / 127u);
        h->per = 1 + (int32_t)(a * a / 1100u);
        break;
    }
}
FE_INLINE int32_t shp_run(shp_t *h, int32_t s)   /* s within +-65535: the same back */
{
    int32_t x = s + h->bias, y;
    switch (h->type) {
    case 0: y = sine_i((uint32_t)(x * h->g) << 7); break;
    case 1: y = clamp((x * h->g) >> 8, -32767, 32767); break;
    case 2: {
        int32_t r = clamp((x < 0 ? -x : x) * 2 - 32767, -32767, 32767), c = clamp(s, -32767, 32767);
        y = c + mulq15(r - c, h->g);      /* (c: r - c within 16 bits) */
        break;
    }
    default:
        if (++h->cnt >= h->per) {
            int32_t q = clamp(x, -32768, 32767) + 32768;            /* 0..65535: the bits off from the bottom */
            h->cnt = 0;
            h->hold = (int32_t)(((uint32_t)q >> h->sh) << h->sh) - 32768;
        }
        y = h->hold;
        break;
    }
    h->hp += y - h->x1 - (h->hp >> 9);
    h->x1 = y;
    return clamp(h->hp, -65535, 65535);
}
FE_INLINE void shp_end(const shp_t *h, voice_t *v)
{
    v->s[4] = h->x1;
    v->s[5] = h->hp;
    v->s[6] = h->hold;
    v->s[7] = h->cnt;
}

FE_INLINE int32_t osc_w(uint32_t w, uint32_t ph, uint32_t inc, uint32_t pw)   /* WAVE 0..4 */
{
    switch (w) {
    case 1: return osc_pulse(ph, inc, 0x80000000u);
    case 2: return osc_tri(ph);
    case 3: return osc_sine(ph);
    case 4: return osc_pulse(ph, inc, pw);
    default: return osc_saw(ph, inc);
    }
}

/* WAVE 0..4 with any of OSC 2's own wave (P_WAVE2 - 1), FM, RING, the SHAPER or the matrix's OSC 2 pitch: analog_osc's
 * chain with them in (its own loop: analog_osc's stays what it was).
 *   FM    OSC 2 moves OSC 1's phase (phase modulation, as the DX's): up to 1.5 cycles each way at 127 (the amount
 *         squared), less above A5 (1/f); OSC 1's band-limiting steps by how far its phase really moved
 *   RING  the mix crossfades to OSC 1 x OSC 2 */
#define FM_REF ((uint32_t)((880ull << 32) / FS))
FE_RAM static __attribute__((noinline)) void analog_render_r(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                      const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)p[P_E0], w2 = p[P_WAVE2] > 0 ? (uint32_t)p[P_WAVE2] - 1u : w1, i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 512;
    uint32_t inc1 = m->inc;
    int32_t intv = p[P_INTV], o2 = p[P_O2];
    uint32_t inc2 = det || intv || o2 ? cents_inc(clamp(m->pitch16 + intv * 16 + o2, 0, 2047), det, m->fine) : inc1;
    int32_t sbl = p[P_SUB] * 200;
    uint32_t ph2 = v->ph[2], incs = inc1 >> 1;
    uint32_t pw = 0x80000000u + ((uint32_t)(m->shape - (64 << 8)) << 15);
    int32_t m2 = mix * 258, m1 = 32767 - m2;
    int32_t nz = noise * 200, drv = p[P_E6];
    int32_t ring = p[P_RING] * 258;
    uint32_t fm = (uint32_t)clamp(p[P_FM], 0, 127), fmk = fm * fm * 49152u / 16129u;   /* (b * fmk) << 2: the phase */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], pe0 = ph0 - inc1;
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    uint32_t ft = (uint32_t)p[P_FTYPE];
    int32_t fk = ANALOG_K(p[P_E5]);
    uint32_t shp = p[P_SHA] ? 1u + (uint32_t)(p[P_SHPOS] != 0) : 0u;
    shp_t sh;
    if (fmk && inc1 > FM_REF)
        fmk = fmk * (FM_REF >> 12) / (inc1 >> 12);
    if (shp)
        shp_begin(&sh, p, v, inc1);
    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t a, b, s, y;
        b = osc_w(w2, ph1, inc2, pw);
        if (fmk) {
            uint32_t pe = ph0 + ((uint32_t)(b * (int32_t)fmk) << 2), ie = pe - pe0;
            if ((int32_t)ie <= 0)         /* (moving back: the band-limiting by the plain step) */
                ie = inc1;
            a = osc_w(w1, pe, ie, pw);
            pe0 = pe;
        } else {
            a = osc_w(w1, ph0, inc1, pw);
        }
        ph0 += inc1;
        ph1 += inc2;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (ring)
            s += mulq15(mulq15(a, b) - s, ring);
        if (sbl)
            s += mulq15(osc_pulse(ph2, incs, 0x80000000u), sbl);
        ph2 += incs;
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (shp == 1u)
            s = shp_run(&sh, s);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        y = soft_knee(analog_tap(&flt, fk, s >> 1, &ic1, &ic2, ft), 16000);
        if (shp == 2u)
            y = clamp(shp_run(&sh, y), -32767, 32767);
        out[i] += voice_amp(y * 2, m, i) * 2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
    if (shp)
        shp_end(&sh, v);
}

/* WAVE SYNC / SUB: OSC 1 a saw, OSC 2 the synced saw / the sub square; then noise, drive and the filter as
 * analog_render. Its own function: analog_render's loop stays what it was */
FE_RAM static __attribute__((noinline)) void analog_render_x(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                      const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t sub = p[P_E0] == AW_SUB, i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 512;
    int32_t m2 = mix * 258, m1 = 32767 - m2;
    int32_t nz = noise * 200, drv = p[P_E6];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], inc1 = m->inc, inc2;
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    int32_t jl = v->s[3];                             /* SYNC: 1 + the step of a restart a sample ago (16 bits), 0 none */
    if (sub) {                                        /* an octave below OSC 1, DTN as OSC 2 always had it */
        inc2 = (det ? cents_inc(m->pitch16, det, 0) : inc1) >> 1;
    } else {                                          /* DTN and the shape above OSC 1, 3/16 semitone a step */
        inc2 = pitch_inc(clamp(m->pitch16 + det * 3 + (((m->shape - (64 << 8)) * 3) >> 8) + p[P_O2], 0, 2047));
        if (m->fine)
            inc2 += (uint32_t)((int32_t)(inc2 >> 12) * m->fine);
    }
    uint32_t ft = (uint32_t)p[P_FTYPE];
    int32_t fk = ANALOG_K(p[P_E5]);
    uint32_t shp = p[P_SHA] ? 1u + (uint32_t)(p[P_SHPOS] != 0) : 0u;   /* GhettoMachine: the SHAPER */
    shp_t sh;
    if (shp)
        shp_begin(&sh, p, v, inc1);
    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t a = osc_saw(ph0, inc1), b, s;
        if (sub) {
            b = osc_pulse(ph1, inc2, 0x80000000u);
        } else {
            int32_t bl = blep(ph1, inc2);
            if (jl) {                                 /* just restarted: the step was jl - 1, not a whole cycle */
                bl = (bl * ((jl - 1) >> 1)) >> 15;
                jl = 0;
            }
            b = (int32_t)(ph1 >> 16) - 32768 - bl;
            if (ph0 > 0xFFFFFFFFu - inc1)             /* OSC 1 restarts it before the next sample: round that step */
                b -= (blep(ph0, inc1) * (int32_t)(ph1 >> 17)) >> 15;
        }
        ph0 += inc1;
        ph1 += inc2;
        if (!sub && ph0 < inc1) {                     /* OSC 1 wrapped: OSC 2 restarts at the same point */
            uint32_t f = ph0 / ((inc1 >> 16) | 1u);   /* the part of a sample since, 0..65535 */
            uint32_t at = ph1 - f * (inc2 >> 16);     /* OSC 2 at the wrap */
            jl = (int32_t)(at >> 16) + 1;
            ph1 = f * (inc2 >> 16);
        }
        s = mulq15(a, m1) + mulq15(b, m2);
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (shp == 1u)
            s = shp_run(&sh, s);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        s = soft_knee(analog_tap(&flt, fk, s >> 1, &ic1, &ic2, ft), 16000);
        if (shp == 2u)
            s = clamp(shp_run(&sh, s), -32767, 32767);
        out[i] += voice_amp(s * 2, m, i) * 2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
    v->s[3] = jl;
    if (shp)
        shp_end(&sh, v);
}

/* WAVE 0..4 through the filter TYPE ft: analog_render's loop with ft 0 (a constant: LP, the code as it always was),
 * analog_render_f's with BP / HP (its own copy: the LP loop gains nothing) */
static inline __attribute__((always_inline)) void analog_osc(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                             const vmod_t *m, uint32_t ft, uint32_t two)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 512;                       /* 1x .. 3x */
    uint32_t inc1 = m->inc;
    int32_t intv = p[P_INTV];
    uint32_t inc2 = det || intv ? cents_inc(clamp(m->pitch16 + intv * 16, 0, 2047), det, m->fine) : inc1;   /* DTN in cents */
    int32_t sbl = p[P_SUB] * 200;                               /* GhettoMachine: the sub's level */
    uint32_t ph2 = v->ph[2], incs = inc1 >> 1;
    uint32_t pw = 0x80000000u + ((uint32_t)(m->shape - (64 << 8)) << 15);
    int32_t m2 = mix * 258, m1 = 32767 - m2;                    /* osc mix Q15 */
    int32_t nz = noise * 200, drv = p[P_E6];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1];                  /* state in locals: out[] may alias v->s[] */
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    int32_t fk = ft ? ANALOG_K(p[P_E5]) : 0;
    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t a, b, s;
        if (two) {
            switch (wave) {
            case 1:
                a = osc_pulse(ph0, inc1, 0x80000000u);
                b = osc_pulse(ph1, inc2, 0x80000000u);
                break;
            case 2:
                a = osc_tri(ph0);
                b = osc_tri(ph1);
                break;
            case 3:
                a = osc_sine(ph0);
                b = osc_sine(ph1);
                break;
            case 4:
                a = osc_pulse(ph0, inc1, pw);
                b = osc_pulse(ph1, inc2, pw);
                break;
            default:
                a = osc_saw(ph0, inc1);
                b = osc_saw(ph1, inc2);
                break;
            }
        } else {                          /* (GhettoMachine 0.5: MIX at 0, OSC 2 times nothing: not worked out; its
                                           * phase goes on below, so turning MIX up finds it where it would be) */
            b = 0;
            switch (wave) {
            case 1: a = osc_pulse(ph0, inc1, 0x80000000u); break;
            case 2: a = osc_tri(ph0); break;
            case 3: a = osc_sine(ph0); break;
            case 4: a = osc_pulse(ph0, inc1, pw); break;
            default: a = osc_saw(ph0, inc1); break;
            }
        }
        ph0 += inc1;
        ph1 += inc2;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (sbl)
            s += mulq15(osc_pulse(ph2, incs, 0x80000000u), sbl);
        ph2 += incs;
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);   /* pre-shifts: drive is up to 3x, no overflow */
        {   /* filter, linear up to half scale, then a soft knee (only resonance peaks saturate); soft_knee()
             * written out: the host compiler makes this loop 1 % slower with the call */
            int32_t y = ft ? analog_tap(&flt, fk, s >> 1, &ic1, &ic2, ft) : tsvf_lp(&flt, s >> 1, &ic1, &ic2);
            int32_t a = y < 0 ? -y : y;
            if (a > 16000) {
                a = 16000 + (softclip((a - 16000) * 2) >> 1);
                y = y < 0 ? -a : a;
            }
            s = y * 2;                          /* soft knee bounds y; multiplication permits negatives */
        }
        out[i] += voice_amp(s, m, i) * 2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}
FE_RAM static __attribute__((noinline)) void analog_render_f(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    if (t->p[P_E2])                       /* (two is a constant in each: a loop with OSC 2, a loop without) */
        analog_osc(t, v, out, n, m, (uint32_t)t->p[P_FTYPE], 1);
    else
        analog_osc(t, v, out, n, m, (uint32_t)t->p[P_FTYPE], 0);
}

FE_RAM static void analog_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    if (p[P_E0] >= AW_SYNC)
        analog_render_x(t, v, out, n, m);
    else if (p[P_WAVE2] || p[P_FM] || p[P_RING] || p[P_SHA] || p[P_O2])   /* GhettoMachine: the synth update's loop */
        analog_render_r(t, v, out, n, m);
    else if (t->p[P_FTYPE])
        analog_render_f(t, v, out, n, m);
    else if (p[P_E2])
        analog_osc(t, v, out, n, m, 0, 1);
    else
        analog_osc(t, v, out, n, m, 0, 0);
}

static const preset_t ANALOG_PRESETS[] = {
    {"SAW LEAD", {0, 12, 64, 0, 90, 30, 10, 64}, {4, 70, 100, 50}, 20, 1, FX(0, 10, 45, 30), PAT(4)},
    {"SOFT PAD", {0, 20, 64, 4, 60, 10, 0, 32}, {80, 90, 110, 95}, 10, 0, FX(0, 60, 20, 70), PAT(5)},
    {"SQR BASS", {1, 0, 0, 0, 50, 70, 40, 64}, {0, 60, 40, 30}, 30, 1, FX(5, 0, 10, 10), PAT(2)},
    {"PWM STR", {4, 8, 40, 0, 75, 20, 0, 48}, {60, 80, 110, 85}, 8, 0, FX(0, 50, 20, 60), PAT(5)},
    {"ACID", {0, 0, 0, 0, 50, 100, 25, 64}, {0, 55, 20, 30}, 48, 1, FX(20, 0, 45, 15), PAT(1)},
    {"SINE KEY", {3, 6, 50, 0, 127, 0, 0, 0}, {2, 80, 30, 70}, 0, 0, FX(0, 30, 25, 40), PAT(6)},
    {"RAVE", {4, 30, 64, 0, 85, 20, 30, 64}, {20, 80, 110, 60}, 10, 1, FX(30, 40, 30, 30), PAT(13)},
    {"SUB BASS", {3, 0, 0, 0, 40, 0, 20, 0}, {0, 60, 100, 20}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"PLUCK", {0, 8, 50, 0, 30, 40, 0, 64}, {0, 88, 0, 60}, 55, 0, FX(0, 20, 50, 30), PAT(3)},
    {"BRASS", {0, 10, 64, 0, 45, 20, 10, 64}, {35, 70, 90, 45}, 40, 0, FX(0, 20, 20, 40), PAT(6)},
    {"WIND", {0, 0, 0, 90, 30, 90, 0, 0}, {60, 90, 60, 80}, 50, 0, FX(0, 30, 30, 70), PAT(5)},
    {"STRINGS", {0, 25, 64, 0, 70, 10, 0, 32}, {70, 90, 115, 90}, 5, 0, FX(0, 60, 20, 70), PAT(5)},
};

static const engine_t ENG_ANALOG = {
    .name = "ANALOG",
    .page_title = {"OSC", "FLT"},
    .edit = {
        {"WAVE", F_ENUM, 0, 6, 0, N_ANALOG_WAVE, 0},
        {"DTN", F_INT, 0, 127, 10, 0, "ct"},
        {"MIX", F_PCT, 0, 127, 64, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
        {"KTR", F_PCT, 0, 127, 64, 0, 0},
    },
    .presets = ANALOG_PRESETS,
    .npresets = NELEM(ANALOG_PRESETS),
    .note_on = analog_note_on,
    .render = analog_render,
    .desc = analog_desc,
    .knob = {P_E4, P_E5, P_ATK, P_REL},
    .keep = 0x03,                /* the filter */
};
