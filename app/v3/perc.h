#ifndef FM1_ORIGINAL_PERC_H
#define FM1_ORIGINAL_PERC_H

#include "kit_patch.h"
#include <stdint.h>

typedef struct {
    uint32_t phase, mod_phase, noise;
    int mode;
    float level, release, attack, amplitude;
    float pitch_end, pitch_offset, pitch_decay;
    float cutoff_min, cutoff_offset, cutoff_decay;
    float pole1, pole2;
    float last, carry;
} PercVoice;

void perc_init(PercVoice *voice);
void perc_hit(PercVoice *voice, float velocity, const PercPatch *patch);
void perc_render(PercVoice *voice, float *mono, unsigned n);
#endif
