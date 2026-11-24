#ifndef FM1_ORIGINAL_METAL_H
#define FM1_ORIGINAL_METAL_H
#include "kit_patch.h"
#include <stdint.h>

/* Two tuning profiles, each with independent state for the two hats and each
   pad-9 articulation. Changing a selector never edits a sounding voice. */
typedef struct {
    float level, gain, accent, accent_gain, movement, cutoff, filter1, filter2;
    float ring1, ring2, old_ring1, old_ring2;
    float body_weight, bright_weight, edge_weight, decay_rate;
    float sweep_rate;
    float dark, tone;                     /* (0.5) the ring's darkening: 1 at the strike, falling; its low-pass */
    unsigned rise_frames;
    uint32_t age;
    unsigned choke, bell_blend;
} MetalVoice;

typedef struct {
    uint32_t phase[6], step[6], next_frame;
    unsigned active;
    float low, air_low, air_high_low, mid1, mid2, high1, high2;
    float lp_p1, lp_p2, lp_n1, lp_n2;     /* (0.5) the pulses' and the noise's 13 kHz low-pass */
    MetalVoice voice[6];
#ifdef METAL_TEST_COUNTERS
    uint64_t rendered_frames;
#endif
} MetalBank;

typedef struct {
    MetalBank bank[2];
    uint32_t frame;
} Metal;

void metal_init(Metal *m);
void metal_hit(Metal *m, int pad, float velocity, const KitPatch *patch);
/* Add mono samples. Caller owns the bus and splits render windows at events. */
/* Lanes are closed hat, open hat and pad 9. Aliasing the outputs is supported. */
void metal_render_routed(Metal *m, float *lanes[3], unsigned n);
void metal_render(Metal *m, float *mono, unsigned n);
#endif
