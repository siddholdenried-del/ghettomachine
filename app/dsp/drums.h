/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine: the drum track's three kits behind one small interface.
 *
 * The 909 (X0X's drum909, after 9W9) and the 808 (X0X's drum808, after 8W8) share one pad layout,
 * so a pattern keeps its meaning when the kit changes:
 *
 *   pad   0  1  2  3  4  5  6  7  8  9  10
 *   909   BD SD LT MT HT RS CP CH OH CR RD
 *   808   BD SD LT MT HT RS CP CH OH CY CB
 *   606   BD SD LT -- HT -- CP CH OH CY --
 *
 * This unit is compiled on its own (fm1.toml o2), so the engines' headers (and X0X's fastmath.h)
 * never meet the platform's in the unity build: the app sees only this header.
 *
 * Threading: drums_trigger / drums_set run between render blocks: the audio interrupt itself, or
 * the main loop inside plat_lock(). */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

#define DRUMS_NPADS 11
#define DRUMS_PAD_KIT DRUMS_NPADS      /* the kit-wide parameters, as a pad index */
#define DRUMS_MAX_BLOCK 256
#define DRUMS_VEL_NORMAL (88.0f / 127.0f)   /* an unaccented sequencer hit; 1.0 is an accent */

enum { KIT_909 = 0, KIT_808 = 1, KIT_606 = 2, KIT_707 = 3, KIT_NUM };
/* Pad models include the four kits and two alternate 707 voices. A factory
 * kit is still strictly below KIT_NUM. The auxiliary engines are shared,
 * at most one lane per model. */
enum { DRUM_MODEL_707_CB = KIT_NUM, DRUM_MODEL_707_TB, DRUM_MODEL_NUM };
const char *drums_model_name(int model);
const char *drums_kit_name(int kit);
int drums_has_pad(int kit, int pad);
/* Persisted zero = inherit base, 1..DRUM_MODEL_NUM = explicit model ID plus one. */
int drums_model(int base, const uint8_t *map, int pad);
void drums_trigger_mapped(int base, const uint8_t *map, int pad, float vel);

void drums_init(void);
/* vel 0..1: DRUMS_VEL_NORMAL for a plain step, 1.0 for an accented one; a played key's velocity in between */
void drums_trigger(int kit, int pad, float vel);
/* Cut the pad in all kits, including another kit's held-over tail. */
void drums_cut_pad(int pad);
void drums_cut_mapped(int base, const uint8_t *map, int pad);
/* renders n (<= DRUMS_MAX_BLOCK) samples of the kit, plus the other kit's tails while they ring, into l / r, and the
 * pads' reverb and delay sends (each pad's Rev and Dly knobs) into rev / dly (all overwritten) */
void drums_render(int kit, float *l, float *r, float *rev, float *dly, int n);
int drums_active(int kit);

/* the parameters of a pad (or DRUMS_PAD_KIT): the engine's own descriptors and integer pots */
int drums_nparams(int kit, int pad);
const x0x_param_t *drums_param(int kit, int pad, int i);
int drums_get(int kit, int pad, int i);
void drums_set(int kit, int pad, int i, int value);
const char *drums_pad_name(int kit, int pad);
