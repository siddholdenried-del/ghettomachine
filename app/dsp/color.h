#ifndef GM_COLOR_H
#define GM_COLOR_H
#include <stdint.h>

/* Stereo post-send Color. All levels 0..127, except ON 0/1. */
enum { COL_AMOUNT, COL_WEAR, COL_WOW, COL_DIGITAL, COL_DROP, COL_ON, COL_NUM };
typedef struct { uint8_t v[COL_NUM], spare[2]; } color_params_t;
#define COL_RING 128
typedef struct {
    float envelope, slow, fast, dropout, crack, hold_l, hold_r;
    float hiss_lp[2];
    uint32_t rand;
    uint16_t pos, valid, remain, countdown, clock;
    int16_t ring[2][COL_RING]; /* no need to clear: reset invalidates reads */
} color_state_t;

typedef struct {
    float hold[2];
    int16_t ring[2][COL_RING];
} color_sends_t;

int color_active(const color_params_t *p);
void color_reset(color_state_t *s);
void color_defaults(color_params_t *p);
void color_run(color_state_t *s, const color_params_t *p, float *l, float *r, unsigned n);
/* Preserve existing per-lane send weights on DRUMS: transform each independent
 * preweighted send at the same instant with the dry track's control/modulation. */
void color_run_drums(color_state_t *s, color_sends_t *sends, const color_params_t *p,
                     float *l, float *r, float *rev, float *dly, unsigned n);
#endif
