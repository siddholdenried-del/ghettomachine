/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine adapter for JimmyKaps' GPL-3.0-only TR-707C synthesized voice bank.
 * 44.1 kHz, no sample data, no allocation. See drum707.md for provenance. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

enum { D7_BD, D7_SD, D7_LT, D7_MT, D7_HT, D7_RS, D7_CP, D7_CH, D7_OH, D7_CR, D7_RD, D7_CB, D7_TB,
       D7_NUM, D7_KIT = D7_NUM };
enum { D7P_LEVEL, D7P_TUNE, D7P_DECY, D7P_PAN, D7P_REV, D7P_DLY, D7P_NUM };
enum { D7K_CHOKE, D7K_NUM };

typedef struct {
    uint32_t ph1, ph2, inc1, inc2, pitch_sweep, increment, noise_hp, decay, at, rng;
    int32_t env, lp, pe;
    float vel, pan_l, pan_r;
    uint8_t active, pot[D7P_NUM];
} drum707_voice_t;
typedef struct {
    uint8_t choke;
    drum707_voice_t voice[D7_NUM];
} drum707_t;
typedef char drum707_ram_budget[(sizeof(drum707_t) <= 1024u) ? 1 : -1];

/* Trigger/cut/set run between render blocks, or under the app's existing audio lock. */
void drum707_init(drum707_t *d);
void drum707_trigger(drum707_t *d, int voice, float vel);
void drum707_cut(drum707_t *d, int voice);
int drum707_active(const drum707_t *d);
/* Adds into all four buffers; caller handles output clearing and master. */
void drum707_render_st(drum707_t *d, float *l, float *r, float *rev, float *dly, int n);
int drum707_nparams(int voice);
const x0x_param_t *drum707_param(int voice, int slot);
int drum707_get(const drum707_t *d, int voice, int slot);
void drum707_set(drum707_t *d, int voice, int slot, int value);
