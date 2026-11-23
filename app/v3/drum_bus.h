/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_ORIGINAL_DRUM_BUS_H
#define FM1_ORIGINAL_DRUM_BUS_H
#include <stdint.h>
#include "../dsp/fastmath.h"
#include "../dsp/x0x_param.h"

enum { BUS_FRAMES = 256, BUS_PADS = 11 };
typedef struct {
    float level, pan, reverb_send, delay_send;
    int comp_on, transient_on, limiter_on;
    float threshold_db, ratio, attack_ms, release_ms, makeup_db;
    float transient_attack, transient_sustain; /* -1..1 */
    float ceiling_db, limiter_release_ms;
} BusPatch;
typedef struct { float level, pan, reverb, delay; } BusPad;
typedef struct { BusPatch bus; BusPad pad[BUS_PADS]; int dry_only; } BusMix;

/* GhettoMachine 0.5: the drum bus without private returns. Each pad's reverb and delay sends leave through the
   rev/dly buffers to GhettoMachine's shared ROOM and DIGI (as the 808's and 909's sends do), so the drums and the
   synths share one reverb and one delay, set on the FX page; BongDrums' browser build has none (DRUM_BUS_NO_SPATIAL).
   No delay line or room of its own: 64 KB of the pool and their processing time are freed. */
typedef struct {
    float track_gain; /* 1: the track's fader, pan and sends follow (app.c drum_mix) */
    unsigned saturation; /* board SAT macro, zero bypasses */
    float comp_env, fast_env, slow_env, limiter_reduction;
    /* Positive maximum reduction in the most recently processed span, dB.
       Read on the render thread or publish a synchronized UI copy. */
    float comp_gr_db, limiter_gr_db;
} DrumBus;

BusMix bus_default_mix(void);
void drum_bus_init(DrumBus *b);
/* Ordered mono lanes -> stereo dry (pads' level and pan) and the pads' reverb and delay sends -> bus level/pan
   -> saturation -> compressor -> transient (BUS) on the dry only. Null lanes are silent. Overwrites left, right,
   rev and dly; the caller supplies distinct buffers. The sends carry the dry's .75 trim, not the bus processing */
void drum_bus_render(DrumBus *b, const BusMix *mix, float *lanes[BUS_PADS], const float *trim, float dry_out,
                     float *left, float *right, float *rev, float *dly, unsigned n);
/* trim: each pad's lane gain (null: 1). dry_out: with dry_only, the gain after it all (the caller's trims and scale),
   folded into the one pass (render_dry); with the bus on, the caller applies its own after the limiter */
/* Call AFTER the common audition trim and live de-click, immediately before
   handing samples to the sink. Stereo-linked, zero lookahead, sample peak. */
void drum_bus_limit_final(DrumBus *b, const BusMix *mix, float *left, float *right, unsigned n);
#endif
