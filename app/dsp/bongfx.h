/* SPDX-License-Identifier: GPL-3.0-only */
/* BongFX on the FM-1: BongVerb (Motion Plate) and BongDelay (Clock Echo), BongSoft's shared send effects, ported from
 * their repositories (bongverb verb.c 100717b, kernel SHA-256 45183a17...; bongdelay delay.c 017d298, 7cabd390...).
 * The arithmetic is theirs, line for line; what differs:
 *   - each engine's state is a struct the caller owns (the repositories keep one static), so the reverb's 26 KB can go
 *     in the pool and the delay's ring can be the line GhettoMachine lends between the delay, a sampler recording,
 *     STUTTER and TAPE STOP (fx.c: fx_delay_line);
 *   - libm's expf, powf and fabsf are fastmath.h's (relative error under 1e-5, in the control-rate coefficients only:
 *     tests/bongfx_unit.c holds the port to the kernels within -90 dB);
 *   - a block call around the kernels' sample tick, and a sleep: an engine whose input and output have stayed under
 *     -120 dBFS long enough for its lines to have emptied gives silence without running, and wakes on the first
 *     sample in (the same output to -120 dB).
 * Both run at 44.1 kHz with their own half-rate cores. Controls: 0..1, between blocks (the render's lock). */
#pragma once
#include <stdint.h>

/* ---- BongVerb: mono send in, stereo return out (wet only) */
enum { BV_MODE, BV_DECAY, BV_PREDELAY, BV_DAMP, BV_NPARAMS };   /* mode: 0 Plate, 1 Hall (>= .5) */
/* the lines sized for 44.1 kHz, the FM-1's only rate (the kernel's for 48 kHz too: 4100, 560, 3010): at 22.05 kHz its
 * four tank lines take 653 + 773 + 957 + 1139 = 3522 samples, its two diffusers 103 + 205, its pre-delay 2716; 4.4 KB
 * less, the same arithmetic on the same samples (tests/bongfx_unit.c checks the lengths) */
enum { BV_LINES = 4, BV_RING = 3528, BV_AP = 312, BV_PRE = 2720, BV_MOTION_BLOCK = 16 };
typedef struct {
    float ring[BV_RING], diff[BV_AP], pre[BV_PRE];
    int len[BV_LINES], pos[BV_LINES], off[BV_LINES], ap_len[2], ap_pos[2], ap_off[2], pre_len, pre_pos, half;
    float rate, coef, input, old_l, old_r, new_l, new_r;
    uint32_t motion_phase[2], motion_inc[2];
    unsigned motion_clock;
    float motion[2], motion_step[2];
    float param[4], mode, delay, damp, gain[BV_LINES], mode_target, delay_target, damp_target, gain_target[BV_LINES],
        lp[BV_LINES];
    uint32_t quiet;                       /* samples in and out under the floor (the sleep) */
    uint8_t settled;                      /* its glides have stopped moving (the sleep skips them) */
} bongverb_t;

void bongverb_init(bongverb_t *v);
void bongverb_set(bongverb_t *v, int id, float value);
/* n samples of the send in; the return added into l and r times gain (0: not added, the engine still runs) */
void bongverb_block(bongverb_t *v, const float *in, float *l, float *r, float gain, int n);
int bongverb_asleep(const bongverb_t *v);

/* ---- BongDelay: mono send in, dual-mono return (wet only) on a lent 16-bit ring of BD_RING samples */
enum { BD_TIME, BD_REGEN, BD_TONE, BD_DRIVE, BD_NPARAMS };
enum { BD_RING = 23040, BD_CONTROL = 32 };
typedef struct {
    int16_t *ring;                        /* BD_RING samples, zeroed by the lender */
    unsigned at, phase, started, control;
    uint32_t drift, drift_delta;
    float hz, prev_in, prev_wet, wet;
    float lag, lag_to, lag_coef, inv_hz;
    float regen, regen_to, tone, tone_to, drive, drive_to, smooth;
    float in_a, in_b, read_a, read_b, dc, dc_coef;
    float alpha, input_gain, loop_gain, motion;
    uint32_t quiet;
    uint8_t settled;                      /* 2: its glides have stopped (a retune still due), 1: and retuned */
} bongdelay_t;

void bongdelay_init(bongdelay_t *d, int16_t *ring);
void bongdelay_set(bongdelay_t *d, int id, float value);
/* the time in ms (45..950) as BD_TIME's setting; and back */
float bongdelay_time_value(float ms);
float bongdelay_ms(const bongdelay_t *d);   /* the time it is gliding to */
/* the line back from a lender (zeroed): the engine as from silence, its settings kept */
void bongdelay_restart(bongdelay_t *d);
void bongdelay_block(bongdelay_t *d, const float *in, float *l, float *r, float gain, int n);
int bongdelay_asleep(const bongdelay_t *d);
