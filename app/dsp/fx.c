/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's effects and master bus: BongFX's BongVerb and BongDelay on the sends, then BongMaster (master.c and
 * mbus.c), behind fx.h. */
#define FM_ALWAYS_INLINE                  /* (code in RAM here: fastmath.h, gm_ram.h) */
#include "fx.h"
#include "bongfx.h"
#include "master.h"
#include "mbus.h"
#include "fastmath.h"
#include "gm_ram.h"

#ifdef PLAT_HOST
#define FX_POOL
#else
#define FX_POOL __attribute__((section(".pool")))   /* (plat.h's PLAT_POOL: zeroed RAM for big state) */
#endif

static bongverb_t verb FX_POOL;           /* its lines: 31 KB */
static bongdelay_t dly;                   /* its ring is the lent line (fx_init) */
static uint8_t send_pot[12];              /* the sends' knobs (FXP_RV_DECAY .. FXP_RSV11) */
static uint8_t dl_off;                    /* the line lent out: the delay out of the chain */
static float bpm_now = 120.0f, gain_rv, gain_dl, gain_rv_now, gain_dl_now;   /* the returns' gains: set; now (gliding) */
static master_t mst;
static mbus_t mb;
static int16_t *line;
static int line_len;
static uint8_t own[2];                    /* FXP_M_PRESET, FXP_M_BYPASS */
static volatile float in_pk[2], out_pk[2];   /* the bus's peaks since the screen [0] / the tests [1] last looked */
static volatile float out_side[2];        /* each side's output peak (before MASTER) since the band's meter looked */

/* the mix comes to the bus this much lower (-14 dB). The tracks' sum peaks 11..13 dB over full scale in the demos: X0X
 * put the MASTER volume (a square law, -12 dB at half) before its limiter, which caught the rest. Here the volume is
 * last, so the chain reads the mix at its own level whatever the knob: peaks near -2 dBFS, room for EQ and TAPE,
 * and the limiter's GAIN decides how loud the master is */
#define BUS_TRIM FX_BUS_TRIM

/* each FX knob: which section has it (0 the sends here, 1 master, 2 mbus, 3 this file's own), and its index there */
enum { S_BUS, S_MST, S_MB, S_OWN };
static const struct { uint8_t sec, idx; } MAP[FXP_NUM] = {
    {S_BUS, 0}, {S_BUS, 1}, {S_BUS, 2}, {S_BUS, 3}, {S_BUS, 4}, {S_BUS, 5}, {S_BUS, 6}, {S_BUS, 7},
    {S_BUS, 8}, {S_BUS, 9}, {S_BUS, 10}, {S_BUS, 11},
    {S_MST, MST_THRESH}, {S_MST, MST_RATIO}, {S_MST, MST_ATTACK}, {S_MST, MST_RELEASE},
    {S_MST, MST_MAKEUP}, {S_MST, MST_MIX}, {S_MST, MST_PUMP},
    {S_MST, MST_DJF}, {S_MST, MST_RESO}, {S_MST, MST_LIMIT},
    {S_MB, MB_EQ_LOW}, {S_MB, MB_EQ_MID}, {S_MB, MB_EQ_HIGH}, {S_MB, MB_EQ_CUT},
    {S_MB, MB_EQ_LFREQ}, {S_MB, MB_EQ_MFREQ}, {S_MB, MB_EQ_MQ}, {S_MB, MB_EQ_HFREQ},
    {S_MB, MB_TP_DRIVE}, {S_MB, MB_TP_WARM}, {S_MB, MB_TP_TONE}, {S_MB, MB_TP_MIX},
    {S_MB, MB_ST_WIDTH}, {S_MB, MB_ST_BASS}, {S_MB, MB_ST_BAL},
    {S_MST, MST_LGAIN}, {S_MST, MST_CEIL}, {S_MST, MST_LREL},
    {S_OWN, 0}, {S_OWN, 1},
};

/* ---- the chain presets: the knobs they set, and each one's values (in that order) */
static const uint8_t PK[] = {
    FXP_C_THRESH, FXP_C_RATIO, FXP_C_ATTACK, FXP_C_RELEASE, FXP_C_MAKEUP, FXP_C_MIX,
    FXP_EQ_LOW, FXP_EQ_MID, FXP_EQ_HIGH, FXP_EQ_CUT, FXP_EQ_LFREQ, FXP_EQ_MFREQ, FXP_EQ_MQ, FXP_EQ_HFREQ,
    FXP_TP_DRIVE, FXP_TP_WARM, FXP_TP_TONE, FXP_TP_MIX,
    FXP_ST_WIDTH, FXP_ST_BASS, FXP_ST_BAL,
    FXP_L_GAIN, FXP_L_CEIL, FXP_L_REL,
};
#define NPK ((int)sizeof PK)
#define NPRESET 7
static const char *const PRESET_N[NPRESET] = {"CLEAN", "GLUE", "PUNCH", "WARM", "WIDE", "LOUD", "LO-FI"};
/* comp: threshold (79 -18 dB, 74 -20, 69 -22, 58 -26), ratio (2 2:1, 3 3:1, 4 4:1, 5 6:1), attack (42 1 ms, 85 10,
 * 97 20, 105 30), release (53 80 ms, 58 100, 69 150, 76 200, 86 300), makeup (5 1 dB, 11 2, 16 3, 21 4), mix;
 * EQ: low, mid, high (24 0 dB, 0.5 dB a step), low cut (2 25 Hz, 3 30, 10 150), low freq (1 60 Hz, 2 80, 3 100,
 * 4 120), mid freq (31 400 Hz, 61 1 kHz, 74 1.5 kHz), mid Q (0 0.4, 1 0.7), high freq (4 6k, 5 8k, 6 10k, 7 12k);
 * TAPE: drive (21 2 dB, 32 3, 64 6, 95 9), warmth, tone (34 5k, 74 9k, 127 open), mix; STEREO: width (20 100 %, 5 % a
 * step), bass mono (3 100 Hz, 4 120), balance; limiter: GAIN (0.5 dB a step), CEILING (3 -0.3 dB), RELEASE (63 100 ms,
 * 75 150, 83 200) */
static const uint8_t PV[NPRESET][NPK] = {
    {79, 2, 85, 69, 16, 127, 24, 24, 24, 0, 2, 61, 1, 5, 0, 0, 127, 127, 20, 0, 64, 0, 3, 75},      /* the defaults */
    {69, 2, 105, 76, 11, 127, 24, 24, 24, 2, 2, 61, 1, 5, 32, 32, 127, 127, 20, 0, 64, 10, 3, 75},
    {74, 4, 105, 53, 16, 89, 28, 22, 27, 3, 2, 31, 1, 5, 21, 0, 127, 127, 20, 3, 64, 10, 3, 63},
    {79, 2, 85, 86, 5, 127, 27, 24, 22, 0, 4, 61, 1, 5, 64, 60, 74, 127, 20, 0, 64, 10, 3, 83},
    {79, 2, 85, 69, 0, 127, 24, 24, 28, 3, 2, 61, 1, 7, 0, 0, 127, 127, 28, 4, 64, 9, 3, 75},
    {69, 3, 97, 58, 16, 127, 26, 24, 27, 2, 1, 61, 1, 6, 32, 20, 127, 127, 22, 3, 64, 20, 3, 63},
    {58, 5, 42, 58, 21, 127, 20, 32, 16, 10, 3, 74, 0, 4, 95, 80, 34, 127, 12, 0, 64, 18, 3, 75},
};
static const char *const ONOFF_N[] = {"OFF", "ON"};
static const x0x_param_t OWN_P[2] = {{"PRESET", NPRESET - 1, 0, PRESET_N}, {"BYPASS", 1, 0, ONOFF_N}};

/* ---- the sends' knobs. Their defaults are the selected audition presets (bongverb's and bongdelay's presets.json:
 * Motion Plate 0 / .49 / .16 / .44, return .48; Clock Echo .27 (111 ms) / .46 / .52 / .21, return .53) on 0..127; the
 * delay's time a tempo division, 1/16 (125 ms at 120 BPM) by default; LEVEL 85 is the preset's return gain */
static const char *const DIV_N[13] = {"1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T",
                                      "1/4.", "1/2", "1/2."};
static const float DIV_BEATS[13] = {0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.375f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f,
                                    4.0f / 3.0f, 1.5f, 2.0f, 3.0f};
static const char *const MODE_N[2] = {"PLATE", "HALL"};
static const char *const RSV_N[1] = {"-"};
/* (0.5) The returns' LEVEL: 0..127 as it always was (linear, 85 the preset's return: 0 dB), then on past 127 in
   quarter-dB steps to +12 dB more (175: +15.5 dB over the preset's), for dub: returns louder than the dry. A saved
   project's 0..127 sounds the same. */
#define LVL_MAX 175
static float lvl_gain(int v)              /* LEVEL -> times the preset's return */
{
    return v <= 127 ? (float)v * (1.0f / 85.0f) : (127.0f / 85.0f) * fm_exp2f((float)(v - 127) * (0.25f / 6.0206f));
}

static const x0x_param_t SEND_P[12] = {
    {"Decay", 127, 62, 0}, {"Damp", 127, 56, 0}, {"PreDly", 127, 20, 0}, {"RvLvl", LVL_MAX, 85, 0},
    {"DlTime", 12, 2, DIV_N}, {"Regen", 127, 58, 0}, {"Tone", 127, 66, 0}, {"DlLvl", LVL_MAX, 85, 0},
    {"Mode", 1, 0, MODE_N}, {"Drive", 127, 27, 0}, {"-", 0, 0, RSV_N}, {"-", 0, 0, RSV_N},
};
#define RV_GAIN 0.48f                     /* the returns' gains at LEVEL 85 */
#define DL_GAIN 0.53f

static void retime(void)                  /* the division at this tempo, into BongDelay's 45..950 ms */
{
    float ms = DIV_BEATS[send_pot[FXP_DL_TIME] % 13] * 60000.0f / (bpm_now > 20.0f ? bpm_now : 120.0f);
    while (ms > 950.0f)                   /* longer than its line: half of it, still on the beat */
        ms *= 0.5f;
    bongdelay_set(&dly, BD_TIME, bongdelay_time_value(ms));
}

static void send_set(int i, int v)
{
    const float x = (float)v * (1.0f / 127.0f);
    send_pot[i] = (uint8_t)v;
    switch (i) {
    case FXP_RV_DECAY: bongverb_set(&verb, BV_DECAY, x); break;
    case FXP_RV_DAMP: bongverb_set(&verb, BV_DAMP, x); break;
    case FXP_RV_PREDELAY: bongverb_set(&verb, BV_PREDELAY, x); break;
    case FXP_RV_LEVEL: gain_rv = v <= 127 ? RV_GAIN * (float)v * (1.0f / 85.0f) : RV_GAIN * lvl_gain(v); break;   /* (to 127: as it was, to the bit) */
    case FXP_DL_TIME: retime(); break;
    case FXP_DL_FDBK: bongdelay_set(&dly, BD_REGEN, x); break;
    case FXP_DL_TONE: bongdelay_set(&dly, BD_TONE, x); break;
    case FXP_DL_LEVEL: gain_dl = v <= 127 ? DL_GAIN * (float)v * (1.0f / 85.0f) : DL_GAIN * lvl_gain(v); break;
    case FXP_RV_MODE: bongverb_set(&verb, BV_MODE, (float)v); break;
    case FXP_DL_DRIVE: bongdelay_set(&dly, BD_DRIVE, x); break;
    default: break;
    }
}

void fx_init(int16_t *dly_line, int dly_len)
{
    int i;
    line = dly_line;
    line_len = dly_len;                   /* (>= BD_RING: the delay's ring is its first BD_RING samples) */
    for (i = 0; i < dly_len; i++)
        dly_line[i] = 0;
    bongverb_init(&verb);
    bongdelay_init(&dly, dly_line);
    dl_off = 0;
    for (i = 0; i < 12; i++)
        send_set(i, SEND_P[i].def);
    gain_rv_now = gain_rv;                /* (the glide starts where they're set) */
    gain_dl_now = gain_dl;
    master_init(&mst);
    master_set(&mst, MST_PUMPSRC, PUMP_BOTH);   /* PUMP follows the kick of either kit */
    master_set(&mst, MST_THRESH, 79);     /* the compressor on, gently: -18 dB (on the bus's level: after BUS_TRIM) */
    master_set(&mst, MST_RATIO, 2);       /* 2:1 */
    master_set(&mst, MST_ATTACK, 85);     /* about 10 ms: a kick's front gets through */
    master_set(&mst, MST_RELEASE, 69);    /* about 150 ms */
    master_set(&mst, MST_MAKEUP, 16);     /* +3 dB: as loud as the chain bypassed (the demos: -15 LUFS both) */
    mbus_init(&mb);
    own[0] = own[1] = 0;
    master_bypass(&mst, 0);
}

const x0x_param_t *fx_param(int i)
{
    if (i < 0 || i >= FXP_NUM)
        return 0;
    switch (MAP[i].sec) {
    case S_BUS: return &SEND_P[MAP[i].idx];
    case S_MST: return master_param(MAP[i].idx);
    case S_MB: return mbus_param(MAP[i].idx);
    default: return &OWN_P[MAP[i].idx];
    }
}

int fx_get(int i)
{
    if (i < 0 || i >= FXP_NUM)
        return 0;
    switch (MAP[i].sec) {
    case S_BUS: return send_pot[MAP[i].idx];
    case S_MST: return master_get(&mst, MAP[i].idx);
    case S_MB: return mbus_get(&mb, MAP[i].idx);
    default: return own[MAP[i].idx];
    }
}

void fx_set(int i, int v)
{
    const x0x_param_t *p = fx_param(i);
    if (!p)
        return;
    v = v < 0 ? 0 : v > p->max ? p->max : v;
    switch (MAP[i].sec) {
    case S_BUS: send_set(MAP[i].idx, v); return;
    case S_MST: master_set(&mst, MAP[i].idx, v); return;   /* (COMP and FILTER set their own pots: master.c) */
    case S_MB: mbus_set(&mb, MAP[i].idx, v); return;
    default:                               /* PRESET only names what was loaded (the app sets its knobs); BYPASS */
        own[MAP[i].idx] = (uint8_t)v;
        if (i == FXP_M_BYPASS)
            master_bypass(&mst, v);
        return;
    }
}

void fx_format(int i, char *buf)
{
    buf[0] = 0;
    if (i < 0 || i >= FXP_NUM)
        return;
    if (i == FXP_RV_LEVEL || i == FXP_DL_LEVEL) {   /* the returns: dB against the preset's return */
        int v = send_pot[i];
        float db = v > 0 ? 6.0206f * fm_log2f(lvl_gain(v)) : -99.0f;
        int t = (int)(db * 10.0f + (db < 0.0f ? -0.5f : 0.5f)), j = 0, a;
        char d[8];
        if (v == 0) {
            buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = 0;
            return;
        }
        buf[j++] = t < 0 ? '-' : '+';
        a = t < 0 ? -t : t;
        {
            int n = 0, q = a / 10;
            do { d[n++] = (char)('0' + q % 10); q /= 10; } while (q);
            while (n) buf[j++] = d[--n];
        }
        buf[j++] = '.';
        buf[j++] = (char)('0' + a % 10);
        buf[j++] = 'd';
        buf[j++] = 'B';
        buf[j] = 0;
        return;
    }
    if (MAP[i].sec == S_MST)
        master_format(&mst, MAP[i].idx, buf);
    else if (MAP[i].sec == S_MB)
        mbus_format(&mb, MAP[i].idx, buf);
}

void fx_bpm(float bpm)
{
    if (bpm > 20.0f && bpm != bpm_now) {
        bpm_now = bpm;
        retime();
    }
}

/* the dry mix of the tracks and both returns, summed (the returns from the sends alone: nothing returned is sent) */
void fx_process(const float *dry_l, const float *dry_r, const float *rev, const float *dly_in, float *out_l,
                float *out_r, int n)
{
    int i;
    if (out_l != dry_l || out_r != dry_r)
        for (i = 0; i < n; i++) {
            out_l[i] = dry_l[i];
            out_r[i] = dry_r[i];
        }
    /* the returns' gains glide to what they're set to (a fifth of the way a block: a big turn of LEVEL, no click) */
    gain_rv_now += (gain_rv - gain_rv_now) * 0.2f;
    gain_dl_now += (gain_dl - gain_dl_now) * 0.2f;
    if (gain_rv - gain_rv_now < 1e-5f && gain_rv_now - gain_rv < 1e-5f)
        gain_rv_now = gain_rv;
    if (gain_dl - gain_dl_now < 1e-5f && gain_dl_now - gain_dl < 1e-5f)
        gain_dl_now = gain_dl;
    GM_FAR(bongverb_block)(&verb, rev, out_l, out_r, gain_rv_now, n);   /* (RAM code: gm_ram.h) */
    if (!dl_off)
        GM_FAR(bongdelay_block)(&dly, dly_in, out_l, out_r, gain_dl_now, n);
}

int fx_returns_asleep(void) { return bongverb_asleep(&verb) | (dl_off || bongdelay_asleep(&dly)) << 1; }

/* (GhettoMachine 0.5, CPU: from RAM with the compressor, the limiter and the volume (gm_ram.h); the trim and the
 * input's peak in one pass) */
GM_RAM void fx_master(float *l, float *r, int n, float volume)
{
    int i, byp = own[1];
    float pk = 0.0f;
    for (i = 0; i < n; i++) {
        float a = l[i] * BUS_TRIM, b = r[i] * BUS_TRIM, p;
        l[i] = a;
        r[i] = b;
        p = fm_maxf(fm_fabsf(a), fm_fabsf(b));
        if (p > pk)
            pk = p;
    }
    in_pk[0] = fm_maxf(in_pk[0], pk);
    in_pk[1] = fm_maxf(in_pk[1], pk);
    if (!byp && mb.eq_on)                 /* (the EQ, TAPE and STEREO and the meters: in the flash, called while on) */
        GM_FAR(mbus_eq)(&mb, l, r, n);
    master_pre_st(&mst, l, r, n);         /* the compressor (not while bypassed), the filter */
    if (!byp && (mb.tape_on || mb.st_on))
        GM_FAR(mbus_tape_stereo)(&mb, l, r, n);
    master_post_st(&mst, l, r, n);        /* GAIN, the limiter */
    {
        float a = out_side[0], b = out_side[1];
        for (i = 0; i < n; i++) {
            float x = fm_fabsf(l[i]), y = fm_fabsf(r[i]);
            if (x > a)
                a = x;
            if (y > b)
                b = y;
        }
        out_side[0] = a;
        out_side[1] = b;
        pk = fm_maxf(a, b);
    }
    out_pk[0] = fm_maxf(out_pk[0], pk);
    out_pk[1] = fm_maxf(out_pk[1], pk);
    if (mb.meter_on)
        GM_FAR(mbus_meter)(&mb, l, r, n);
    master_volume_st(&mst, l, r, n, volume);   /* the MASTER volume, last */
}

void fx_key(int kit, float vel) { master_key(&mst, kit, vel); }

void fx_delay_zero(void)                  /* (the main loop, no lock: nothing plays the lent line) */
{
    int i;
    if (!dl_off)
        return;
    for (i = 0; i < line_len; i++)        /* what sampling left in it: gone */
        line[i] = 0;
}

void fx_delay_line(int on)
{
    if (!on) {
        dl_off = 1;
        return;
    }
    if (!dl_off)
        return;
    bongdelay_restart(&dly);              /* (from its zeroed line: asleep until something comes) */
    dl_off = 0;
}

int fx_delay_on(void) { return !dl_off; }

float fx_gr(void) { return mst.comp_on && !own[1] ? mst.gr : 0.0f; }   /* (bypassed at 1:1 with no PUMP: none) */
float fx_lgr(void) { return mst.lgr_view; }

static float take(volatile float *p)
{
    float v = *p;
    *p = 0.0f;
    return v;
}
float fx_in_peak(int who) { return take(&in_pk[who & 1]); }
float fx_out_peak(int who) { return take(&out_pk[who & 1]); }
float fx_db(float lin) { return lin > 1e-6f ? 6.0206f * fm_log2f(lin) : -120.0f; }
void fx_out_sides(float *l, float *r)
{
    *l = out_side[0];
    *r = out_side[1];
    out_side[0] = out_side[1] = 0.0f;
}
float fx_delay_ms(void) { return bongdelay_ms(&dly); }
float fx_delay_fb(void) { return dly.regen_to; }

int fx_presets(void) { return NPRESET; }
const char *fx_preset_name(int p) { return p >= 0 && p < NPRESET ? PRESET_N[p] : ""; }

int fx_preset_value(int p, int i)
{
    int k;
    if (p < 0 || p >= NPRESET)
        return -1;
    for (k = 0; k < NPK; k++)
        if (PK[k] == i)
            return PV[p][k];
    return -1;
}

int fx_preset_edited(void)
{
    int k, p = own[0];
    for (k = 0; k < NPK; k++)
        if (fx_get(PK[k]) != PV[p][k])
            return 1;
    return 0;
}

int fx_stage_on(int stage)
{
    int byp = own[1];
    switch (stage) {
    case 0: return !byp && mbus_active(&mb, 0);
    case 1: return !byp && mst.comp_on;
    case 2: return !byp && mbus_active(&mb, 1);
    case 3: return !byp && mbus_active(&mb, 2);
    default: return mst.pot[MST_LIMIT] != 0;
    }
}

float fx_eq_db(float hz) { return mbus_eq_db(&mb, hz); }

void fx_eq_curve(float *db, int n)
{
    int i;
    for (i = 0; i < n; i++)
        db[i] = mbus_eq_db(&mb, 20.0f * fm_exp2f((float)i * (9.9658f / (float)(n > 1 ? n - 1 : 1))));
}
void fx_meter(int on) { mbus_meter_on(&mb, on); }
float fx_lufs(int momentary) { return mbus_lufs(&mb, momentary); }
float fx_corr(void) { return mbus_corr(&mb); }
