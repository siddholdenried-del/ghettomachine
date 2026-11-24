/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_V3_TRIAL_H
#define FM1_V3_TRIAL_H
#include "kit_patch.h"
#include <stdint.h>

/* Audio-thread operations, or main-loop calls made with the render locked.
 * Initialize once while stopped. Native pad IDs are 1 through 11. */
void trial_init(void);
/* a genre kit (presets.h KIT_GENRES) as the patch: every drum on its preset (the edits made since gone) */
void trial_kit(int genre);
void trial_hit(unsigned pad1, float velocity);
/* the same hit with one of the pad's controls (presets.h's index) moved by move times its whole range, for this hit
 * alone (the drums' LFO): the patch itself is left as it is */
void trial_hit_moved(unsigned pad1, float velocity, int param, float move);
/* A pad muted while it sounds: its sound fades out in 1.5 ms and stays out until it strikes again */
void trial_cut(unsigned pad1);
/* a lane's audio muted (1) or not (0): its hits go on (the voices sound underneath), the lane fades out and back in */
void trial_mute(unsigned pad1, int on);
int trial_lane_gate(unsigned pad1);      /* its gate now, thousandths (the tests) */
void trial_capture(int pad1, uint8_t *env, int len, int step);   /* (the screen) a lane's next hit: its peaks */
int trial_capture_pos(void);             /* how many of them so far; -1: none asked */
float trial_bus_gr(int which);            /* (the screen) the drum bus's reduction, dB: 0 the compressor, 1 the limiter */
#ifdef PLAT_HOST
int trial_lane_peak(unsigned pad1);      /* its peak out since last asked, dB, -200 silent (the tests; resets) */
#endif
/* A factory recipe's level trim, tenths of a dB (v3/levels.inc) */
int trial_trim_db10(unsigned pad1, unsigned recipe);
/* 1 kick preset, 2 cymbal mode, 3 Perc A mode, 4 Perc B mode. */
void trial_patch(int what, int value);
/* Control 0 selects the preset. Kick controls 1..6 use tenths of Hz,
 * whole ms, tenths of semitones, tenths of ms, and tenths of percent.
 * Other pads use 7-bit controls. Changes apply to the next strike. */
int trial_kick_bound(unsigned control, unsigned upper);
int trial_voice_get(unsigned pad, unsigned control);
void trial_voice_set(unsigned pad, unsigned control, int value);
/* Physical editor. Presets and parameter indices are zero-based. Custom = -1.
 * The last recalled factory recipe anchors browsing after a manual edit. */
int trial_recipe(unsigned pad);
void trial_recipe_turn(unsigned pad, int delta);
void trial_snapshot(unsigned pad, KitPatch *copy);
void trial_parameter_turn(unsigned pad, int param, int delta, int fine);
/* Explicit transition to stock: the kit's voices and the bus's dynamics cleared */
void trial_quiet(void);
/* SAT, COMP, TRANS, LIMIT at indices 0..3. Values use 0..127. */
void trial_macro(unsigned which, unsigned value);
unsigned trial_macro_value(unsigned which);
/* Overwrite L/R and the pads' reverb and delay sends (for the shared ROOM and DIGI), on the drum track's scale at
 * level 100 (the track's fader, pan and sends are the caller's: app.c drum_mix). Mode 1 selects dry and 2 the
 * processed bus (SAT, COMP, TRANS, LIMIT). Optional tick outputs receive this call's voice and bus totals (24
 * ticks/us). Either core, one at a time */
void trial_render(float *left, float *right, float *rev, float *dly, unsigned n, unsigned mode,
                  unsigned *voice_ticks, unsigned *bus_ticks);
#endif
