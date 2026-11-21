#ifndef GM_TRACK_INSERT_H
#define GM_TRACK_INSERT_H
#include <stdint.h>

/* Stable mode IDs in saved projects. Rate is 44.1 kHz, set by the groovebox. */
enum { TI_OFF, TI_PHASE, TI_TREM, TI_CHORUS, TI_FLANGE, TI_COMB, TI_ENV, TI_SHIFT, TI_DOUBLE, TI_RESON, TI_MODES };
typedef struct { uint8_t mode, v[4]; } track_insert_param_t;
#define TI_DELAY 1024
typedef struct {
    float phase, rate[2], osc_s[2], osc_c[2];
    float ap[2][4], fb[2], env, lp[2], band[2];
    float reson[2][2][2];
    uint16_t pos, hist;
    int16_t ring[2][TI_DELAY]; /* last: reset invalidates history rather than clearing 4 KiB */
} track_insert_t;

const char *track_insert_name(int mode);
const char *track_insert_label(int mode, int knob);
void track_insert_default(track_insert_param_t *p, int mode);
void track_insert_reset(track_insert_t *s);
void track_insert_process(track_insert_t *s, const track_insert_param_t *p, float *l, float *r, unsigned n);
#endif
