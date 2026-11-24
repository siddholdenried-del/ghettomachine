/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's effects and master bus (PLAN.md, the unified engine), behind one small interface:
 *   the sends (BongFX, bongfx.c): one shared BongVerb (Motion Plate: Plate or Hall) and one shared BongDelay (Clock
 *   Echo, its time a tempo division), each track's sends their inputs; their returns added to the dry mix of the four
 *   tracks; the returns never reach a send;
 *   the master bus (BongMaster: master.c, mbus.c, as the bongmaster repository has them), in this order: EQ (low cut, low shelf, mid bell, high shelf: mbus.c); a compressor (threshold, ratio,
 *   attack, release, makeup, mix, a 6 dB soft knee; PUMP ducks it at each kick), on by default and gentle (2:1 over
 *   -18 dB, 10 ms, 150 ms); a one-knob DJ filter (master.c); TAPE and STEREO (mbus.c); the limiter's GAIN, then a
 *   look-ahead limiter under its CEILING with a soft knee (master.c); last the MASTER volume, so the knob sets how loud
 *   you hear it and never what the chain does. Chain presets set the bus's knobs at once; BYPASS takes the chain out
 *   (the limiter stays, at 0 dB GAIN) to compare.
 * Its own unit (fm1.toml o2): X0X's fastmath.h never meets the platform's headers.
 * Threading: fx_set runs between render blocks (the main loop inside plat_lock()); the rest in the audio interrupt. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

/* the knobs the FX pages show: REVERB, DELAY, DELAY 2, COMP, COMP 2 (and the reduction), MASTER */
enum {
    /* BongVerb's and BongDelay's (project format 9; up to 8 these slots held the ROOM reverb's and the DIGI / TAPE
     * delay's knobs: tone, low cut, type, wear, width, low cut, where damp, pre-delay, the reverb's mode and drive are
     * now: app.c fx_migrate) */
    FXP_RV_DECAY, FXP_RV_DAMP, FXP_RV_PREDELAY, FXP_RV_LEVEL,
    FXP_DL_TIME, FXP_DL_FDBK, FXP_DL_TONE, FXP_DL_LEVEL,
    FXP_RV_MODE, FXP_DL_DRIVE, FXP_RSV10, FXP_RSV11,
    FXP_C_THRESH, FXP_C_RATIO, FXP_C_ATTACK, FXP_C_RELEASE,
    FXP_C_MAKEUP, FXP_C_MIX, FXP_PUMP,
    FXP_FILTER, FXP_F_RESO, FXP_LIMIT,
    /* the master bus (0.2): mbus.h's, then the limiter's, then the chain's own */
    FXP_EQ_LOW, FXP_EQ_MID, FXP_EQ_HIGH, FXP_EQ_CUT,
    FXP_EQ_LFREQ, FXP_EQ_MFREQ, FXP_EQ_MQ, FXP_EQ_HFREQ,
    FXP_TP_DRIVE, FXP_TP_WARM, FXP_TP_TONE, FXP_TP_MIX,
    FXP_ST_WIDTH, FXP_ST_BASS, FXP_ST_BAL,
    FXP_L_GAIN, FXP_L_CEIL, FXP_L_REL,
    FXP_M_PRESET, FXP_M_BYPASS,
    FXP_NUM
};

/* the delay's line: int16 samples, lent by the caller (it may take it back with fx_delay_line(0)) */
/* the mix comes to the master bus this much lower (-14 dB): fx.c says why. The screen's track meters use it too */
#define FX_BUS_TRIM 0.2f

void fx_init(int16_t *dly_line, int dly_len);
const x0x_param_t *fx_param(int i);       /* its range, default and value names (X0X's) */
int fx_get(int i);
void fx_format(int i, char *buf);         /* the master bus's knobs in their units ("-12dB", "2:1", "10ms"); 16 bytes;
                                           * "" for the sends' (their values or names as they are) */
void fx_set(int i, int v);
void fx_bpm(float bpm);                   /* the delay's divisions follow the tempo (BongDelay's 45..950 ms: halved past it) */
/* the mix: the dry sum and the two send buses (mono) in, the dry with the reverb and the delay out (n <= 256) */
void fx_process(const float *dry_l, const float *dry_r, const float *rev, const float *dly, float *out_l, float *out_r,
                int n);
/* then the master bus, in place: EQ, compressor, filter, TAPE, STEREO, GAIN, limiter, volume (0..1, gliding) */
void fx_master(float *l, float *r, int n, float volume);
void fx_key(int kit, float vel);          /* a kick: PUMP's key */
/* the delay's line lent out (0: the delay leaves the chain at once; the caller may write the line) or back (1: the
 * delay back in, on a line the caller has zeroed with fx_delay_zero); between render blocks. fx_delay_zero: the lent
 * line zeroed, from the main loop without the lock (132 KB: 1.5 ms on the FM-1, which the audio must not wait for) */
void fx_delay_line(int on);
void fx_delay_zero(void);
int fx_delay_on(void);
float fx_gr(void);                        /* the master compressor's gain reduction now, dB (the screen's meter) */
float fx_lgr(void);                       /* the limiter's, dB */
/* the bus's input peak since the last look, and its output's (before MASTER); who: 0 the screen, 1 the tests */
float fx_in_peak(int who);
float fx_out_peak(int who);
float fx_db(float lin);                   /* 20 log10, -120 for silence */
void fx_out_sides(float *l, float *r);    /* each side's output peak (before MASTER) since the last look: the band's meter */
float fx_delay_ms(void);                  /* the delay's time now, and its regeneration (the FX page's picture) */
float fx_delay_fb(void);
float fx_return_now(int which);           /* (the screen) a return's output now: 0 the verb, 1 the delay */
int fx_returns_asleep(void);              /* bit 0 BongVerb, bit 1 BongDelay asleep (silence in and out: not run) */
/* the chain presets: they set the compressor's, EQ's, TAPE's, STEREO's and the limiter's knobs (not PUMP, the filter,
 * the limiter's switch or the sends) */
int fx_presets(void);
const char *fx_preset_name(int p);
int fx_preset_value(int p, int i);        /* knob i's value in preset p, -1: the preset leaves it */
int fx_preset_edited(void);               /* a knob moved since FXP_M_PRESET's preset was loaded */
/* the screen's: stage 0 EQ, 1 COMP, 2 TAPE, 3 STEREO, 4 LIMIT doing anything (bypassed: 0 but the limiter); the EQ's
 * response at hz, dB; the meters (only measured while on: the screens that show them) */
int fx_stage_on(int stage);
float fx_eq_db(float hz);
void fx_eq_curve(float *db, int n);       /* the EQ's response at n points, 20 Hz .. 20 kHz evenly in octaves */
void fx_meter(int on);
float fx_lufs(int momentary);             /* short-term (3 s) or momentary (400 ms), LUFS; -70: silence / not yet */
float fx_corr(void);                      /* the sides' correlation, -1..1 */
