#ifndef FM1_ORIGINAL_MATH_H
#define FM1_ORIGINAL_MATH_H
#include <stdint.h>
#define KIT_RATE 44100

/* Unit phase, 2^32 steps per cycle. No transcendentals in this oscillator. */
float kit_sine(uint32_t phase);
static inline uint32_t kit_increment(float hz)
{
    return (uint32_t)(hz * (4294967296.0f / KIT_RATE));
}
static inline float kit_unit(float x)
{
    return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
}
/* Velocity shaping (GhettoMachine 0.5). A hit at the sequencer's normal velocity (88/127) sounds as the patch says;
   kit_hard is how far above it a hit is (0 .. 1 at full velocity: an accent), kit_soft how far below (0 .. 1 at
   nothing). Each voice turns them into its own differences once, at the strike. */
#define KIT_NORMAL_VELOCITY (88.0f / 127.0f)
static inline float kit_hard(float velocity)
{
    return velocity > KIT_NORMAL_VELOCITY ? (velocity - KIT_NORMAL_VELOCITY) * (1.0f / (1.0f - KIT_NORMAL_VELOCITY)) : 0.0f;
}
static inline float kit_soft(float velocity)
{
    return velocity < KIT_NORMAL_VELOCITY ? (KIT_NORMAL_VELOCITY - velocity) * (1.0f / KIT_NORMAL_VELOCITY) : 0.0f;
}
static inline float kit_noise(uint32_t *seed)
{
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
#endif
