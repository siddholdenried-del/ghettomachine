/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ORIGINAL_KIT_SHELLS_H
#define ORIGINAL_KIT_SHELLS_H
#include <stdint.h>
#include "kit_patch.h"

/* A membrane begins at a struck phase and settles to its own pitched decay. */
typedef struct {
    uint32_t phase;
    float base, fast, slow, fast_rate, slow_rate, level, level_rate;
} Membrane;

typedef struct {
    Membrane head;
    uint32_t overtone_phase;
    float overtone_inc, overtone, overtone_rate, shape, stick, last, carry;
    uint32_t mallet_noise;
    float mallet_lp, mallet_level;
    unsigned age;
} BodyVoice;

typedef struct {
    Membrane head[2];
    uint32_t noise;
    float wire, wire_rate, wire_hp, wire_lp, low, band;
    float edge, edge_rate, last, carry;
    unsigned age;
} SnareVoice;

typedef struct {
    uint32_t noise;
    unsigned age, next, spacing, fired, count, cluster;
    float velocity, tooth, tooth_rate, wash, wash_rise, wash_rate, wash_gain;
    float low, band, air, low_rate, high_rate, last, carry;
} ClapVoice;

typedef struct {
    BodyVoice body[4]; /* kick, then low/mid/high tom */
    struct KickImpact {
        uint32_t seed;
        unsigned hold;
        float click_level, filtered_noise, pressure;
    } kick_impact;
    SnareVoice snare;
    ClapVoice clap;
} Shells;

void shells_init(Shells *shells);
void shells_hit(Shells *shells, int pad, float velocity, const KitPatch *patch);
void shells_render(Shells *shells, float *mono, unsigned frames);
/* Six pad-order bits from the same retirement predicates as the renderer. */
unsigned shells_active_mask(const Shells *shells);
/* Add into six buffers in pad order. Buffers may alias for a mono mix.
 * For pads absent from shells_active_mask at span entry, a null is valid. */
void shells_render_routed(Shells *shells, float *lanes[6], unsigned frames);
#endif
