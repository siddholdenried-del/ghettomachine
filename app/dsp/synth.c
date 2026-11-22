/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine: the instrument tracks' synth (see synth.h).
 *
 * The voice is Felucca-Engines' ANALOG (MIT, Leo Kuroshita / Hügelton Instruments: dsp/felucca/, built as the engines
 * ask, in one unit with their host; eng_analog.c gained a sub level and an interval for OSC 2). The host around it, the
 * envelopes, glide, unison, the note stack, the caps and the pool, is GhettoMachine's. The factory sounds are after
 * Felucca's ANALOG presets, retuned to this host's envelope times. */
#define FM_ALWAYS_INLINE                  /* (code in RAM here: fastmath.h, gm_ram.h) */
#include "synth.h"

/* ---------------------------------------------- the engine, as it asks to be built --- */
#define NVOICE SYN_VOICES
#define NPART SYN_TRACKS
/* P_WAVE2 .. P_O2: GhettoMachine's synth update (eng_analog.c): OSC 2's wave (0 OSC 1's), FM, RING, the SHAPER (type,
 * amount, bias, PRE / POST) and OSC 2's pitch moved by the matrix (1/16 semitone; a voice's own, 0 in the track's) */
enum { P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_FTYPE, P_SUB, P_INTV, P_ATK, P_REL,
       P_WAVE2, P_FM, P_RING, P_SHT, P_SHA, P_BIAS, P_SHPOS, P_O2, P_NUM };
#include "felucca/felucca_engine.h"
typedef struct track {
    int16_t p[P_NUM];                     /* the engine's view of a track's sound (push below) */
} track_t;
static track_t trk[NPART];
/* GhettoMachine 0.5 (CPU): the voices run from RAM (gm_ram.h), out of the XIP cache's way, on both cores: the render
 * loops in RAM (FE_RAM: dsp.c), every helper they use inlined into them (FE_INLINE) */
#include "gm_ram.h"
#define FE_INLINE GM_INLINE
#define FE_RAM GM_RAM
#ifndef PLAT_HOST                         /* the engine's tables (sine, pitch, filter, tanh: 3.6 KB) in RAM too: the */
#define FE_TAB static __attribute__((section(".data.fe_tab")))   /* voices read them every sample */
#endif
#include "fe_tables.h"
#include "felucca/dsp.c"
#include "felucca/eng_analog.c"

/* -------------------------------------------------------------------- parameters --- */
static const char *const N_WAVE[] = {"SAW", "SQUARE", "TRI", "SINE", "PWM", "SYNC", "SUB"};
static const char *const N_FTYPE[] = {"LP", "BP", "HP"};
static const char *const N_ONOFF[] = {"OFF", "ON"};
static const char *const N_LEGATO[] = {"OFF", "ON", "SLIDE"};   /* SLIDE (0.4): ON, gliding only between tied notes */
static const char *const N_WAVE2[] = {"SAME", "SAW", "SQUARE", "TRI", "SINE", "PWM"};
static const char *const N_SHTYPE[] = {"FOLD", "CLIP", "RECTIFY", "CRUSH"};
static const char *const N_SHPOS[] = {"PRE FLT", "POST FLT"};
static const char *const N_LSHAPE[] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "S&H", "SMOOTH"};
static const char *const N_L2KEY[] = {"EACH NOTE", "FREE"};
static const char *const N_MSRC[MS_NUM] = {"OFF", "LFO 1", "LFO 2", "MOD ENV", "FILT ENV", "AMP ENV", "VELOCITY", "NOTE",
                                          "RANDOM", "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4"};
static const char *const N_MDST[MD_NUM] = {"PITCH", "OSC 2", "DETUNE", "CUTOFF", "RESO", "PWM", "MIX", "SUB", "NOISE",
                                          "DRIVE", "FM", "RING", "SHAPER", "BIAS", "LEVEL", "PAN", "LFO 2 RATE"};
#define SLOT(n) {"MOD" #n " SRC", 0, MS_NUM - 1, 0, SF_ENUM, N_MSRC}, {"MOD" #n " DEST", 0, MD_NUM - 1, 0, SF_ENUM, N_MDST}, \
                {"MOD" #n " AMT", -64, 63, 0, SF_SIGNED, 0}
static const syn_param_t PARAMS[SP_NUM] = {
    {"WAVE", 0, 6, 0, SF_ENUM, N_WAVE},
    {"DETUNE", 0, 127, 10, SF_INT, 0},
    {"MIX", 0, 127, 64, SF_INT, 0},
    {"INTERVAL", -24, 24, 0, SF_SEMI, 0},
    {"SUB", 0, 127, 0, SF_INT, 0},
    {"NOISE", 0, 127, 0, SF_INT, 0},
    {"PWM", 0, 127, 64, SF_INT, 0},
    {"DRIVE", 0, 127, 0, SF_INT, 0},
    {"CUTOFF", 0, 127, 90, SF_HZ, 0},
    {"RESO", 0, 127, 30, SF_INT, 0},
    {"ENV AMT", -64, 63, 20, SF_SIGNED, 0},
    {"FILTER", 0, 2, 0, SF_ENUM, N_FTYPE},
    {"KEYTRACK", 0, 127, 64, SF_INT, 0},
    {"VELOCITY", 0, 127, 64, SF_INT, 0},
    {"SPREAD", 0, 127, 20, SF_INT, 0},
    {"LEGATO", 0, 2, 1, SF_ENUM, N_LEGATO},
    {"VOICES", 1, 8, 8, SF_INT, 0},
    {"GLIDE", 0, 127, 0, SF_GLIDE, 0},
    {"ATTACK", 0, 127, 2, SF_TIME, 0},
    {"DECAY", 0, 127, 60, SF_TIME, 0},
    {"SUSTAIN", 0, 127, 100, SF_INT, 0},
    {"RELEASE", 0, 127, 40, SF_TIME, 0},
    {"ATTACK", 0, 127, 0, SF_TIME, 0},
    {"DECAY", 0, 127, 50, SF_TIME, 0},
    {"SUSTAIN", 0, 127, 40, SF_INT, 0},
    {"RELEASE", 0, 127, 40, SF_TIME, 0},
    {"WAVE 2", 0, 5, 0, SF_ENUM, N_WAVE2},
    {"FM", 0, 127, 0, SF_INT, 0},
    {"RING", 0, 127, 0, SF_INT, 0},
    {"DRIFT", 0, 127, 0, SF_INT, 0},
    {"SHAPER", 0, 3, 0, SF_ENUM, N_SHTYPE},
    {"AMOUNT", 0, 127, 0, SF_INT, 0},
    {"BIAS", -64, 63, 0, SF_SIGNED, 0},
    {"PLACE", 0, 1, 0, SF_ENUM, N_SHPOS},
    {"MACRO 1", 0, 127, 0, SF_INT, 0},
    {"MACRO 2", 0, 127, 0, SF_INT, 0},
    {"MACRO 3", 0, 127, 0, SF_INT, 0},
    {"MACRO 4", 0, 127, 0, SF_INT, 0},
    {"ATTACK", 0, 127, 0, SF_TIME, 0},
    {"DECAY", 0, 127, 60, SF_TIME, 0},
    {"SUSTAIN", 0, 127, 0, SF_INT, 0},
    {"RELEASE", 0, 127, 40, SF_TIME, 0},
    {"SHAPE", 0, 6, 0, SF_ENUM, N_LSHAPE},
    {"RATE", 0, 127, 91, SF_LFOHZ, 0},
    {"FADE IN", 0, 127, 0, SF_FADE, 0},
    {"RESTART", 0, 1, 0, SF_ENUM, N_L2KEY},
    SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6),
};
_Static_assert(SP_NUM == 64, "a sound's parameters: 64 (the app's lock mask is 64 bits)");
_Static_assert(SP_NUM_V1 == 26, "0.2's sounds had 26");

const syn_param_t *syn_param(int id) { return id >= 0 && id < SP_NUM ? &PARAMS[id] : 0; }

#define BLOCK_MS (1000.0f * SYN_CTL / 44100.0f)

static float exp_neg(float x)             /* e^-x, x >= 0, without libm: (1 + x / 1024)^1024 */
{
    float y = 1.0f + x * (1.0f / 1024.0f);
    int i;
    for (i = 0; i < 10; i++)
        y *= y;
    return 1.0f / y;
}

/* an envelope stage's time (attack: to full; decay and release: to -40 dB), and glide's, for 0..127 */
static float time_ms(int v)
{
    float t = (float)v / 127.0f;
    return 1.0f + 5000.0f * t * t * t;
}
static float glide_ms(int v)
{
    float t = (float)v / 127.0f;
    return 1500.0f * t * t;
}
static float lfo_hz(int v) { return 0.05f / exp_neg((float)v * (6.39693f / 127.0f)); }   /* 0.05 * 600^(v / 127) */

static char *put_int(char *b, int v)
{
    char t[12];
    int n = 0;
    unsigned u = (unsigned)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    return b;
}
static char *put_str(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    return b;
}
static char *put_tenths(char *b, float x, const char *unit)   /* 1.2 S */
{
    int d = (int)(x * 10.0f + 0.5f);
    b = put_int(b, d / 10);
    *b++ = '.';
    b = put_int(b, d % 10);
    return put_str(b, unit);
}

void syn_param_text(int id, int v, char *buf)
{
    const syn_param_t *p = syn_param(id);
    char *b = buf;
    if (p) {
        switch (p->fmt) {
        case SF_ENUM:
            if (v >= p->min && v <= p->max)
                b = put_str(b, p->names[v - p->min]);
            break;
        case SF_SIGNED:
        case SF_SEMI:
            if (v > 0)
                *b++ = '+';
            b = put_int(b, v);
            if (p->fmt == SF_SEMI)
                b = put_str(b, " ST");
            break;
        case SF_LFOHZ: {                  /* LFO 2's rate, as LFO 1's: two decimals below 1 Hz, one below 10 */
            float hz = lfo_hz(v);
            int c = (int)(hz * 100.0f + 0.5f);
            if (hz < 1.0f) {
                b = put_str(put_int(b, c / 100), ".");
                *b++ = (char)('0' + c % 100 / 10);
                *b++ = (char)('0' + c % 10);
                b = put_str(b, " HZ");
            } else if (hz < 10.0f) {
                b = put_tenths(b, hz, " HZ");
            } else {
                b = put_str(put_int(b, (int)(hz + 0.5f)), " HZ");
            }
            break;
        }
        case SF_TIME:
        case SF_GLIDE:
        case SF_FADE: {
            float ms = p->fmt == SF_GLIDE ? glide_ms(v) : time_ms(v);
            if (p->fmt != SF_TIME && !v)
                b = put_str(b, "OFF");
            else if (ms < 1000.0f)
                b = put_str(put_int(b, (int)(ms + 0.5f)), " MS");
            else
                b = put_tenths(b, ms / 1000.0f, " S");
            break;
        }
        case SF_HZ: {                     /* the engine's cutoffs: 30 Hz .. 16 kHz */
            float hz = 30.0f / exp_neg((float)v * 0.0494417f);
            if (hz < 1000.0f)
                b = put_str(put_int(b, (int)(hz + 0.5f)), " HZ");
            else
                b = put_tenths(b, hz / 1000.0f, " KHZ");
            break;
        }
        default:
            b = put_int(b, v);
            break;
        }
    }
    *b = 0;
}

/* ------------------------------------------------------------------ the sounds --- */
typedef struct {
    const char *name;
    uint8_t mono;
    int8_t v[SP_NUM_V1];
    const int8_t *ext;                    /* the synth update's parameters, pairs (SP_*, value) ending -1; the rest
                                           * at their defaults */
} syn_preset_t;
#define X(...) (const int8_t[]){__VA_ARGS__, -1}
#define MOD(n, s, d, a) SP_XM + 3 * ((n) - 1), s, SP_XM + 3 * ((n) - 1) + 1, d, SP_XM + 3 * ((n) - 1) + 2, a
/* WAVE DETUNE MIX INTV | SUB NOISE PWM DRIVE | CUT RES FENV FTYPE | KTR VEL SPREAD LEGATO | VOICES GLIDE |
 * AMP A D S R | FILTER A D S R; then the update's own */
static const syn_preset_t PRESETS[] = {
    {"SQR BASS", 1, {1, 0, 0, 0, 60, 0, 64, 40, 50, 70, 30, 0, 64, 64, 20, 1, 1, 0, 0, 55, 40, 30, 0, 50, 0, 30}, 0},
    /* ACID (0.4, for the 303 generator): LEGATO SLIDE, so a note glides only when it is tied; GLIDE 25 (58 ms) */
    {"ACID", 1, {0, 0, 0, 0, 0, 0, 64, 25, 42, 108, 48, 0, 64, 80, 20, 2, 1, 25, 0, 50, 90, 25, 0, 45, 0, 25}, 0},
    {"SUB BASS", 1, {3, 0, 0, 0, 40, 0, 64, 20, 40, 0, 0, 0, 0, 40, 0, 1, 1, 0, 2, 60, 110, 25, 0, 50, 40, 25}, 0},
    {"FAT BASS", 1, {0, 8, 64, -12, 50, 0, 64, 30, 55, 40, 35, 0, 64, 64, 25, 1, 3, 0, 0, 55, 90, 30, 0, 45, 20, 30}, 0},
    {"SAW LEAD", 1, {0, 12, 64, 0, 0, 0, 64, 10, 85, 30, 20, 0, 64, 64, 20, 1, 1, 30, 2, 60, 100, 45, 0, 55, 40, 45}, 0},
    {"SUPERSAW", 1, {0, 20, 64, 0, 0, 0, 64, 0, 95, 15, 10, 0, 64, 50, 60, 1, 4, 0, 5, 60, 110, 55, 0, 60, 80, 50}, 0},
    {"SYNC LEAD", 1, {5, 40, 100, 0, 0, 0, 64, 10, 100, 20, 10, 0, 64, 64, 20, 1, 1, 20, 2, 60, 100, 40, 0, 60, 60, 40}, 0},
    {"SOFT PAD", 0, {0, 20, 64, 0, 0, 4, 64, 0, 60, 10, 10, 0, 32, 40, 20, 0, 8, 0, 75, 80, 110, 85, 70, 80, 90, 85}, 0},
    {"PWM STRINGS", 0, {4, 8, 40, 0, 0, 0, 40, 0, 75, 20, 8, 0, 48, 40, 20, 0, 8, 0, 60, 80, 110, 80, 50, 70, 100, 80}, 0},
    {"PLUCK", 0, {0, 8, 50, 0, 0, 0, 64, 0, 30, 40, 55, 0, 64, 64, 20, 0, 8, 0, 0, 50, 0, 45, 0, 42, 0, 42}, 0},
    {"BRASS", 0, {0, 10, 64, 0, 0, 0, 64, 10, 45, 20, 40, 0, 64, 64, 20, 0, 8, 0, 30, 70, 90, 45, 35, 60, 60, 45}, 0},
    {"SINE KEYS", 0, {3, 6, 50, 0, 0, 0, 64, 0, 127, 0, 0, 0, 0, 64, 20, 0, 8, 0, 2, 65, 30, 55, 0, 50, 40, 40}, 0},
    {"RAVE STAB", 0, {4, 30, 64, 12, 0, 0, 64, 30, 85, 20, 10, 0, 64, 64, 20, 0, 8, 0, 0, 45, 0, 40, 0, 45, 0, 40}, 0},
    {"ORGAN", 0, {1, 3, 64, 12, 0, 0, 64, 0, 110, 0, 0, 0, 0, 20, 20, 0, 8, 0, 2, 20, 127, 20, 0, 20, 127, 20}, 0},
    {"WIND", 0, {0, 0, 0, 0, 0, 100, 64, 0, 60, 90, 20, 1, 0, 30, 20, 0, 8, 0, 60, 90, 60, 80, 60, 90, 60, 80}, 0},
    /* the synth update's: FM (OSC 2 the modulator, INTERVAL its ratio, MIX 0: OSC 1 alone heard), the SHAPER, drift,
     * LFO 2 and the matrix; MACRO 1 does something on each */
    {"FM BELL", 0, {3, 0, 0, 22, 0, 0, 64, 0, 127, 0, 0, 0, 0, 64, 20, 0, 8, 0, 0, 95, 0, 85, 0, 50, 40, 40},
     X(SP_FM, 15, SP_MD, 75, SP_MR, 60, MOD(1, MS_MENV, MD_FM, 40), MOD(2, MS_VEL, MD_FM, 20),
       MOD(3, MS_M1, MD_FM, 50))},
    {"FM BASS", 1, {3, 0, 0, 0, 0, 0, 64, 20, 90, 20, 10, 0, 64, 64, 20, 1, 1, 0, 0, 60, 100, 25, 0, 50, 60, 30},
     X(SP_FM, 30, SP_MD, 45, SP_MR, 30, MOD(1, MS_MENV, MD_FM, 45), MOD(2, MS_M1, MD_FM, 40))},
    {"FM KEYS", 0, {3, 0, 0, 12, 0, 0, 64, 0, 127, 0, 0, 0, 0, 64, 20, 0, 8, 0, 0, 85, 20, 60, 0, 50, 40, 40},
     X(SP_FM, 20, SP_MD, 55, SP_MR, 50, MOD(1, MS_MENV, MD_FM, 30), MOD(2, MS_VEL, MD_FM, 30),
       MOD(3, MS_M1, MD_RING, 40), SP_WAVE2, 4)},
    {"FOLD LEAD", 1, {2, 0, 0, 0, 0, 0, 64, 0, 110, 20, 10, 0, 64, 64, 20, 1, 1, 20, 2, 60, 110, 40, 0, 55, 60, 40},
     X(SP_SHAPE, 50, SP_MD, 70, SP_L2FADE, 60, MOD(1, MS_LFO2, MD_PITCH, 4), MOD(2, MS_MENV, MD_SHAPE, 30),
       MOD(3, MS_M1, MD_SHAPE, 60))},
    {"WEST PLUCK", 0, {3, 0, 0, 0, 0, 0, 64, 0, 70, 10, 40, 0, 64, 64, 20, 0, 8, 0, 0, 60, 0, 50, 0, 45, 0, 40},
     X(SP_SHAPE, 20, SP_MD, 50, MOD(1, MS_MENV, MD_SHAPE, 50), MOD(2, MS_VEL, MD_SHAPE, 30),
       MOD(3, MS_M1, MD_BIAS, 40))},
    {"RING METAL", 0, {0, 15, 0, 7, 0, 0, 64, 0, 110, 10, 0, 0, 32, 64, 20, 0, 8, 0, 0, 70, 0, 70, 0, 50, 40, 40},
     X(SP_WAVE2, 4, SP_RING, 110, SP_MD, 40, MOD(1, MS_MENV, MD_OSC2, -20), MOD(2, MS_M1, MD_OSC2, 24))},
    {"DRIFT PAD", 0, {0, 14, 64, 0, 0, 0, 64, 0, 60, 15, 15, 0, 32, 40, 20, 0, 8, 0, 70, 80, 110, 85, 80, 90, 100, 85},
     X(SP_DRIFT, 70, SP_L2RATE, 40, SP_L2KEY, 1, MOD(1, MS_LFO2, MD_PAN, 40), MOD(2, MS_LFO2, MD_CUTOFF, 8),
       MOD(3, MS_M1, MD_CUTOFF, 40))},
    {"CRUSH BASS", 1, {1, 0, 0, 0, 60, 0, 64, 0, 80, 30, 30, 0, 64, 64, 20, 1, 1, 0, 0, 55, 90, 30, 0, 45, 30, 30},
     X(SP_SHTYPE, 3, SP_SHAPE, 60, SP_SHPOS, 1, MOD(1, MS_M1, MD_SHAPE, 60))},
    {"WOBBLE", 1, {0, 8, 64, -12, 0, 0, 64, 40, 40, 60, 0, 0, 64, 64, 20, 1, 1, 0, 0, 60, 110, 30, 0, 50, 60, 30},
     X(SP_L2SHAPE, 1, SP_L2RATE, 80, MOD(1, MS_LFO2, MD_CUTOFF, 35), MOD(2, MS_M1, MD_L2RATE, 40))},
#include "synth_expansion.inc"
#define ENTRY(name,mono,color,vals,ext) {name,mono,vals,ext},
#define V(...) {__VA_ARGS__}
#include "preset_bank_house.inc"
#include "preset_bank_electro.inc"
#include "preset_bank_warehouse.inc"
#include "preset_bank_dub.inc"
#undef V
#undef ENTRY
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))
#undef X
#undef MOD

#define ENTRY(name,mono,color,vals,ext) SYN_PRESET_COLOR_##color,
static const uint8_t NEW_COLOR[] = {
#include "preset_bank_house.inc"
#include "preset_bank_electro.inc"
#include "preset_bank_warehouse.inc"
#include "preset_bank_dub.inc"
};
#undef ENTRY
#define OLD_FACTORY_TOTAL (SYN_FACTORY_LEGACY_COUNT + SYN_MOTOR_CITY_COUNT)
_Static_assert(sizeof NEW_COLOR == SYN_NEW_BANKS * SYN_BANK_SIZE, "four banks of 21 factory sounds");
_Static_assert(NPRESETS == OLD_FACTORY_TOTAL + (int)sizeof NEW_COLOR, "factory table and recipes agree");
int syn_preset_color(int i)
{
    return i >= OLD_FACTORY_TOTAL && i < NPRESETS ? NEW_COLOR[i - OLD_FACTORY_TOTAL] : SYN_PRESET_COLOR_KEEP;
}

int syn_presets(void) { return NPRESETS; }
const char *syn_preset_name(int i) { return i >= 0 && i < NPRESETS ? PRESETS[i].name : ""; }
void syn_defaults(int8_t *v, int from)
{
    int k;
    for (k = from; k < SP_NUM; k++)
        v[k] = PARAMS[k].def;
}
int syn_preset(int i, int8_t *v)
{
    const int8_t *e;
    int k;
    if (i < 0 || i >= NPRESETS)
        i = 0;
    for (k = 0; k < SP_NUM_V1; k++)
        v[k] = PRESETS[i].v[k];
    syn_defaults(v, SP_NUM_V1);
    for (e = PRESETS[i].ext; e && e[0] >= 0; e += 2)
        if (e[0] < SP_NUM)
            v[e[0]] = e[1];
    return PRESETS[i].mono;
}

/* ------------------------------------------------------------------- the host --- */
typedef struct {
    uint8_t trk;                          /* its track, 0: free */
    uint8_t held;                         /* held by a key until a note off */
    uint8_t uni, nuni;                    /* its place in a unison stack */
    uint8_t astage, fstage;               /* 1 attack, 2 decay / sustain, 3 release */
    uint8_t half;                         /* which half renders it this block */
    uint8_t choke;                        /* shed (syn_pool): fading out fast, whatever its release */
    int32_t gate;                         /* a gated note: samples left before it lets go */
    float aenv, fenv;                     /* 0..1 */
    float pc, pt;                         /* the pitch now and where it goes, 1/16 semitone */
    float vel;                            /* the velocity's gain */
    float pos, sinc;                      /* SAMPLER: the read position (frames) and its step a sample */
    int32_t p16;                          /* the pitch this block (syn_probe) */
    float gl, gr;                         /* pan gains (both 1: the centre) */
    int32_t amp;                          /* the amplitude a block ago, Q15 */
    int32_t fine;                         /* unison detune, 1/4096 of the phase increment */
    vmod_t m;                             /* this block's modulation */
    /* the synth update's */
    uint8_t mstage;                       /* MOD ENV: as astage */
    uint8_t usep;                         /* pv holds this voice's own engine parameters (the matrix moved some) */
    uint16_t drn;                         /* DRIFT: blocks before it heads somewhere new */
    float menv;                           /* MOD ENV, 0..1 */
    float l2ph, l2out, l2a, l2b, l2fade;  /* LFO 2: its phase 0..1, its value -1..1 (faded in), S&H / SMOOTH's values */
    float l2rate;                         /* the matrix's move of LFO 2's rate (steps), from the last block */
    float rnd, velr;                      /* RANDOM (-1..1) and VELOCITY (0..1): this note's */
    float dr, drt;                        /* DRIFT: where it has wandered, where it heads (-1..1) */
    float pan0;                           /* its place in a unison stack's spread, -1..1 */
    /* 0.4: the 303's marks (SN_*) */
    uint8_t slide;                        /* slid into: with GLIDE at 0, it glides in SLIDE_GLIDE's time */
    uint8_t acc;                          /* accented: louder for the note */
    float accenv;                         /* the accent's filter sweep, 1 at the note, falling (ACC_FALL a block) */
    track_t pv;                           /* the engine's parameters for this voice alone (usep) */
} vx_t;

static voice_t vv[NVOICE];
static vx_t vx[NVOICE];
static struct {
    int8_t v[SP_NUM];
    float level;
    uint8_t stack[8], nstack;             /* MONO: the keys held, the newest last */
    /* SAMPLER */
    uint8_t sampler, loop, root;
    const uint8_t *smp;
    uint32_t frames;
    float srate, tunef;                   /* the sample's rate / 44100; the tune as a factor */
    int16_t pm[3];                        /* start, end, loop start, thousandths */
    uint32_t s0, s1, sl;                  /* the same in frames */
    /* the LFO */
    uint8_t mdest;
    float mlfo, mdepth;
    uint8_t xm_on;                        /* a matrix slot in use (xm_scan): else the voices skip the matrix */
} tk[SYN_TRACKS];
static int pool = NVOICE;
static uint32_t ages;
static float ATK_INC[128], EXP_COEF[128], GLIDE_COEF[128], L2_INC[128];
/* the 303's marks (0.4): a slide with GLIDE at 0 glides at GLIDE 25's speed (58 ms); an accent is ACC_GAIN louder and
 * opens the filter ACC_CUT cutoff steps (about an octave and a quarter), falling away over about 200 ms */
#define SLIDE_GLIDE 25
#define ACC_GAIN 1.4f
#define ACC_CUT 20.0f
static float ACC_FALL;
static int nflags;                        /* the marks (SN_*) of the note being started */
static uint32_t srng = 0x6D2B79F5u;       /* RANDOM, S&H, DRIFT (the control tick only) */
static float rnd1(void)                   /* -1..1 */
{
    srng ^= srng << 13;
    srng ^= srng >> 17;
    srng ^= srng << 5;
    return (float)(int32_t)srng * (1.0f / 2147483648.0f);
}
static const float UNI_GAIN[5] = {1.0f, 1.0f, 0.75f, 0.62f, 0.54f};
/* each half's sums, a track at a time: its centred voices (mono), and its unison voices off the centre (L, R) */
static int32_t acc_m[2][SYN_TRACKS][SYN_CTL], acc_l[2][SYN_TRACKS][SYN_CTL], acc_r[2][SYN_TRACKS][SYN_CTL];
static int32_t acc_t[2][SYN_CTL];
static uint8_t used_m[2][SYN_TRACKS], used_lr[2][SYN_TRACKS];
static struct {
    float pl, pr, rs, ds;                 /* the track's pan gains and its sends */
} mix[SYN_TRACKS];

static void push(int t)                   /* the track's sound -> the engine's parameters */
{
    const int8_t *v = tk[t].v;
    int16_t *p = trk[t].p;
    p[P_E0] = v[SP_WAVE];
    p[P_E1] = v[SP_DETUNE];
    p[P_E2] = v[SP_MIX];
    p[P_E3] = v[SP_NOISE];
    p[P_E4] = v[SP_CUT];
    p[P_E5] = v[SP_RES];
    p[P_E6] = v[SP_DRIVE];
    p[P_E7] = v[SP_KTR];
    p[P_FTYPE] = v[SP_FTYPE];
    p[P_SUB] = v[SP_SUB];
    p[P_INTV] = v[SP_INTV];
    p[P_ATK] = v[SP_AA];
    p[P_REL] = v[SP_AR];
    p[P_WAVE2] = v[SP_WAVE2];
    p[P_FM] = v[SP_FM];
    p[P_RING] = v[SP_RING];
    p[P_SHT] = v[SP_SHTYPE];
    p[P_SHA] = v[SP_SHAPE];
    p[P_BIAS] = v[SP_BIAS];
    p[P_SHPOS] = v[SP_SHPOS];
    p[P_O2] = 0;
}

/* the LFO on the engine's own parameters: the track's values, moved, for this block */
static void push_mod(int t)
{
    static const int8_t P_OF[SM_NUM] = {-1, -1, -1, -1, P_E5, P_E1, P_E2, P_E6, P_E3, P_SUB, -1};
    int d = tk[t].mdest, k;
    push(t);
    if (d < 0 || d >= SM_NUM || (k = P_OF[d]) < 0)
        return;
    trk[t].p[k] = (int16_t)clamp(trk[t].p[k] + (int32_t)(tk[t].mlfo * tk[t].mdepth * 64.0f), 0, 127);
}

void syn_mod(int t, int dest, float lfo, float depth)
{
    if (t < 1 || t >= SYN_TRACKS)
        return;
    tk[t].mdest = (uint8_t)(dest >= 0 && dest < SM_NUM ? dest : SM_CUTOFF);
    tk[t].mlfo = lfo < -1.0f ? -1.0f : lfo > 1.0f ? 1.0f : lfo;
    tk[t].mdepth = depth < -1.0f ? -1.0f : depth > 1.0f ? 1.0f : depth;
}

int syn_get(int t, int id) { return t >= 1 && t < SYN_TRACKS && id >= 0 && id < SP_NUM ? tk[t].v[id] : 0; }

int syn_probe(int t, int what)
{
    int i, best = -1;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk == t && vx[i].astage && (best < 0 || (int32_t)(vv[i].age - vv[best].age) > 0))
            best = i;
    if (best < 0)
        return -1;
    switch (what) {
    case 0: return vx[best].p16;
    case 1: return vx[best].m.cutoff + (trk[t].p[P_E4] << 8);
    case 2: return (int)(vx[best].menv * 1000.0f);
    case 3: return (int)(vx[best].l2out * 1000.0f);
    case 4: return vx[best].amp;
    case 5: return (int)((vx[best].gr - vx[best].gl) * 1000.0f);
    case 6: return vx[best].m.fine;
    case 7: return (int)(vx[best].accenv * 1000.0f);
    case 8: return vx[best].acc;
    default: break;
    }
    if (what >= 100 && what < 100 + SP_NUM) {   /* a parameter as this voice plays it */
        static const int8_t P_OF[SP_NUM_V1 + 8] = {P_E0, P_E1, P_E2, P_INTV, P_SUB, P_E3, -1, P_E6, -1, P_E5, -1,
                                                   P_FTYPE, P_E7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                                   P_WAVE2, P_FM, P_RING, -1, P_SHT, P_SHA, P_BIAS, P_SHPOS};
        int id = what - 100, k = id < (int)sizeof P_OF ? P_OF[id] : -1;
        const track_t *tp = vx[best].usep ? &vx[best].pv : &trk[t];
        if (id == SP_PWM)
            return vx[best].m.shape >> 8;
        return k >= 0 ? tp->p[k] : tk[t].v[id];
    }
    return -1;
}

static void xm_scan(int t);

void syn_init(void)
{
    int i, t;
    for (i = 0; i < 128; i++) {
        float a = BLOCK_MS / time_ms(i);
        ATK_INC[i] = a > 1.0f ? 1.0f : a;
        EXP_COEF[i] = exp_neg(4.6f * BLOCK_MS / time_ms(i));
        GLIDE_COEF[i] = i ? 1.0f - exp_neg(3.0f * BLOCK_MS / glide_ms(i)) : 1.0f;
        L2_INC[i] = lfo_hz(i) * (float)SYN_CTL / 44100.0f;
    }
    ACC_FALL = exp_neg(BLOCK_MS / 200.0f);
    syn_kill();
    for (t = 0; t < SYN_TRACKS; t++) {
        for (i = 0; i < SP_NUM; i++)
            tk[t].v[i] = PARAMS[i].def;
        tk[t].level = 0.8f;
        mix[t].pl = mix[t].pr = 1.0f;
        mix[t].rs = mix[t].ds = 0.0f;
        tk[t].sampler = 0;
        tk[t].smp = 0;
        tk[t].frames = 0;
        tk[t].srate = tk[t].tunef = 1.0f;
        tk[t].root = 60;
        tk[t].pm[1] = 1000;
        xm_scan(t);
        push(t);
    }
    pool = NVOICE;
}

static void xm_scan(int t)                /* whether any of the track's matrix slots does anything (matrix()'s test) */
{
    const int8_t *s = tk[t].v;
    int k, on = 0;
    for (k = 0; k < SYN_SLOTS; k++) {
        int src = s[SP_XM + 3 * k], dst = s[SP_XM + 3 * k + 1], amt = s[SP_XM + 3 * k + 2];
        on |= !(!src || !amt || dst < 0 || dst >= MD_NUM || src >= MS_NUM);
    }
    tk[t].xm_on = (uint8_t)on;
}

void syn_sound(int t, const int8_t *v)
{
    int i;
    if (t < 1 || t >= SYN_TRACKS)
        return;
    for (i = 0; i < SP_NUM; i++)
        tk[t].v[i] = (int8_t)clamp(v[i], PARAMS[i].min, PARAMS[i].max);
    xm_scan(t);
    push(t);
}

void syn_set(int t, int id, int v)
{
    if (t < 1 || t >= SYN_TRACKS || id < 0 || id >= SP_NUM)
        return;
    tk[t].v[id] = (int8_t)clamp(v, PARAMS[id].min, PARAMS[id].max);
    if (id >= SP_XM)
        xm_scan(t);
    push(t);
}

/* ------------------------------------------------------------- SAMPLER source --- */
GM_INLINE int32_t s12_q15(const uint8_t *d, uint32_t i)   /* frame i, Q15 */
{
    const uint8_t *b = d + (i >> 1) * 3u;
    int32_t v = (i & 1u) ? (int32_t)(b[1] >> 4) | (int32_t)b[2] << 4 : (int32_t)b[0] | (int32_t)(b[1] & 0x0Fu) << 8;
    if (v & 0x800)
        v -= 4096;
    return v * 16;                        /* (a negative << is undefined in C) */
}

static uint32_t zero_near(const uint8_t *d, uint32_t at, uint32_t lo, uint32_t hi)   /* a rising zero crossing */
{
    uint32_t k;
    for (k = 0; k < 256u; k++) {
        if (at + k + 1u < hi && s12_q15(d, at + k) <= 0 && s12_q15(d, at + k + 1u) > 0)
            return at + k + 1u;
        if (at >= lo + k + 1u && s12_q15(d, at - k - 1u) <= 0 && s12_q15(d, at - k) > 0)
            return at - k;
    }
    return at;
}

static void shape(int t)                  /* the thousandths -> frames */
{
    uint32_t n = tk[t].frames, s0, s1, sl;
    if (n < 2u) {
        tk[t].s0 = tk[t].s1 = tk[t].sl = 0;
        return;
    }
    s0 = (uint32_t)tk[t].pm[0] * n / 1000u;
    s1 = (uint32_t)tk[t].pm[1] * n / 1000u;
    if (s1 > n)
        s1 = n;
    if (s0 + 2u > s1)                     /* at least two frames, start before end */
        s0 = s1 >= 2u ? s1 - 2u : 0;
    if (s0 + 2u > s1)
        s1 = s0 + 2u;
    sl = (uint32_t)tk[t].pm[2] * n / 1000u;
    if (sl < s0)
        sl = s0;
    if (sl + 2u > s1)
        sl = s1 - 2u;
    if (tk[t].smp)
        sl = zero_near(tk[t].smp, sl, s0, s1 - 1u);
    tk[t].s0 = s0;
    tk[t].s1 = s1;
    tk[t].sl = sl;
}

static void free_track(int t)             /* the track's voices, gone at once (its source changed) */
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk == t) {
            vx[i].trk = 0;
            vx[i].astage = vx[i].fstage = vx[i].mstage = 0;
            vv[i].active = 0;
        }
    tk[t].nstack = 0;
}

void syn_source(int t, int sampler)
{
    if (t < 1 || t >= SYN_TRACKS || tk[t].sampler == (sampler != 0))
        return;
    free_track(t);
    tk[t].sampler = (uint8_t)(sampler != 0);
}

void syn_sample(int t, const uint8_t *packed, uint32_t frames, uint32_t rate)
{
    if (t < 1 || t >= SYN_TRACKS)
        return;
    free_track(t);
    tk[t].smp = frames >= 2u ? packed : 0;
    tk[t].frames = tk[t].smp ? frames : 0;
    tk[t].srate = (float)(rate ? rate : 44100u) / 44100.0f;
    shape(t);
}

void syn_sample_shape(int t, int start, int end, int loop, int lstart, int root, int tune)
{
    if (t < 1 || t >= SYN_TRACKS)
        return;
    tk[t].pm[0] = (int16_t)clamp(start, 0, 1000);
    tk[t].pm[1] = (int16_t)clamp(end, 0, 1000);
    tk[t].pm[2] = (int16_t)clamp(lstart, 0, 1000);
    tk[t].loop = (uint8_t)(loop != 0);
    tk[t].root = (uint8_t)clamp(root, 0, 127);
    tk[t].tunef = 1.0f + (float)clamp(tune, -50, 50) * 0.000578f;   /* 2^(c / 1200), near enough for +-50 cents */
    shape(t);
}

uint32_t syn_sample_frames(int t) { return t >= 1 && t < SYN_TRACKS ? tk[t].frames : 0; }

/* a SAMPLER voice's block: the sample read at its step, linearly between frames, round its loop or ending at its end,
 * then the drive and the filter as ANALOG's, and the amplitude */
GM_RAM static void sample_render(int t, int i, const track_t *tp, int32_t *out, uint32_t n)
{
    voice_t *v = &vv[i];
    vx_t *x = &vx[i];
    const int16_t *p = tp->p;
    const uint8_t *d = tk[t].smp;
    float pos = x->pos, inc = x->sinc, end = (float)tk[t].s1, lst = (float)tk[t].sl;
    uint32_t last = tk[t].frames - 1u, k, ft = (uint32_t)p[P_FTYPE];
    int loop = tk[t].loop && tk[t].s1 > tk[t].sl + 16u;
    int32_t cut = (p[P_E4] << 8) + x->m.cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    int32_t ic1 = v->s[0], ic2 = v->s[1], drv = p[P_E6], drive = 32768 + p[P_E6] * 512, fk = ANALOG_K(p[P_E5]);
    tsvf_t flt;
    shp_t sh;
    uint32_t shp = p[P_SHA] ? 1u + (uint32_t)(p[P_SHPOS] != 0) : 0u;   /* the SHAPER: 0 off, 1 PRE, 2 POST */
    if (!d)
        return;
    if (shp)
        shp_begin(&sh, p, v, x->m.inc);
    tsvf_coef(&flt, cut, p[P_E5]);
    for (k = 0; k < n; k++) {
        int32_t a, b, s, y;
        uint32_t ip;
        if (pos >= end) {
            if (!loop) {                  /* once through: the voice is done (freed at the next block) */
                x->astage = x->fstage = x->mstage = 3;
                x->aenv = 0.0f;
                break;
            }
            pos = lst + (pos - end);
            if (pos >= end)
                pos = lst;
        }
        ip = (uint32_t)pos;
        a = s12_q15(d, ip);
        b = s12_q15(d, ip < last ? ip + 1u : ip);
        s = a + (int32_t)((float)(b - a) * (pos - (float)ip));
        pos += inc;
        if (shp == 1u)
            s = shp_run(&sh, s);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        y = soft_knee(analog_tap(&flt, fk, s >> 1, &ic1, &ic2, ft), 16000);
        if (shp == 2u)
            y = clamp(shp_run(&sh, y), -32767, 32767);
        out[k] += voice_amp(y * 2, &x->m, k) * 2;
    }
    x->pos = pos;
    v->s[0] = ic1;
    v->s[1] = ic2;
    if (shp)
        shp_end(&sh, v);
}

GM_INLINE void render_voice(int i, int32_t *out)
{
    int t = vx[i].trk;
    const track_t *tp = vx[i].usep ? &vx[i].pv : &trk[t];   /* the matrix moved some of its parameters: its own */
    if (tk[t].sampler)
        sample_render(t, i, tp, out, SYN_CTL);
    else                                  /* (ENG_ANALOG.render, called directly: RAM code calls no pointer) */
        analog_render((track_t *)tp, &vv[i], out, SYN_CTL, &vx[i].m);
}

void syn_mixer(int t, int pan, float rev, float dly)
{
    float p;
    if (t < 1 || t >= SYN_TRACKS)
        return;
    p = (float)(clamp(pan, 1, 127) - 64) / 63.0f;   /* a balance: the centre keeps both sides whole */
    mix[t].pl = p > 0.0f ? 1.0f - p : 1.0f;
    mix[t].pr = p < 0.0f ? 1.0f + p : 1.0f;
    mix[t].rs = rev < 0.0f ? 0.0f : rev;
    mix[t].ds = dly < 0.0f ? 0.0f : dly;
}

void syn_level(int t, float level)
{
    if (t >= 1 && t < SYN_TRACKS)
        tk[t].level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
}

static void rel(int i)                    /* a voice lets go */
{
    if (vx[i].astage && vx[i].astage < 3) {
        vx[i].astage = vx[i].fstage = vx[i].mstage = 3;
        vx[i].held = 0;
        vx[i].gate = 0;
        vv[i].gate = 0;
    }
}

/* the voices sounding that the pool counts: not the ones shed and fading out (syn_pool) */
static int pool_used(void)
{
    int i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += vx[i].trk && !vx[i].choke;
    return n;
}

/* the voice to give up, of those the pool counts (not already shed): the quietest letting go, else the oldest, of
 * those not on track `except` (0: any) */
static int voice_victim(int except)
{
    int i, best = -1;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk && !vx[i].choke && vx[i].trk != except && vx[i].astage == 3 &&
            (best < 0 || vx[i].aenv < vx[best].aenv))
            best = i;
    if (best >= 0)
        return best;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk && !vx[i].choke && vx[i].trk != except &&
            (best < 0 || (int32_t)(vv[i].age - vv[best].age) < 0))
            best = i;
    return best;
}

/* a voice for a new note: a free one while the pool has room (0.5, the budgets: the pool is a count of voices
 * sounding, not the first `pool` slots, so shedding picks which voice goes); else the one to give up */
static int voice_take(int except)
{
    int i;
    if (pool_used() < pool) {
        for (i = 0; i < NVOICE; i++)
            if (!vx[i].trk)
                return i;
        for (i = 0; i < NVOICE; i++)      /* every slot taken, some by voices shed and fading: one of those */
            if (vx[i].choke && vx[i].trk != except)
                return i;
    }
    return voice_victim(except);
}

static void voice_start(int i, int t, int note, float vel, int32_t gate, int uni, int nuni, int glide)
{
    voice_t *v = &vv[i];
    vx_t *x = &vx[i];
    const int8_t *s = tk[t].v;
    int sounding = x->trk == t && x->astage;   /* the track's own voice, still sounding: phases and filter go on */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t s0 = v->s[0], s1 = v->s[1];
    float spread, pan;
    if (!sounding) {                      /* the engine wants a new voice's state zero */
        uint32_t k;
        for (k = 0; k < sizeof *v; k++)
            ((uint8_t *)v)[k] = 0;
        x->aenv = x->fenv = 0.0f;
        x->amp = 0;
        x->menv = 0.0f;
        x->dr = x->drt = 0.0f;
        x->drn = 0;
        x->l2b = rnd1();
        x->l2rate = 0.0f;
    }
    v->note = (uint8_t)note;
    v->vel = (uint8_t)(vel * 127.0f);
    v->gate = 1;
    v->active = 1;
    v->age = ++ages;
    ENG_ANALOG.note_on(&trk[t], v);
    if (sounding) {                       /* engine_t.keep: the phases and the filter (s[0], s[1]) */
        v->ph[0] = ph0;
        v->ph[1] = ph1;
        v->ph[2] = ph2;
        v->s[0] = s0;
        v->s[1] = s1;
    }
    x->pos = (float)tk[t].s0;             /* SAMPLER: from its start (the LFO's START moves it) */
    if (tk[t].mdest == SM_START && tk[t].mdepth != 0.0f && tk[t].s1 > tk[t].s0 + 2u) {
        float p = x->pos + tk[t].mlfo * tk[t].mdepth * 0.5f * (float)(tk[t].s1 - tk[t].s0);
        x->pos = p < 0.0f ? 0.0f : p > (float)(tk[t].s1 - 2u) ? (float)(tk[t].s1 - 2u) : p;
    }
    x->trk = (uint8_t)t;
    x->choke = 0;
    x->held = (uint8_t)(gate < 0);
    x->gate = gate < 0 ? 0 : gate > 0 ? gate : 1;
    x->uni = (uint8_t)uni;
    x->nuni = (uint8_t)nuni;
    x->astage = x->fstage = x->mstage = 1;
    x->velr = vel;
    x->slide = 0;
    x->acc = (uint8_t)((nflags & SN_ACCENT) != 0);
    x->accenv = x->acc ? 1.0f : 0.0f;
    x->rnd = rnd1();
    x->l2ph = s[SP_L2KEY] ? (rnd1() + 1.0f) * 0.5f : 0.0f;   /* LFO 2: from its start each note, or anywhere */
    x->l2a = x->l2b;
    x->l2b = rnd1();
    x->l2fade = s[SP_L2FADE] ? 0.0f : 1.0f;
    x->pt = (float)(note * 16);
    if (!(glide && sounding))
        x->pc = x->pt;
    x->vel = 1.0f - (float)s[SP_VEL] / 127.0f * (1.0f - vel);
    spread = nuni > 1 ? 2.0f * (float)uni / (float)(nuni - 1) - 1.0f : 0.0f;
    x->fine = (int32_t)(spread * (float)s[SP_SPREAD] * (50.0f / 127.0f) * 2.367f);   /* up to +-50 cents */
    pan = spread * 0.8f;
    x->pan0 = pan;
    x->gl = pan > 0.0f ? 1.0f - pan : 1.0f;
    x->gr = pan < 0.0f ? 1.0f + pan : 1.0f;
}

static void retarget(int i, int note, int32_t gate, int glide)   /* a tied note: the pitch moves, no new attack */
{
    vx[i].slide = (uint8_t)((nflags & SN_SLIDE) != 0);
    vx[i].acc = (uint8_t)((nflags & SN_ACCENT) != 0);
    if (vx[i].acc)
        vx[i].accenv = 1.0f;
    vx[i].pt = (float)(note * 16);
    if (!glide)
        vx[i].pc = vx[i].pt;
    vv[i].note = (uint8_t)note;
    vx[i].held = (uint8_t)(gate < 0);
    vx[i].gate = gate < 0 ? 0 : gate > 0 ? gate : 1;
}

static void poly_on(int t, int note, float vel, int32_t gate)
{
    int i, n = 0, cap = clamp(tk[t].v[SP_VOICES], 1, 8), same = -1, oldest = -1;
    for (i = 0; i < NVOICE; i++) {
        if (vx[i].trk != t || !vx[i].astage)
            continue;
        if (vv[i].note == note)
            same = i;
        if (vx[i].astage < 3) {
            n++;
            if (oldest < 0 || (int32_t)(vv[i].age - vv[oldest].age) < 0)
                oldest = i;
        }
    }
    if (same >= 0)                        /* the same note again: its own voice */
        i = same;
    else if (n >= cap)                    /* the track's cap: its oldest note gives way */
        i = oldest;
    else
        i = voice_take(0);
    if (i >= 0)
        voice_start(i, t, note, vel, gate, 0, 1, 0);
}

static void mono_on(int t, int note, float vel, int32_t gate)
{
    int U = clamp(tk[t].v[SP_VOICES], 1, 4), have[4] = {-1, -1, -1, -1}, gated = 0, i, u;
    int glide = tk[t].v[SP_GLIDE] != 0;
    for (i = 0; i < NVOICE; i++) {
        vx_t *x = &vx[i];
        if (x->trk != t || !x->astage)
            continue;
        if (x->nuni == U && x->uni < U && have[x->uni] < 0) {
            have[x->uni] = i;
            gated |= x->astage < 3;
        } else {
            rel(i);                       /* a voice of another stack size: lets go */
        }
    }
    for (u = 0; u < U; u++) {
        i = have[u];
        if (i >= 0 && gated && tk[t].v[SP_LEGATO]) {
            retarget(i, note, gate, glide);
            continue;
        }
        if (i < 0)
            i = voice_take(t);
        if (i >= 0)                       /* (LEGATO SLIDE: a note not tied starts where it is, as a 303's) */
            voice_start(i, t, note, vel, gate, u, U, glide && tk[t].v[SP_LEGATO] != 2);
    }
}

static void stack_drop(int t, int note)
{
    int i, j;
    for (i = j = 0; i < tk[t].nstack; i++)
        if (tk[t].stack[i] != note)
            tk[t].stack[j++] = tk[t].stack[i];
    tk[t].nstack = (uint8_t)j;
}

static void stack_push(int t, int note)
{
    int i;
    stack_drop(t, note);
    if (tk[t].nstack == sizeof tk[t].stack) {
        for (i = 1; i < (int)sizeof tk[t].stack; i++)
            tk[t].stack[i - 1] = tk[t].stack[i];
        tk[t].nstack--;
    }
    tk[t].stack[tk[t].nstack++] = (uint8_t)note;
}

/* SN_SLIDE: the track's note still held (MONO: its unison stack; POLY: its newest) glides to note, whatever LEGATO
 * says, with no new attack; 0 if none is held (the note then starts as any other) */
static int slide_to(int t, int note, int32_t gate, int mono)
{
    int i, n = 0, newest = -1;
    for (i = 0; i < NVOICE; i++) {
        if (vx[i].trk != t || !vx[i].astage || vx[i].astage >= 3)
            continue;
        if (mono) {
            retarget(i, note, gate, 1);
            n++;
        } else if (newest < 0 || (int32_t)(vv[i].age - vv[newest].age) > 0) {
            newest = i;
        }
    }
    if (newest >= 0) {
        retarget(newest, note, gate, 1);
        n++;
    }
    return n;
}

void syn_note_on(int t, int note, float vel, int32_t gate, int mono) { syn_note_on_f(t, note, vel, gate, mono, 0); }

void syn_note_on_f(int t, int note, float vel, int32_t gate, int mono, int flags)
{
    if (t < 1 || t >= SYN_TRACKS || note < 1 || note > 127 || (tk[t].sampler && !tk[t].smp))
        return;
    nflags = flags;
    if ((flags & SN_SLIDE) && slide_to(t, note, gate, mono)) {
        nflags = 0;
        return;
    }
    if (mono) {
        if (gate < 0)
            stack_push(t, note);
        mono_on(t, note, vel, gate);
    } else {
        poly_on(t, note, vel, gate);
    }
    nflags = 0;
}

void syn_note_off(int t, int note)
{
    int i, top;
    if (t < 1 || t >= SYN_TRACKS)
        return;
    if (tk[t].nstack) {                   /* MONO: the stack of keys held */
        top = tk[t].stack[tk[t].nstack - 1];
        stack_drop(t, note);
        if (top != note)
            return;                       /* a key under the top let go: nothing changes */
        if (tk[t].nstack) {               /* back down to the key below, tied */
            top = tk[t].stack[tk[t].nstack - 1];
            for (i = 0; i < NVOICE; i++)
                if (vx[i].trk == t && vx[i].held && vx[i].astage < 3)
                    retarget(i, top, -1, tk[t].v[SP_GLIDE] != 0);
            return;
        }
    }
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk == t && vx[i].held && vv[i].note == note)
            rel(i);
}

void syn_release(int t, int seq_only)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk && (t < 0 || vx[i].trk == t) && (!seq_only || !vx[i].held))
            rel(i);
    if (!seq_only)
        for (i = 1; i < SYN_TRACKS; i++)
            if (t < 0 || i == t)
                tk[i].nstack = 0;
}

void syn_kill(void)
{
    int i;
    for (i = 0; i < NVOICE; i++) {
        vx[i].trk = 0;
        vx[i].astage = vx[i].fstage = vx[i].mstage = 0;
        vv[i].active = 0;
    }
    for (i = 0; i < SYN_TRACKS; i++)
        tk[i].nstack = 0;
}

/* the voices that may sound. Over it, the quietest letting go, then the oldest, are shed: let go and faded out in
 * about 10 ms whatever their release (a pad's long release kept a shed voice rendering for seconds) */
void syn_pool(int n)
{
    int i;
    pool = clamp(n, 1, NVOICE);
    while (pool_used() > pool && (i = voice_victim(0)) >= 0) {
        rel(i);
        vx[i].astage = vx[i].fstage = vx[i].mstage = 3;
        vx[i].choke = 1;
    }
}

int syn_voices(int t)
{
    int i, c = 0;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk && vx[i].astage && (t < 0 || vx[i].trk == t))
            c++;
    return c;
}

int syn_pitch16(int t)
{
    int i, best = -1;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk == t && vx[i].astage && (best < 0 || (int32_t)(vv[i].age - vv[best].age) > 0))
            best = i;
    return best < 0 ? -1 : (int)(vx[best].pc + 0.5f);
}

/* ---------------------------------------------------------------- a block --- */
static float env_step(uint8_t *stage, float e, const int8_t *s, int a, int d, int su, int r)
{
    switch (*stage) {
    case 1:
        e += ATK_INC[s[a]];
        if (e >= 1.0f) {
            e = 1.0f;
            *stage = 2;
        }
        break;
    case 2: {
        float sus = (float)s[su] * (1.0f / 127.0f);
        e = sus + (e - sus) * EXP_COEF[s[d]];
        break;
    }
    default:
        e *= EXP_COEF[s[r]];
        break;
    }
    return e;
}

/* LFO 2's shapes (app.c lfo_wave's): -1..1; SINE and TRI start at 0, rising */
static float lfo2_wave(int shape, float ph, float a, float b)
{
    switch (shape) {
    case 0: {                             /* SINE: two parabolas */
        float u = 2.0f * ph;
        return u < 1.0f ? 4.0f * u * (1.0f - u) : -4.0f * (u - 1.0f) * (2.0f - u);
    }
    case 1: return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    case 2: return 2.0f * ph - 1.0f;
    case 3: return 1.0f - 2.0f * ph;
    case 4: return ph < 0.5f ? 1.0f : -1.0f;
    case 5: return a;                     /* S&H */
    default: return a + (b - a) * ph;     /* SMOOTH */
    }
}

/* the matrix, for one voice: each destination's move (MD_*), the sources times the amounts (PITCH and OSC 2 squared) */
static void matrix(int t, int i, float *acc)
{
    const int8_t *s = tk[t].v;
    const vx_t *x = &vx[i];
    int k;
    for (k = 0; k < MD_NUM; k++)
        acc[k] = 0.0f;
    for (k = 0; k < SYN_SLOTS; k++) {
        int src = s[SP_XM + 3 * k], dst = s[SP_XM + 3 * k + 1], amt = s[SP_XM + 3 * k + 2];
        float sv, a;
        if (!src || !amt || dst < 0 || dst >= MD_NUM || src >= MS_NUM)
            continue;
        switch (src) {
        case MS_LFO1: sv = tk[t].mlfo; break;
        case MS_LFO2: sv = x->l2out; break;
        case MS_MENV: sv = x->menv; break;
        case MS_FENV: sv = x->fenv; break;
        case MS_AENV: sv = x->aenv; break;
        case MS_VEL: sv = x->velr; break;
        case MS_NOTE: sv = ((float)vv[i].note - 60.0f) * (1.0f / 60.0f); break;
        case MS_RND: sv = x->rnd; break;
        default: sv = (float)s[SP_M1 + src - MS_M1] * (1.0f / 127.0f); break;
        }
        a = (float)amt * (1.0f / 64.0f);
        if (dst == MD_PITCH || dst == MD_OSC2)
            a *= a < 0.0f ? -a : a;
        acc[dst] += sv * a;
    }
}

void syn_prepare(void)
{
    int i, h = 0;
    for (i = 1; i < SYN_TRACKS; i++)
        push_mod(i);
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &vv[i];
        vx_t *x = &vx[i];
        const int8_t *s;
        static const float ACC0[MD_NUM];  /* no slot in use: every destination unmoved */
        float a, accb[MD_NUM], pan;
        const float *acc = ACC0;
        int32_t amp, p16, mfine, t;
        uint32_t inc;
        if (!x->trk)
            continue;
        t = x->trk;
        s = tk[t].v;
        if (!x->held && x->gate > 0 && (x->gate -= SYN_CTL) <= 0)
            rel(i);
        x->aenv = env_step(&x->astage, x->aenv, s, SP_AA, SP_AD, SP_AS, SP_AR);
        if (x->choke)                     /* shed: down by half every 0.73 ms, silent in about 10 */
            x->aenv *= 0.5f;
        if (x->astage == 3 && x->aenv < 0.0003f) {   /* faded out: free */
            x->trk = 0;
            x->astage = x->fstage = x->mstage = 0;
            v->active = 0;
            continue;
        }
        x->fenv = env_step(&x->fstage, x->fenv, s, SP_FA, SP_FD, SP_FS, SP_FR);
        x->menv = env_step(&x->mstage, x->menv, s, SP_MA, SP_MD, SP_MS, SP_MR);
        {   /* LFO 2: each note's own */
            int ri = clamp(s[SP_L2RATE] + (int32_t)x->l2rate, 0, 127);
            x->l2ph += L2_INC[ri];
            if (x->l2ph >= 1.0f) {
                x->l2ph -= (float)(int32_t)x->l2ph;
                x->l2a = x->l2b;
                x->l2b = rnd1();
            }
            if (x->l2fade < 1.0f && (x->l2fade += ATK_INC[s[SP_L2FADE]]) > 1.0f)
                x->l2fade = 1.0f;
            x->l2out = lfo2_wave(s[SP_L2SHAPE], x->l2ph, x->l2a, x->l2b) * x->l2fade;
        }
        if (s[SP_DRIFT]) {                /* DRIFT: a slow wander, somewhere new every 0.3 .. 1 s */
            if (!x->drn) {
                x->drt = rnd1();
                x->drn = (uint16_t)(400u + (srng >> 22));
            }
            x->drn--;
            x->dr += (x->drt - x->dr) * 0.003f;
        }
        if (tk[t].xm_on) {                /* (0.5: no slot in use, no matrix: the same zeros without the work) */
            matrix(t, i, accb);
            acc = accb;
        }
        x->l2rate = acc[MD_L2RATE] * 127.0f;
        if (x->pc != x->pt) {
            float d = x->pt - x->pc;
            x->pc += d * GLIDE_COEF[s[SP_GLIDE] || !x->slide ? s[SP_GLIDE] : SLIDE_GLIDE];
            if (d < 0.5f && d > -0.5f)
                x->pc = x->pt;
        }
        mfine = x->fine;
        {
            int32_t mp = tk[t].mdest == SM_PITCH ? (int32_t)(tk[t].mlfo * tk[t].mdepth * 192.0f) : 0;
            if (acc[MD_PITCH] == 0.0f && !s[SP_DRIFT]) {
                p16 = clamp((int32_t)(x->pc + 0.5f) + mp, 0, 2047);
            } else {                      /* the matrix's and DRIFT's moves: between 1/16 semitones by the fine tune */
                float pf = x->pc + (float)mp + acc[MD_PITCH] * 768.0f;
                pf = pf < 0.0f ? 0.0f : pf > 2047.0f ? 2047.0f : pf;
                p16 = (int32_t)pf;
                mfine += (int32_t)((pf - (float)p16) * 14.786f) +
                         (int32_t)(x->dr * (float)s[SP_DRIFT] * (35.5f / 127.0f));
            }
        }
        x->p16 = p16;
        inc = pitch_inc((uint32_t)p16);
        if (mfine)
            inc += (uint32_t)((int32_t)(inc >> 12) * mfine);
        v->pitch16 = v->pitch_cur = p16;
        x->m.inc = inc;
        if (tk[t].sampler)                /* the sample's step: the note over its root, its rate, the tune, unison */
            x->sinc = (float)pitch_inc((uint32_t)p16) / (float)pitch_inc((uint32_t)tk[t].root * 16u) * tk[t].srate *
                      tk[t].tunef * (1.0f + (float)mfine * (1.0f / 4096.0f));
        x->m.pitch16 = p16;
        x->m.fine = mfine;
        a = x->aenv * x->vel * tk[t].level * UNI_GAIN[x->nuni <= 4 ? x->nuni : 4];
        if (tk[t].mdest == SM_LEVEL) {    /* tremolo: a dip of up to all of it, at the LFO's low (or high) */
            float d = tk[t].mdepth, l = d >= 0.0f ? tk[t].mlfo : -tk[t].mlfo;
            a *= 1.0f - (d >= 0.0f ? d : -d) * 0.5f * (1.0f - l);
        }
        if (acc[MD_LEVEL] != 0.0f) {
            float g = 1.0f + acc[MD_LEVEL];
            a *= g < 0.0f ? 0.0f : g > 2.0f ? 2.0f : g;
        }
        if (x->acc)                       /* an accent (a 303's): louder */
            a *= ACC_GAIN;
        amp = (int32_t)(a * 32767.0f);
        if (amp > 32767)
            amp = 32767;
        x->m.amp0 = x->amp;
        x->m.amp1 = amp;
        x->amp = amp;
        x->m.cutoff = (int32_t)(x->fenv * (float)s[SP_FENV] * (127.0f * 256.0f / 64.0f));
        x->m.shape = (int32_t)s[SP_PWM] << 8;
        if (tk[t].mdest == SM_CUTOFF)
            x->m.cutoff += (int32_t)(tk[t].mlfo * tk[t].mdepth * (64.0f * 256.0f));
        else if (tk[t].mdest == SM_PWM)
            x->m.shape = clamp(x->m.shape + (int32_t)(tk[t].mlfo * tk[t].mdepth * (63.0f * 256.0f)), 0,
                               127 << 8);
        if (acc[MD_CUTOFF] != 0.0f)
            x->m.cutoff += (int32_t)(acc[MD_CUTOFF] * (127.0f * 256.0f));
        if (s[SP_DRIFT])
            x->m.cutoff += (int32_t)(x->dr * (float)s[SP_DRIFT] * (3.0f * 256.0f / 127.0f));
        if (x->accenv > 0.0f) {           /* and its sweep */
            x->m.cutoff += (int32_t)(x->accenv * (ACC_CUT * 256.0f));
            x->accenv = x->accenv > 0.001f ? x->accenv * ACC_FALL : 0.0f;
        }
        if (acc[MD_PWM] != 0.0f)
            x->m.shape = clamp(x->m.shape + (int32_t)(acc[MD_PWM] * (127.0f * 256.0f)), 0, 127 << 8);
        pan = x->pan0 + acc[MD_PAN];      /* (a unison stack's spread, the matrix's PAN on it) */
        pan = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
        x->gl = pan > 0.0f ? 1.0f - pan : 1.0f;
        x->gr = pan < 0.0f ? 1.0f + pan : 1.0f;
        x->usep = 0;
        if (tk[t].xm_on &&
            (acc[MD_OSC2] != 0.0f || acc[MD_DETUNE] != 0.0f || acc[MD_RESO] != 0.0f || acc[MD_MIX] != 0.0f ||
            acc[MD_SUB] != 0.0f || acc[MD_NOISE] != 0.0f || acc[MD_DRIVE] != 0.0f || acc[MD_FM] != 0.0f ||
            acc[MD_RING] != 0.0f || acc[MD_SHAPE] != 0.0f || acc[MD_BIAS] != 0.0f)) {
            const int16_t *tp = trk[t].p; /* this voice's own engine parameters: the track's, the matrix's moves on */
            int16_t *q = x->pv.p;
            int k;
            for (k = 0; k < P_NUM; k++)
                q[k] = tp[k];
#define MV(P, d, lo, hi, sc) \
    if (acc[d] != 0.0f)      \
        q[P] = (int16_t)clamp(tp[P] + (int32_t)(acc[d] * (sc)), lo, hi)
            MV(P_O2, MD_OSC2, -768, 768, 768.0f);
            MV(P_E1, MD_DETUNE, 0, 127, 127.0f);
            MV(P_E5, MD_RESO, 0, 127, 127.0f);
            MV(P_E2, MD_MIX, 0, 127, 127.0f);
            MV(P_SUB, MD_SUB, 0, 127, 127.0f);
            MV(P_E3, MD_NOISE, 0, 127, 127.0f);
            MV(P_E6, MD_DRIVE, 0, 127, 127.0f);
            MV(P_FM, MD_FM, 0, 127, 127.0f);
            MV(P_RING, MD_RING, 0, 127, 127.0f);
            MV(P_SHA, MD_SHAPE, 0, 127, 127.0f);
            MV(P_BIAS, MD_BIAS, -64, 63, 64.0f);
#undef MV
            x->usep = 1;
        }
        x->m.envq15 = (int32_t)(x->aenv * 32767.0f);
        v->env = (int32_t)(x->aenv * 16777216.0f);
        x->half = (uint8_t)h;
        h ^= 1;
    }
}

GM_RAM void syn_half(uint32_t h)
{
    int32_t *tmp = acc_t[h];
    int i, k, t;
    for (t = 0; t < SYN_TRACKS; t++)
        used_m[h][t] = used_lr[h][t] = 0;
    for (i = 0; i < NVOICE; i++) {
        vx_t *x = &vx[i];
        if (!x->trk || x->half != h)
            continue;
        t = x->trk;
        if (x->gl >= 1.0f && x->gr >= 1.0f) {
            if (!used_m[h][t]) {
                for (k = 0; k < SYN_CTL; k++)
                    acc_m[h][t][k] = 0;
                used_m[h][t] = 1;
            }
            render_voice(i, acc_m[h][t]);
        } else {                          /* a unison voice off the centre */
            int32_t gl = (int32_t)(x->gl * 32767.0f), gr = (int32_t)(x->gr * 32767.0f);
            int32_t *l = acc_l[h][t], *r = acc_r[h][t];
            if (!used_lr[h][t]) {
                for (k = 0; k < SYN_CTL; k++)
                    l[k] = r[k] = 0;
                used_lr[h][t] = 1;
            }
            for (k = 0; k < SYN_CTL; k++)
                tmp[k] = 0;
            render_voice(i, tmp);
            for (k = 0; k < SYN_CTL; k++) {   /* (mulq15's result, without its overflow: a loud voice passes 2^16) */
                l[k] += mulq16(tmp[k], (uint32_t)gl << 1);
                r[k] += mulq16(tmp[k], (uint32_t)gr << 1);
            }
        }
    }
}

static volatile float trk_pk[SYN_TRACKS];   /* each track's peak in the mix since the screen last looked */
static float trk_l[SYN_TRACKS][SYN_CTL], trk_r[SYN_TRACKS][SYN_CTL];   /* the last block, each track alone */
static float trk_send[SYN_TRACKS][SYN_CTL];
static syn_insert_fn trk_insert[SYN_TRACKS];
static uint8_t trk_idle[SYN_TRACKS];      /* the track's insert said it sleeps on silence (syn_insert_fn's 1): nothing to
                                           * mix, nothing to call, until a voice sounds on the track or its insert is set */
static uint32_t trk_used;                 /* bit t: track t sounded in it */

void syn_cut_track(int t)
{
    int i;
    if (t < 1 || t >= SYN_TRACKS)
        return;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].trk == t) {
            vx[i].trk = 0;
            vx[i].astage = vx[i].fstage = vx[i].mstage = 0;
            vv[i].active = 0;
        }
    tk[t].nstack = 0;
    used_m[0][t] = used_m[1][t] = used_lr[0][t] = used_lr[1][t] = 0;
    trk_used &= ~(1u << t);
    trk_pk[t] = 0.0f;
}

const float *syn_track(int t, int side)
{
    if (t < 1 || t >= SYN_TRACKS || !(trk_used >> t & 1u))
        return 0;
    return side ? trk_r[t] : trk_l[t];
}

void syn_set_insert(int t, syn_insert_fn fn)
{
    if (t >= 1 && t < SYN_TRACKS) {
        trk_insert[t] = fn;
        trk_idle[t] = 0;
    }
}

/* (GhettoMachine 0.5: the mix runs from RAM, gm_ram.h, with the tracks' gains read once a block: the stores into the
 * float buffers may alias them for all the compiler knows, so it read them again every sample) */
GM_RAM void syn_mix_repack(float *l, float *r, float *rev, float *dly)
{
    int k, t;
    for (k = 0; k < SYN_CTL; k++)
        l[k] = r[k] = rev[k] = dly[k] = 0.0f;
    for (t = 1; t < SYN_TRACKS; t++)
        if (trk_used >> t & 1u) {
            const float rs = mix[t].rs * (1.f/36000.f), ds = mix[t].ds * (1.f/36000.f);
            const float *tl = trk_l[t], *tr = trk_r[t], *ts = trk_send[t];
            for (k = 0; k < SYN_CTL; k++) {
                l[k] += tl[k];
                r[k] += tr[k];
                rev[k] += ts[k] * rs;
                dly[k] += ts[k] * ds;
            }
        }
}

/* The tracks' voices (the halves' sums: centred voices in acc_m, unison voices off the centre in acc_l / acc_r) into
 * each track's block, through its insert, its pan and its sends' feed, then the mix. A track with no voice this block
 * and no insert is left out; so is one whose insert said it sleeps on silence (trk_idle). The usual track has only
 * centred voices: one sum and one conversion a sample, its peak taken as a whole number and scaled once (the same
 * value: scaling by a gain that is not negative keeps the order). The same samples as the loop this replaces */
GM_RAM void syn_mix(float *l, float *r, float *rev, float *dly)
{
    static const int32_t NONE[SYN_CTL];
    const float g = 1.0f / 36000.0f;      /* one voice of a typical sound near -12 dBFS */
    int k, t;
    trk_used = 0;
    for (t = 1; t < SYN_TRACKS; t++) {
        const int m0 = used_m[0][t], m1 = used_m[1][t], s0 = used_lr[0][t], s1 = used_lr[1][t];
        const syn_insert_fn fn = trk_insert[t];
        const float pl = mix[t].pl * g, pr = mix[t].pr * g;
        const int32_t *a0 = m0 ? acc_m[0][t] : NONE, *a1 = m1 ? acc_m[1][t] : NONE;
        float *tl = trk_l[t], *tr = trk_r[t], *ts = trk_send[t];
        float pk = trk_pk[t];
        if (m0 | m1 | s0 | s1)
            trk_idle[t] = 0;
        else if (!fn || trk_idle[t])
            continue;
        trk_used |= 1u << t;
        if (s0 | s1) {                    /* unison voices off the centre too */
            const int32_t *l0 = s0 ? acc_l[0][t] : NONE, *l1 = s1 ? acc_l[1][t] : NONE;
            const int32_t *r0 = s0 ? acc_r[0][t] : NONE, *r1 = s1 ? acc_r[1][t] : NONE;
            const float gl = fn ? g : pl, gr = fn ? g : pr;
            for (k = 0; k < SYN_CTL; k++) {
                float m = (float)(a0[k] + a1[k]);
                float ul = (float)(l0[k] + l1[k]);
                float ur = (float)(r0[k] + r1[k]);
                tl[k] = (m + ul) * gl;
                tr[k] = (m + ur) * gr;
                if (!fn)
                    ts[k] = m + 0.5f * (ul + ur);
            }
        } else if (fn) {                  /* centred voices into an insert: one signal, both sides */
            for (k = 0; k < SYN_CTL; k++)
                tl[k] = tr[k] = (float)(a0[k] + a1[k]) * g;
        } else {                          /* centred voices, no insert: panned, the sends' feed, the peak */
            int32_t top = 0;
            for (k = 0; k < SYN_CTL; k++) {
                int32_t v = a0[k] + a1[k];
                float m = (float)v;
                tl[k] = m * pl;
                tr[k] = m * pr;
                ts[k] = m;
                v = v < 0 ? -v : v;
                if (v > top)
                    top = v;
            }
            {
                float p = (float)top * (pl > pr ? pl : pr);
                if (p > pk)
                    pk = p;
            }
            trk_pk[t] = pk;
            continue;
        }
        if (fn) {
            const float gl = mix[t].pl, gr = mix[t].pr;
            if (fn(t, tl, tr, SYN_CTL) && !(m0 | m1 | s0 | s1)) {   /* asleep on silence: left out from here */
                trk_idle[t] = 1;
                trk_used &= ~(1u << t);
                continue;
            }
            for (k = 0; k < SYN_CTL; k++) {
                ts[k] = (tl[k] + tr[k]) * (0.5f * 36000.f);
                tl[k] *= gl;
                tr[k] *= gr;
            }
        }
        for (k = 0; k < SYN_CTL; k++) {
            float a = tl[k], b = tr[k];
            a = a < 0.f ? -a : a;
            b = b < 0.f ? -b : b;
            if (a > pk) pk = a;
            if (b > pk) pk = b;
        }
        trk_pk[t] = pk;
    }
    syn_mix_repack(l, r, rev, dly);
}

float syn_peak(int t)
{
    float v;
    if (t < 1 || t >= SYN_TRACKS)
        return 0.0f;
    v = trk_pk[t];
    trk_pk[t] = 0.0f;
    return v;
}
