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
