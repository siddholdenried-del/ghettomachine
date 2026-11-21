/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine: the instrument tracks' synth (PLAN.md, phase 2). Felucca-Engines' ANALOG voice (MIT, dsp/felucca/: two
 * band-limited oscillators, a sub, noise, drive and a trapezoidal state-variable filter) played by a host of our own:
 * a pool of 16 voices shared by T2..T4, an amp and a filter ADSR, glide, unison, a mono note stack with legato, voice
 * caps and stealing, and a pool size the app lowers when the CPU is short (shedding).
 *
 * Tracks are the app's numbers, 1..3 (0, the drums, never plays here).
 * Rendering is in blocks of SYN_CTL samples: syn_prepare (the control tick: envelopes, glide, each voice's block
 * modulation; it also deals the voices into two halves), syn_half(0) and syn_half(1) (either core: each renders its
 * half into its own buffers), then syn_mix. Everything else runs between blocks: the audio interrupt, or the main loop
 * inside plat_lock().
 * This unit is compiled on its own (fm1.toml o2): the engine's dsp.c and the platform's headers never meet. */
#pragma once
#include <stdint.h>

#define SYN_CTL 32
#define SYN_VOICES 16
#define SYN_TRACKS 4

enum {                                    /* a sound's parameters (the order of the EDIT and ENV pages) */
    SP_WAVE, SP_DETUNE, SP_MIX, SP_INTV,  /* OSC */
    SP_SUB, SP_NOISE, SP_PWM, SP_DRIVE,   /* TONE */
    SP_CUT, SP_RES, SP_FENV, SP_FTYPE,    /* FILTER */
    SP_KTR, SP_VEL, SP_SPREAD, SP_LEGATO, /* MORE */
    SP_VOICES, SP_GLIDE,                  /* VOICE (with the app's TYPE and LEVEL) */
    SP_AA, SP_AD, SP_AS, SP_AR,           /* AMP ENV */
    SP_FA, SP_FD, SP_FS, SP_FR,           /* FILTER ENV */
    /* the synth update (project format 2): 0.2's sounds had the 26 above (SP_NUM_V1), these take their defaults */
    SP_WAVE2, SP_FM, SP_RING, SP_DRIFT,   /* OSC 2: its own wave (0 the same as OSC 1's), FM and ring from it, drift */
    SP_SHTYPE, SP_SHAPE, SP_BIAS, SP_SHPOS,   /* SHAPER: FOLD CLIP RECTIFY CRUSH, its amount (0 off), bias, PRE/POST */
    SP_M1, SP_M2, SP_M3, SP_M4,           /* MACRO: four knobs, sources of the matrix */
    SP_MA, SP_MD, SP_MS, SP_MR,           /* MOD ENV: a third envelope, a source of the matrix */
    SP_L2SHAPE, SP_L2RATE, SP_L2FADE, SP_L2KEY,   /* LFO 2: each note's own, a source of the matrix */
    SP_XM,                                /* the matrix: SYN_SLOTS slots, each a source (MS_*), destination (MD_*),
                                           * amount (-64..63) */
    SP_NUM = SP_XM + 3 * 6
};
#define SYN_SLOTS 6
#define SP_NUM_V1 SP_WAVE2                /* the parameters a sound had in project format 1 (0.2) */

/* the matrix's sources: the bipolar ones -1..1 (LFO 1, LFO 2, NOTE: the note from C4, RANDOM: each note's own),
 * the others 0..1 */
enum { MS_OFF, MS_LFO1, MS_LFO2, MS_MENV, MS_FENV, MS_AENV, MS_VEL, MS_NOTE, MS_RND, MS_M1, MS_M2, MS_M3, MS_M4, MS_NUM };
/* its destinations: at amount +-64 a source of 1 moves PITCH and OSC 2 (OSC 2's pitch) +-48 semitones (the amount
 * squared: fine near 0), DETUNE +-127 cents, CUTOFF its whole range, the 0..127 parameters 127 steps, BIAS 64, LEVEL
 * up to twice or to nothing, PAN all the way, LFO 2 RATE 127 steps of its rate */
enum { MD_PITCH, MD_OSC2, MD_DETUNE, MD_CUTOFF, MD_RESO, MD_PWM, MD_MIX, MD_SUB, MD_NOISE, MD_DRIVE, MD_FM, MD_RING,
       MD_SHAPE, MD_BIAS, MD_LEVEL, MD_PAN, MD_L2RATE, MD_NUM };

enum { SF_INT, SF_ENUM, SF_SIGNED, SF_SEMI, SF_TIME, SF_GLIDE, SF_HZ, SF_LFOHZ, SF_FADE };
typedef struct {
    const char *name;
    int8_t min, max, def;
    uint8_t fmt;                          /* SF_* */
    const char *const *names;             /* SF_ENUM */
} syn_param_t;

const syn_param_t *syn_param(int id);
void syn_param_text(int id, int v, char *buf);   /* the value as the screen shows it (buf: 16 bytes) */

/* the factory sounds */
int syn_presets(void);
const char *syn_preset_name(int i);
int syn_preset(int i, int8_t *v);         /* fills v[SP_NUM]; returns 1 for a MONO sound, 0 for POLY */
void syn_defaults(int8_t *v, int from);   /* v[from..SP_NUM - 1] at their defaults (a format-1 sound: from SP_NUM_V1) */

void syn_init(void);
void syn_sound(int trk, const int8_t *v); /* every parameter of the track's sound */
void syn_set(int trk, int id, int v);
void syn_level(int trk, float level);     /* 0..1 */
/* a note: vel 0..1; gate in samples before it lets go by itself, < 0 held until syn_note_off. mono: MONO (one note,
 * SP_VOICES the unison stack, the held keys a stack it goes back down, SP_LEGATO: 1 ON, glide whenever the voice still
 * sounds; 2 SLIDE, glide only into a tied note, as a 303; SP_GLIDE); else POLY (SP_VOICES
 * the track's cap) */
void syn_note_on(int trk, int note, float vel, int32_t gate, int mono);
/* the same with a 303's marks (0.4: the sequencer's steps): SN_SLIDE, the track's note still held glides to this one
 * (MONO, POLY or SAMPLER, whatever LEGATO says; GLIDE's time, or 58 ms with GLIDE at 0), no new attack; SN_ACCENT,
 * louder and a filter sweep */
#define SN_SLIDE 1
#define SN_ACCENT 2
void syn_note_on_f(int trk, int note, float vel, int32_t gate, int mono, int flags);
void syn_note_off(int trk, int note);
void syn_release(int trk, int seq_only);  /* the track's notes let go (trk < 0: all); seq_only: gated ones only */
void syn_kill(void);
/* Hard-cut this track, including its contribution to the last rendered block. */
void syn_cut_track(int trk);
void syn_pool(int n);                     /* the voices that may sound, 1..SYN_VOICES (the CPU's shedding) */
int syn_voices(int trk);                  /* voices sounding (trk < 0: all) */
int syn_pitch16(int trk);                 /* the track's newest voice's pitch now, 1/16 semitone; -1 none */

/* SAMPLER tracks (PLAN.md, phase 3): the track's source is a sample instead of the oscillators, through the same drive,
 * filter, envelopes and pool. 12-bit, packed two in three bytes (s0 low 8 bits; s0 high 4 | s1 low 4 << 4; s1 high 8
 * bits), frames at rate Hz. The data stays the caller's (in place in the flash, or a recording in RAM) and must not
 * change while it is set: set 0 first. A new source or sample frees the track's voices; a note on a SAMPLER track with
 * no sample is silent */
void syn_source(int trk, int sampler);
void syn_sample(int trk, const uint8_t *packed, uint32_t frames, uint32_t rate);
/* where the sample plays: start, end and the loop's start in thousandths of it (the loop start moved to the nearest
 * rising zero crossing); loop 0 once, 1 round from the loop start to the end; root the note that plays it at its own
 * pitch; tune -50..50 cents */
void syn_sample_shape(int trk, int start, int end, int loop, int lstart, int root, int tune);
uint32_t syn_sample_frames(int trk);

/* modulation (PLAN.md, phase 4): a track's LFO, set once a block before syn_prepare. lfo -1..1, depth -1..1. CUTOFF moves
 * +-64 of the 128 cutoff steps (about 4 octaves), PITCH +-12 semitones, LEVEL dips by up to all of it, PWM and the rest
 * +-64 of 127, START (a SAMPLER) +-half the sample, as each note starts */
enum { SM_CUTOFF, SM_PITCH, SM_LEVEL, SM_PWM, SM_RESO, SM_DETUNE, SM_MIX, SM_DRIVE, SM_NOISE, SM_SUB, SM_START, SM_NUM };
void syn_mod(int trk, int dest, float lfo, float depth);
int syn_get(int trk, int id);             /* a parameter as the voices use it now (a lock's value included) */
int syn_probe(int trk, int what);         /* tests, the track's newest voice: 0 its pitch (1/16 semitone, the LFO's
                                           * in), 1 its cutoff (0..127 << 8, the envelope's and the LFO's in), 2 its MOD
                                           * ENV x 1000, 3 its LFO 2 x 1000, 4 its amplitude (Q15), 5 its pan x 1000
                                           * (-1000 left .. 1000 right), 6 its fine tune (1/4096 of a step: drift, the
                                           * matrix's pitch between 1/16 semitones, unison), 7 its accent's sweep x
                                           * 1000, 8 accented (0/1), 100 + SP_*: that parameter
                                           * as the voice plays it (the matrix's moves in); -1 none */

void syn_prepare(void);
void syn_half(uint32_t half);
/* the mixer's view of a track (phase 5): pan 0..127 (64 the centre), its reverb and delay sends 0..1 */
void syn_mixer(int trk, int pan, float rev, float dly);
void syn_mix(float *l, float *r, float *rev, float *dly);   /* SYN_CTL samples each, overwritten: the mix and its sends */
/* the last syn_mix's block of one track alone (its part of l and r: its level and pan, no sends), SYN_CTL samples a
 * side (0 left, 1 right): the USB stems. NULL: the track was silent in it */
const float *syn_track(int trk, int side);
float syn_peak(int trk);                  /* the track's peak in the mix since the last look (the screen's meters) */
