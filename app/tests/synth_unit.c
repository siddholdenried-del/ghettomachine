/* GhettoMachine's synth update checked on app/dsp/synth.c alone: OSC 2's wave, FM, RING, the SHAPER, the matrix, MOD ENV,
 * LFO 2, DRIFT and MACRO do what they say, and nothing overflows (built with the undefined-behaviour sanitizer: a
 * signed overflow stops it). From the repository's top, after a host build (app/build/gen/fe_tables.h):
 *   cc -std=c99 -O1 -g -fsanitize=undefined,address -fno-sanitize-recover=all -Iapp/build/gen -Iapp/dsp \
 *      app/tests/synth_unit.c app/dsp/synth.c -lm -o "$TMPDIR/su" && "$TMPDIR/su"
 * It also times a plain voice against one with FM and the SHAPER (the host's CPU: a ratio, not the FM-1's load). */
#include "synth.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

static int fails, checks;
static void check(int ok, const char *what, double got)
{
    checks++;
    if (!ok)
        fails++;
    printf("%s %s (%.4g)\n", ok ? "  ok" : "FAIL", what, got);
}

#define N 8192
static float bl[N], br[N];

/* track 1 with the sound v, a note held from the start: n samples of L and R after skip */
static void render(const int8_t *v, int mono, int note, float vel, int n, int skip)
{
    float l[SYN_CTL], r[SYN_CTL], rv[SYN_CTL], dl[SYN_CTL];
    int i, k = 0, j;
    syn_kill();
    syn_sound(1, v);
    syn_level(1, 0.8f);
    syn_mixer(1, 64, 0.0f, 0.0f);
    syn_note_on(1, note, vel, -1, mono);
    for (i = 0; k < n; i++) {
        syn_mod(1, SM_CUTOFF, 0.0f, 0.0f);
        syn_prepare();
        syn_half(0);
        syn_half(1);
        syn_mix(l, r, rv, dl);
        for (j = 0; j < SYN_CTL; j++)
            if (i * SYN_CTL + j >= skip && k < n) {
                bl[k] = l[j];
                br[k++] = r[j];
            }
    }
}

static double mag(const float *x, int n, double f)   /* the Hann-windowed amplitude at f Hz */
{
    double re = 0, im = 0, w = 6.283185307 * f / 44100.0;
    int k;
    for (k = 0; k < n; k++) {
        double h = 0.5 - 0.5 * cos(6.283185307 * k / (n - 1));
        re += x[k] * h * cos(w * k);
        im -= x[k] * h * sin(w * k);
    }
    return sqrt(re * re + im * im) * 4.0 / n;
}
static double rms(const float *x, int n)
{
    double s = 0;
    int k;
    for (k = 0; k < n; k++)
        s += (double)x[k] * x[k];
    return sqrt(s / n);
}
static double freq(const float *x, int n)   /* rising zero crossings, interpolated */
{
    double first = -1, last = -1;
    int k, c = 0;
    for (k = 1; k < n; k++)
        if (x[k - 1] <= 0 && x[k] > 0) {
            double at = k - 1 + x[k - 1] / (x[k - 1] - x[k]);
            if (first < 0)
                first = at;
            else
                c++;
            last = at;
        }
    return c ? 44100.0 * c / (last - first) : 0;
}

static void plain(int8_t *v)              /* a sine, wide open, held: the base the checks change */
{
    syn_preset(0, v);
    v[SP_WAVE] = 3;
    v[SP_DETUNE] = 0;
    v[SP_MIX] = 0;
    v[SP_INTV] = 0;
    v[SP_SUB] = v[SP_NOISE] = v[SP_DRIVE] = 0;
    v[SP_CUT] = 127;
    v[SP_RES] = v[SP_FENV] = v[SP_KTR] = 0;
    v[SP_VEL] = 0;
    v[SP_SPREAD] = 0;
    v[SP_VOICES] = 1;
    v[SP_GLIDE] = 0;
    v[SP_AA] = 0;
    v[SP_AD] = 0;
    v[SP_AS] = 127;
    v[SP_AR] = 40;
}

static void blocks(int n)                 /* n control blocks of the synth as it is */
{
    float l[SYN_CTL], r[SYN_CTL], rv[SYN_CTL], dl[SYN_CTL];
    while (n-- > 0) {
        syn_mod(1, SM_CUTOFF, 0.0f, 0.0f);
        syn_prepare();
        syn_half(0);
        syn_half(1);
        syn_mix(l, r, rv, dl);
    }
}

/* the 303's marks (0.4): note 57 held (gated), then 64 with flags, on a MONO or POLY sound with LEGATO and GLIDE as
 * given: 64's pitch one block later (1/16 semitone); *voices: the track's voices sounding then */
static int marked_pitch(int mono, int legato, int glide, int flags, int *voices)
{
    int8_t v[SP_NUM];
    plain(v);
    v[SP_GLIDE] = (int8_t)glide;
    v[SP_LEGATO] = (int8_t)legato;
    v[SP_VOICES] = (int8_t)(mono ? 1 : 8);
    syn_kill();
    syn_sound(1, v);
    syn_level(1, 0.8f);
    syn_note_on(1, 57, 0.8f, 6000, mono);
    blocks(60);
    syn_note_on_f(1, 64, 0.8f, 4000, mono, flags);
    blocks(1);
    if (voices)
        *voices = syn_voices(1);
    return syn_probe(1, 0);
}

/* LEGATO (0.4): a MONO sound with GLIDE plays note a for gate samples, then (after wait blocks) note b, also gated:
 * b's pitch one block later, 1/16 semitone */
static int legato_pitch(int legato, int32_t gate, int wait)
{
    int8_t v[SP_NUM];
    plain(v);
    v[SP_GLIDE] = 40;                     /* 150 ms */
    v[SP_LEGATO] = (int8_t)legato;
    syn_kill();
    syn_sound(1, v);
    syn_level(1, 0.8f);
    syn_note_on(1, 57, 0.8f, gate, 1);
    blocks(wait);
    syn_note_on(1, 64, 0.8f, 4000, 1);
    blocks(1);
    return syn_probe(1, 0);
}

static void slot(int8_t *v, int n, int src, int dst, int amt)
{
    v[SP_XM + 3 * n] = (int8_t)src;
    v[SP_XM + 3 * n + 1] = (int8_t)dst;
    v[SP_XM + 3 * n + 2] = (int8_t)amt;
}

int main(void)
{
    int8_t v[SP_NUM];
    const double f0 = 220.0;              /* A3, note 57 */
    double a, b, c;
    int i, w, ty, pos, note;
    syn_init();

    plain(v);
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(a > 0.01, "a plain sine sounds", a);
    check(mag(bl, N, 2 * f0) / a < 0.05 && mag(bl, N, 3 * f0) / a < 0.05, "  with little else", mag(bl, N, 3 * f0) / a);
    check(fabs(freq(bl, N) - f0) < 0.5, "  at 220 Hz", freq(bl, N));

    /* FM: 1:1 brings every harmonic; 2:1 (INTERVAL +12) the odd ones only */
    plain(v);
    v[SP_FM] = 64;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 2 * f0) / a > 0.2, "FM 64 at 1:1: a strong 2nd harmonic", mag(bl, N, 2 * f0) / a);
    check(fabs(freq(bl, N) - f0) < 2.0 || mag(bl, N, f0) > 0.003, "  the pitch kept", freq(bl, N));
    v[SP_INTV] = 12;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 3 * f0) / a > 0.2, "FM at 2:1: the 3rd harmonic", mag(bl, N, 3 * f0) / a);
    check(mag(bl, N, 2 * f0) / a < 0.05, "  and no 2nd", mag(bl, N, 2 * f0) / a);
    plain(v);
    v[SP_FM] = 20;
    render(v, 1, 57, 1.0f, N, 4096);
    b = mag(bl, N, 2 * f0) / mag(bl, N, f0);
    v[SP_FM] = 100;
    render(v, 1, 57, 1.0f, N, 4096);
    c = mag(bl, N, 3 * f0) / mag(bl, N, f0);
    check(c > b, "more FM, brighter", c);

    /* RING: OSC 1 x OSC 2 (a fifth up): the sum and the difference, OSC 1's own gone */
    plain(v);
    v[SP_INTV] = 7;
    v[SP_RING] = 127;
    render(v, 1, 57, 1.0f, N, 4096);
    {
        double f2 = f0 * pow(2.0, 7.0 / 12.0);
        a = mag(bl, N, f0);
        b = mag(bl, N, f2 + f0);
        c = mag(bl, N, f2 - f0);
        check(b > 0.005 && c > 0.005, "RING 127: the sum and difference", b);
        check(a / b < 0.1, "  OSC 1's own gone", a / b);
    }

    /* WAVE 2: a square OSC 2 under a sine OSC 1 (MIX 127: OSC 2 alone) */
    plain(v);
    v[SP_WAVE2] = 2;
    v[SP_MIX] = 127;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 3 * f0) / a > 0.25, "WAVE 2 SQUARE: odd harmonics", mag(bl, N, 3 * f0) / a);
    check(mag(bl, N, 2 * f0) / a < 0.05, "  no even ones", mag(bl, N, 2 * f0) / a);

    /* the SHAPER on the sine */
    plain(v);
    v[SP_SHAPE] = 127;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 3 * f0) / a > 0.3, "FOLD 127: odd harmonics", mag(bl, N, 3 * f0) / a);
    check(mag(bl, N, 2 * f0) / a < 0.05, "  no even ones (no bias)", mag(bl, N, 2 * f0) / a);
    v[SP_BIAS] = 40;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 2 * f0) / (a + mag(bl, N, 3 * f0)) > 0.1, "  BIAS 40: even ones too", mag(bl, N, 2 * f0) / a);
    plain(v);
    v[SP_SHTYPE] = 1;
    v[SP_SHAPE] = 127;
    render(v, 1, 57, 1.0f, N, 4096);
    a = mag(bl, N, f0);
    check(mag(bl, N, 3 * f0) / a > 0.2, "CLIP 127: square-ish", mag(bl, N, 3 * f0) / a);
    plain(v);
    v[SP_SHTYPE] = 2;
    v[SP_SHAPE] = 127;
    render(v, 1, 57, 1.0f, N, 4096);
    check(mag(bl, N, 2 * f0) > mag(bl, N, f0), "RECTIFY 127: an octave up", mag(bl, N, 2 * f0) / mag(bl, N, f0));
    plain(v);
    v[SP_SHTYPE] = 3;
    v[SP_SHAPE] = 127;
    v[SP_SHPOS] = 1;
    render(v, 1, 57, 1.0f, N, 4096);
    {
        int flat = 0;
        float pk = 0.0f;
        for (i = 0; i < N; i++)
            pk = fabsf(bl[i]) > pk ? fabsf(bl[i]) : pk;
        for (i = 1; i < N; i++)
            flat += fabsf(bl[i] - bl[i - 1]) < pk * 0.002f;
        check(flat > N / 2, "CRUSH 127 POST: held steps", (double)flat / N);
    }

    /* MOD ENV -> PITCH +32 (12 semitones, decaying): an octave up at first, the note's own later */
    plain(v);
    slot(v, 0, MS_MENV, MD_PITCH, 32);
    render(v, 1, 57, 1.0f, 1024, 0);
    a = freq(bl, 1024);
    render(v, 1, 57, 1.0f, 4096, 88200);
    b = freq(bl, 4096);
    check(a > 1.7 * f0, "MOD ENV > PITCH: up at the start", a);
    check(fabs(b - f0) < 1.0, "  down to the note", b);

    /* LFO 2 > PITCH +4: a vibrato of about +-0.19 semitone (EACH NOTE: from its start) */
    plain(v);
    slot(v, 0, MS_LFO2, MD_PITCH, 4);
    v[SP_L2SHAPE] = 4;                    /* SQUARE: up a half cycle, down the other */
    v[SP_L2RATE] = 40;                    /* ~0.3 Hz */
    render(v, 1, 57, 1.0f, 8192, 0);
    a = freq(bl, 8192);
    render(v, 1, 57, 1.0f, 8192, 88200);
    b = freq(bl, 8192);
    check(a > f0 * 1.008 && a < f0 * 1.016, "LFO 2 > PITCH: up ~0.19 st first", 1200 * log2(a / f0));
    check(b < f0 * 0.992 && b > f0 * 0.984, "  then down", 1200 * log2(b / f0));

    /* DRIFT 127: a slow wander of a few cents */
    plain(v);
    v[SP_DRIFT] = 127;
    {
        double lo = 1e9, hi = 0;
        int k;
        render(v, 1, 57, 1.0f, 2048, 0);
        for (k = 0; k < 16; k++) {
            render(v, 1, 57, 1.0f, 2048, 22050 * k);
            a = freq(bl, 2048);
            lo = a < lo ? a : lo;
            hi = a > hi ? a : hi;
        }
        c = 1200 * log2(hi / lo);
        check(c > 2 && c < 40, "DRIFT 127: wanders a few cents", c);
    }

    /* VELOCITY > LEVEL -64: loud notes silent, soft ones heard */
    plain(v);
    slot(v, 0, MS_VEL, MD_LEVEL, -64);
    render(v, 1, 57, 1.0f, 4096, 4096);
    a = rms(bl, 4096);
    render(v, 1, 57, 0.3f, 4096, 4096);
    b = rms(bl, 4096);
    check(a < 1e-4 && b > 0.01, "VELOCITY > LEVEL -64", b);

    /* MACRO 1 > CUTOFF: a shut filter opened by the macro */
    plain(v);
    v[SP_WAVE] = 0;
    v[SP_CUT] = 10;
    slot(v, 0, MS_M1, MD_CUTOFF, 63);
    render(v, 1, 57, 1.0f, 4096, 4096);
    a = mag(bl, N / 2, 5 * f0);
    v[SP_M1] = 127;
    render(v, 1, 57, 1.0f, 4096, 4096);
    b = mag(bl, N / 2, 5 * f0);
    check(b > 10 * a, "MACRO 1 > CUTOFF opens it", b / (a + 1e-9));

    /* NOTE > PAN 63: a high note right, a low one left */
    plain(v);
    slot(v, 0, MS_NOTE, MD_PAN, 63);
    render(v, 1, 108, 1.0f, 4096, 4096);
    a = rms(br, 4096) / (rms(bl, 4096) + 1e-9);
    render(v, 1, 24, 1.0f, 4096, 4096);
    b = rms(bl, 4096) / (rms(br, 4096) + 1e-9);
    check(a > 2 && b > 2, "NOTE > PAN: high right, low left", a);

    /* everything at once, every wave and shaper, across the keys: finite, bounded (and no overflow: the sanitizer) */
    {
        float pk = 0.0f;
        int bad = 0;
        for (w = 0; w <= 6; w++)
            for (ty = 0; ty < 4; ty++)
                for (pos = 0; pos < 2; pos++)
                    for (note = 24; note <= 108; note += 28) {
                        plain(v);
                        v[SP_WAVE] = (int8_t)w;
                        v[SP_WAVE2] = (int8_t)(1 + (w + 2) % 5);
                        v[SP_FM] = v[SP_RING] = v[SP_SHAPE] = v[SP_DRIVE] = v[SP_RES] = 127;
                        v[SP_NOISE] = v[SP_SUB] = v[SP_DETUNE] = v[SP_DRIFT] = 127;
                        v[SP_INTV] = 24;
                        v[SP_BIAS] = 63;
                        v[SP_SHTYPE] = (int8_t)ty;
                        v[SP_SHPOS] = (int8_t)pos;
                        v[SP_VOICES] = 4;
                        v[SP_SPREAD] = 127;
                        v[SP_M1] = 127;
                        slot(v, 0, MS_RND, MD_PITCH, 63);
                        slot(v, 1, MS_MENV, MD_FM, 63);
                        slot(v, 2, MS_LFO2, MD_SHAPE, 63);
                        slot(v, 3, MS_VEL, MD_LEVEL, 63);
                        slot(v, 4, MS_NOTE, MD_PAN, 63);
                        slot(v, 5, MS_M1, MD_OSC2, -64);
                        render(v, 1, note, 1.0f, 2048, 0);
                        for (i = 0; i < 2048; i++) {
                            float x = fabsf(bl[i]) > fabsf(br[i]) ? fabsf(bl[i]) : fabsf(br[i]);
                            if (!(x == x) || x > 1e3f)
                                bad++;
                            pk = x > pk ? x : pk;
                        }
                    }
        check(!bad && pk < 20.0f, "all of it at once: finite and bounded", pk);
    }

    /* the cost on the host: 16 voices, plain against FM + FOLD (a ratio only) */
    {
        float l[SYN_CTL], r[SYN_CTL], rv[SYN_CTL], dl[SYN_CTL];
        double t0, t1;
        int k;
        for (k = 0; k < 2; k++) {
            clock_t c0;
            int n;
            plain(v);
            v[SP_WAVE] = 0;
            v[SP_VOICES] = 8;
            v[SP_DETUNE] = 10;
            v[SP_MIX] = 64;
            v[SP_CUT] = 90;
            if (k) {
                v[SP_FM] = 40;
                v[SP_SHAPE] = 60;
                slot(v, 0, MS_MENV, MD_FM, 30);
                slot(v, 1, MS_LFO2, MD_PITCH, 4);
            }
            syn_kill();
            for (n = 1; n <= 2; n++) {
                syn_sound(n, v);
                syn_level(n, 0.5f);
            }
            for (n = 0; n < 8; n++) {
                syn_note_on(1, 48 + n * 3, 0.8f, -1, 0);
                syn_note_on(2, 50 + n * 3, 0.8f, -1, 0);
            }
            c0 = clock();
            for (n = 0; n < 20000; n++) {
                syn_prepare();
                syn_half(0);
                syn_half(1);
                syn_mix(l, r, rv, dl);
            }
            if (k)
                t1 = (double)(clock() - c0);
            else
                t0 = (double)(clock() - c0);
        }
        printf("  -- the host's time, 16 voices: FM + FOLD + the matrix %.2fx a plain voice's\n", t1 / t0);
    }

    /* LEGATO SLIDE (the 303 generator's slides): glide only into a tied note; ON glides whenever the voice sounds */
    {
        int tied = legato_pitch(2, 4000, 40), apart = legato_pitch(2, 600, 40), on = legato_pitch(1, 600, 40);
        check(tied > 57 * 16 && tied < 64 * 16, "LEGATO SLIDE: a tied note glides", tied);
        check(apart == 64 * 16, "  a note after the gate starts at its pitch", apart);
        check(on > 57 * 16 && on < 64 * 16, "LEGATO ON: that note glides (as before)", on);
        check(legato_pitch(0, 4000, 40) > 57 * 16 && legato_pitch(0, 4000, 40) < 64 * 16, "LEGATO OFF: a new attack, gliding",
              legato_pitch(0, 4000, 40));
    }

    /* the 303's marks: a slide glides on any sound, LEGATO off, POLY, GLIDE 0 (58 ms then); an accent is louder and
     * opens the filter, falling away */
    {
        int nv = 0, p;
        p = marked_pitch(1, 0, 0, SN_SLIDE, &nv);
        check(p > 57 * 16 && p < 64 * 16 && nv == 1, "SN_SLIDE, MONO, LEGATO OFF, GLIDE 0: glides, one voice", p);
        p = marked_pitch(0, 0, 0, SN_SLIDE, &nv);
        check(p > 57 * 16 && p < 64 * 16 && nv == 1, "  POLY: the held note's voice glides (no second voice)", nv);
        p = marked_pitch(0, 0, 0, 0, &nv);
        check(p == 64 * 16 && nv == 2, "  POLY unmarked: a second voice, at its pitch", nv);
        p = marked_pitch(1, 1, 100, SN_SLIDE, &nv);
        b = p;
        p = marked_pitch(1, 1, 10, SN_SLIDE, &nv);
        check(b < p, "  GLIDE's time when it has one (slower at 100 than at 10)", b);
    }
    {
        int8_t v[SP_NUM];
        int a0, a1, c0, c1, e0, e1;
        plain(v);
        v[SP_CUT] = 40;
        v[SP_VEL] = 0;
        syn_kill();
        syn_sound(1, v);
        syn_level(1, 0.5f);
        syn_note_on_f(1, 48, 0.8f, 40000, 1, 0);
        blocks(40);
        a0 = syn_probe(1, 4);
        c0 = syn_probe(1, 1);
        syn_kill();
        syn_note_on_f(1, 48, 0.8f, 40000, 1, SN_ACCENT);
        blocks(40);
        a1 = syn_probe(1, 4);
        c1 = syn_probe(1, 1);
        e0 = syn_probe(1, 7);
        blocks(600);
        e1 = syn_probe(1, 7);
        check(a1 > a0 * 13 / 10 && syn_probe(1, 8) == 1, "SN_ACCENT: louder (x1.4)", (double)a1 / a0);
        check(c1 > c0 + 15 * 256, "  the filter opened (up to 20 steps)", (c1 - c0) / 256.0);
        check(e0 > 800 && e0 < 900 && e1 < 200, "  its sweep falls away in about 200 ms (0.86 after 29 ms)", e0);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
