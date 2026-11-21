/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal (fm1-base's template, which this starts from) */
/* GhettoMachine: four tracks (PLAN.md, phase 1).
 *
 * T1 is the drum track, X0X-style: two kits (909 / 808, PRESETS), 11 pads on the black keys, accents (SEQ held +
 * steps), per-pad editing (EDIT), per-step chance (a step held + SELECT) and locks (EDIT, a step held + a knob), and a
 * MUTE page (ENV) whose mutes belong to the pattern.
 * T2..T4 are instrument tracks. Each has a TYPE: MONO, POLY or SAMPLER. MONO and POLY play the synth (dsp/synth.h:
 * Felucca's ANALOG voice, 16 voices shared, on both cores); SAMPLER plays the track's sample through the same voice
 * (a bank in the flash: a WAV from the Mac, or a recording of the FM-1's own sound, REC held + SEL). A step holds a chord of up to 4 notes, a velocity, a gate
 * and a chance. The KEYS page plays them from all 27 keys; REC records them, live while playing (to the nearest
 * step) or step by step while stopped (a chord per step, the cursor moving on when the keys are let go).
 * A pattern holds all four tracks; each track's part has its own length and rate. ALGORITHM (or SEL held + pad keys
 * 1-4) picks the track; HOME + pad keys 1-4 mute tracks. The project is a file in the data area (A / B copies).
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope.
 * The engines are behind dsp/drums.h and dsp/synth.h (their own units). */
#include "dsp/drums.h"
#include "dsp/synth.h"
#include "dsp/track_insert.h"
#include "dsp/color.h"
#include "dsp/fx.h"
#include "dsp/uimath.h"
#include "logo.h"                       /* tools/gen_logo.py: the boot screen's logo */
#include "mono_font.h"                  /* tools/gen_mono.py: FONT_MS, FONT_MB (IBM Plex Mono) */
#include <stddef.h>

#define NPADS DRUMS_NPADS
#define NPAT 16
#define NSTEPS 128
#define OLD_STEPS 64
#define STEP_WORDS (NSTEPS / 32)
#define NPOTS 12                          /* knob positions kept per pad (the engines have at most 11) */
#define NLOCKS 16                         /* per drum part: a step's chance or a knob locked for one pad's step */
#define LK_KIT606 0x40u                    /* 606 locks keep their own pots */
#define LK_KIT808 0x80u                   /* lock_t.param: the 808's knob (else the 909's) */
#define LK_KIT707 (LK_KIT606 | LK_KIT808) /* both bits unused by legacy locks */
#define LK_PROB 0x7Fu                     /* lock_t.param: the step's chance, either kit */
#define NTRK 4                            /* tracks: the drums, then T2..T4 */
#define NINST 3                           /* instrument tracks */
#define TR_DRUM 0
#define NCHORD 4                          /* notes a step holds */
#define NCHORDS 16                        /* chord ID shares a byte with held-key inversion */
#define GATE_DEF 6                        /* a new step's gate: 6/8 of a step */
#define GATE_MAX (NSTEPS * 8)             /* 128 steps, still edited in eighths of one step */
#define NKEYS 27
#define KEY_NOTE0 53                      /* key 0 (F3) at octave 0 */
#define PROJ_MAGIC 0x31544745u            /* "EGT1": fm1-eight 0.1's project; another magic loads the default project */
#define PROJ_VERSION 7                    /* combined Color and mixed kits; 1–5 readable, private divergent v6 rejected */
#define EIGHT_VERSION "0.4"
#define NSONG 32                          /* the song chain's entries */
#define NUSER 16                          /* user sounds (T2..T4) */
/* IDs are persisted in tracks and per-pattern sounds: old factory 0..23, USER 1..16 at 24..39,
 * new factory from 40. The physical PRESETS order follows those IDs (skipping empty user slots).
 * The DSP factory table itself stays contiguous; never use its growing count as a user offset. */
#define PRESET_USER_FIRST SYN_FACTORY_LEGACY_COUNT
#define PRESET_NEW_FIRST (PRESET_USER_FIRST + NUSER)
static int user_preset_slot(int id) { return id >= PRESET_USER_FIRST && id < PRESET_NEW_FIRST ? id - PRESET_USER_FIRST : -1; }
static int factory_preset_index(int id) { return id >= PRESET_NEW_FIRST ? id - NUSER : id; }
#define NUKIT 8                           /* user kits (the drums) */
#define DPRESET_606 (2 + NUKIT)           /* append; keep every old user kit ID */
#define DPRESET_707 (DPRESET_606 + 1)
#define DPRESET_COUNT (DPRESET_707 + 1)
static int dpreset_factory(int i) { return i >= 0 && i < 2 ? i : i == DPRESET_606 ? KIT_606 : i == DPRESET_707 ? KIT_707 : -1; }
static int kit_factory(int i) { return i == KIT_606 ? DPRESET_606 : i == KIT_707 ? DPRESET_707 : i; }
static int dpreset_user(int i) { return i >= 2 && i < DPRESET_606; }
static int dpreset_display(int i) { return i == DPRESET_606 ? 2 : i == DPRESET_707 ? 3 : dpreset_user(i) ? i + 2 : i; }
static int dpreset_id(int i) { return i == 2 ? DPRESET_606 : i == 3 ? DPRESET_707 : i > 3 ? i - 2 : i; }


/* ------------------------------------------------------------------ project --- */
enum { RATE_16, RATE_8, RATE_32, RATE_16T, RATE_8T, NRATES };
static const char *const RATE_NAME[NRATES] = {"1/16", "1/8", "1/32", "1/16T", "1/8T"};
static const float RATE_MUL[NRATES] = {1.0f, 2.0f, 0.5f, 2.0f / 3.0f, 4.0f / 3.0f};
enum { TY_MONO, TY_POLY, TY_SAMPLER, NTYPES };

typedef struct {
    uint8_t step, pad;
    uint8_t param;                        /* the knob's index (| LK_KIT808), or LK_PROB */
    uint8_t val;                          /* the knob's value, or the chance 0..99 % */
} lock_t;

typedef struct {                          /* the drum track's part of a pattern */
    uint32_t hit[NPADS][STEP_WORDS];       /* 32 steps per word */
    uint32_t acc[STEP_WORDS];                      /* accented steps */
    uint16_t mute;                        /* a bit per pad: muted in this pattern (the MUTE page) */
    uint8_t len;                          /* 1..128 */
    uint8_t rate;                         /* RATE_* */
    uint8_t nlk, rsv[3];                  /* locks in use */
    lock_t lk[NLOCKS];
} dpart_t;

typedef struct {                          /* an instrument step (format 1 had a spare byte more: nstep_v1_t) */
    uint8_t note[NCHORD];                 /* MIDI notes, packed from the first; note[0] == 0: the step is off */
    uint8_t vel;                          /* 1..127 */
    uint8_t prob;                         /* chance, 5..100 % */
    uint16_t gate;                        /* eighths of a step, 1..GATE_MAX */
} nstep_t;

/* a 303's marks in the steps' spare top bits (0.4; no file before has them set): the velocity's, an accent; the
 * chance's, a slide into the next step. A 303's lengths are gates: half a step, a WHOLE note held to just before the
 * next note (over the rests between), a slide past the next step's start */
#define NS_ACC 0x80u
#define NS_SLIDE 0x80u
#define GATE_HALF 4
#define GATE_SLIDE 10
static inline int ns_vel(const nstep_t *s) { return s->vel & 0x7F; }
static inline int ns_prob(const nstep_t *s) { return s->prob & 0x7F; }

#define NILOCKS 16                        /* per instrument part: a sound parameter locked for one step */
typedef struct {
    uint8_t step, param;                  /* param: dsp/synth.h SP_* */
    int8_t val;
    uint8_t rsv;
} ilock_t;

typedef struct {                          /* an instrument track's part of a pattern */
    nstep_t st[NSTEPS];
    uint8_t len, rate, nlk, rsv;
    ilock_t lk[NILOCKS];
} ipart_t;

typedef struct {
    dpart_t d;
    ipart_t t[NINST];                     /* T2..T4 */
} pattern_t;

typedef struct {                          /* an instrument track's settings */
    uint8_t type;                         /* TY_* */
    uint8_t level;                        /* 0..127 */
    int8_t oct;                           /* the keys' octave, -3..3 */
    uint8_t preset;                       /* the factory sound it started from */
    uint8_t edited;                       /* changed since */
    uint8_t rsv[3];
    int8_t snd[SP_NUM];                   /* the sound (dsp/synth.h SP_*) */
    uint16_t s_start, s_end, s_lstart;    /* SAMPLER: where it plays, thousandths of the sample */
    uint8_t s_loop;                       /* 0 once, 1 round its loop */
    uint8_t s_root;                       /* the note that plays it at its own pitch */
    int8_t s_tune;                        /* cents, -50..50 */
    uint8_t s_q;                          /* the next recording: 0 44.1 kHz, 1 22.05 kHz */
    uint8_t arp_mode, arp_rate, arp_oct, arp_gate, arp_latch;   /* the arpeggiator: AM_*, ARP_RATE index, 1..4, %, 0/1 */
    uint8_t chord, chord_inv;             /* chord mode (ARP 2): a key plays CHORD_IV[chord], inverted chord_inv times */
    uint8_t rsv2;
} track_t;

typedef struct {                          /* a track's sound kept with a pattern (SETUP: SOUNDS PER PATTERN) */
    int8_t snd[SP_NUM];
    uint8_t type, level, preset, edited;
    uint8_t used;                         /* 0: none yet (the pattern takes the sound playing when it starts) */
} psound_t;

typedef struct {                          /* a track's LFO */
    uint8_t shape;                        /* LS_* */
    uint8_t rate, div;                    /* free: 0..127 (0.05..30 Hz); synced: a DIV_* */
    uint8_t sync, retrig;                 /* LR_* */
    uint8_t fade, phase;                  /* NOTE: the time it fades in; where it starts, 0..127 a cycle */
    uint8_t dest;                         /* instrument: SM_*; drums: the pad's knob (LFO_OFF: none) */
    int8_t depth;                         /* -64..63 */
    uint8_t pad;                          /* drums: the pad it moves */
    uint8_t rsv[2];                       /* drum model +1 in rsv[0], zero retains legacy LFO targeting */
} lfo_t;
#define LFO_OFF 0xFFu

enum { GS_MINOR, GS_PHRYG, GS_DORIAN, GS_PENTA, GS_BLUES, GS_HARM, GS_MAJOR, GS_MIXO, NGSCALES };   /* its scales */
typedef struct {                          /* a part's 303 generator (the ACID pages, 0.4) */
    uint16_t seed;                        /* the line, 1..999; 0: never generated (it starts from the track's last) */
    uint8_t mut;                          /* MUTATEs since the seed */
    uint8_t root;                         /* the root, MIDI 24..71 */
    uint8_t scale;                        /* GS_* */
    uint8_t notes;                        /* the scale's notes it uses, 1..7 (its favourites first) */
    uint8_t dens, slide, accent, oct;     /* chances, 0..100 %: a step plays; slides into the next; accented; an octave */
    uint8_t whole;                        /* the chance a note is WHOLE (held to the next note), else HALF, 0..100 % */
    uint8_t rsv;
} gen_t;

typedef struct {
    uint32_t magic;
    uint8_t kit;                          /* KIT_909 / KIT_808 / KIT_606 */
    uint8_t pat;                          /* the pattern playing and edited */
    uint8_t swing;                        /* 50..75 % */
    uint8_t tmute;                        /* a bit per track: muted (HOME + pad keys 1-4) */
    uint16_t bpm10;                       /* tempo x10: 600..2400 */
    uint8_t dpreset;                      /* 0=909, 1=808, 2..9=user kits, 10=606 */
    uint8_t version;                      /* PROJ_VERSION */
    track_t tr[NINST];
    lfo_t lfo[NTRK];
    struct {                              /* the mixer (GLO): a track's level (the drums'; T2..T4 keep theirs in track_t),
                                           * pan (64 the centre) and its reverb and delay sends, 0..127 */
        uint8_t level, pan, rev, dly;
    } mix[NTRK];
    uint8_t fx[FXP_NUM];                  /* the FX pages' knobs (dsp/fx.h) */
    uint8_t song_n, song_on;              /* the song: its entries, and whether PLAY plays it */
    uint8_t click;                        /* the metronome (hold REC): bits 0-1 OFF, ON, REC (while recording); 2-3 the
                                           * count-in's bars (0 none); 4-7 its level, 1..15 (0: 10) */
    uint8_t recq;                         /* bits 0-1 recording's grid: 0 a step, 1 two, 2 four (1/16, 1/8, 1/4 at 1/16);
                                           * 2-3 MIDI CLOCK: 0 off, 1 sent, 2 followed; 4-5 LAUNCH, when a clip or
                                           * scene starts: 0 the bar, 1 the beat, 2 at once; 6 SOUNDS PER PATTERN */
    struct {
        uint8_t pat, bars;                /* a pattern (0..15) for 1..16 bars */
    } song[NSONG];
    pattern_t p[NPAT];
    uint8_t pots[2][NPADS + 1][NPOTS];   /* each kit's knobs: pads, then the kit */
    /* appended in 0.2's performance update: a project saved before it loads, these zeroed (project_load) */
    psound_t psnd[NPAT][NINST];           /* SOUNDS PER PATTERN: each pattern's T2..T4 sound */
    uint8_t pkit[NPAT];                   /* and its drum kit: the preset + 1 (0: none yet) */
    uint8_t follow[NPAT];                 /* a scene's follow action: bits 0-2 OFF NEXT PREV FIRST RANDOM, 3-6 bars - 1 */
    /* appended in 0.4 (format 3): a format-2 file (0.3's) loads, these zeroed */
    gen_t gen[NPAT][NINST];               /* each part's 303 generator */
    uint8_t pots606[NPADS + 1][NPOTS];    /* appended in format 4; legacy prefix and tail stay fixed */
    track_insert_param_t insert[NINST];  /* format 5: track-wide inserts, one per instrument */
    uint8_t pots707[NPADS + 1][NPOTS];    /* format 7: older metadata ends immediately before this field */
    uint8_t model[NPADS];                /* 0 inherits, 1..KIT_NUM picks an engine */
    uint8_t pattern_model[NPAT][NPADS];  /* saved with the pattern when Sounds per Pattern is active */
    color_params_t color[NTRK];          /* format 7: independent Color per track */
} project_t;
/* Freeze the persisted 64-step layouts; legacy files end before the 606 pots. */
typedef struct {
    uint32_t hit[NPADS][2], acc[2];
    uint16_t mute;
    uint8_t len, rate, nlk, rsv[3];
    lock_t lk[NLOCKS];
} dpart_old_t;
typedef struct {
    uint8_t note[NCHORD], vel, gate, prob;
} nstep_old_t;
_Static_assert(sizeof(nstep_old_t) == 7u && sizeof(nstep_t) == 8u, "stable legacy and format-4 step layouts");
typedef struct {
    nstep_old_t st[OLD_STEPS];
    uint8_t len, rate, nlk, rsv;
    ilock_t lk[NILOCKS];
} ipart_old_t;
typedef struct { dpart_old_t d; ipart_old_t t[NINST]; } pattern_old_t;
#define PROJ_PREFIX ((uint32_t)offsetof(project_t, p))
#define PROJ_TAIL ((uint32_t)offsetof(project_t, pots))
#define PROJ_TAIL_BYTES (sizeof(project_t) - PROJ_TAIL)
#define OLD_TAIL (PROJ_PREFIX + NPAT * sizeof(pattern_old_t))
#define PROJ_LEGACY_TAIL_BYTES ((uint32_t)offsetof(project_t, pots606) - PROJ_TAIL)
#define PROJ_V3_SIZE (OLD_TAIL + PROJ_LEGACY_TAIL_BYTES)
#define PROJ_V2_SIZE (PROJ_V3_SIZE - sizeof(((project_t *)0)->gen))
#define PROJ_CAPACITY (8u * 4096u - PLAT_FILE_HEAD)
#define PROJ_META (PROJ_PREFIX + PROJ_TAIL_BYTES)
#define PROJ_V5_META_BYTES (offsetof(project_t, insert) + sizeof(((project_t *)0)->insert) - PROJ_TAIL)
#define PROJ_V5_TAIL_BYTES ((PROJ_V5_META_BYTES + 3u) & ~3u)
#define PROJ_V4_TAIL_BYTES (((offsetof(project_t, insert) + 3u) & ~3u) - PROJ_TAIL)

/* project format 1 (0.1, 0.2 and 0.2's performance update), frozen: a file saved by them is read a part at a time
 * into format 2 (project_load_v1). Its sounds had 26 parameters, its steps a spare byte; 0.2 saved it up to psnd */
typedef struct {
    uint8_t note[NCHORD];
    uint8_t vel, gate, prob, rsv;
} nstep_v1_t;
typedef struct {
    nstep_v1_t st[OLD_STEPS];
    uint8_t len, rate, nlk, rsv;
    ilock_t lk[NILOCKS];
} ipart_v1_t;
typedef struct {
    dpart_old_t d;
    ipart_v1_t t[NINST];
} pattern_v1_t;
typedef struct {
    uint8_t type, level;
    int8_t oct;
    uint8_t preset, edited, rsv[3];
    int8_t snd[SP_NUM_V1];
    uint16_t s_start, s_end, s_lstart;
    uint8_t s_loop, s_root;
    int8_t s_tune;
    uint8_t s_q;
    uint8_t arp_mode, arp_rate, arp_oct, arp_gate, arp_latch;
    uint8_t chord, chord_inv, rsv2;
} track_v1_t;
typedef struct {
    int8_t snd[SP_NUM_V1];
    uint8_t type, level, preset, edited, used;
} psound_v1_t;
typedef struct {
    uint32_t magic;
    uint8_t kit, pat, swing, tmute;
    uint16_t bpm10;
    uint8_t dpreset, version;
    track_v1_t tr[NINST];
    lfo_t lfo[NTRK];
    struct {
        uint8_t level, pan, rev, dly;
    } mix[NTRK];
    uint8_t fx[FXP_NUM];
    uint8_t song_n, song_on, click, recq;
    struct {
        uint8_t pat, bars;
    } song[NSONG];
    pattern_v1_t p[NPAT];
    uint8_t pots[2][NPADS + 1][NPOTS];
    psound_v1_t psnd[NPAT][NINST];
    uint8_t pkit[NPAT];
    uint8_t follow[NPAT];
} project_v1_t;
#define V1_OFF(f) ((uint32_t)offsetof(project_v1_t, f))
#define PRESETS_V1 15                     /* format 1's factory sounds: its user slots (preset) began there */
_Static_assert(sizeof(project_v1_t) == 32680u && V1_OFF(psnd) == 31160u, "format 1 as 0.2's performance update saved it");
_Static_assert(offsetof(project_t, tr) == V1_OFF(tr), "the header as format 1's");
_Static_assert(offsetof(project_t, song) - offsetof(project_t, lfo) == V1_OFF(song) - V1_OFF(lfo), "lfo .. song as format 1's");
_Static_assert(offsetof(project_t, follow) - offsetof(project_t, pkit) == V1_OFF(follow) - V1_OFF(pkit), "pkit, follow");
_Static_assert(PROJ_V2_SIZE == 31544u, "format 2 as 0.3 saved it");

/* the project's file: two copies of 32 KiB at the start of the data area (plat_file_*) */
static const plat_file_t PROJ_FILE = {0x97000u, 0x9F000u, 8u * 4096u};
_Static_assert(PROJ_V3_SIZE == 32120u, "format 3 as 0.4 saved it");

static project_t proj PLAT_POOL;
/* The FM-1 linker has distinct BSS/POOL budgets. Balance six fixed delay
 * channels between them; no flash or sampler memory is borrowed. */
static track_insert_t insert_rt[NINST];
static color_state_t color_rt[NTRK];
static color_sends_t color_drums_send;
static int16_t insert_ring[(NINST * 2) - 1][TI_DELAY];
static int16_t insert_pool[TI_DELAY] PLAT_POOL;
static uint8_t (*kit_pots(int k))[NPOTS] { return k == KIT_707 ? proj.pots707 : k == KIT_606 ? proj.pots606 : proj.pots[k]; }
static int drum_model(int pad) { return drums_model(proj.kit, proj.model, pad); }
static int kit_is_custom(const uint8_t *map)
{
    if (map)
        for (int pad = 0; pad < NPADS; pad++)
            if (map[pad])
                return 1;
    return 0;
}
static const char *drum_kit_title(void) { return kit_is_custom(proj.model) ? "CUSTOM" : drums_kit_name(proj.kit); }
static void model_defaults(int kit)
{
    for (int pad = 0; pad <= NPADS; ++pad)
        for (int i = 0; i < NPOTS; ++i) {
            const x0x_param_t *p = drums_param(kit, pad, i);
            kit_pots(kit)[pad][i] = p ? p->def : 0;
        }
}
static uint8_t dirty;                     /* changed since the last save: SAVE is lit */

/* the screen's meters, written where the sound is made (the audio interrupt), read and let go by the screen */
static volatile float pad_hit[NPADS];     /* a pad's last hit (its velocity), until the screen has shown it */
static volatile float drum_pk;            /* the drums' peak in the mix since the screen looked */

static inline int bit_get(const uint32_t *b, int s) { return (int)(b[s >> 5] >> (s & 31) & 1u); }
static inline void bit_clear(uint32_t *b, int s) { b[s >> 5] &= ~(1u << (s & 31)); }
static inline void bit_flip(uint32_t *b, int s) { b[s >> 5] ^= 1u << (s & 31); }
static inline void bit_set(uint32_t *b, int s) { b[s >> 5] |= 1u << (s & 31); }
/* the scene grid (HOME): the 16 patterns' four parts are 16 x 4 clips. clip[t]: the pattern whose part track t plays,
 * and edits (-1: stopped; it edits the focused pattern's, proj.pat). A pattern started whole (a scene) sets all
 * four; a track's own launch only its own. qclip[t]: one waiting to start (-1 none, -2 to stop), at the bar, the beat
 * or at once (LAUNCH, on the SONG page) */
static volatile int8_t clip[NTRK], qclip[NTRK];
static volatile uint8_t fill_on;          /* FILL held (HOME): the drums play the next pattern's part */
static int clip_of(int t) { return clip[t] >= 0 ? clip[t] : proj.pat; }
static void set_clip(int t, int n);       /* a track onto a pattern's part, its sound with it (SOUNDS PER PATTERN) */
static void clips_raw(int n)              /* (a project loaded: nothing moved) */
{
    int t;
    for (t = 0; t < NTRK; t++) {
        clip[t] = (int8_t)n;
        qclip[t] = -1;
    }
}
static void clips_all(int n)
{
    int t;
    for (t = 0; t < NTRK; t++) {
        set_clip(t, n);
        qclip[t] = -1;
    }
}
static pattern_t *cur(void) { return &proj.p[proj.pat]; }
static dpart_t *dcur(void) { return &proj.p[fill_on ? (clip_of(TR_DRUM) + 1) % NPAT : clip_of(TR_DRUM)].d; }
static uint16_t drum_mutes_applied;
/* Mutes cancel the current ROLL snapshot; a fresh ROLL hold can replay it. */
static uint16_t roll_inhibit_pads;
static uint8_t roll_inhibit_tracks;

static int track_is_muted(int tr) { return (int)(proj.tmute >> tr & 1u); }

static int drum_lane_muted(int pad)
{
    return track_is_muted(TR_DRUM) || (dcur()->mute >> pad & 1u);
}

/* Same lock as the DSP: refresh after a pattern/FILL change as well as the UI.
 * Only rising edges cut a voice. Turning a mute off cannot resume a tail. */
static void sync_drum_mutes(void)
{
    uint16_t mask = track_is_muted(TR_DRUM) ? (uint16_t)((1u << NPADS) - 1u) : dcur()->mute;
    uint16_t cut = mask & (uint16_t)~drum_mutes_applied;
    int pad;
    if (mask == drum_mutes_applied)
        return;
    drum_mutes_applied = mask;
    roll_inhibit_pads |= cut;
    if (cut) color_reset(&color_rt[TR_DRUM]); /* no history from a newly cut lane */
    for (pad = 0; cut && pad < NPADS; pad++)
        if (cut >> pad & 1u) {
            drums_cut_pad(pad);
            pad_hit[pad] = 0.0f;
        }
}

static void pad_fire(int pad, float vel)
{
    if (pad < 0 || pad >= NPADS || drum_lane_muted(pad))
        return;
    if (!drums_has_pad(drum_model(pad), pad))
        return;
    drums_trigger_mapped(proj.kit, proj.model, pad, vel);
    if (vel > pad_hit[pad])
        pad_hit[pad] = vel;
}
static ipart_t *ipart(int tr) { return &proj.p[clip_of(tr)].t[tr - 1]; }   /* tr 1..3 */
static track_t *tset(int tr) { return &proj.tr[tr - 1]; }
static uint8_t *part_len(int tr) { return tr == TR_DRUM ? &dcur()->len : &ipart(tr)->len; }
static uint8_t *part_rate(int tr) { return tr == TR_DRUM ? &dcur()->rate : &ipart(tr)->rate; }

static int drum_row_used(const uint32_t *bits)
{
    int i;
    for (i = 0; i < STEP_WORDS; i++)
        if (bits[i])
            return 1;
    return 0;
}
static int dpart_empty(const dpart_t *d)
{
    int i;
    for (i = 0; i < NPADS; i++)
        if (drum_row_used(d->hit[i]))
            return 0;
    return 1;
}

static int ipart_empty(const ipart_t *p)
{
    int s;
    for (s = 0; s < NSTEPS; s++)
        if (p->st[s].note[0])
            return 0;
    return 1;
}

static int pattern_empty(const pattern_t *p)
{
    int t;
    if (!dpart_empty(&p->d))
        return 0;
    for (t = 0; t < NINST; t++)
        if (!ipart_empty(&p->t[t]))
            return 0;
    return 1;
}

static void pattern_clear(pattern_t *p)
{
    uint32_t i;
    for (i = 0; i < sizeof *p; i++)
        ((uint8_t *)p)[i] = 0;
    p->d.len = 16;
    p->d.rate = RATE_16;
    for (i = 0; i < NINST; i++) {
        p->t[i].len = 16;
        p->t[i].rate = RATE_16;
    }
}

/* instrument steps */
static void nstep_clear(nstep_t *s)
{
    uint32_t i;
    for (i = 0; i < sizeof *s; i++)
        ((uint8_t *)s)[i] = 0;
}

static int nstep_count(const nstep_t *s)
{
    int i;
    for (i = 0; i < NCHORD && s->note[i]; i++) {
    }
    return i;
}

static int nstep_add(nstep_t *s, int note, int vel)   /* 0: the note is in the step; -1: the chord is full */
{
    int i;
    if (!s->note[0]) {
        s->vel = (uint8_t)clamp(vel, 1, 127);
        s->gate = GATE_DEF;
        s->prob = 100;
    }
    for (i = 0; i < NCHORD; i++) {
        if (s->note[i] == note)
            return 0;
        if (!s->note[i]) {
            s->note[i] = (uint8_t)note;
            return 0;
        }
    }
    return -1;
}

/* an instrument part's locks: a sound parameter's value for one step */
static int ilock_find(const ipart_t *p, int step, int param)
{
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step && p->lk[i].param == param)
            return i;
    return -1;
}

static int ilock_put(ipart_t *p, int step, int param, int val)   /* 0, or -1: no lock left */
{
    int i = ilock_find(p, step, param);
    if (i < 0) {
        if (p->nlk >= NILOCKS)
            return -1;
        i = p->nlk++;
        p->lk[i].step = (uint8_t)step;
        p->lk[i].param = (uint8_t)param;
    }
    p->lk[i].val = (int8_t)val;
    return 0;
}

static int ilock_clear_step(ipart_t *p, int step)   /* how many went */
{
    int i = 0, j, n = 0;
    while (i < p->nlk) {
        if (p->lk[i].step == step) {
            for (j = i; j + 1 < p->nlk; j++)
                p->lk[j] = p->lk[j + 1];
            p->nlk--;
            n++;
        } else {
            i++;
        }
    }
    return n;
}

static int istep_locked(const ipart_t *p, int step)
{
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step)
            return 1;
    return 0;
}

/* locks: a step's chance (LK_PROB) or one knob's value for one pad on one step */
static int lock_find(const dpart_t *p, int step, int pad, int param)
{
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step && p->lk[i].pad == pad && p->lk[i].param == param)
            return i;
    return -1;
}

static int lock_put(dpart_t *p, int step, int pad, int param, int val)   /* 0, or -1: no lock left */
{
    int i = lock_find(p, step, pad, param);
    if (i < 0) {
        if (p->nlk >= NLOCKS)
            return -1;
        i = p->nlk++;
        p->lk[i].step = (uint8_t)step;
        p->lk[i].pad = (uint8_t)pad;
        p->lk[i].param = (uint8_t)param;
    }
    p->lk[i].val = (uint8_t)val;
    return 0;
}

static void lock_del(dpart_t *p, int i)
{
    for (; i + 1 < p->nlk; i++)
        p->lk[i] = p->lk[i + 1];
    if (p->nlk)
        p->nlk--;
}

static int lock_clear_step(dpart_t *p, int step, int pad)   /* every lock of that pad's step: how many went */
{
    int i = 0, n = 0;
    while (i < p->nlk) {
        if (p->lk[i].step == step && p->lk[i].pad == pad) {
            lock_del(p, i);
            n++;
        } else {
            i++;
        }
    }
    return n;
}

static int step_locked(const dpart_t *p, int step, int pad)
{
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step && p->lk[i].pad == pad)
            return 1;
    return 0;
}

static int lk_param(int kit, int i) { return (kit == KIT_707 ? (int)LK_KIT707 : kit == KIT_606 ? (int)LK_KIT606 : kit == KIT_808 ? (int)LK_KIT808 : 0) | i; }
static int lk_model(uint8_t code)
{
    const int id = code & LK_KIT707;
    return id == LK_KIT707 ? KIT_707 : id == LK_KIT606 ? KIT_606 : id == LK_KIT808 ? KIT_808 : KIT_909;
}

/* the default project: a beat in pattern 1, the rest empty; T2 a mono bass an octave down, T3 a pluck, T4 a pad */
static void project_default(void)
{
    uint32_t i;
    int s;
    for (i = 0; i < sizeof proj; i++)
        ((uint8_t *)&proj)[i] = 0;
    proj.magic = PROJ_MAGIC;
    proj.kit = KIT_909;
    proj.pat = 0;
    proj.swing = 50;
    proj.bpm10 = 1200;
    proj.version = PROJ_VERSION;
    for (i = 0; i < NTRK; i++) color_defaults(&proj.color[i]);
    for (i = 0; i < NPAT; i++)
        pattern_clear(&proj.p[i]);
    for (s = 0; s < 16; s += 4)
        bit_set(proj.p[0].d.hit[0], s);   /* BD on the beat */
    bit_set(proj.p[0].d.hit[1], 4);       /* SD on 2 and 4 */
    bit_set(proj.p[0].d.hit[1], 12);
    for (s = 2; s < 16; s += 4)
        bit_set(proj.p[0].d.hit[7], s);   /* CH off the beat */
    bit_set(proj.p[0].d.acc, 0);
    bit_set(proj.p[0].d.acc, 8);
    for (i = 0; i < NINST; i++) {
        static const uint8_t SOUND[NINST] = {0, 9, 7};   /* SQR BASS, PLUCK, SOFT PAD */
        track_t *ts = &proj.tr[i];
        ts->level = 100;
        ts->preset = SOUND[i];
        ts->type = syn_preset(ts->preset, ts->snd) ? TY_MONO : TY_POLY;
        ts->s_end = 1000;
        ts->s_root = 60;
        ts->arp_rate = 3;                 /* 1/16 */
        ts->arp_oct = 1;
        ts->arp_gate = 50;
    }
    proj.tr[0].oct = -1;
    {   /* the mixer: the drums as the kit is (their pads' own sends decide), the bass dry, the pluck and the pad wet */
        static const uint8_t PAN[NTRK] = {64, 64, 52, 76}, REV[NTRK] = {127, 0, 45, 60}, DLY[NTRK] = {127, 0, 35, 10};
        for (i = 0; i < NTRK; i++) {
            proj.mix[i].level = 100;
            proj.mix[i].pan = PAN[i];
            proj.mix[i].rev = REV[i];
            proj.mix[i].dly = DLY[i];
        }
        for (i = 0; i < FXP_NUM; i++)
            proj.fx[i] = (uint8_t)fx_get((int)i);   /* (fx_init's: X0X's defaults, the delay centred) */
    }
    for (i = 0; i < NTRK; i++) {
        lfo_t *l = &proj.lfo[i];
        l->rate = 64;                     /* about 1.2 Hz */
        l->div = 5;                       /* 1/4 */
        l->dest = i == TR_DRUM ? LFO_OFF : 0;   /* the drums: no knob yet; the others: CUTOFF (at depth 0) */
    }
}

/* the engines' knobs <-> the project (main loop) */
static int param_is_send(const x0x_param_t *p)
{
    return p && ((p->name[0] == 'R' && p->name[1] == 'e' && p->name[2] == 'v') ||
                 (p->name[0] == 'D' && p->name[1] == 'l' && p->name[2] == 'y'));
}
static int param_is_switch_sound(const x0x_param_t *p)
{
    return p && p->name[0] == 'S' && p->name[1] == 'o' && p->name[2] == 'u';
}

static void pots_from_engines(void)
{
    int k, pad, i;
    for (k = 0; k < KIT_NUM; k++)
        for (pad = 0; pad <= NPADS; pad++)
            for (i = 0; i < NPOTS; i++)
                kit_pots(k)[pad][i] = (uint8_t)(i < drums_nparams(k, pad) ? drums_get(k, pad, i) : 0);
}

static void pots_apply(void)              /* (the lock held, or the interrupt) */
{
    int k, pad, i, pass;
    for (pass = 0; pass < 2; pass++)      /* a switch (808 tom / conga...) first: the knobs after it edit its sound */
        for (k = 0; k < KIT_NUM; k++)
            for (pad = 0; pad <= NPADS; pad++)
                for (i = 0; i < drums_nparams(k, pad) && i < NPOTS; i++)
                    if (param_is_switch_sound(drums_param(k, pad, i)) == (pass == 0))
                        drums_set(k, pad, i, kit_pots(k)[pad][i]);
}

static void pots_to_engines(void)
{
    uint32_t f = plat_lock();
    pots_apply();
    plat_unlock(f);
}

static void tracks_to_voices(void)
{
    int t;
    uint32_t f = plat_lock();
    for (t = 1; t < NTRK; t++) {
        const track_t *ts = tset(t);
        syn_sound(t, ts->snd);
        syn_level(t, (float)ts->level / 127.0f);
        syn_source(t, ts->type == TY_SAMPLER);
        syn_sample_shape(t, ts->s_start, ts->s_end, ts->s_loop, ts->s_lstart, ts->s_root, ts->s_tune);
        syn_mixer(t, proj.mix[t].pan, (float)proj.mix[t].rev / 127.0f, (float)proj.mix[t].dly / 127.0f);
    }
    for (t = 0; t < FXP_NUM; t++)
        fx_set(t, proj.fx[t]);
    plat_unlock(f);
}

static void track_shape(int t)           /* a SAMPLER track's start, end, loop and pitch -> the synth */
{
    const track_t *ts = tset(t);
    uint32_t f = plat_lock();
    syn_sample_shape(t, ts->s_start, ts->s_end, ts->s_loop, ts->s_lstart, ts->s_root, ts->s_tune);
    plat_unlock(f);
}

static uint32_t crc32_more(uint32_t c, const uint8_t *d, uint32_t n)
{
    uint32_t k;
    while (n--) {
        c ^= *d++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
static uint32_t crc32_of(const uint8_t *d, uint32_t n) { return ~crc32_more(0xFFFFFFFFu, d, n); }

static void insert_process(int t, float *l, float *r, unsigned n)
{
    track_insert_process(&insert_rt[t-1], &proj.insert[t-1], l, r, n);
    color_run(&color_rt[t], &proj.color[t], l, r, n);
}

static void insert_route(int t)
{
    const track_insert_param_t *p = &proj.insert[t-1];
    syn_set_insert(t, (((p->mode != TI_OFF && p->v[3]) || color_active(&proj.color[t])) && !(proj.tmute >> t & 1u)) ? insert_process : 0);
}

static void project_check(void)           /* a loaded project's values kept in range */
{
    int i, t, s, n;
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < COL_NUM; i++)
            proj.color[t].v[i] = (uint8_t)clamp(proj.color[t].v[i], 0, i == COL_ON ? 1 : 127);

    for (t = 0; t < NINST; t++) {
        track_insert_param_t *p = &proj.insert[t];
        if (p->mode >= TI_MODES)
            track_insert_default(p, TI_OFF);
        for (i = 0; i < 4; i++)
            p->v[i] = (uint8_t)clamp(p->v[i], 0, 127);
    }

    for (i = 0; i < NPAT; i++) {
        dpart_t *d = &proj.p[i].d;
        if (d->len < 1 || d->len > NSTEPS)
            d->len = 16;
        if (d->rate >= NRATES)
            d->rate = RATE_16;
        if (d->nlk > NLOCKS)
            d->nlk = 0;
        for (t = 0; t < NINST; t++) {
            ipart_t *p = &proj.p[i].t[t];
            if (p->len < 1 || p->len > NSTEPS)
                p->len = 16;
            if (p->rate >= NRATES)
                p->rate = RATE_16;
            for (s = 0; s < NSTEPS; s++) {
                nstep_t *st = &p->st[s];
                for (n = 0; n < NCHORD; n++)
                    st->note[n] &= 0x7Fu;
                if (!st->note[0]) {
                    nstep_clear(st);
                    continue;
                }
                st->vel = (uint8_t)((st->vel & NS_ACC) | (uint32_t)clamp(ns_vel(st), 1, 127));
                st->gate = (uint16_t)clamp(st->gate, 1, GATE_MAX);
                st->prob = (uint8_t)((st->prob & NS_SLIDE) | (uint32_t)clamp(ns_prob(st), 5, 100));
            }
        }
    }
    for (t = 0; t < NINST; t++) {
        if (proj.tr[t].type >= NTYPES)
            proj.tr[t].type = TY_POLY;
        if (proj.tr[t].level > 127)
            proj.tr[t].level = 100;
        proj.tr[t].oct = (int8_t)clamp(proj.tr[t].oct, -3, 3);
        if (proj.tr[t].preset >= syn_presets() + NUSER)   /* a factory sound or a user slot */
            proj.tr[t].preset = 0;
        for (n = 0; n < SP_NUM; n++)
            proj.tr[t].snd[n] = (int8_t)clamp(proj.tr[t].snd[n], syn_param(n)->min, syn_param(n)->max);
        proj.tr[t].s_start = (uint16_t)clamp(proj.tr[t].s_start, 0, 1000);
        proj.tr[t].s_end = (uint16_t)clamp(proj.tr[t].s_end, 0, 1000);
        proj.tr[t].s_lstart = (uint16_t)clamp(proj.tr[t].s_lstart, 0, 1000);
        proj.tr[t].s_loop &= 1u;
        proj.tr[t].s_root = (uint8_t)clamp(proj.tr[t].s_root, 0, 127);
        proj.tr[t].s_tune = (int8_t)clamp(proj.tr[t].s_tune, -50, 50);
        proj.tr[t].s_q &= 1u;
        proj.tr[t].arp_mode = (uint8_t)(proj.tr[t].arp_mode < 6u ? proj.tr[t].arp_mode : 0u);
        proj.tr[t].arp_rate = (uint8_t)(proj.tr[t].arp_rate < 6u ? proj.tr[t].arp_rate : 3u);
        proj.tr[t].arp_oct = (uint8_t)clamp(proj.tr[t].arp_oct, 1, 4);
        proj.tr[t].arp_gate = (uint8_t)clamp(proj.tr[t].arp_gate, 5, 100);
        proj.tr[t].arp_latch &= 1u;
        proj.tr[t].chord = (uint8_t)(proj.tr[t].chord < NCHORDS ? proj.tr[t].chord : 0u);
        proj.tr[t].chord_inv &= 3u;
    }
    for (t = 0; t < NTRK; t++) {
        lfo_t *l = &proj.lfo[t];
        l->shape = (uint8_t)(l->shape < 7u ? l->shape : 0u);
        l->rate &= 127u;
        l->div = (uint8_t)(l->div < 13u ? l->div : 5u);
        l->sync &= 1u;
        l->retrig = (uint8_t)(l->retrig < 3u ? l->retrig : 0u);
        l->fade &= 127u;
        l->phase &= 127u;
        l->depth = (int8_t)clamp(l->depth, -64, 63);
        if (t == TR_DRUM) {
            if (l->rsv[0] > KIT_NUM)
                l->rsv[0] = 0;
            if (l->pad >= NPADS || (l->dest != LFO_OFF && l->dest >= NPOTS))
                l->dest = LFO_OFF, l->pad = 0;
        } else if (l->dest >= SM_NUM) {
            l->dest = 0;
        }
    }
    for (t = 0; t < NTRK; t++) {
        proj.mix[t].level &= 127u;
        proj.mix[t].pan &= 127u;
        proj.mix[t].rev &= 127u;
        proj.mix[t].dly &= 127u;
    }
    for (t = 0; t < FXP_NUM; t++)
        proj.fx[t] = (uint8_t)clamp(proj.fx[t], 0, fx_param(t)->max);
    for (i = 0; i < NPAT; i++)
        for (t = 0; t < NINST; t++) {
            ipart_t *p = &proj.p[i].t[t];
            if (p->nlk > NILOCKS)
                p->nlk = 0;
            for (s = 0; s < p->nlk; s++)
                if (p->lk[s].param >= SP_NUM || p->lk[s].step >= NSTEPS)
                    p->lk[s].param = 0, p->lk[s].step = 0, p->lk[s].val = (int8_t)syn_param(0)->def;
        }
    for (i = 0; i < NPAT; i++)            /* the generators: one never used (seed 0) all zero */
        for (t = 0; t < NINST; t++) {
            gen_t *g = &proj.gen[i][t];
            if (!g->seed || g->seed > 999) {
                uint32_t k;
                for (k = 0; k < sizeof *g; k++)
                    ((uint8_t *)g)[k] = 0;
                continue;
            }
            g->root = (uint8_t)clamp(g->root, 24, 71);
            g->scale = (uint8_t)(g->scale < NGSCALES ? g->scale : 0u);
            g->notes = (uint8_t)clamp(g->notes, 1, 7);
            g->dens = (uint8_t)clamp(g->dens, 0, 100);
            g->slide = (uint8_t)clamp(g->slide, 0, 100);
            g->accent = (uint8_t)clamp(g->accent, 0, 100);
            g->oct = (uint8_t)clamp(g->oct, 0, 100);
            g->whole = (uint8_t)clamp(g->whole, 0, 100);
        }
    if (proj.swing < 50 || proj.swing > 75)
        proj.swing = 50;
    proj.tmute &= 0x0Fu;
    if ((proj.click & 3u) > 2u)
        proj.click &= (uint8_t)~3u;
    if ((proj.click >> 2 & 3u) > 2u)
        proj.click &= (uint8_t)~12u;
    if ((proj.recq & 3u) > 2u)
        proj.recq &= (uint8_t)~3u;
    if ((proj.recq >> 4 & 3u) > 2u)
        proj.recq &= (uint8_t)~0x30u;
    if ((proj.recq >> 2 & 3u) > 2u)
        proj.recq &= (uint8_t)~0x0Cu;
    proj.recq &= 0x7Fu;
    for (i = 0; i < NPAT; i++) {          /* the patterns' own sounds, kits and follow actions */
        for (t = 0; t < NINST; t++) {
            psound_t *q = &proj.psnd[i][t];
            int k;
            q->used &= 1u;
            if (q->type >= NTYPES)
                q->type = TY_MONO;
            q->level &= 127u;
            q->edited &= 1u;
            if (q->preset >= syn_presets() + NUSER)
                q->preset = 0;
            for (k = 0; k < SP_NUM; k++)
                q->snd[k] = (int8_t)clamp(q->snd[k], syn_param(k)->min, syn_param(k)->max);
        }
        if (proj.pkit[i] > DPRESET_COUNT)
            proj.pkit[i] = 0;
        for (s = 0; s < NPADS; s++)
            if (!drums_has_pad((int)proj.pattern_model[i][s] - 1, s))
                proj.pattern_model[i][s] = 0;
        if ((proj.follow[i] & 7u) > 4u)
            proj.follow[i] = 0;
        proj.follow[i] &= 0x7Fu;
    }
    for (s = 0; s < NPADS; s++)
        if (!drums_has_pad((int)proj.model[s] - 1, s) || proj.model[s] == proj.kit + 1)
            proj.model[s] = 0;
    proj.version = PROJ_VERSION;
    if (proj.dpreset >= DPRESET_COUNT)
        proj.dpreset = (uint8_t)kit_factory(proj.kit);
    if (proj.song_n > NSONG)
        proj.song_n = 0;
    proj.song_on &= 1u;
    for (i = 0; i < NSONG; i++) {
        proj.song[i].pat &= 15u;
        proj.song[i].bars = (uint8_t)clamp(proj.song[i].bars, 1, 16);
    }
}

static void psnd_fetch_all(void);
static void psnd_store_all(void);

/* Identify the persisted layout without treating a legacy project as a current RAM struct. */
static int proj_file_format(void)
{
    int sz = plat_file_size(&PROJ_FILE);
    uint8_t ver = 0;
    if (sz < (int)offsetof(project_t, tr) || plat_file_read(&PROJ_FILE, offsetof(project_t, version), &ver, 1) != 0)
        return 0;
    if (sz >= (int)PROJ_META && sz <= (int)PROJ_CAPACITY && ver == PROJ_VERSION)
        return PROJ_VERSION;
    if (sz >= (int)(PROJ_PREFIX + PROJ_V5_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 5)
        return 5;
    if (sz >= (int)(PROJ_PREFIX + PROJ_V4_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 4)
        return 4;
    if (sz == (int)PROJ_V3_SIZE && ver == 3)
        return 3;
    if (sz == (int)PROJ_V2_SIZE && ver == 2)
        return 2;
    if (ver < 2 && (sz == (int)sizeof(project_v1_t) || sz == (int)V1_OFF(psnd)))
        return 1;
    return 0;
}

static uint8_t preset_from_v1(uint8_t p)  /* format 1's USER n came right after its 15 factory sounds */
{
    return (uint8_t)(p >= PRESETS_V1 ? p - PRESETS_V1 + PRESET_USER_FIRST : p);
}

static int track_from_v1(int i, track_t *d)   /* format 1's track i (0..2) from the file: 0 read */
{
    static track_v1_t v;
    int k;
    if (plat_file_read(&PROJ_FILE, V1_OFF(tr) + (uint32_t)i * sizeof v, &v, sizeof v) != 0)
        return -1;
    d->type = v.type;
    d->level = v.level;
    d->oct = v.oct;
    d->preset = preset_from_v1(v.preset);
    d->edited = v.edited;
    for (k = 0; k < 3; k++)
        d->rsv[k] = 0;
    for (k = 0; k < SP_NUM_V1; k++)
        d->snd[k] = v.snd[k];
    syn_defaults(d->snd, SP_NUM_V1);      /* the update's parameters: as a new sound has them */
    d->s_start = v.s_start;
    d->s_end = v.s_end;
    d->s_lstart = v.s_lstart;
    d->s_loop = v.s_loop;
    d->s_root = v.s_root;
    d->s_tune = v.s_tune;
    d->s_q = v.s_q;
    d->arp_mode = v.arp_mode;
    d->arp_rate = v.arp_rate;
    d->arp_oct = v.arp_oct;
    d->arp_gate = v.arp_gate;
    d->arp_latch = v.arp_latch;
    d->chord = v.chord;
    d->chord_inv = v.chord_inv;
    d->rsv2 = 0;
    return 0;
}

static int psound_from_v1(int p, int t, psound_t *d)   /* format 1's pattern p, track t (0..2) sound: 0 read */
{
    static psound_v1_t v;
    int k;
    if (plat_file_read(&PROJ_FILE, V1_OFF(psnd) + ((uint32_t)p * NINST + (uint32_t)t) * sizeof v, &v, sizeof v) != 0)
        return -1;
    for (k = 0; k < SP_NUM_V1; k++)
        d->snd[k] = v.snd[k];
    syn_defaults(d->snd, SP_NUM_V1);
    d->type = v.type;
    d->level = v.level;
    d->preset = preset_from_v1(v.preset);
    d->edited = v.edited;
    d->used = v.used;
    return 0;
}

static int read_old_drums(uint32_t at, dpart_t *d)
{
    dpart_old_t old;
    int pad, w, i;
    if (plat_file_read(&PROJ_FILE, at, &old, sizeof old) != 0)
        return -1;
    for (w = 0; w < 2; w++) {
        d->acc[w] = old.acc[w];
        for (pad = 0; pad < NPADS; pad++)
            d->hit[pad][w] = old.hit[pad][w];
    }
    d->mute = old.mute; d->len = old.len; d->rate = old.rate; d->nlk = old.nlk;
    for (i = 0; i < NLOCKS; i++)
        d->lk[i] = old.lk[i];
    return 0;
}

static int project_load_v23(int fmt)
{
    static ipart_old_t old;
    int i, t, k;
    if (plat_file_read(&PROJ_FILE, 0, &proj, PROJ_PREFIX) ||
        plat_file_read(&PROJ_FILE, OLD_TAIL, proj.pots, fmt == 2 ? PROJ_V2_SIZE - OLD_TAIL : PROJ_LEGACY_TAIL_BYTES))
        return -1;
    for (i = 0; i < NPAT; i++) {
        uint32_t at = PROJ_PREFIX + (uint32_t)i * sizeof(pattern_old_t);
        if (read_old_drums(at, &proj.p[i].d))
            return -1;
        for (t = 0; t < NINST; t++) {
            ipart_t *d = &proj.p[i].t[t];
            if (plat_file_read(&PROJ_FILE, at + offsetof(pattern_old_t, t) + (uint32_t)t * sizeof old,
                               &old, sizeof old))
                return -1;
            d->len = old.len; d->rate = old.rate; d->nlk = old.nlk;
            for (k = 0; k < NILOCKS; k++)
                d->lk[k] = old.lk[k];
            for (k = 0; k < OLD_STEPS; k++) {
                int n;
                for (n = 0; n < NCHORD; n++) d->st[k].note[n] = old.st[k].note[n];
                d->st[k].vel = old.st[k].vel;
                d->st[k].prob = old.st[k].prob;
                d->st[k].gate = old.st[k].gate;
            }
        }
    }
    return 0;
}

#include "project_codec.h"

/* a format-1 file into proj (cleared first), a part at a time from the flash: 0 read */
static int project_load_v1(void)
{
    static ipart_v1_t ip;
    int sz = plat_file_size(&PROJ_FILE), i, t, k;
    uint8_t *b = (uint8_t *)&proj;
    if (plat_file_read(&PROJ_FILE, 0, b, V1_OFF(tr)) != 0)
        return -1;
    for (i = 0; i < NINST; i++)
        if (track_from_v1(i, &proj.tr[i]) != 0)
            return -1;
    if (plat_file_read(&PROJ_FILE, V1_OFF(lfo), proj.lfo, V1_OFF(song) + (uint32_t)sizeof proj.song - V1_OFF(lfo)) != 0)
        return -1;                        /* lfo .. song: bytes, laid out alike (the padding after them differs) */
    for (i = 0; i < NPAT; i++) {
        uint32_t at = V1_OFF(p) + (uint32_t)i * sizeof(pattern_v1_t);
        if (read_old_drums(at, &proj.p[i].d) != 0)
            return -1;
        for (t = 0; t < NINST; t++) {
            ipart_t *d = &proj.p[i].t[t];
            if (plat_file_read(&PROJ_FILE, at + (uint32_t)offsetof(pattern_v1_t, t) + (uint32_t)t * sizeof ip, &ip,
                               sizeof ip) != 0)
                return -1;
            for (k = 0; k < OLD_STEPS; k++) {
                int n;
                for (n = 0; n < NCHORD; n++)
                    d->st[k].note[n] = ip.st[k].note[n];
                d->st[k].vel = ip.st[k].vel;
                d->st[k].gate = ip.st[k].gate;
                d->st[k].prob = ip.st[k].prob;
            }
            d->len = ip.len;
            d->rate = ip.rate;
            d->nlk = ip.nlk;
            d->rsv = 0;
            for (k = 0; k < NILOCKS; k++)
                d->lk[k] = ip.lk[k];
        }
    }
    if (plat_file_read(&PROJ_FILE, V1_OFF(pots), proj.pots, sizeof proj.pots) != 0)
        return -1;
    if (sz == (int)sizeof(project_v1_t)) { /* the performance update's part */
        for (i = 0; i < NPAT; i++)
            for (t = 0; t < NINST; t++)
                if (psound_from_v1(i, t, &proj.psnd[i][t]) != 0)
                    return -1;
        if (plat_file_read(&PROJ_FILE, V1_OFF(pkit), proj.pkit, V1_OFF(follow) + NPAT - V1_OFF(pkit)) != 0)
            return -1;
    }
    return 0;
}

static int project_load(void)
{
    int fmt = proj_file_format(), r = -1;
    uint32_t i;
    dirty = 0;
    for (i = 0; i < sizeof proj; i++)
        ((uint8_t *)&proj)[i] = 0;
    model_defaults(KIT_606);
    model_defaults(KIT_707);
    if (fmt >= 4) {
        r = project_decode(plat_file_data(&PROJ_FILE), (uint32_t)plat_file_size(&PROJ_FILE), fmt);
    } else if (fmt >= 2) {
        r = project_load_v23(fmt);
    } else if (fmt == 1) {
        r = project_load_v1();
    }
    if (r != 0 || proj.magic != PROJ_MAGIC || proj.kit >= KIT_NUM || proj.pat >= NPAT || proj.bpm10 < 600 ||
        proj.bpm10 > 2400) {
        project_default();
        pots_from_engines();
        tracks_to_voices();
        clips_raw(proj.pat);
        return 0;
    }
    if (fmt < PROJ_VERSION) {
        proj.lfo[TR_DRUM].rsv[0] = 0;
        for (i = 0; i < NTRK; i++) color_defaults(&proj.color[i]);
    }
    project_check();
    clips_raw(proj.pat);
    psnd_fetch_all();                     /* SOUNDS PER PATTERN: the focused pattern's, before they reach the engines */
    pots_to_engines();
    tracks_to_voices();
    return 1;
}

static void locks_release(void);

static int project_save(void)             /* main loop, stopped (each erase stops the sound) */
{
    uint32_t f = plat_lock();
    int result;
    locks_release();                      /* the knobs' own values, not a lock's or the LFO's */
    plat_unlock(f);
    pots_from_engines();
    psnd_store_all();                     /* SOUNDS PER PATTERN: each track's sound with the pattern it's on */
    result = project_encode();
    if (result != 0)
        return result;
    dirty = 0;
    return 0;
}

/* ------------------------------------------------------------ the sample bank --- */
/* The instrument tracks' samples, in place in the data area after the project's file: 0xA7000..0xDBFFF, 53 sectors of
 * 4 KiB (the panel's store objects sit above). A sample takes whole sectors from its record's head (bank_rec_t, 32
 * bytes, written last: a power cut leaves no half sample), then the sample's file (smp_head_t and the 12-bit frames),
 * which the computer reads and replaces (the companion's files: tools/fm1_sample.py). A track's newest whole record is
 * its sample; a new one is written to free sectors before the old one's head is erased, when there is room for both.
 * Main loop, stopped (an erase stops the sound) */
#define BANK_LO 0xA7000u
#define BANK_HI 0xDC000u
#define BANK_REC 32u
#define REC_MAGIC 0x314B4253u             /* "SBK1" */
#define SMP_MAGIC 0x31323153u             /* "S121": a sample file's head */
typedef struct {
    uint32_t magic, seq;
    uint8_t trk, rsv[3];
    uint32_t size, crc;                   /* the file after the record: its bytes and CRC-32 */
    uint32_t rsv2[2];
    uint32_t hcrc;                        /* CRC-32 of the 28 bytes before */
} bank_rec_t;
_Static_assert(sizeof(bank_rec_t) == BANK_REC, "a bank record is 32 bytes");
typedef struct {
    uint32_t magic, frames, rate;         /* rate 44100 or 22050 */
    uint8_t root, rsv[3];
} smp_head_t;
#define SMP_HEAD ((uint32_t)sizeof(smp_head_t))
static inline uint32_t s12_bytes(uint32_t frames) { return (frames * 3u + 1u) / 2u; }

static struct {
    uint32_t at[NTRK], size[NTRK];        /* a track's record (0: none) and its file's bytes */
    uint32_t seq;                         /* the newest record's */
    int8_t wtrk;                          /* a write under way: its track, -1 none */
    uint32_t wat, wsize, wend;            /* its record, its file's bytes, erased up to */
} bank;
static const uint8_t *smp_data[NTRK];     /* what each track plays (the flash's, or a recording in RAM) */
static uint32_t smp_frames[NTRK], smp_rate[NTRK];

static uint32_t bank_sectors(uint32_t size) { return (BANK_REC + size + 4095u) / 4096u; }

static const bank_rec_t *bank_rec(uint32_t at)   /* a whole record's head at at, or 0 */
{
    const bank_rec_t *r = (const bank_rec_t *)plat_data(at);
    if (!r || r->magic != REC_MAGIC || r->trk < 1 || r->trk >= NTRK || r->size < SMP_HEAD ||
        r->size > BANK_HI - at - BANK_REC || crc32_of((const uint8_t *)r, 28u) != r->hcrc)
        return 0;
    return r;
}

static int bank_used(uint32_t at, int except)   /* a track's sample (not track except's) is in the sector at at */
{
    int t;
    for (t = 1; t < NTRK; t++)
        if (t != except && bank.at[t] && at >= bank.at[t] && at < bank.at[t] + bank_sectors(bank.size[t]) * 4096u)
            return 1;
    return 0;
}

/* the first n free sectors in a row (0: none); longest: the longest run there is */
static uint32_t bank_run(uint32_t n, int except, uint32_t *longest)
{
    uint32_t at, run = 0, start = 0, best = 0;
    for (at = BANK_LO; at < BANK_HI; at += 4096u) {
        if (bank_used(at, except)) {
            run = 0;
            continue;
        }
        if (!run)
            start = at;
        if (++run > best)
            best = run;
        if (n && run >= n)
            break;
    }
    if (longest)
        *longest = best;
    return n && run >= n ? start : 0;
}

static int smp_ok(const smp_head_t *h, uint32_t size)
{
    return h && h->magic == SMP_MAGIC && h->frames >= 2u && (h->rate == 44100u || h->rate == 22050u) &&
           size == SMP_HEAD + s12_bytes(h->frames);
}

static void use_sample(int t, const uint8_t *d, uint32_t frames, uint32_t rate)   /* what the track plays */
{
    uint32_t f = plat_lock();
    syn_sample(t, d, frames, rate);
    plat_unlock(f);
    smp_data[t] = d && frames >= 2u ? d : 0;
    smp_frames[t] = smp_data[t] ? frames : 0;
    smp_rate[t] = smp_data[t] ? rate : 0;
}

static void bank_use(int t)               /* the track plays its sample in the flash (or nothing) */
{
    const smp_head_t *h = bank.at[t] ? (const smp_head_t *)plat_data(bank.at[t] + BANK_REC) : 0;
    if (h && smp_ok(h, bank.size[t]))
        use_sample(t, (const uint8_t *)h + SMP_HEAD, h->frames, h->rate);
    else
        use_sample(t, 0, 0, 0);
}

static void bank_scan(void)               /* at the start: each track's newest whole sample */
{
    uint32_t at, seq[NTRK] = {0};
    int t;
    for (t = 0; t < NTRK; t++)
        bank.at[t] = bank.size[t] = 0;
    bank.seq = 0;
    bank.wtrk = -1;
    for (at = BANK_LO; at < BANK_HI; at += 4096u) {
        const bank_rec_t *r = bank_rec(at);
        if (!r)
            continue;
        if ((int32_t)(r->seq - bank.seq) > 0)
            bank.seq = r->seq;
        if (bank.at[r->trk] && (int32_t)(r->seq - seq[r->trk]) <= 0)
            continue;
        if (crc32_of((const uint8_t *)r + BANK_REC, r->size) != r->crc)
            continue;
        bank.at[r->trk] = at;
        bank.size[r->trk] = r->size;
        seq[r->trk] = r->seq;
    }
    for (t = 1; t < NTRK; t++)
        bank_use(t);
}

static int bank_erase_heads(int t, uint32_t keep)   /* every record of the track's but keep: its head erased */
{
    uint32_t at;
    int r = 0;
    for (at = BANK_LO; at < BANK_HI; at += 4096u) {
        const bank_rec_t *h = at == keep ? 0 : bank_rec(at);
        if (h && h->trk == t && plat_data_erase(at))
            r = -1;
    }
    return r;
}

static int bank_kill(int t)               /* the track's sample gone */
{
    if (smp_data[t] && bank.at[t] && smp_data[t] == plat_data(bank.at[t] + BANK_REC + SMP_HEAD))
        use_sample(t, 0, 0, 0);           /* (nothing reads what is about to be erased) */
    bank.at[t] = bank.size[t] = 0;
    return bank_erase_heads(t, 0);
}

static uint32_t bank_room(int t)          /* the biggest file the track can take now (its own sample's room too) */
{
    uint32_t longest;
    bank_run(0, t, &longest);
    return longest ? longest * 4096u - BANK_REC : 0;
}

/* where n sectors go: the smallest free run that takes them (except: a track whose sectors count as free), at the end of
 * it away from the track's old sample, so that the old one's sectors, freed after, join the space left; 0 none */
static uint32_t bank_place(uint32_t n, int except, int t)
{
    uint32_t at, run = 0, start = 0, best = 0, best_len = 0;
    for (at = BANK_LO; at <= BANK_HI; at += 4096u) {
        if (at < BANK_HI && !bank_used(at, except)) {
            if (!run)
                start = at;
            run++;
            continue;
        }
        if (run >= n && (!best_len || run < best_len)) {
            best = start;
            best_len = run;
        }
        run = 0;
    }
    if (!best_len)
        return 0;
    if (bank.at[t] && bank.at[t] < best)  /* the old one below: the new one at the top of the run */
        best += (best_len - n) * 4096u;
    return best;
}

static int bank_begin(int t, uint32_t size)
{
    uint32_t n = bank_sectors(size), at;
    bank.wtrk = -1;
    if (size < SMP_HEAD || n > (BANK_HI - BANK_LO) / 4096u)
        return -1;
    at = bank_place(n, -1, t);            /* beside the old one, kept until the new one is whole */
    if (!at && bank.at[t] && (at = bank_place(n, t, t)) != 0)
        bank_kill(t);                     /* no room for both: the old one goes first */
    if (!at || plat_data_erase(at))
        return -1;
    bank.wtrk = (int8_t)t;
    bank.wat = at;
    bank.wsize = size;
    bank.wend = at + 4096u;
    return 0;
}

static int bank_write(int t, uint32_t off, const void *src, uint32_t n)
{
    uint32_t at = bank.wat + BANK_REC + off;
    if (bank.wtrk != t || off > bank.wsize || n > bank.wsize - off)
        return -1;
    while (bank.wend < at + n) {          /* each sector erased as the data reaches it */
        if (plat_data_erase(bank.wend))
            return -1;
        bank.wend += 4096u;
    }
    return n ? plat_data_write(at, src, n) : 0;
}

static int bank_commit(int t, uint32_t size, uint32_t crc)   /* checked in the flash, then its head: current */
{
    bank_rec_t r;
    const uint8_t *d = plat_data(bank.wat + BANK_REC);
    uint32_t k;
    if (bank.wtrk != t || size != bank.wsize || !d || crc32_of(d, size) != crc)
        return -1;
    for (k = 0; k < sizeof r; k++)
        ((uint8_t *)&r)[k] = 0;
    r.magic = REC_MAGIC;
    r.seq = ++bank.seq;
    r.trk = (uint8_t)t;
    r.size = size;
    r.crc = crc;
    r.hcrc = crc32_of((const uint8_t *)&r, 28u);
    bank.wtrk = -1;
    if (plat_data_write(bank.wat, &r, sizeof r) || !bank_rec(bank.wat))
        return -1;
    if (smp_data[t] && bank.at[t] && smp_data[t] == plat_data(bank.at[t] + BANK_REC + SMP_HEAD))
        use_sample(t, 0, 0, 0);           /* the old one is about to go */
    bank_erase_heads(t, bank.wat);
    bank.at[t] = bank.wat;
    bank.size[t] = size;
    bank_use(t);
    return 0;
}

/* ------------------------------------------------------------- user presets --- */
/* sounds for T2..T4 and kits for the drums, saved by hand (SAVE held + PRESETS, let go), kept in a store object of their
 * own (OBJ_USER, A / B: a power cut keeps the old one) apart from any project */
#define USER_MAGIC 0x32525355u            /* "USR2": the synth update's (64 parameters a sound) */
#define USER_MAGIC_V1 0x31525355u         /* "USR1": 0.2's (26), converted as they load */
typedef struct {
    uint8_t used, mono, rsv[2];
    int8_t v[SP_NUM];
    uint8_t rsv2[2];
} usound_t;
typedef struct {
    uint8_t used, mono, rsv[2];
    int8_t v[SP_NUM_V1];
    uint8_t rsv2[2];
} usound_v1_t;
typedef struct {
    uint8_t used, kit, rsv[2];
    uint8_t pots[NPADS + 1][NPOTS];
} ukit_t;
typedef struct {
    uint32_t magic;
    usound_t snd[NUSER];
    ukit_t kit[NUKIT];
    uint8_t model[NUKIT][NPADS];  /* suffix, preserving USR2’s complete legacy prefix */
} user_t;
typedef struct {
    uint32_t magic;
    usound_v1_t snd[NUSER];
    ukit_t kit[NUKIT];
} user_v1_t;
_Static_assert(sizeof(user_t) <= 3840u, "the user presets fit one store object");
_Static_assert(sizeof(user_t) > sizeof(user_v1_t), "0.2's user presets convert in place, from the end");
static user_t user PLAT_POOL;

static void user_load(void)
{
    uint8_t *b = (uint8_t *)&user;
    uint32_t i;
    int n = plat_store_load(OBJ_USER, &user, sizeof user), k;
    if ((n == (int)sizeof user || n == (int)offsetof(user_t, model)) && user.magic == USER_MAGIC) {
        for (k = 0; k < NUKIT; k++)
            for (i = 0; i < NPADS; i++)
                if (n != (int)sizeof user || !drums_has_pad((int)user.model[k][i] - 1, (int)i)
                    || user.model[k][i] == user.kit[k].kit + 1)
                    user.model[k][i] = 0;
        return;
    }
    if (n == (int)sizeof(user_v1_t) && user.magic == USER_MAGIC_V1) {   /* 0.2's: moved into place, last first */
        uint32_t from = (uint32_t)offsetof(user_v1_t, kit), to = (uint32_t)offsetof(user_t, kit);
        for (i = sizeof user.kit; i-- > 0;)
            b[to + i] = b[from + i];
        for (k = NUSER - 1; k >= 0; k--) {
            usound_v1_t o = ((const usound_v1_t *)(b + offsetof(user_v1_t, snd)))[k];
            usound_t *u = &user.snd[k];
            u->used = o.used;
            u->mono = o.mono;
            u->rsv[0] = u->rsv[1] = 0;
            for (i = 0; i < SP_NUM_V1; i++)
                u->v[i] = o.v[i];
            syn_defaults(u->v, SP_NUM_V1);
            u->rsv2[0] = u->rsv2[1] = 0;
        }
        user.magic = USER_MAGIC;
        return;
    }
    for (i = 0; i < sizeof user; i++)
        b[i] = 0;
    user.magic = USER_MAGIC;
}

static int user_save(void) { return plat_store_save(OBJ_USER, &user, sizeof user); }

/* SOUNDS PER PATTERN (SETUP): each pattern keeps T2..T4's sounds and the drums' kit. A track moving to another
 * pattern's part (a scene, a clip, the song) leaves its sound with the pattern it was on and takes the new one's; a
 * pattern with none yet takes the sound playing. Saving keeps each track's sound with the pattern it's on. Off, the
 * tracks keep one sound whatever the pattern */
static int sounds_pp(void) { return (int)(proj.recq >> 6 & 1u); }

/* Build the new selection before publishing it to the audio interrupt.
 * Unmodified factory-to-factory changes retain the legacy tail behavior. */
static void drum_map_apply(int base, const uint8_t *map)
{
    const int mixed = kit_is_custom(proj.model) || kit_is_custom(map);
    for (int r = 0; r < NPADS; r++) {
        const int model = drums_model(base, map, r);
        if (mixed && drum_model(r) != model) {
            drums_cut_pad(r);
            roll_inhibit_pads |= (uint16_t)(1u << r);
            pad_hit[r] = 0.0f;
        }
        proj.model[r] = model == base ? 0 : (uint8_t)(model + 1);
    }
    proj.kit = (uint8_t)base;
}

static void kit_raw(int i, const uint8_t *scene, int use_slot_pots)
{
    int base = dpreset_factory(i), r, c;
    const uint8_t *map = 0;
    if (dpreset_user(i)) {
        const ukit_t *u = &user.kit[i - 2];
        if (!u->used || u->kit >= KIT_NUM)
            return;
        base = u->kit;
        map = user.model[i - 2];
        if (use_slot_pots)
            for (r = 0; r <= NPADS; r++)
                for (c = 0; c < NPOTS; c++)
                    kit_pots(drums_model(base, map, r))[r][c] = u->pots[r][c];
    }
    if (base < 0)
        return;
    drum_map_apply(base, scene ? scene : map);
    proj.dpreset = (uint8_t)i;
    if (use_slot_pots && dpreset_user(i))
        pots_apply();
}

static void psnd_store(int t, int p)      /* track t's sound now, kept with pattern p */
{
    if (p < 0 || p >= NPAT)
        return;
    if (t == TR_DRUM) {
        proj.pkit[p] = (uint8_t)(proj.dpreset + 1);
        for (int pad = 0; pad < NPADS; pad++)
            proj.pattern_model[p][pad] = proj.model[pad];
    } else {
        psound_t *q = &proj.psnd[p][t - 1];
        const track_t *ts = tset(t);
        int k;
        for (k = 0; k < SP_NUM; k++)
            q->snd[k] = ts->snd[k];
        q->type = ts->type;
        q->level = ts->level;
        q->preset = ts->preset;
        q->edited = ts->edited;
        q->used = 1;
    }
}

static int psnd_fetch(int t, int p)       /* pattern p's sound into track t (not the engine yet); 0: it had none */
{
    if (t == TR_DRUM) {
        if (!proj.pkit[p])
            return 0;
        return 1;                         /* (the kit is loaded by psnd_load / psnd_fetch_all) */
    } else {
        const psound_t *q = &proj.psnd[p][t - 1];
        track_t *ts = tset(t);
        int k;
        if (!q->used)
            return 0;
        for (k = 0; k < SP_NUM; k++)
            ts->snd[k] = q->snd[k];
        ts->type = q->type;
        ts->level = q->level;
        ts->preset = q->preset;
        ts->edited = q->edited;
        return 1;
    }
}

static void psnd_load(int t, int p)       /* pattern p's sound into track t and its engine (the lock held / interrupt) */
{
    if (!psnd_fetch(t, p)) {              /* none yet: it takes the sound playing */
        psnd_store(t, p);
        return;
    }
    if (t == TR_DRUM) {
        const int id = proj.pkit[p] - 1;
        const int new_slot = id != proj.dpreset;
        int changed = new_slot;
        for (int pad = 0; !changed && pad < NPADS; pad++)
            changed = proj.pattern_model[p][pad] != proj.model[pad];
        if (changed) {
            locks_release();
            kit_raw(id, proj.pattern_model[p], new_slot);
        }
    } else {
        const track_t *ts = tset(t);
        syn_sound(t, ts->snd);
        syn_level(t, (float)ts->level / 127.0f);
        syn_source(t, ts->type == TY_SAMPLER);
    }
}

static void set_clip(int t, int n)        /* track t onto pattern n's part (-1: stopped); its sound with it */
{
    if (sounds_pp() && n >= 0 && n != clip_of(t)) {
        psnd_store(t, clip_of(t));
        psnd_load(t, n);
    }
    clip[t] = (int8_t)n;
}

static void psnd_store_all(void)          /* saving: each track's sound with the pattern it's on */
{
    int t;
    if (sounds_pp())
        for (t = 0; t < NTRK; t++)
            psnd_store(t, clip_of(t));
}

static void psnd_fetch_all(void)          /* loading: the focused pattern's sounds (before they reach the engines) */
{
    int t;
    if (!sounds_pp())
        return;
    for (t = 1; t < NTRK; t++)
        psnd_fetch(t, proj.pat);
    if (proj.pkit[proj.pat] && proj.pkit[proj.pat] - 1 != proj.dpreset)
        kit_raw(proj.pkit[proj.pat] - 1, proj.pattern_model[proj.pat], 1);
}

/* --------------------------------------------------------------- modulation --- */
/* An LFO on every track: the drums' moves one pad's knob, at that pad's hits; an instrument track's moves one of the
 * synth's destinations (dsp/synth.h SM_*), every block. An arpeggiator on T2..T4 plays the keys held (and a step's notes,
 * for its gate). They run in the audio interrupt (syn_block, every SYN_CTL samples); the main loop changes their
 * settings and the arpeggiator's notes inside plat_lock() */
enum { LS_SINE, LS_TRI, LS_SAWUP, LS_SAWDN, LS_SQUARE, LS_SH, LS_SMOOTH, NLSHAPES };
static const char *const LSHAPE_NAME[NLSHAPES] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "S&H", "SMOOTH"};
enum { LR_FREE, LR_NOTE, LR_BAR, NLRETRIG };
static const char *const LRETRIG_NAME[NLRETRIG] = {"FREE", "NOTE", "BAR"};
#define NDIV 13
static const char *const DIV_NAME[NDIV] = {"4 BARS", "2 BARS", "1 BAR", "1/2", "1/4.", "1/4", "1/8.", "1/4T", "1/8", "1/8T",
                                           "1/16", "1/16T", "1/32"};
static const float DIV_BEATS[NDIV] = {16.0f, 8.0f, 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f,
                                      0.25f, 1.0f / 6.0f, 0.125f};
static const char *const LDEST_NAME[SM_NUM] = {"CUTOFF", "PITCH", "LEVEL", "PWM", "RESO", "DETUNE", "MIX", "DRIVE",
                                               "NOISE", "SUB", "START"};
static float LFO_HZ[128];                 /* the free rates: 0.05 Hz .. 30 Hz */
static struct {
    float ph, out;                        /* where in its cycle; its value now, -1..1 */
    float a, b;                           /* S&H, SMOOTH: the value this cycle, and the next */
    float fade;                           /* 0..1 after a note (FADE) */
} lfo_rt[NTRK];
static uint32_t mod_rng = 0x9E3779B9u;

static float mod_rnd(void)                /* -1..1 */
{
    mod_rng ^= mod_rng << 13;
    mod_rng ^= mod_rng >> 17;
    mod_rng ^= mod_rng << 5;
    return (float)(int32_t)mod_rng * (1.0f / 2147483648.0f);
}

static float exp_pos(float x)             /* e^x for 0 <= x < 8, without libm */
{
    float y = 1.0f + x * (1.0f / 1024.0f);
    int i;
    for (i = 0; i < 10; i++)
        y *= y;
    return y;
}

static float time_ms(int v)               /* 0..127 -> 1 ms .. 5 s, as the synth's envelopes */
{
    float t = (float)v / 127.0f;
    return 1.0f + 5000.0f * t * t * t;
}

static void lfo_init(void)
{
    int i;
    for (i = 0; i < 128; i++)
        LFO_HZ[i] = 0.05f * exp_pos((float)i / 127.0f * 6.39693f);   /* 0.05 * 600^(i / 127) */
    for (i = 0; i < NTRK; i++) {
        lfo_rt[i].b = mod_rnd();
        lfo_rt[i].fade = 1.0f;
    }
}

static float lfo_hz(int t)
{
    const lfo_t *l = &proj.lfo[t];
    return l->sync ? (float)proj.bpm10 / 600.0f / DIV_BEATS[l->div] : LFO_HZ[l->rate & 127u];
}

static float lfo_wave(int shape, float ph, float a, float b)   /* -1..1; SINE and TRI start at 0, rising */
{
    switch (shape) {
    case LS_SINE: {                       /* two parabolas: within 6 % of a sine */
        float u = 2.0f * ph;
        return u < 1.0f ? 4.0f * u * (1.0f - u) : -4.0f * (u - 1.0f) * (2.0f - u);
    }
    case LS_TRI: return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    case LS_SAWUP: return 2.0f * ph - 1.0f;
    case LS_SAWDN: return 1.0f - 2.0f * ph;
    case LS_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
    case LS_SH: return a;
    default: return a + (b - a) * ph;     /* SMOOTH */
    }
}

static void lfo_restart(int t)
{
    lfo_rt[t].ph = (float)proj.lfo[t].phase / 128.0f;
    lfo_rt[t].fade = 0.0f;
    lfo_rt[t].a = lfo_rt[t].b;
    lfo_rt[t].b = mod_rnd();
}

static void lfo_note(int t)               /* a note on the track: NOTE restarts it */
{
    if (proj.lfo[t].retrig == LR_NOTE)
        lfo_restart(t);
}

static void lfo_bar(void)                 /* a bar: BAR restarts */
{
    int t;
    for (t = 0; t < NTRK; t++)
        if (proj.lfo[t].retrig == LR_BAR)
            lfo_restart(t);
}

static void lfo_play(void)                /* PLAY: synced and BAR LFOs start with the bar */
{
    int t;
    for (t = 0; t < NTRK; t++)
        if (proj.lfo[t].sync || proj.lfo[t].retrig == LR_BAR)
            lfo_restart(t);
}

static void lfo_tick(int t, uint32_t n)   /* audio interrupt: n samples on */
{
    const lfo_t *l = &proj.lfo[t];
    lfo_rt[t].ph += lfo_hz(t) * (float)n * (1.0f / 44100.0f);
    while (lfo_rt[t].ph >= 1.0f) {
        lfo_rt[t].ph -= 1.0f;
        lfo_rt[t].a = lfo_rt[t].b;
        lfo_rt[t].b = mod_rnd();
    }
    if (l->fade && l->retrig == LR_NOTE) {
        if (lfo_rt[t].fade < 1.0f) {
            lfo_rt[t].fade += (float)n / (time_ms(l->fade) * 44.1f);
            if (lfo_rt[t].fade > 1.0f)
                lfo_rt[t].fade = 1.0f;
        }
    } else {
        lfo_rt[t].fade = 1.0f;
    }
    lfo_rt[t].out = lfo_wave(l->shape, lfo_rt[t].ph, lfo_rt[t].a, lfo_rt[t].b) * lfo_rt[t].fade;
}

/* the arpeggiator */
enum { AM_OFF, AM_UP, AM_DOWN, AM_UPDOWN, AM_RANDOM, AM_ORDER, NARPMODES };
static const char *const ARP_MODE_NAME[NARPMODES] = {"OFF", "UP", "DOWN", "UP+DOWN", "RANDOM", "AS PLAYED"};
/* chord mode (ARP 2): a key plays a chord built on it, up to NCHORD notes (what a step holds), inverted by moving its
 * lowest notes up an octave; recorded as a chord */
static const char *const CHORD_NAME[NCHORDS] = {"OFF", "MAJ", "MIN", "7", "MAJ7", "MIN7", "SUS2", "SUS4", "DIM", "AUG",
                                                "5", "OCT", "MIN9", "MAJ9", "7#9", "7SUS4"};
static const int8_t CHORD_IV[NCHORDS][NCHORD] = {
    {0, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10}, {0, 4, 7, 11},
    {0, 3, 7, 10}, {0, 2, 7, -1}, {0, 5, 7, -1}, {0, 3, 6, -1}, {0, 4, 8, -1},
    {0, 7, -1}, {0, 12, -1}, {0, 3, 10, 14}, {0, 4, 11, 14}, {0, 4, 10, 15}, {0, 5, 7, 10}
};
static const char *const INV_NAME[4] = {"ROOT", "1ST", "2ND", "3RD"};

static int chord_of(int spec, int root, int *out)   /* spec: chord | inversion << 4; the notes, how many */
{
    int c = spec & 15, inv = spec >> 4 & 3, n = 0, i;
    if (c >= NCHORDS)
        c = 0;
    for (i = 0; i < 4 && CHORD_IV[c][i] >= 0; i++)
        out[n++] = root + CHORD_IV[c][i];
    for (i = 0; i < inv && i < n - 1; i++)
        out[i] += 12;
    for (i = 0; i < n; i++)
        out[i] = clamp(out[i], 1, 127);
    return n;
}
#define NARPRATES 6
static const char *const ARP_RATE_NAME[NARPRATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float ARP_BEATS[NARPRATES] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
#define ARP_MAX 16
static struct {
    uint8_t n;                            /* notes in it, in the order they came */
    uint8_t keys;                         /* keys held down now (LATCH) */
    uint8_t note[ARP_MAX], vel[ARP_MAX];
    int32_t life[ARP_MAX];                /* a step's note: samples left; -1 held by a key */
    uint16_t idx;                         /* the next one in the pattern */
    uint8_t running;
    float to_next;                        /* samples until it plays the next */
} arp[NTRK];

static void arp_drop(int t, int i)
{
    for (; i + 1 < arp[t].n; i++) {
        arp[t].note[i] = arp[t].note[i + 1];
        arp[t].vel[i] = arp[t].vel[i + 1];
        arp[t].life[i] = arp[t].life[i + 1];
    }
    if (arp[t].n)
        arp[t].n--;
}

static void arp_add(int t, int note, int vel, int32_t life)
{
    int i;
    for (i = 0; i < arp[t].n; i++)
        if (arp[t].note[i] == note) {     /* there already: the newer velocity, the longer life */
            arp[t].vel[i] = (uint8_t)vel;
            if (life < 0 || (arp[t].life[i] >= 0 && life > arp[t].life[i]))
                arp[t].life[i] = life;
            return;
        }
    if (arp[t].n == ARP_MAX)              /* full: the oldest goes */
        arp_drop(t, 0);
    i = arp[t].n++;
    arp[t].note[i] = (uint8_t)note;
    arp[t].vel[i] = (uint8_t)vel;
    arp[t].life[i] = life;
}

static void arp_clear(int t)
{
    arp[t].n = arp[t].keys = 0;
    arp[t].running = 0;
}

static void arp_key(int t, int note, int vel, int on)   /* a key (or MIDI) on an arpeggiated track */
{
    int i;
    if (on) {
        if (tset(t)->arp_latch && !arp[t].keys)   /* LATCH: a new chord replaces the last */
            for (i = arp[t].n - 1; i >= 0; i--)
                if (arp[t].life[i] < 0)
                    arp_drop(t, i);
        arp[t].keys++;
        arp_add(t, note, vel, -1);
        return;
    }
    if (arp[t].keys)
        arp[t].keys--;
    if (!tset(t)->arp_latch)
        for (i = 0; i < arp[t].n; i++)
            if (arp[t].note[i] == note && arp[t].life[i] < 0) {
                arp_drop(t, i);
                break;
            }
}

static void arp_fire(int t)               /* the next note of the pattern */
{
    const track_t *ts = tset(t);
    uint8_t sorted[ARP_MAX];
    int n = arp[t].n, N = n * ts->arp_oct, i, j, k, note, vel;
    float step = 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * ARP_BEATS[ts->arp_rate];
    for (i = 0; i < n; i++) {             /* the notes low to high */
        uint8_t v = arp[t].note[i];
        for (j = i; j > 0 && sorted[j - 1] > v; j--)
            sorted[j] = sorted[j - 1];
        sorted[j] = v;
    }
    switch (ts->arp_mode) {
    case AM_DOWN: k = N - 1 - arp[t].idx % N; break;
    case AM_UPDOWN: {
        int per = N > 1 ? 2 * N - 2 : 1, x = arp[t].idx % per;
        k = x < N ? x : 2 * N - 2 - x;
        break;
    }
    case AM_RANDOM: k = (int)((mod_rnd() * 0.5f + 0.5f) * (float)N) % N; break;
    default: k = arp[t].idx % N; break;
    }
    arp[t].idx++;
    if (ts->arp_mode == AM_ORDER) {
        note = arp[t].note[k % n];
        vel = arp[t].vel[k % n];
    } else {
        note = sorted[k % n];
        for (vel = 100, i = 0; i < n; i++)
            if (arp[t].note[i] == note)
                vel = arp[t].vel[i];
    }
    note += 12 * (k / n);
    if (note > 127)
        note -= 12 * ((note - 116) / 12 + 1);
    syn_note_on(t, note, (float)vel / 127.0f, (int32_t)(step * (float)ts->arp_gate * 0.01f), ts->type == TY_MONO);
    lfo_note(t);
}

static void arp_feed(int t, int note, int vel, int32_t life) { arp_add(t, note, vel, life > 0 ? life : 1); }

static void arp_tick(int t, uint32_t n)   /* audio interrupt: n samples on */
{
    int i;
    float step;
    if (track_is_muted(t) || !tset(t)->arp_mode)
        return;
    for (i = arp[t].n - 1; i >= 0; i--)   /* a step's notes end with its gate */
        if (arp[t].life[i] >= 0 && (arp[t].life[i] -= (int32_t)n) <= 0)
            arp_drop(t, i);
    if (!arp[t].n) {
        arp[t].running = 0;
        return;
    }
    if (!arp[t].running) {                /* the first note: at once for a step's, 15 ms after a key (the rest of a chord
                                           * pressed with it comes in first) */
        int keyed = 0;
        for (i = 0; i < arp[t].n; i++)
            keyed |= arp[t].life[i] < 0;
        arp[t].running = 1;
        arp[t].to_next = keyed ? 0.015f * 44100.0f : 0.0f;
        arp[t].idx = 0;
    }
    step = 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * ARP_BEATS[tset(t)->arp_rate];
    arp[t].to_next -= (float)n;
    if (arp[t].to_next <= 0.0f) {
        arp_fire(t);
        arp[t].to_next += step;
        if (arp[t].to_next <= 0.0f)
            arp[t].to_next = step;
    }
}

/* --------------------------------------------------------------- sequencer --- */
/* the audio interrupt runs it; the main loop reads it and changes it inside plat_lock(). Each track walks its own
 * part at its own rate; a pattern change waits for the bar (16 sixteenths), where every track starts again */
static struct {
    volatile uint8_t playing;
    volatile int8_t queued;               /* the pattern to change to at the bar, -1 none */
    volatile uint32_t ticks;              /* steps played since PLAY, any track (the screen's cue) */
    uint8_t fbars, fskip;                 /* a scene's follow action: bars since it started; the start's bar not counted */
    float mclk_to;                        /* MIDI CLOCK sent: samples to the next of 24 a beat */
    uint32_t mclk_t[17];                  /* followed: every 6th clock's time (samples rendered), newest first */
    float mclk_avg, mclk_err;             /* a beat's samples from them (0: not yet); the groove's distance from the
                                           * last 16th's clock, before it was pulled to it */
    uint8_t mclk_n, mclk_nt, mclk_go;     /* clocks since the beat; times kept; a Start waiting for its first clock */
    volatile uint8_t ext_stop;            /* followed: a Stop came (the main loop stops) */
    volatile uint32_t clock;              /* samples since PLAY (live recording measures gates by it) */
    float bar_left;                       /* samples until the next bar */
    struct {
        volatile int32_t step;            /* the step playing now (0..len-1), -1 before the first */
        volatile float frac;              /* how far into the step, 0..1 (recording rounds by it) */
        float to_next;                    /* samples until the next step */
        float dur;                        /* this step's length, samples */
    } t[NTRK];
    int8_t skip_once[NPADS];              /* drums: a step a recorded hit already played: left once */
    int8_t iskip[NTRK];                   /* instrument tracks: the same, for a recorded note */
    uint8_t slid[NTRK];                   /* the step just played slides into this one (NS_SLIDE, and it sounded) */
} sq;
static volatile uint8_t sel_trk;          /* the track picked (MIDI channel 1 plays it) */
static struct {                           /* the song playing: its entry (-1: none), that entry's bars left */
    volatile int8_t pos;
    volatile uint8_t left, started;
} srt = {-1, 0, 0};
static int8_t song_from;                  /* the entry PLAY starts the song at (the SONG page's cursor, else 0) */

/* a locked step: its knobs set just before the hit, the knob's own value put back at the pad's next hit that
 * has no lock on it (the locked hit rings out with its value). Audio interrupt, or the main loop in plat_lock() */
static uint8_t ovr_base[NPADS][NPOTS];    /* the knob's own value while a lock holds it */
static uint16_t ovr_mask[NPADS];          /* the knobs a lock holds now, per pad */
static uint32_t chance_rng = 0x2545F491u;

static void locks_apply(const dpart_t *p, int step, int pad)
{
    uint16_t want = 0, back;
    int i;
    for (i = 0; i < p->nlk; i++) {
        const lock_t *l = &p->lk[i];
        int k = l->param & 0x3F;
        if (l->step != step || l->pad != pad || l->param == LK_PROB || k >= NPOTS ||
            lk_model(l->param) != drum_model(pad))
            continue;
        if (!(ovr_mask[pad] >> k & 1u))
            ovr_base[pad][k] = (uint8_t)drums_get(drum_model(pad), pad, k);
        ovr_mask[pad] |= (uint16_t)(1u << k);
        drums_set(drum_model(pad), pad, k, l->val);
        want |= (uint16_t)(1u << k);
    }
    {   /* the drums' LFO on this pad's knob: what the knob (or this step's lock) says, moved by it */
        const lfo_t *lf = &proj.lfo[TR_DRUM];
        int k = lf->dest;
        const x0x_param_t *d;
        if (lf->dest != LFO_OFF && lf->depth && lf->pad == pad && (!lf->rsv[0] || lf->rsv[0] == drum_model(pad) + 1) && k < NPOTS && (d = drums_param(drum_model(pad), pad, k)) != 0 &&
            !d->names) {
            int base = (want >> k & 1u) ? drums_get(drum_model(pad), pad, k)
                     : (ovr_mask[pad] >> k & 1u) ? ovr_base[pad][k] : drums_get(drum_model(pad), pad, k);
            if (!(ovr_mask[pad] >> k & 1u))
                ovr_base[pad][k] = (uint8_t)base;
            ovr_mask[pad] |= (uint16_t)(1u << k);
            drums_set(drum_model(pad), pad, k, clamp(base + (int32_t)(lfo_rt[TR_DRUM].out * (float)lf->depth), 0, d->max));
            want |= (uint16_t)(1u << k);
        }
    }
    back = ovr_mask[pad] & (uint16_t)~want;
    for (i = 0; back; i++, back >>= 1)
        if (back & 1u)
            drums_set(drum_model(pad), pad, i, ovr_base[pad][i]);
    ovr_mask[pad] = want;
}

/* an instrument step's locks: set before its notes, the knob's own value (the project's) put back at the track's next
 * step with notes and no lock on it. Audio interrupt, or the main loop in plat_lock() */
static uint64_t ilk_mask[NTRK];        /* a bit per sound parameter (SP_NUM: 64) */

static void ilocks_apply(int tr, const ipart_t *p, int step)
{
    uint64_t want = 0, back;
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step && p->lk[i].param < SP_NUM) {
            syn_set(tr, p->lk[i].param, p->lk[i].val);
            want |= (uint64_t)1 << p->lk[i].param;
        }
    back = ilk_mask[tr] & ~want;
    for (i = 0; back; i++, back >>= 1)
        if (back & 1u)
            syn_set(tr, i, tset(tr)->snd[i]);
    ilk_mask[tr] = want;
}

static void ilocks_release(void)
{
    int t;
    for (t = 1; t < NTRK; t++) {
        ipart_t none;
        none.nlk = 0;
        ilocks_apply(t, &none, -1);
    }
}

static void locks_release(void)           /* every knob a lock holds back to its own value (stop, kit change) */
{
    int pad, i;
    for (pad = 0; pad < NPADS; pad++) {
        for (i = 0; i < NPOTS; i++)
            if (ovr_mask[pad] >> i & 1u)
                drums_set(drum_model(pad), pad, i, ovr_base[pad][i]);
        ovr_mask[pad] = 0;
    }
}

static uint32_t rnd100(void)              /* 0..99 */
{
    chance_rng ^= chance_rng << 13;
    chance_rng ^= chance_rng >> 17;
    chance_rng ^= chance_rng << 5;
    return chance_rng % 100u;
}

static int chance_skips(const dpart_t *p, int step, int pad)   /* the step's chance says no this time */
{
    int i = lock_find(p, step, pad, LK_PROB);
    return i >= 0 && (int)rnd100() >= p->lk[i].val;
}

static float base_step(int tr)            /* a step of the track's part without swing, samples */
{
    return 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * 0.25f * RATE_MUL[*part_rate(tr)];
}

static float step_samples(int tr, int step)
{
    float base = base_step(tr), sw = (float)proj.swing * 0.01f;   /* a pair of steps keeps its length */
    return (step & 1) ? 2.0f * base * (1.0f - sw) : 2.0f * base * sw;
}

/* Duration through the actual swung step clock, including odd-length
 * wraps. Sum alternating long/short steps in O(1), then the fractional step. */
static int32_t note_gate_samples(int tr, int step, int len, int gate)
{
    int whole = gate / 8, first = whole < len - step ? whole : len - step;
    int after = whole - first, last = (step + whole) % len;
    int swing_units = (first & 1) * ((step & 1) ? -1 : 1) + (len & 1) * (after / len) + (after % len & 1);
    float base = base_step(tr), delta = (proj.swing - 50) * .02f * base;
    return (int32_t)(base * whole + delta * swing_units +
                     (gate & 7) * .125f * (base + ((last & 1) ? -delta : delta)));
}

static float bar_samples(void) { return 4.0f * 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f); }

/* the metronome (hold REC): a click a beat, the bar's first higher; OFF, ON while playing, REC while recording. And
 * the count-in: with recording on, PLAY counts 1 or 2 bars of clicks first, then the sequencer starts. The beats
 * follow the bar: the sequencer's bar start is beat 1 (bar_at), a counter of quarter bars gives the rest */
enum { CK_OFF, CK_ON, CK_REC };
static struct {
    float to_next;                        /* samples to the next beat */
    int beat;                             /* the next beat in the bar (0: its first) */
    float env, y1, y2, c;                 /* the click sounding: its level, a sine by its recurrence */
    volatile int32_t count;               /* the count-in: samples before the sequencer starts (0: none) */
    volatile uint8_t pre;                 /* notes recorded in its last half beat: step 1 left once (a bit a track) */
    volatile uint16_t pre_pads;           /* (the drums': a bit a pad) */
    volatile uint8_t rec;                 /* recording on (the main loop's ui.rec, for the interrupt) */
    volatile uint32_t n;                  /* clicks so far (the tests) */
} ck;
/* the perform keys (HOME: black keys 5-11 held; OCT+ held: TAPE STOP). The main loop sets want; the interrupt follows
 * it at the next block. ROLL replays each track's step as it was pressed, at RATE, while the sequencer goes on
 * silently underneath; STUTTER loops a slice of the output; TAPE STOP slows the output to a halt (both in the
 * delay's memory, lent as for sampling); FILTER, THROW and WASH borrow the DJ filter, the delay and the reverb;
 * CRUSH lowers the output's bits and rate; FILL plays the next pattern's drums. Let go: as before */
enum { PF_ROLL, PF_FILL, PF_STUT, PF_FILT, PF_THROW, PF_WASH, PF_CRUSH, NPF, PF_TAPE = NPF };
static const char *const PF_NAME[NPF + 1] = {"ROLL", "FILL", "STUTTER", "FILTER", "THROW", "WASH", "CRUSH", "TAPE STOP"};
static const char *const ROLL_NAME[5] = {"1/8", "1/16", "1/32", "1/16T", "1/32T"};
static const float ROLL_F[5] = {2.0f, 1.0f, 0.5f, 2.0f / 3.0f, 1.0f / 3.0f};
static const char *const STUT_NAME[3] = {"1/32", "1/16", "1/8"};
static struct {
    volatile uint8_t want;                /* the keys held (a bit each, PF_*) */
    uint8_t on;                           /* the interrupt's, as it last saw them */
    uint8_t roll_rate, stut_len, filt, throw_fb, wash, crush;   /* each one's knob (K1 while it's held) */
    float roll_to, roll_len;              /* ROLL: samples to the next, between them */
    int8_t roll_step[NTRK];               /* the steps it replays */
    int32_t st_len, st_n, st_pos;         /* STUTTER: the slice, how much is in, where it plays */
    int32_t tp_w;                         /* TAPE STOP: written, read (by speed) */
    float tp_r, tp_v;
    int cr_n;                             /* CRUSH: the held sample */
    float cr_l, cr_r;
    float fade;                           /* back to the live sound after STUTTER or TAPE STOP: 0..1 */
    volatile uint32_t rolls;              /* ROLL's replays so far (the tests) */
} perf = {0, 0, 1, 1, 30, 90, 127, 70, 0.0f, 0.0f, {0}, 0, 0, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, 1.0f, 0};

static int click_mode(void) { return (int)(proj.click & 3u); }
static int countin_bars(void) { return (int)(proj.click >> 2 & 3u); }
static int click_level(void) { return proj.click >> 4 ? (int)(proj.click >> 4) : 10; }
static int recq_grid(void) { return (int)(proj.recq & 3u); }
static int recq_steps(void) { return recq_grid() == 2 ? 4 : recq_grid() == 1 ? 2 : 1; }
enum { LN_BAR, LN_BEAT, LN_NOW };
static int launch_mode(void) { return (int)(proj.recq >> 4 & 3u); }

static void click_hit(int first)          /* 1.5 kHz on the bar's first beat, 1 kHz on the others, ~10 ms */
{
    ck.c = first ? 1.95450f : 1.97973f;   /* 2 cos(2 pi f / 44100) */
    ck.y2 = 0.0f;
    ck.y1 = first ? 0.21209f : 0.14200f;  /* sin(2 pi f / 44100) */
    ck.env = (float)click_level() * (first ? 0.034f : 0.024f);
    ck.n++;
}

/* where recording puts a note played at step st and fr of the next: the nearest point of the grid (hold REC,
 * QUANTIZE), round to the start after the last */
static int rec_target(int st, float fr, int len)
{
    int g = recq_steps(), t = (int)(((float)st + fr) / (float)g + 0.5f) * g;
    return t >= len ? 0 : t;
}

static void seq_drum(void)                /* audio interrupt: the drum part's next step, and its hits */
{
    dpart_t *p = dcur();
    int next = sq.t[TR_DRUM].step + 1, pad, muted = (int)(proj.tmute & 1u);
    if (next >= p->len)
        next = 0;
    sq.t[TR_DRUM].step = next;
    for (pad = 0; pad < NPADS && !(perf.on >> PF_ROLL & 1u); pad++) {   /* (ROLL: silent underneath) */
        if (!bit_get(p->hit[pad], next) || (p->mute >> pad & 1u))
            continue;
        if (sq.skip_once[pad] == next) {
            sq.skip_once[pad] = -1;
            continue;
        }
        if (muted || chance_skips(p, next, pad))
            continue;
        locks_apply(p, next, pad);
        pad_fire(pad, bit_get(p->acc, next) ? 1.0f : DRUMS_VEL_NORMAL);
        if (pad == 0)                     /* the bass drum: the master's PUMP ducks to it */
            fx_key(drum_model(0), bit_get(p->acc, next) ? 1.0f : DRUMS_VEL_NORMAL);
    }
    sq.t[TR_DRUM].dur = step_samples(TR_DRUM, next);
    sq.t[TR_DRUM].to_next += sq.t[TR_DRUM].dur;
}

static void seq_inst(int tr)              /* audio interrupt: an instrument part's next step, and its notes */
{
    ipart_t *p = ipart(tr);
    const track_t *ts = tset(tr);
    const nstep_t *s;
    int next = sq.t[tr].step + 1, i, slide_in = sq.slid[tr], marks;
    int32_t gate;
    if (next >= p->len)
        next = 0;
    sq.t[tr].step = next;
    sq.t[tr].dur = step_samples(tr, next);
    sq.t[tr].to_next += sq.t[tr].dur;
    sq.slid[tr] = 0;
    s = &p->st[next];
    if (!s->note[0] || (perf.on >> PF_ROLL & 1u))   /* (ROLL: silent underneath) */
        return;
    if (sq.iskip[tr] == next) {
        sq.iskip[tr] = -1;
        return;
    }
    if ((proj.tmute >> tr & 1u) || (ns_prob(s) < 100 && (int)rnd100() >= ns_prob(s)))
        return;
    ilocks_apply(tr, p, next);
    gate = note_gate_samples(tr, next, p->len, s->gate);
    marks = (s->vel & NS_ACC) ? SN_ACCENT : 0;   /* a 303's marks: on any sound (a slide: the chord's first note) */
    for (i = 0; i < NCHORD && s->note[i]; i++) {
        if (ts->arp_mode)                 /* arpeggiated: the notes are the arpeggiator's, for the gate */
            arp_feed(tr, s->note[i], ns_vel(s), gate);
        else
            syn_note_on_f(tr, s->note[i], (float)ns_vel(s) / 127.0f, gate, ts->type == TY_MONO,
                          marks | (slide_in && i == 0 ? SN_SLIDE : 0));
    }
    sq.slid[tr] = (uint8_t)(!ts->arp_mode && (s->prob & NS_SLIDE));
    if (!ts->arp_mode)
        lfo_note(tr);
}

static void roll_fire(void)               /* ROLL: each track's step as it was pressed, again */
{
    int t, pad;
    perf.rolls++;
    const dpart_t *d = dcur();
    int st = perf.roll_step[TR_DRUM] % (d->len ? d->len : 1);
    if (clip[TR_DRUM] >= 0 && !((proj.tmute | roll_inhibit_tracks) & 1u))
        for (pad = 0; pad < NPADS; pad++)
            if (bit_get(d->hit[pad], st) && !((d->mute | roll_inhibit_pads) >> pad & 1u)) {
                float v = bit_get(d->acc, st) ? 1.0f : DRUMS_VEL_NORMAL;
                pad_fire(pad, v);
                if (pad == 0)
                    fx_key(drum_model(0), v);
            }
    for (t = 1; t < NTRK; t++) {
        const ipart_t *p = ipart(t);
        const nstep_t *s = &p->st[perf.roll_step[t] % (p->len ? p->len : 1)];
        int i;
        if (clip[t] < 0 || ((proj.tmute | roll_inhibit_tracks) >> t & 1u) || !s->note[0])
            continue;
        for (i = 0; i < NCHORD && s->note[i]; i++)
            syn_note_on(t, s->note[i], (float)ns_vel(s) / 127.0f, (int32_t)(perf.roll_len * 0.6f), tset(t)->type == TY_MONO);
    }
}

static void seq_reset_tracks(void)
{
    int t;
    for (t = 0; t < NTRK; t++) {
        sq.t[t].step = -1;
        sq.t[t].to_next = 0.0f;
        sq.t[t].dur = 1.0f;
        sq.t[t].frac = 0.0f;
        sq.iskip[t] = -1;
        sq.slid[t] = 0;
    }
    for (t = 0; t < NPADS; t++)
        sq.skip_once[t] = -1;
}

static void track_restart(int t)          /* a track from its step 1, now */
{
    sq.t[t].step = -1;
    sq.t[t].to_next = 0.0f;
    sq.t[t].dur = 1.0f;
    sq.t[t].frac = 0.0f;
    sq.iskip[t] = -1;
    sq.slid[t] = 0;
    if (t == TR_DRUM) {
        int p;
        for (p = 0; p < NPADS; p++)
            sq.skip_once[p] = -1;
    }
}

enum { FO_OFF, FO_NEXT, FO_PREV, FO_FIRST, FO_RANDOM };
static const char *const FOLLOW_NAME[5] = {"OFF", "NEXT", "PREV", "FIRST", "RANDOM"};

static int follow_target(int p)           /* where a scene's follow action goes */
{
    int i;
    switch (proj.follow[p] & 7u) {
    case FO_NEXT: return (p + 1) % NPAT;
    case FO_PREV: return (p + NPAT - 1) % NPAT;
    case FO_FIRST: return 0;
    default:                              /* RANDOM: another pattern with something in it */
        for (i = 0; i < 24; i++) {
            int q = (int)(rnd100() % NPAT);
            if (q != p && !pattern_empty(&proj.p[q]))
                return q;
        }
        return p;
    }
}

static void clips_start(void)             /* the clips waiting, started (or their tracks stopped); the lock held */
{
    int t;
    for (t = 0; t < NTRK; t++) {
        if (qclip[t] == -1)
            continue;
        if (qclip[t] == -2) {
            clip[t] = -1;
            if (t != TR_DRUM)
                syn_release(t, 1);        /* its notes let go */
        } else {
            set_clip(t, qclip[t]);
            track_restart(t);
        }
        qclip[t] = -1;
    }
}

static int clips_waiting(void)
{
    int t;
    for (t = 0; t < NTRK; t++)
        if (qclip[t] != -1)
            return 1;
    return 0;
}

static void seq_begin(void)               /* (the lock held, or in the audio interrupt: the count-in's end) */
{
    if (proj.song_on && proj.song_n) {    /* the song: its first (or the cursor's) entry's pattern */
        int e = song_from >= 0 && song_from < proj.song_n ? song_from : 0;
        srt.pos = (int8_t)e;
        srt.left = proj.song[e].bars;
        srt.started = 0;
        proj.pat = proj.song[e].pat;
        sq.queued = -1;
        clips_all(proj.pat);
    } else {
        srt.pos = -1;
    }
    seq_reset_tracks();
    sq.bar_left = 0.0f;
    sq.clock = 0;
    sq.fbars = 0;
    sq.fskip = 1;
    sq.mclk_to = 0.0f;
    if ((proj.recq >> 2 & 3u) == 1u)      /* MIDI CLOCK sent: Start */
        plat_midi_send(0x0Fu | 0xFAu << 8, TO_USB);
    lfo_play();
    sq.playing = 1;
}

static void seq_start(void)
{
    uint32_t f = plat_lock();
    ck.count = 0;
    seq_begin();
    plat_unlock(f);
}

static void seq_stop(void)
{
    uint32_t f = plat_lock();
    int t;
    if (sq.playing && (proj.recq >> 2 & 3u) == 1u)   /* MIDI CLOCK sent: Stop */
        plat_midi_send(0x0Fu | 0xFCu << 8, TO_USB);
    sq.playing = 0;
    srt.pos = -1;
    for (t = 0; t < NTRK; t++)
        sq.t[t].step = -1;
    locks_release();
    ilocks_release();
    syn_release(-1, 1);                    /* the sequencer's notes let go; notes held on the keys keep sounding */
    for (t = 1; t < NTRK; t++) {           /* and the steps' notes in the arpeggiators */
        int i;
        for (i = arp[t].n - 1; i >= 0; i--)
            if (arp[t].life[i] >= 0)
                arp_drop(t, i);
    }
    if (sq.queued >= 0) {
        proj.pat = (uint8_t)sq.queued;
        sq.queued = -1;
        clips_all(proj.pat);
    }
    for (t = 0; t < NTRK; t++)            /* clips that were waiting: theirs now */
        if (qclip[t] != -1) {
            if (qclip[t] == -2)
                clip[t] = -1;
            else
                set_clip(t, qclip[t]);
            qclip[t] = -1;
        }
    plat_unlock(f);
}

/* ------------------------------------------------------------------- audio --- */
/* MIDI in: channel 10 plays the pads (General MIDI drum notes), channels 2..4 play T2..T4, channel 1 the track
 * picked */
static int gm_pad(uint32_t note)
{
    switch (note) {
    case 35: case 36: return 0;
    case 38: case 40: return 1;
    case 41: case 43: return 2;
    case 45: case 47: return 3;
    case 48: case 50: return 4;
    case 37: return 5;
    case 39: return 6;
    case 42: case 44: return 7;
    case 46: return 8;
    case 49: case 52: case 55: case 57: return 9;
    case 51: case 53: case 56: case 59: return 10;
    default: return -1;
    }
}
static const uint8_t PAD_GM[NPADS] = {36, 38, 41, 45, 48, 37, 39, 42, 46, 49, 51};

/* MIDI CLOCK followed: a clock's time is the block it came in (samples rendered: the same on the FM-1, which reads
 * MIDI once a block, and in the simulator), so each is up to a block (5.8 ms) late. The tempo is timed over the last
 * 4 beats, which spreads that thin (a sudden change: from the last beat alone, at once); the sequencer runs on it, and
 * every 6th clock (a 16th) pulls the groove an eighth of the way to it (a quarter when it is more than 10 ms off), so
 * the two never drift apart and a block's lateness moves it by a millisecond or so */
static uint32_t render_t;                 /* samples rendered */
static uint16_t bpm_was;                  /* the tempo the sequencer's timers are in */

static void tempo_stretch(void)           /* audio interrupt: a new tempo (the knob, TAP, MIDI CLOCK): what is left of the
                                           * bar, each track's step, the click's beat, ROLL's and the clock sent
                                           * stretched to it, so they stay where they are in the bar (else the steps
                                           * take it at their next step, the bar at its next bar, and they drift apart) */
{
    if (bpm_was && proj.bpm10 != bpm_was && sq.playing) {
        float r = (float)bpm_was / (float)proj.bpm10;
        int t;
        sq.bar_left *= r;
        for (t = 0; t < NTRK; t++)
            if (sq.t[t].to_next < 1e8f)
                sq.t[t].to_next *= r;
        ck.to_next *= r;
        sq.mclk_to *= r;
        perf.roll_to *= r;
        perf.roll_len *= r;
    }
    bpm_was = proj.bpm10;
}

static void mclk_tick(void)               /* every 6th clock (a 16th): its time kept, the tempo from them, the groove
                                           * pulled to it (audio interrupt) */
{
    uint32_t t = render_t;
    int i, n;
    for (i = 16; i > 0; i--)
        sq.mclk_t[i] = sq.mclk_t[i - 1];
    sq.mclk_t[0] = t;
    if (sq.mclk_nt < 17u)
        sq.mclk_nt++;
    n = sq.mclk_nt;
    if (n >= 2) {
        float tb = (float)(t - sq.mclk_t[n - 1]) * 4.0f / (float)(n - 1);   /* a beat: over the 16ths kept */
        if (n >= 5) {                     /* a beat's worth: a sudden change starts again from the last beat */
            float t1 = (float)(t - sq.mclk_t[4]);
            if (sq.mclk_avg > 0.0f && (t1 - sq.mclk_avg > sq.mclk_avg * 0.0625f ||
                                       sq.mclk_avg - t1 > sq.mclk_avg * 0.0625f)) {
                sq.mclk_nt = 5;
                tb = t1;
            }
        }
        if (tb > 10584.0f && tb < 44100.0f) {   /* 60..250 BPM */
            sq.mclk_avg = tb;
            proj.bpm10 = (uint16_t)clamp((int32_t)(26460000.0f / tb + 0.5f), 600, 2400);   /* 600 x 44100 / beat */
            tempo_stretch();              /* (before the beat is looked for, in the new tempo's samples) */
        } else {
            sq.mclk_nt = 1;               /* (not a tempo: timed again from here) */
        }
    }
    if (sq.playing) {                     /* where the groove is against the clock: this 16th's place in the beat */
        float bl = bar_samples() * 0.25f, rem = sq.bar_left - (float)(int)(sq.bar_left / bl) * bl;
        float d = (bl - rem) - bl * (float)sq.mclk_n * (1.0f / 24.0f);   /* > 0: the groove past it, < 0: behind */
        if (d >= bl * 0.5f)
            d -= bl;
        if (d < -bl * 0.5f)
            d += bl;
        sq.mclk_err = d;
        d *= (d > 441.0f || d < -441.0f) ? 0.25f : 0.125f;   /* (over 10 ms: faster) */
        sq.bar_left += d;                 /* (as NUDGE: later for > 0) */
        for (i = 0; i < NTRK; i++)
            if (sq.t[i].to_next < 1e8f)
                sq.t[i].to_next += d;
        if (sq.bar_left < 0.0f)
            sq.bar_left = 0.0f;
    }
}

static void midi_in(void)                 /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu, ch = st & 0x0Fu;
        if (st >= 0xF8u) {                /* real time: MIDI CLOCK followed (SETUP) */
            if ((proj.recq >> 2 & 3u) != 2u)
                continue;
            if (st == 0xF8u) {            /* a clock: the first after a Start starts; every 6th times the tempo */
                if (sq.mclk_go) {
                    sq.mclk_go = 0;
                    if (!sq.playing)
                        seq_begin();
                }
                if (++sq.mclk_n >= 24u)
                    sq.mclk_n = 0;
                if (sq.mclk_n % 6u == 0u)
                    mclk_tick();
            } else if ((st == 0xFAu || st == 0xFBu) && !sq.playing) {   /* Start (Continue: from the top too) */
                sq.mclk_go = 1;
                sq.mclk_n = 23;           /* (the next clock is the first beat's) */
                sq.mclk_nt = 0;           /* (the 6th clocks counted from it) */
            } else if (st == 0xFCu) {
                sq.mclk_go = 0;
                if (sq.playing)
                    sq.ext_stop = 1;
            }
            continue;
        }
        int on = (st & 0xF0u) == 0x90u && d2, off = (st & 0xF0u) == 0x80u || ((st & 0xF0u) == 0x90u && !d2);
        int tr = ch == 9u ? TR_DRUM : ch == 0u ? (int)sel_trk : ch <= 3u ? (int)ch : -1;
        (void)port;
        if (tr == TR_DRUM) {
            int pad = gm_pad(d1);
            if (on && pad >= 0)
                pad_fire(pad, d2 >= 127u ? 1.0f : (float)d2 / 127.0f);
        } else if (tr > 0 && d1 > 0u) {
            if (on && track_is_muted(tr))
                continue;
            if (tset(tr)->arp_mode) {
                if (on || off)
                    arp_key(tr, (int)d1, (int)d2, on);
            } else if (on) {
                syn_note_on(tr, (int)d1, (float)d2 / 127.0f, -1, tset(tr)->type == TY_MONO);
                lfo_note(tr);
            } else if (off) {
                syn_note_off(tr, (int)d1);
            }
        }
    }
}

/* the synth, a block of SYN_CTL samples at a time (its voices dealt between the two cores), taken out as the render
 * needs it: a note starts at the next block, within 0.73 ms */
static float syn_l[SYN_CTL], syn_r[SYN_CTL], syn_rv[SYN_CTL], syn_dl[SYN_CTL];
static uint32_t syn_at = SYN_CTL;
static uint8_t cpu2;                      /* the second core takes half the voices */
static uint8_t pool_max, pool_now;        /* the voices allowed: 16 on two cores, 8 on one; fewer while the CPU is short */

static void syn_job(uint32_t half) { syn_half(half); }

static void syn_block(void)
{
    int t;
    for (t = 0; t < NTRK; t++)
        lfo_tick(t, SYN_CTL);
    for (t = 1; t < NTRK; t++) {
        arp_tick(t, SYN_CTL);
        syn_mod(t, proj.lfo[t].dest, lfo_rt[t].out, (float)proj.lfo[t].depth / 64.0f);
    }
    syn_prepare();
    if (cpu2 && plat_cpu2_on() && plat_cpu2_run(syn_job, 1u)) {
        syn_half(0);
        if (plat_cpu2_wait() != 0)        /* it was given up (cpu2_fallback): its half here */
            syn_half(1);
    } else {
        syn_half(0);
        syn_half(1);
    }
    syn_mix(syn_l, syn_r, syn_rv, syn_dl);
    syn_at = 0;
}

/* the USB stems (fm1.toml usb_audio_channels = 10): beside the master on USB audio inputs 1-2, each track alone on its
 * own pair, the drums on 3-4 and T2..T4 on 5-6, 7-8, 9-10. A stem is the track as it comes into the mix: its level,
 * pan, mutes and solo, before the reverb and the delay (their sends are heard on the master only) and before the master
 * chain and MASTER; the metronome and the perform keys' STUTTER, TAPE STOP and CRUSH are the master's alone too.
 * They're at the level the screen's track meters show: the mix as the master bus takes it (after FX_BUS_TRIM), so the
 * four add up to the master bus's input less the reverb and the delay; the demos peak 6 to 14 dB under full scale
 * (12 dB more clipped the drums). 16 bits each, clipped past full scale. plat_usb_ch gives nothing while no computer
 * records: no work then */
#define STEMS (PLAT_USB_CH >= 2 * NTRK)
#define STEM_GAIN FX_BUS_TRIM
static uint8_t stems_on;                  /* this block: a computer records the stems */
#ifdef PLAT_HOST
static float stem_pk[NTRK][2];            /* each stem's peak since the last look, 1 full scale (the tests) */
static uint32_t stem_overs;               /* samples clipped since the last look */
#endif

static int16_t stem_q(float x)
{
    return (int16_t)(x >= 1.0f ? 32767 : x <= -1.0f ? -32767 : (int)(x * 32767.0f));
}

/* n frames of track s (l, r: the mix's scale) into its stem, from the frame off into the block being rendered */
static void stem_put(int s, const float *l, const float *r, uint32_t off, uint32_t n)
{
    while (n) {
        uint32_t run, k;
        int16_t *p = plat_usb_ch(off, &run);
        if (!p || !run)
            return;
        if (run > n)
            run = n;
        p += 2 * s;
        for (k = 0; k < run; k++, p += PLAT_USB_CH_STRIDE) {
            float a = l[k] * STEM_GAIN, b = r[k] * STEM_GAIN;
            p[0] = stem_q(a);
            p[1] = stem_q(b);
#ifdef PLAT_HOST
            a = a < 0.0f ? -a : a;
            b = b < 0.0f ? -b : b;
            if (a > stem_pk[s][0])
                stem_pk[s][0] = a;
            if (b > stem_pk[s][1])
                stem_pk[s][1] = b;
            stem_overs += (a > 1.0f) + (b > 1.0f);
#endif
        }
        l += run;
        r += run;
        off += run;
        n -= run;
    }
}

/* the synth into the mix (and each of its tracks into its stem), from the frame off into the block being rendered */
static void synth_add(float *l, float *r, float *rv, float *dl, uint32_t n, uint32_t off)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t k, m;
        if (syn_at >= SYN_CTL)
            syn_block();
        m = SYN_CTL - syn_at;
        if (m > n - i)
            m = n - i;
        for (k = 0; k < m; k++) {
            l[i + k] += syn_l[syn_at + k];
            r[i + k] += syn_r[syn_at + k];
            rv[i + k] += syn_rv[syn_at + k];
            dl[i + k] += syn_dl[syn_at + k];
        }
        if (STEMS && stems_on) {
            int t;
            for (t = 1; t < NTRK; t++) {
                const float *a = syn_track(t, 0), *b = syn_track(t, 1);
                if (a && b)
                    stem_put(t, a + syn_at, b + syn_at, off + i, m);
            }
        }
        syn_at += m;
        i += m;
    }
}

/* a recording of the FM-1's own sound for a SAMPLER track (REC held + SEL): in RAM (its file: a head, then the frames),
 * armed until the first sound over -40 dB, then up to REC_FRAMES (2 s at 44.1 kHz, 4 s at 22.05 kHz: every two frames
 * one); the track plays it from here until it is stored in the bank, at once when stopped, else at the stop */
#define REC_FRAMES 88200u
enum { RS_IDLE, RS_ARMED, RS_REC, RS_DONE };
#define REC_BYTES (SMP_HEAD + (REC_FRAMES * 3u + 1u) / 2u + 4u)
/* one buffer, two uses: a recording, or else (the FX's) the delay's line, 1.5 s of int16: the delay leaves the chain
 * while sampling has it (from REC + SEL until the recording is in the bank) */
static int16_t shared_buf[(REC_BYTES + 1u) / 2u] PLAT_POOL;
#define rec_buf ((uint8_t *)shared_buf)
#define DLY_LEN ((int)((REC_BYTES + 1u) / 2u))
static struct {
    volatile uint8_t state;               /* RS_* */
    uint8_t trk;                          /* the track recording, or whose recording rec_buf holds */
    uint8_t half;                         /* 22.05 kHz */
    uint8_t pending;                      /* rec_buf holds a recording not in the bank yet */
    uint8_t odd;
    float first;                          /* 22.05 kHz: the first of a pair */
    volatile uint32_t pos;                /* frames recorded */
} rec;

static void rec_frame(float m)            /* audio interrupt: a frame into rec_buf, packed as it goes */
{
    uint8_t *b;
    int32_t v;
    if (rec.half) {
        if (!rec.odd) {
            rec.first = m;
            rec.odd = 1;
            return;
        }
        m = 0.5f * (rec.first + m);
        rec.odd = 0;
    }
    b = rec_buf + SMP_HEAD + (rec.pos >> 1) * 3u;
    v = (int32_t)(m * 2047.0f);
    if (v > 2047) v = 2047; else if (v < -2048) v = -2048;
    v &= 0xFFF;
    if (rec.pos & 1u) {
        b[1] = (uint8_t)((b[1] & 0x0Fu) | (v & 0x0F) << 4);
        b[2] = (uint8_t)(v >> 4);
    } else {
        b[0] = (uint8_t)v;
        b[1] = (uint8_t)(v >> 8);
        b[2] = 0;
    }
    if (++rec.pos >= REC_FRAMES)
        rec.state = RS_DONE;
}

/* the render's buses: the dry mix, the reverb and delay sends, and the FX's out */
static float buf_l[APP_BLOCK], buf_r[APP_BLOCK], bus_rev[APP_BLOCK], bus_dly[APP_BLOCK], mix_out_l[APP_BLOCK], mix_out_r[APP_BLOCK];
static volatile float peak_l, peak_r;     /* each side's peak since the last look (the tests) */
static volatile uint32_t clips;           /* samples past the ceiling (the tests: the limiter keeps it 0) */

static void drum_mix(float *l, float *r, float *rv, float *dl, uint32_t n)   /* the drums' level, pan and sends */
{
    float lv = (float)proj.mix[0].level * 0.01f, p = (float)(clamp(proj.mix[0].pan, 1, 127) - 64) / 63.0f;
    float gl = lv * (p > 0.0f ? 1.0f - p : 1.0f), gr = lv * (p < 0.0f ? 1.0f + p : 1.0f);
    float rs = lv * (float)proj.mix[0].rev / 127.0f, ds = lv * (float)proj.mix[0].dly / 127.0f;
    uint32_t i;
    for (i = 0; i < n; i++) {
        l[i] *= gl;
        r[i] *= gr;
        rv[i] *= rs;
        dl[i] *= ds;
    }
}
static volatile float peak;               /* the output's peak since the screen last looked (its meter) */

static int16_t q16(float x) { return (int16_t)(x > 1.0f ? 32767 : x < -1.0f ? -32767 : (int)(x * 32767.0f)); }

static void pf_output(float *l, float *r, uint32_t n)   /* STUTTER, TAPE STOP, CRUSH on the output (under MASTER) */
{
    int16_t *b = shared_buf;
    int32_t cap = DLY_LEN / 2;
    uint32_t i;
    uint8_t on = perf.on;
    int bits = 16 - perf.crush * 12 / 127, hold = 1 + perf.crush * 15 / 127;
    float lv = (float)(1 << (bits - 1));
    for (i = 0; i < n; i++) {
        float x = l[i], y = r[i];
        if (on >> PF_TAPE & 1u) {         /* written as it comes, read slower and slower: 0.6 s to a stop */
            int32_t w = perf.tp_w % cap, i0 = (int32_t)perf.tp_r, a = i0 % cap, c = (i0 + 1) % cap;
            float fr = perf.tp_r - (float)i0, g = perf.tp_v < 0.05f ? perf.tp_v * 20.0f : 1.0f;
            b[2 * w] = q16(x);
            b[2 * w + 1] = q16(y);
            perf.tp_w++;
            x = ((float)b[2 * a] + (float)(b[2 * c] - b[2 * a]) * fr) * (g / 32767.0f);
            y = ((float)b[2 * a + 1] + (float)(b[2 * c + 1] - b[2 * a + 1]) * fr) * (g / 32767.0f);
            perf.tp_r += perf.tp_v;
            perf.tp_v = perf.tp_v > 0.0f ? perf.tp_v - 1.0f / 26460.0f : 0.0f;
        } else if (on >> PF_STUT & 1u) {  /* the slice: in as it plays, then round and round */
            if (perf.st_n < perf.st_len) {
                b[2 * perf.st_n] = q16(x);
                b[2 * perf.st_n + 1] = q16(y);
                perf.st_n++;
            } else if (perf.st_len > 0) {
                int32_t p = perf.st_pos, e = p < perf.st_len - 1 - p ? p : perf.st_len - 1 - p;
                float g = e < 48 ? (float)e / 48.0f : 1.0f;   /* (its ends faded: no click at the turn) */
                x = (float)b[2 * p] * (g / 32767.0f);
                y = (float)b[2 * p + 1] * (g / 32767.0f);
                perf.st_pos = (p + 1) % perf.st_len;
            }
        } else if (perf.fade < 1.0f) {      /* back to the live sound */
            perf.fade += 1.0f / 256.0f;
            x *= perf.fade;
            y *= perf.fade;
        }
        if (on >> PF_CRUSH & 1u) {        /* fewer bits, a lower rate */
            if (++perf.cr_n >= hold) {
                perf.cr_n = 0;
                perf.cr_l = (float)(int)(x * lv) / lv;
                perf.cr_r = (float)(int)(y * lv) / lv;
            }
            x = perf.cr_l;
            y = perf.cr_r;
        }
        l[i] = x;
        r[i] = y;
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t done = 0, i;
    int bar_at = -1;
    float gain = (float)master_q12 / 4096.0f, pk = peak, pkl = peak_l, pkr = peak_r;
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    if (STEMS) {                          /* a computer records the USB stems this block */
        uint32_t run;
        stems_on = plat_usb_ch(0, &run) != 0;
    }
    midi_in();
    render_t += n;
    tempo_stretch();
    fx_bpm((float)proj.bpm10 * 0.1f);
    {   /* the perform keys: what changed since the last block */
        uint8_t w = perf.want, up = (uint8_t)(w & ~perf.on), down = (uint8_t)(perf.on & ~w);
        if (up >> PF_ROLL & 1u) {
            int t;
            roll_inhibit_tracks = proj.tmute;
            roll_inhibit_pads = dcur()->mute;
            for (t = 0; t < NTRK; t++)
                perf.roll_step[t] = (int8_t)(sq.t[t].step < 0 ? 0 : sq.t[t].step);
            perf.roll_to = 0.0f;
            perf.roll_len = bar_samples() * (1.0f / 16.0f) * ROLL_F[perf.roll_rate % 5];
        }
        if (up >> PF_STUT & 1u) {
            perf.st_len = (int32_t)(bar_samples() * (1.0f / 32.0f) * (float)(1 << (perf.stut_len % 3)));
            if (perf.st_len > DLY_LEN / 2)
                perf.st_len = DLY_LEN / 2;
            perf.st_n = perf.st_pos = 0;
        }
        if (up >> PF_TAPE & 1u) {
            perf.tp_w = 0;
            perf.tp_r = 0.0f;
            perf.tp_v = 1.0f;
        }
        if ((down >> PF_STUT & 1u) || (down >> PF_TAPE & 1u))
            perf.fade = 0.0f;
        perf.on = w;
    }
    if (sq.playing && (proj.recq >> 2 & 3u) == 1u) {   /* MIDI CLOCK sent: 24 a beat (to the block) */
        float tick = bar_samples() / 96.0f;
        while (sq.mclk_to < (float)n) {
            plat_midi_send(0x0Fu | 0xF8u << 8, TO_USB);
            sq.mclk_to += tick;
        }
        sq.mclk_to -= (float)n;
    }
    if (ck.count > 0) {                   /* the count-in: the sequencer starts with the block it runs out in */
        ck.count -= (int32_t)n;
        if (ck.count <= 0) {
            int t;
            ck.count = 0;
            seq_begin();
            for (t = 1; t < NTRK; t++)    /* notes recorded on step 1 in its last half beat: heard, so left once */
                if (ck.pre >> t & 1u)
                    sq.iskip[t] = 0;
            for (t = 0; t < NPADS; t++)
                if (ck.pre_pads >> t & 1u)
                    sq.skip_once[t] = 0;
            ck.pre = 0;
            ck.pre_pads = 0;
        }
    }
    while (done < n) {                     /* rendered in pieces that end where a step (or the bar) starts */
        uint32_t chunk = n - done;
        int t;
        if (sq.playing) {
            float m;
            if (sq.bar_left < 0.5f) {      /* the bar first: a queued pattern starts there, every track at its step 1 */
                if (srt.pos >= 0 && proj.song_on && proj.song_n) {   /* the song: an entry's bars, then the next */
                    if (srt.pos >= proj.song_n)
                        srt.pos = 0;
                    if (!srt.started) {
                        srt.started = 1;
                    } else if (srt.left <= 1) {
                        srt.pos = (int8_t)((srt.pos + 1) % proj.song_n);   /* (round again after the last) */
                        srt.left = proj.song[srt.pos].bars;
                        sq.queued = (int8_t)proj.song[srt.pos].pat;
                    } else {
                        srt.left--;
                    }
                }
                if (sq.fskip) {           /* a scene's follow action: after its bars, the next scene */
                    sq.fskip = 0;
                } else if (sq.queued < 0 && srt.pos < 0 && (proj.follow[proj.pat] & 7u) &&
                           ++sq.fbars >= (proj.follow[proj.pat] >> 3) + 1) {
                    sq.queued = (int8_t)follow_target(proj.pat);
                }
                if (sq.queued >= 0) {     /* a scene: every track on it, from step 1 */
                    proj.pat = (uint8_t)sq.queued;
                    sq.queued = -1;
                    clips_all(proj.pat);
                    seq_reset_tracks();
                    sq.fbars = 0;
                }
                if (launch_mode() == LN_BAR || launch_mode() == LN_BEAT)
                    clips_start();        /* (a bar's start is a beat too) */
                sq.bar_left += bar_samples();
                lfo_bar();
                bar_at = (int)done;       /* (the metronome's beat 1) */
            }
            if (launch_mode() == LN_BEAT && (clips_waiting() || sq.queued >= 0)) {   /* LAUNCH at the beat */
                float bl = bar_samples() * 0.25f, bt = sq.bar_left - (float)(int)(sq.bar_left / bl) * bl;
                if (bt < 0.5f || bt > bl - 0.5f) {
                    if (sq.queued >= 0) {
                        proj.pat = (uint8_t)sq.queued;
                        sq.queued = -1;
                        clips_all(proj.pat);
                        seq_reset_tracks();
                    }
                    clips_start();
                } else if (bt < (float)chunk) {
                    chunk = bt < 1.0f ? 1u : (uint32_t)bt;
                }
            }
            for (t = 0; t < NTRK; t++)
                if (sq.t[t].to_next < 0.5f) {
                    if (clip[t] < 0) {    /* stopped: silent until a clip starts it */
                        sq.t[t].step = -1;
                        sq.t[t].to_next = 1e9f;
                        continue;
                    }
                    if (t == TR_DRUM)
                        seq_drum();
                    else
                        seq_inst(t);
                    sq.ticks++;
                }
            if (perf.on >> PF_ROLL & 1u) {  /* ROLL: its own beat */
                if (perf.roll_to < 0.5f) {
                    roll_fire();
                    perf.roll_to += perf.roll_len;
                }
            }
            m = sq.bar_left;
            for (t = 0; t < NTRK; t++)
                if (sq.t[t].to_next < m)
                    m = sq.t[t].to_next;
            if ((perf.on >> PF_ROLL & 1u) && perf.roll_to < m)
                m = perf.roll_to;
            if (m < (float)chunk)
                chunk = m < 1.0f ? 1u : (uint32_t)m;
        }
        sync_drum_mutes();
        drums_render(proj.kit, buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, (int)chunk);
        if (!track_is_muted(TR_DRUM) && color_active(&proj.color[TR_DRUM]))
            color_run_drums(&color_rt[TR_DRUM], &color_drums_send, &proj.color[TR_DRUM],
                            buf_l+done, buf_r+done, bus_rev+done, bus_dly+done, chunk);
        drum_mix(buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, chunk);
        if (STEMS && stems_on)            /* (the mix holds the drums alone until the synth comes in) */
            stem_put(TR_DRUM, buf_l + done, buf_r + done, done, chunk);
        {   /* the drums' level, for the screen */
            float pk = drum_pk;
            for (i = done; i < done + chunk; i++) {
                float a = buf_l[i] < 0.0f ? -buf_l[i] : buf_l[i], b = buf_r[i] < 0.0f ? -buf_r[i] : buf_r[i];
                if (a > pk)
                    pk = a;
                if (b > pk)
                    pk = b;
            }
            drum_pk = pk;
        }
        synth_add(buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, chunk, done);
        if (sq.playing) {
            sq.bar_left -= (float)chunk;
            perf.roll_to -= (float)chunk;
            for (t = 0; t < NTRK; t++) {
                sq.t[t].to_next -= (float)chunk;
                sq.t[t].frac = sq.t[t].dur > 0.0f ? 1.0f - sq.t[t].to_next / sq.t[t].dur : 0.0f;
            }
            sq.clock += chunk;
        }
        done += chunk;
    }
    if (perf.on & (1u << PF_THROW | 1u << PF_WASH))   /* THROW, WASH: the whole mix into the delay, the reverb */
        for (i = 0; i < n; i++) {
            float d = 0.4f * (buf_l[i] + buf_r[i]);
            if (perf.on >> PF_THROW & 1u)
                bus_dly[i] += d;
            if (perf.on >> PF_WASH & 1u)
                bus_rev[i] += d;
        }
    fx_process(buf_l, buf_r, bus_rev, bus_dly, mix_out_l, mix_out_r, (int)n);   /* the reverb and the delay */
    if (rec.state == RS_ARMED || rec.state == RS_REC)   /* sampling: the mix as it sounds, before the master */
        for (i = 0; i < n && (rec.state == RS_ARMED || rec.state == RS_REC); i++) {
            float m = 0.5f * (mix_out_l[i] + mix_out_r[i]);
            if (rec.state == RS_ARMED && (m > 0.01f || m < -0.01f))
                rec.state = RS_REC;       /* the first sound over -40 dB starts it */
            if (rec.state == RS_REC)
                rec_frame(m > 1.0f ? 1.0f : m < -1.0f ? -1.0f : m);
        }
    fx_master(mix_out_l, mix_out_r, (int)n, gain);   /* compressor, filter, MASTER (gliding), limiter */
    if (perf.on & (1u << PF_STUT | 1u << PF_TAPE | 1u << PF_CRUSH) || perf.fade < 1.0f)
        pf_output(mix_out_l, mix_out_r, n);
    {
        float bl = bar_samples() * 0.25f;
        int mode = click_mode(), on = (sq.playing && (mode == CK_ON || (mode == CK_REC && ck.rec))) || ck.count > 0;
        for (i = 0; i < n; i++) {         /* the metronome's beats: the bar's start, then the counter */
            if (!sq.playing && ck.count <= 0) {
                ck.beat = 0;
            } else if ((int)i == bar_at) {
                if (on)
                    click_hit(1);
                ck.beat = 1;
                ck.to_next = bl;
            } else if ((ck.to_next -= 1.0f) <= 0.0f) {
                ck.to_next += bl;
                if (on && (ck.beat || !sq.playing))   /* (playing, beat 1 is the bar's) */
                    click_hit(ck.beat == 0);
                ck.beat = (ck.beat + 1) & 3;
            }
            if (ck.env > 0.0001f) {       /* after the master bus, under MASTER: not in a sample or the meters (in the
                                           * USB master pair, and under MASTER on the speaker: plat_usb_audio_full) */
                float y = ck.c * ck.y1 - ck.y2, v = ck.y1 * ck.env * gain;
                ck.y2 = ck.y1;
                ck.y1 = y;
                ck.env *= 0.9975f;
                mix_out_l[i] += v;
                mix_out_r[i] += v;
            }
        }
    }
    for (i = 0; i < n; i++) {
        float l = mix_out_l[i], r = mix_out_r[i];
        if (l > 0.999f || l < -0.999f || r > 0.999f || r < -0.999f)
            clips++;
        if (l > 0.999f) l = 0.999f; else if (l < -0.999f) l = -0.999f;
        if (r > 0.999f) r = 0.999f; else if (r < -0.999f) r = -0.999f;
        if (l > pkl) pkl = l; else if (-l > pkl) pkl = -l;
        if (r > pkr) pkr = r; else if (-r > pkr) pkr = -r;
        lr[2 * i] = (int32_t)(l * 8388607.0f);
        lr[2 * i + 1] = (int32_t)(r * 8388607.0f);
    }
    peak = pk > pkl ? (pk > pkr ? pk : pkr) : (pkl > pkr ? pkl : pkr);
    peak_l = pkl;
    peak_r = pkr;
}

void app_panic(void)
{
    sq.playing = 0;
    syn_kill();
}

void app_init(void)
{
    int t;
    drums_init();
    syn_init();
    lfo_init();
    fx_init(shared_buf, DLY_LEN);
    /* the USB mix is set by the tracks' own levels, not the MASTER knob (the user's call, 0.5): the render comes at
     * full level (app_render's master_q12 4096), the USB master pair takes it so, and the platform puts MASTER on for
     * the speaker and headphones after (device/uac_mix.h); the stems never had MASTER */
    plat_usb_audio_full(1);
    cpu2 = (uint8_t)(plat_cpu2_set(1) == 0);
    pool_max = pool_now = (uint8_t)(cpu2 ? SYN_VOICES : SYN_VOICES / 2);
    syn_pool(pool_max);
    user_load();
    project_load();
    for (t = 0; t < NTRK; t++) color_reset(&color_rt[t]);
    for (t = 1; t < NTRK; t++) {
        track_insert_bind(&insert_rt[t-1], insert_ring[(t-1)*2],
                          t < NINST ? insert_ring[(t-1)*2+1] : insert_pool);
        insert_route(t);
    }
    bank_scan();
    seq_reset_tracks();
    sq.queued = -1;
}

/* ---------------------------------------------------------------------- undo --- */
/* a snapshot is a whole pattern (all four tracks) */
#define NUNDO 8
typedef struct { pattern_t p; gen_t g[NINST]; uint8_t idx; } snap_t;   /* (the parts' generators with them) */
/* The total can never exceed NUNDO: a new edit drops redo, and undo/redo
 * each move one item. Grow the stacks from opposite ends of one pool. */
static snap_t history[NUNDO] PLAT_POOL;
static int undo_n, redo_n;
static uint32_t undo_t, undo_kind;

static void undo_capture(snap_t *s, uint8_t idx)
{
    s->p = proj.p[idx];
    for (int i = 0; i < NINST; i++)
        s->g[i] = proj.gen[idx][i];
    s->idx = idx;
}

static void undo_push(uint8_t idx)
{
    redo_n = 0;                          /* edits invalidate the future */
    if (undo_n == NUNDO) {
        for (int i = 1; i < NUNDO; i++)
            history[i - 1] = history[i]; /* discard the oldest past state */
        undo_n--;
    }
    undo_capture(&history[undo_n++], idx);
}

/* before an edit of the current pattern: a snapshot, unless the same kind of edit (a knob turned on, notes
 * recorded one after another) is still going on */
static int edit_pat(void);               /* the pattern an edit is in: the picked track's clip (after ui) */
static void undo_mark(uint32_t kind)
{
    uint32_t now = plat_ms();
    if (!(kind && kind == undo_kind && now - undo_t < 1000u))
        undo_push((uint8_t)edit_pat());
    undo_kind = kind;
    undo_t = now;
    redo_n = 0;
    dirty = 1;
}

/* At capacity the opposing stacks touch. Exchange the active pattern and
 * the boundary snapshot in place using one byte of scratch, not a new slot. */
static void undo_swap(void *live, void *saved, size_t len)
{
    unsigned char *a = live, *b = saved;
    for (size_t i = 0; i < len; i++) {
        unsigned char old = a[i];
        a[i] = b[i];
        b[i] = old;
    }
}

static void undo_step(int redo)
{
    snap_t *from, *to;
    uint32_t f;
    uint8_t idx;
    if (redo ? redo_n == 0 : undo_n == 0) {
        app_say(redo ? "REDO" : "UNDO", "NOTHING");
        return;
    }
    from = &history[redo ? NUNDO - redo_n : undo_n - 1];
    to = &history[redo ? undo_n : NUNDO - redo_n - 1];
    idx = from->idx;
    if (from != to)
        undo_capture(to, idx);
    f = plat_lock();
    if (from == to) {
        undo_swap(&proj.p[idx], &from->p, sizeof from->p);
        undo_swap(proj.gen[idx], from->g, sizeof from->g);
    } else {
        proj.p[idx] = from->p;
        for (int i = 0; i < NINST; i++)
            proj.gen[idx][i] = from->g[i];
    }
    plat_unlock(f);
    undo_n += redo ? 1 : -1;
    redo_n += redo ? -1 : 1;
    undo_kind = 0;
    dirty = 1;
    app_say(redo ? "REDO" : "UNDO", "");
}

/* ------------------------------------------------------------------------ UI --- */
enum { PG_SEQ, PG_EDIT, PG_MUTE, PG_HOME, PG_KIT, PG_KEYS, PG_ENV, PG_LFO, PG_ARP, PG_MIX, PG_FX, PG_SONG, PG_MST, PG_SETUP,
       PG_GEN, PG_PICK, PG_INSERT, PG_COLOR };
static const char *const PAGE_NAME[] = {"STEPS", "EDIT", "MUTE", "PATTERNS", "KIT", "KEYS", "ENV", "LFO", "ARP", "MIXER",
                                        "FX", "SONG", "MASTER", "SETUP", "ACID", "PICK NOTES", "INSERT", "COLOR"};
#define NGENPAGE 3                        /* the 303 generator's (SEQ after STEPS, T2..T4): ACID, NOTES, LINE (SELECT) */
static const char *const TRK_NAME[NTRK] = {"DRUMS", "T2", "T3", "T4"};
static const char *const TYPE_NAME[NTYPES] = {"MONO", "POLY", "SAMPLER"};
/* an instrument track's EDIT pages and ENV pages: the four knobs' parameters (dsp/synth.h SP_*, or the track's own) */
#define NEPAGE 8                          /* (the synth update's three after MORE: 0.2's pages kept their places) */
#define K_TYPE (-1)
#define K_LEVEL (-2)
static const char *const EPAGE_NAME[NEPAGE] = {"VOICE", "OSC", "TONE", "FILTER", "MORE", "OSC 2", "SHAPER", "MACRO"};
static const int8_t EPAGE_P[NEPAGE][4] = {
    {K_TYPE, K_LEVEL, SP_VOICES, SP_GLIDE},
    {SP_WAVE, SP_DETUNE, SP_MIX, SP_INTV},
    {SP_SUB, SP_NOISE, SP_PWM, SP_DRIVE},
    {SP_CUT, SP_RES, SP_FENV, SP_FTYPE},
    {SP_KTR, SP_VEL, SP_SPREAD, SP_LEGATO},
    {SP_WAVE2, SP_FM, SP_RING, SP_DRIFT},
    {SP_SHTYPE, SP_SHAPE, SP_BIAS, SP_SHPOS},
    {SP_M1, SP_M2, SP_M3, SP_M4},
};
#define EP_OSC2 5
#define EP_SHAPER 6
#define EP_MACRO 7
/* a SAMPLER track's: the sample's pages in place of OSC and TONE */
#define K_START (-3)
#define K_END (-4)
#define K_LOOP (-5)
#define K_LSTART (-6)
#define K_ROOT (-7)
#define K_TUNE (-8)
#define K_QUAL (-9)
#define NSPAGE 7
static const char *const SPAGE_NAME[NSPAGE] = {"VOICE", "SAMPLE", "PITCH", "FILTER", "MORE", "SHAPER", "MACRO"};
static const int8_t SPAGE_P[NSPAGE][4] = {
    {K_TYPE, K_LEVEL, SP_VOICES, SP_GLIDE},
    {K_START, K_END, K_LOOP, K_LSTART},
    {K_ROOT, K_TUNE, SP_DRIVE, K_QUAL},
    {SP_CUT, SP_RES, SP_FENV, SP_FTYPE},
    {SP_KTR, SP_VEL, SP_SPREAD, SP_LEGATO},
    {SP_SHTYPE, SP_SHAPE, SP_BIAS, SP_SHPOS},
    {SP_M1, SP_M2, SP_M3, SP_M4},
};
#define NENVPAGE 3
static const char *const ENVPAGE_NAME[NENVPAGE] = {"AMP ENV", "FILTER ENV", "MOD ENV"};
static const int8_t ENVPAGE_P[NENVPAGE][4] = {
    {SP_AA, SP_AD, SP_AS, SP_AR}, {SP_FA, SP_FD, SP_FS, SP_FR}, {SP_MA, SP_MD, SP_MS, SP_MR}};
static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
#define C_RED RGB(232, 76, 60)
#define C_GREEN RGB(90, 210, 130)
#define C_MUTEBG RGB(70, 18, 14)
static const uint16_t KIT_RGB[KIT_NUM] = {RGB(255, 120, 40), RGB(70, 160, 255), RGB(192, 124, 232), RGB(246, 179, 111)};
static const uint16_t TRK_RGB[NTRK] = {0, RGB(90, 210, 130), RGB(185, 125, 255), RGB(245, 205, 60)};

static struct {
    uint8_t page;
    uint8_t trk;                          /* the track picked */
    uint8_t pad;                          /* the drum pad being edited */
    uint8_t color_page;
    uint8_t spage;                        /* the step page, 0..7 */
    uint8_t seq_prev, seq_used;           /* a resize chord leaves the previous SEQ view alone */
    uint8_t epage;                        /* the EDIT page (four knobs each) */
    uint8_t envpage;                      /* an instrument track's ENV page: 0 the amp's, 1 the filter's, 2 MOD ENV */
    uint8_t lfopage, arppage;             /* the LFO's pages (LFO 1, its MORE; an instrument's LFO 2, MATRIX); ARP's 0, 1 */
    uint8_t mslot;                        /* MATRIX: the slot on the knobs, 0..SYN_SLOTS - 1 */
    uint8_t genpage;                      /* the 303 generator's page: 0 ACID (the chances), 1 NOTES, 2 LINE */
    uint8_t mixpage, fxpage;              /* the mixer's page (LEVEL, PAN, REVERB, DELAY); the FX's */
    uint8_t mstpage;                      /* the master bus's: CHAIN, EQ, EQ 2, COMP, COMP 2, TAPE, STEREO, LIMIT */
    int8_t knob_k;                        /* the knob turned last (its card stands out a moment), -1 none */
    uint32_t knob_t, live_t;              /* when; when the meters were last drawn */
    uint8_t scur;                         /* the SONG page's entry (song_n: the end, where a new one goes) */
    int8_t save_slot;                     /* SAVE held + PRESETS: the user slot the release saves to, -1 none */
    uint32_t shed_t;                      /* when the voice pool last moved */
    uint8_t rec;                          /* recording armed */
    uint8_t rec_shown;                    /* REC held long enough (or used): its page shows */
    uint32_t rec_t;                       /* when REC went down */
    int8_t count_shown;                   /* the count-in's beat on the band */
    uint8_t tkey_down, tkey_used;         /* HOME: track keys (black 1-4) held; used as a modifier (no mute) */
    int8_t pf_key;                        /* HOME: the perform key held last (its knob on K1), -1 none */
    int8_t solo_trk;                      /* the track soloed (HOME, a track key + OCT+), -1 none */
    uint8_t solo_save;                    /* the mutes before it */
    uint32_t tap_t[4];                    /* tap tempo: the last taps */
    uint8_t tap_n;
    int8_t scene_key;                     /* HOME: the scene key (white) held: K1, K2 set its follow action */
    uint8_t save_pending;                 /* SAVE while playing: saved at the stop */
    uint32_t spage_t;                     /* when the step page was last moved by hand (it follows the playhead) */
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint8_t edit_used, save_used, sel_used, rec_used;   /* EDIT / SAVE / SEL / REC held as a modifier: no tap */
    uint32_t gate_t;
    int8_t gate_dir;
    uint8_t gate_run;
    int8_t held_step;                     /* a step held (white key down on the step pages), -1 none */
    uint8_t held_key, held_was_on, held_used;   /* its key; on before the press; a knob or SELECT used on it */
    uint32_t last_ticks;
    uint8_t redraw;
    /* instrument tracks */
    int8_t pick_oct;                      /* independent of the track's playable octave */
    uint8_t pick_step, pick_clip;        /* index / original clip while PICK NOTES is open */
    uint8_t last_note[NTRK];              /* the note a new step gets */
    uint8_t key_note[NKEYS], key_trk[NKEYS];   /* a key down: the note it plays, on which track (0: none) */
    uint8_t key_mode[NKEYS];              /* 1: live-recorded (key_step), 2: step-recorded */
    uint8_t key_chord[NKEYS];             /* the chord mode it was pressed in (chord_of's spec): it lets go the same */
    int8_t key_step[NKEYS];
    uint32_t key_t0[NKEYS];               /* sq.clock at the press (its gate, live) */
    uint8_t cursor[NTRK];                 /* step recording's step, per track */
    uint8_t srec_held, srec_n;            /* step recording: keys down in this chord; its notes so far */
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void two_digits(int v, char *b)
{
    b[0] = (char)('0' + v / 10 % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
}

static void note_text(int n, char *b)     /* MIDI 60 = C4 */
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char *e;
    b[0] = 0;
    text_cat(b, NN[n % 12]);
    for (e = b; *e; e++) {
    }
    int_text(n / 12 - 1, e);
}

static void gate_text(int g, char *b)     /* eighths of a step, as steps: 6 -> 0.75 */
{
    char u[4];
    int_text(g / 8, b);
    text_cat(b, ".");
    two_digits(g % 8 * 125 / 10, u);
    text_cat(b, u);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static uint16_t trk_rgb(int tr) { return tr == TR_DRUM ? KIT_RGB[proj.kit] : TRK_RGB[tr]; }
static int home_page(int tr) { return tr == TR_DRUM ? PG_SEQ : PG_KEYS; }
static int npages(void) { return (*part_len(ui.trk) + 15) / 16; }

/* the drums' EDIT page knobs: the pad's parameters without the sends (no FX in this build) */
static int edit_params(int kit, int pad, int *idx)
{
    int i, n = 0, np = drums_nparams(kit, pad);
    for (i = 0; i < np && n < NPOTS; i++)
        idx[n++] = i;                     /* (the pads' Rev and Dly sends too, now that there are FX) */
    return n;
}

static int drum_epages(void)              /* the picked pad's pages of knobs, then the kit's */
{
    int idx[NPOTS], n = edit_params(drum_model(ui.pad), ui.pad, idx);
    return (n + 3) / 4 + 2;
}
static int drum_kit_page(void) { return ui.epage == drum_epages() - 2; }
static int drum_model_page(void) { return ui.epage == drum_epages() - 1; }

static void drum_model_turn(int pad, int32_t e)
{
    const int old = drum_model(pad), dir = e > 0 ? 1 : -1;
    int which = old;
    int count = e > KIT_NUM ? KIT_NUM : e < -KIT_NUM ? KIT_NUM : e * dir;
    for (; count > 0; count--) {
        int next = which;
        for (int tries = 0; tries < KIT_NUM; tries++) {
            next = (next + dir + KIT_NUM) % KIT_NUM;
            if (drums_has_pad(next, pad))
                break;
        }
        which = next;
    }
    if (which != old) {
        uint32_t f = plat_lock();
        for (int k = 0; k < NPOTS; k++)
            if (ovr_mask[pad] >> k & 1u)
                drums_set(old, pad, k, ovr_base[pad][k]);
        ovr_mask[pad] = 0;
        drums_cut_pad(pad);
        roll_inhibit_pads |= (uint16_t)(1u << pad);
        pad_hit[pad] = 0.0f;
        proj.model[pad] = which == proj.kit ? 0 : (uint8_t)(which + 1);
        if (proj.lfo[TR_DRUM].pad == pad && proj.lfo[TR_DRUM].dest != LFO_OFF && !proj.lfo[TR_DRUM].rsv[0])
            proj.lfo[TR_DRUM].rsv[0] = (uint8_t)(old + 1);
        plat_unlock(f);
        dirty = 1;
        ui.epage = (uint8_t)(drum_epages() - 1);
    }
    app_say(drums_pad_name(which, pad), drums_kit_name(which));
}

static void param_text(const x0x_param_t *p, int v, char *t)
{
    if (p->names && v >= 0 && v <= p->max) {
        t[0] = 0;
        text_cat(t, p->names[v]);
    } else {
        int_text(v, t);
    }
}

/* ------------------------------------------------------------ the drum track --- */
static void param_turn(int pad, int i, int32_t e)
{
    const x0x_param_t *p = drums_param(drum_model(pad), pad, i);
    int v;
    uint32_t f;
    char t[16];
    if (!p)
        return;
    f = plat_lock();                      /* a knob a lock holds: its own value is turned, and wins (as X0X) */
    v = pad < NPADS && i < NPOTS && (ovr_mask[pad] >> i & 1u) ? ovr_base[pad][i] : drums_get(drum_model(pad), pad, i);
    v = clamp(v + e, 0, p->max);
    if (pad < NPADS && i < NPOTS)
        ovr_mask[pad] &= (uint16_t)~(1u << i);
    drums_set(drum_model(pad), pad, i, v);
    plat_unlock(f);
    dirty = 1;
    if (ui.rec && sq.playing && pad < NPADS && i < NPOTS && !param_is_switch_sound(p) && sq.t[TR_DRUM].step >= 0) {
        int r;                            /* motion: on the step playing */
        undo_mark(4);
        f = plat_lock();
        r = lock_put(dcur(), sq.t[TR_DRUM].step, pad, lk_param(drum_model(pad), i), v);
        plat_unlock(f);
        if (r) {
            app_say("NO LOCK LEFT", "");
            return;
        }
    }
    param_text(p, drums_get(drum_model(pad), pad, i), t);
    app_say(p->name, t);
}

static void pad_pick(int pad)
{
    if (pad < 0 || pad >= NPADS)
        return;
    if (ui.pad != pad) {
        ui.pad = (uint8_t)pad;
        ui.epage = 0;
        ui.held_step = -1;                /* a held step belonged to the other pad */
    }
    ui.redraw = 1;
}

static void pad_play(int pad, uint32_t vel)   /* a pad played by hand: heard, sent, and recorded when armed */
{
    float v = vel >= 127u ? 1.0f : (float)vel / 127.0f;
    uint32_t f = plat_lock();
    if (!drum_lane_muted(pad)) {
        locks_apply(dcur(), -1, pad);     /* no step's locks; the drums' LFO, if it moves this pad */
        pad_fire(pad, v);
        if (pad == 0)
            fx_key(drum_model(0), v);
    }
    plat_unlock(f);
    plat_midi_send(0x09u | 0x99u << 8 | (uint32_t)PAD_GM[pad] << 16 | (vel ? vel : 1u) << 24, TO_USB);
    if (ui.rec && (sq.playing || (ck.count > 0 && ck.count <= (int32_t)(bar_samples() * 0.125f)))) {
        /* to the nearest point of the grid (hold REC, QUANTIZE); one ahead is left once by the sequencer; in the
         * count-in's last half beat, on step 1 */
        dpart_t *p = dcur();
        int st, target, rel;
        float fr;
        f = plat_lock();
        st = sq.t[TR_DRUM].step;
        fr = sq.t[TR_DRUM].frac;
        plat_unlock(f);
        if (st < 0 || !sq.playing) {
            st = 0;
            fr = 0.0f;
        }
        target = rec_target(st, fr, p->len);
        rel = (target - st + p->len) % p->len;
        if (!bit_get(p->hit[pad], target)) {
            undo_mark(0);
            f = plat_lock();
            bit_set(p->hit[pad], target);
            if (!sq.playing)
                ck.pre_pads |= (uint16_t)(1u << pad);
            else if (rel > 0 && rel <= p->len / 2)
                sq.skip_once[pad] = (int8_t)target;
            plat_unlock(f);
        }
    }
}

/* a white key on the drums' step pages, pressed. SEQ held: the step's accent. Otherwise the picked pad's step: an
 * empty one goes on at once; one that is on is held, and goes off at the release unless its chance or a lock was set
 * meanwhile (a step held + SELECT, + a knob on EDIT, + OCT-) */
static void step_down(int k)
{
    dpart_t *p = dcur();
    int s = ui.spage * 16 + k;
    uint32_t f;
    if (s >= p->len) {
        app_say("STEP", "PAST THE END");
        return;
    }
    if (ui.btn >> B_SEQ & 1u) {
        undo_mark(0);
        f = plat_lock();
        bit_flip(p->acc, s);
        plat_unlock(f);
        return;
    }
    if (ui.held_step >= 0) {              /* a second step while one is held: a plain toggle */
        undo_mark(0);
        f = plat_lock();
        bit_flip(p->hit[ui.pad], s);
        if (!bit_get(p->hit[ui.pad], s))
            lock_clear_step(p, s, ui.pad);
        plat_unlock(f);
        return;
    }
    ui.held_step = (int8_t)s;
    ui.held_key = (uint8_t)k;
    ui.held_used = 0;
    ui.held_was_on = (uint8_t)bit_get(p->hit[ui.pad], s);
    if (!ui.held_was_on) {
        undo_mark(0);
        f = plat_lock();
        bit_set(p->hit[ui.pad], s);
        plat_unlock(f);
    }
}

static void step_up(int k)
{
    dpart_t *p = dcur();
    int s = ui.held_step;
    uint32_t f;
    if (s < 0 || k != ui.held_key)
        return;
    ui.held_step = -1;
    if (ui.held_was_on && !ui.held_used && s < p->len && bit_get(p->hit[ui.pad], s)) {
        undo_mark(0);
        f = plat_lock();
        bit_flip(p->hit[ui.pad], s);
        lock_clear_step(p, s, ui.pad);
        plat_unlock(f);
    }
    ui.redraw = 1;
}

static void held_chance(int32_t e)        /* a step held + SELECT: its chance, 100 % (no lock) down to 5 % */
{
    dpart_t *p = dcur();
    int s = ui.held_step, i = lock_find(p, s, ui.pad, LK_PROB), c = i >= 0 ? p->lk[i].val : 100, r = 0;
    char t[12];
    uint32_t f;
    c = clamp(c + 5 * e, 5, 100);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (c >= 100) {
        if (i >= 0)
            lock_del(p, i);
    } else {
        r = lock_put(p, s, ui.pad, LK_PROB, c);
    }
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    int_text(c, t);
    text_cat(t, "%");
    app_say("CHANCE", t);
}

static void held_lock(int i, int32_t e)   /* EDIT, a step held + a knob: that knob locked for the step */
{
    dpart_t *p = dcur();
    const x0x_param_t *d = drums_param(drum_model(ui.pad), ui.pad, i);
    int s = ui.held_step, pr = lk_param(drum_model(ui.pad), i), j = lock_find(p, s, ui.pad, pr), v, r;
    char t[16];
    uint32_t f;
    if (!d || param_is_switch_sound(d) || i >= NPOTS) {
        app_say("NOT LOCKABLE", "");
        return;
    }
    if (j >= 0)
        v = p->lk[j].val;
    else
        v = (ovr_mask[ui.pad] >> i & 1u) ? ovr_base[ui.pad][i] : drums_get(drum_model(ui.pad), ui.pad, i);
    v = clamp(v + e, 0, d->max);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    r = lock_put(p, s, ui.pad, pr, v);
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    param_text(d, v, t);
    app_say(d->name, t);
}

static void held_clear(void)              /* a step held + OCT-: its chance and locks gone */
{
    dpart_t *p = dcur();
    uint32_t f;
    int n;
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    n = lock_clear_step(p, ui.held_step, ui.pad);
    plat_unlock(f);
    app_say(n ? "LOCKS CLEARED" : "NO LOCKS", "");
}

static void mute_toggle(int pad)
{
    dpart_t *p;
    uint32_t f;
    int muted;
    undo_mark(0);
    f = plat_lock();
    p = dcur();
    p->mute ^= (uint16_t)(1u << pad);
    muted = (int)(p->mute >> pad & 1u);
    sync_drum_mutes();
    plat_unlock(f);
    app_say(drums_pad_name(drum_model(pad), pad), muted ? "MUTED" : "ON");
}

/* ------------------------------------------------------ the instrument tracks --- */
static int track_note(int tr)             /* the note a new step gets: the last one played, or C of the octave */
{
    return ui.last_note[tr] ? ui.last_note[tr] : 60 + 12 * tset(tr)->oct;
}

static void step_stamp(int tr, nstep_t *s)
{
    const track_t *t = tset(tr);
    int i, note[NCHORD], n = chord_of(t->chord | t->chord_inv << 4, track_note(tr), note);
    for (i = 0; i < n; i++)
        nstep_add(s, note[i], 100);
}

static void preview(int tr, const nstep_t *s)   /* a step heard by itself (stopped): its notes for one step */
{
    int i, mono = tset(tr)->type == TY_MONO;
    uint32_t f;
    int32_t gate;
    if (sq.playing || !s->note[0] || track_is_muted(tr))
        return;
    gate = (int32_t)((float)s->gate * 0.125f * base_step(tr));
    f = plat_lock();
    for (i = 0; i < NCHORD && s->note[i]; i++)
        syn_note_on_f(tr, s->note[i], (float)ns_vel(s) / 127.0f, gate, mono, (s->vel & NS_ACC) ? SN_ACCENT : 0);
    lfo_note(tr);
    plat_unlock(f);
}

static void live_rec_on(int key, int tr, int note, int vel)   /* REC while playing: to the nearest step */
{
    ipart_t *p = ipart(tr);
    int st, target, r;
    float fr;
    uint32_t f = plat_lock();
    st = sq.t[tr].step;
    fr = sq.t[tr].frac;
    plat_unlock(f);
    if (st < 0 || !sq.playing) {          /* (the count-in: step 1) */
        st = 0;
        fr = 0.0f;
    }
    target = rec_target(st, fr, p->len);
    undo_mark(3);
    f = plat_lock();
    r = nstep_add(&p->st[target], note, vel);
    {
        int rel = (target - st + p->len) % p->len;
        if (!r && rel > 0 && rel <= p->len / 2)
            sq.iskip[tr] = (int8_t)target;    /* ahead, heard now: the sequencer leaves it once */
    }
    plat_unlock(f);
    if (!r) {
        ui.key_mode[key] = 1;
        ui.key_step[key] = (int8_t)target;
        ui.key_t0[key] = sq.clock;
    }
}

static void live_rec_off(int key, int tr)  /* the key let go: the step's gate is how long it was held */
{
    ipart_t *p = ipart(tr);
    int s = ui.key_step[key], g;
    uint32_t held = sq.clock - ui.key_t0[key], f;
    if (s < 0 || s >= NSTEPS)
        return;
    g = clamp((int32_t)((float)held / base_step(tr) * 8.0f + 0.5f), 1, GATE_MAX);
    f = plat_lock();
    if (p->st[s].note[0])
        p->st[s].gate = (uint16_t)g;
    plat_unlock(f);
}

static void step_rec_on(int tr, int note, int vel)   /* REC while stopped: the chord into the cursor's step */
{
    ipart_t *p = ipart(tr);
    int s = ui.cursor[tr] < p->len ? ui.cursor[tr] : 0;
    uint32_t f;
    if (!ui.srec_n)
        undo_mark(0);
    f = plat_lock();
    if (!ui.srec_n)
        nstep_clear(&p->st[s]);
    nstep_add(&p->st[s], note, vel);
    plat_unlock(f);
    ui.cursor[tr] = (uint8_t)s;
    ui.srec_n++;
    ui.srec_held++;
}

static void step_rec_off(int tr)          /* every key of the chord let go: the cursor moves on */
{
    if (!ui.srec_held)
        return;
    if (--ui.srec_held == 0) {
        ui.cursor[tr] = (uint8_t)((ui.cursor[tr] + 1) % *part_len(tr));
        ui.srec_n = 0;
        ui.spage = (uint8_t)(ui.cursor[tr] / 16);
    }
}

/* QUANTIZE (hold REC + OCT-): every note (or hit) of the track's part in this pattern to the nearest point of the
 * grid, round to the start after the last; notes meeting on a step become a chord (the loudest velocity, the longest
 * gate); locks go with their steps. LEGATO (hold REC + OCT+, T2-T4): each step's gate to the next note's start (and
 * a little past, so a MONO sound with LEGATO ties them), the last to the first's, round the loop (at most 16 steps).
 * Both undoable (EDIT + OCT-) */
static nstep_t q_old[NSTEPS];

static int q_to(int s, int g, int len)
{
    int t = (s + g / 2) / g * g;
    return t >= len ? 0 : t;
}

static void quantize(void)
{
    int tr = ui.trk, g = recq_steps(), len = *part_len(tr), s, i, j, moved = 0;
    char t[16];
    uint32_t f;
    if (g == 1) {
        app_say("QUANTIZE", "1 STEP: ALREADY");
        return;
    }
    undo_mark(0);
    f = plat_lock();
    if (tr == TR_DRUM) {
        dpart_t *p = dcur();
        int pad;
        for (pad = 0; pad <= NPADS; pad++) {
            uint32_t *bits = pad < NPADS ? p->hit[pad] : p->acc, old[STEP_WORDS];
            int word;
            for (word = 0; word < STEP_WORDS; word++) old[word] = bits[word];
            for (s = 0; s < len; s++)
                if (bit_get(old, s))
                    bits[s >> 5] &= ~(1u << (s & 31));
            for (s = 0; s < len; s++)
                if (bit_get(old, s)) {
                    bit_set(bits, q_to(s, g, len));
                    moved += pad < NPADS && q_to(s, g, len) != s;
                }
        }
        for (i = 0; i < p->nlk; i++)
            p->lk[i].step = (uint8_t)q_to(p->lk[i].step, g, len);
        for (i = 0; i < p->nlk; i++)      /* two locks of a knob now on one step: the first stays */
            for (j = i + 1; j < p->nlk; j++)
                if (p->lk[j].step == p->lk[i].step && p->lk[j].pad == p->lk[i].pad && p->lk[j].param == p->lk[i].param) {
                    p->lk[j] = p->lk[--p->nlk];
                    j--;
                }
    } else {
        ipart_t *p = ipart(tr);
        for (s = 0; s < len; s++) {
            q_old[s] = p->st[s];
            nstep_clear(&p->st[s]);
        }
        for (s = 0; s < len; s++) {
            nstep_t *o = &q_old[s], *d;
            if (!o->note[0])
                continue;
            d = &p->st[q_to(s, g, len)];
            moved += q_to(s, g, len) != s;
            if (!d->note[0]) {
                *d = *o;
                continue;
            }
            for (i = 0; i < NCHORD && o->note[i]; i++)
                nstep_add(d, o->note[i], ns_vel(o));
            if (ns_vel(o) > ns_vel(d))
                d->vel = (uint8_t)((d->vel & NS_ACC) | (uint32_t)ns_vel(o));
            d->vel |= (uint8_t)(o->vel & NS_ACC);
            if (o->gate > d->gate)
                d->gate = o->gate;
            if (ns_prob(o) > ns_prob(d))
                d->prob = (uint8_t)((d->prob & NS_SLIDE) | (uint32_t)ns_prob(o));
            d->prob |= (uint8_t)(o->prob & NS_SLIDE);
        }
        for (i = 0; i < p->nlk; i++)
            p->lk[i].step = (uint8_t)q_to(p->lk[i].step, g, len);
        for (i = 0; i < p->nlk; i++)
            for (j = i + 1; j < p->nlk; j++)
                if (p->lk[j].step == p->lk[i].step && p->lk[j].param == p->lk[i].param) {
                    p->lk[j] = p->lk[--p->nlk];
                    j--;
                }
    }
    plat_unlock(f);
    int_text(moved, t);
    text_cat(t, " MOVED");
    app_say("QUANTIZED", t);
}

static void legato(void)
{
    int tr = ui.trk, len = *part_len(tr), s, first = -1, prev = -1, n = 0;
    ipart_t *p;
    char t[16];
    uint32_t f;
    if (tr == TR_DRUM) {
        app_say("LEGATO", "ON T2-T4");
        return;
    }
    p = ipart(tr);
    undo_mark(0);
    f = plat_lock();
    for (s = 0; s < len; s++)
        if (p->st[s].note[0]) {
            if (prev >= 0)
                p->st[prev].gate = (uint16_t)clamp((s - prev) * 8 + 1, 1, GATE_MAX);
            else
                first = s;
            prev = s;
            n++;
        }
    if (prev >= 0)                        /* the last: round the loop to the first */
        p->st[prev].gate = (uint16_t)clamp((first + len - prev) * 8 + 1, 1, GATE_MAX);
    plat_unlock(f);
    int_text(n, t);
    text_cat(t, n == 1 ? " NOTE" : " NOTES");
    app_say("LEGATO", n ? t : "NO NOTES");
}

/* REC held + GLO: the selected track's part of the pattern it plays (or, stopped, would play) emptied, to record it
 * again: its notes or hits, accents, chance and locks go; its LENGTH, RATE, pad mutes and ACID settings stay. Recording
 * stays armed or not as it was, the transport runs on, and the step recording's cursor goes back to step 1. Undo (EDIT +
 * OCT-) brings it back */
static void part_erase(void)
{
    int tr = ui.trk, n = clip_of(tr), s;
    char t[16];
    uint32_t f;
    undo_mark(0);                         /* (the pattern clip_of(tr): edit_pat's) */
    f = plat_lock();
    if (tr == TR_DRUM) {
        dpart_t *p = &proj.p[n].d;
        int word;
        for (s = 0; s < NPADS; s++) {
            for (word = 0; word < STEP_WORDS; word++) p->hit[s][word] = 0;
            sq.skip_once[s] = -1;
        }
        for (word = 0; word < STEP_WORDS; word++) p->acc[word] = 0;
        p->nlk = 0;
        locks_release();                  /* a knob a lock held: its own value again */
        ck.pre_pads = 0;
    } else {
        ipart_t *p = &proj.p[n].t[tr - 1], none;
        for (s = 0; s < NSTEPS; s++)
            nstep_clear(&p->st[s]);
        p->nlk = 0;
        none.nlk = 0;
        ilocks_apply(tr, &none, -1);      /* a knob a lock held: its own value again */
        sq.iskip[tr] = -1;
        ck.pre &= (uint8_t)~(1u << tr);
        ui.cursor[tr] = 0;
        ui.srec_held = ui.srec_n = 0;
    }
    plat_unlock(f);
    ui.held_step = -1;
    {
        char u[4];
        u[0] = 'P';                       /* (no initialiser: the device links no memcpy) */
        two_digits(n + 1, u + 1);
        t[0] = 0;
        text_cat(t, TRK_NAME[tr]);
        text_cat(t, " ");
        text_cat(t, u);
    }
    app_say("ERASED", t);
}

static void note_down(int key)            /* a key on the KEYS (or EDIT) page of an instrument track */
{
    int tr = ui.trk, note, vel;
    const track_t *ts = tset(tr);
    uint32_t f;
    if (ts->type == TY_SAMPLER && !smp_frames[tr]) {
        app_say("NO SAMPLE", "HOLD REC, PRESS SEL");
        return;
    }
    int cn[4], nc, i;
    note = KEY_NOTE0 + key + 12 * ts->oct;
    vel = touch_vel((uint32_t)key);
    if (!vel)
        vel = 100;
    ui.key_chord[key] = (uint8_t)(ts->chord | ts->chord_inv << 4);
    nc = chord_of(ui.key_chord[key], note, cn);
    f = plat_lock();
    if (!track_is_muted(tr)) {
        for (i = 0; i < nc; i++) {
            if (ts->arp_mode)
                arp_key(tr, cn[i], vel, 1);
            else
                syn_note_on(tr, cn[i], (float)vel / 127.0f, -1, ts->type == TY_MONO);
        }
        if (!ts->arp_mode)
            lfo_note(tr);
    }
    plat_unlock(f);
    for (i = 0; i < nc; i++)
        plat_midi_send(0x09u | (0x90u | (uint32_t)tr) << 8 | (uint32_t)cn[i] << 16 | (uint32_t)vel << 24, TO_USB);
    ui.key_note[key] = (uint8_t)note;
    ui.key_trk[key] = (uint8_t)tr;
    ui.key_mode[key] = 0;
    ui.key_step[key] = -1;
    ui.last_note[tr] = (uint8_t)note;
    if (ui.rec && sq.playing) {
        for (i = 0; i < nc; i++)
            live_rec_on(key, tr, cn[i], vel);
    } else if (ck.count > 0) {            /* the count-in: only its last half beat records (on step 1) */
        if (ui.rec && ck.count <= (int32_t)(bar_samples() * 0.125f)) {
            for (i = 0; i < nc; i++)
                live_rec_on(key, tr, cn[i], vel);
            ck.pre |= (uint8_t)(1u << tr);
        }
    } else if (ui.rec && ui.page == PG_KEYS) {
        step_rec_on(tr, cn[0], vel);
        if (nc > 1) {                     /* the chord's other notes, into the same step */
            ipart_t *p = ipart(tr);
            uint32_t g = plat_lock();
            for (i = 1; i < nc; i++)
                nstep_add(&p->st[ui.cursor[tr]], cn[i], vel);
            plat_unlock(g);
        }
        ui.key_mode[key] = 2;
    }
}

static void note_up(int key)
{
    int note = ui.key_note[key], tr = ui.key_trk[key], cn[4], nc, i;
    uint32_t f;
    if (!note)
        return;
    nc = chord_of(ui.key_chord[key], note, cn);
    f = plat_lock();
    for (i = 0; i < nc; i++) {
        if (tset(tr)->arp_mode)
            arp_key(tr, cn[i], 0, 0);
        else
            syn_note_off(tr, cn[i]);
    }
    plat_unlock(f);
    for (i = 0; i < nc; i++)
        plat_midi_send(0x08u | (0x80u | (uint32_t)tr) << 8 | (uint32_t)cn[i] << 16, TO_USB);
    if (ui.key_mode[key] == 1)
        live_rec_off(key, tr);
    else if (ui.key_mode[key] == 2)
        step_rec_off(tr);
    ui.key_note[key] = 0;
    ui.key_mode[key] = 0;
}

static void keys_let_go(void)             /* every note the keys hold, let go (a track change) */
{
    int k;
    for (k = 0; k < NKEYS; k++)
        note_up(k);
    ui.srec_held = ui.srec_n = 0;
}

/* a white key on an instrument track's step page: as the drums' (an empty step goes on at once with the last note
 * played; one that is on is held, and goes off at the release unless it was edited meanwhile) */
static void istep_down(int k)
{
    int tr = ui.trk;
    ipart_t *p = ipart(tr);
    int s = ui.spage * 16 + k;
    uint32_t f;
    if (s >= p->len) {
        app_say("STEP", "PAST THE END");
        return;
    }
    if (ui.held_step >= 0) {              /* a second step while one is held: a plain toggle */
        undo_mark(0);
        f = plat_lock();
        if (p->st[s].note[0]) {
            nstep_clear(&p->st[s]);
            ilock_clear_step(p, s);
        } else {
            step_stamp(tr, &p->st[s]);
        }
        plat_unlock(f);
        return;
    }
    ui.held_step = (int8_t)s;
    ui.held_key = (uint8_t)k;
    ui.held_used = 0;
    ui.held_was_on = (uint8_t)(p->st[s].note[0] != 0);
    if (!ui.held_was_on) {
        undo_mark(0);
        f = plat_lock();
        step_stamp(tr, &p->st[s]);
        plat_unlock(f);
    }
    preview(tr, &p->st[s]);
}

static void pick_leave(void)
{
    ui.page = PG_SEQ;
    ui.redraw = 1;
}

static void pick_open(void)
{
    ui.pick_step = (uint8_t)ui.held_step;
    ui.pick_clip = (uint8_t)clip_of(ui.trk);
    ui.pick_oct = tset(ui.trk)->oct;
    ui.held_step = -1;                   /* releasing the trigger key must not clear the step */
    ui.page = PG_PICK;
    ui.msg_until = 0;                    /* make the mode and target step obvious immediately */
}

static int pick_pitch(int key)
{
    return KEY_NOTE0 + key + 12 * ui.pick_oct;
}

static int pick_is_selected(const nstep_t *s, int pitch)
{
    int i;
    for (i = 0; i < NCHORD && s->note[i]; i++)
        if (s->note[i] == pitch)
            return 1;
    return 0;
}

static void pick_toggle(int key)
{
    int tr = ui.trk, step = ui.pick_step, pitch = pick_pitch(key), i, count;
    ipart_t *p = &proj.p[ui.pick_clip].t[tr - 1];
    nstep_t *s = &p->st[step];
    uint32_t f;
    if (clip_of(tr) != ui.pick_clip || step >= p->len) {
        pick_leave();
        return;
    }
    count = nstep_count(s);
    for (i = 0; i < count && s->note[i] != pitch; i++) {}
    if (i == NCHORD) {
        app_say("4 NOTES MAX", "TAP A LIT NOTE TO REMOVE");
        return;
    }
    undo_mark(0);
    f = plat_lock();
    if (i == count) {
        nstep_add(s, pitch, 100);
    } else {
        for (; i < count - 1; i++)
            s->note[i] = s->note[i + 1];
        s->note[count - 1] = 0;
    }
    plat_unlock(f);
    preview(tr, s);                      /* no audition while transport plays */
    ui.redraw = 1;
}

static void istep_up(int k)
{
    ipart_t *p = ipart(ui.trk);
    int s = ui.held_step;
    uint32_t f;
    if (s < 0 || k != ui.held_key)
        return;
    ui.held_step = -1;
    if (ui.held_was_on && !ui.held_used && s < p->len && p->st[s].note[0]) {
        undo_mark(0);
        f = plat_lock();
        nstep_clear(&p->st[s]);
        ilock_clear_step(p, s);
        plat_unlock(f);
    }
    ui.redraw = 1;
}

/* Fine detents always edit one eighth of a step. A burst snaps to
 * musical bar boundaries for this track division, so eight bars don't
 * take 1024 detents. Reset on a new step or on a different held knob. */
static int gate_edit(int gate, int rate, int32_t e)
{
    static const int per_bar[NRATES] = {128, 64, 256, 192, 96};
    uint32_t now = plat_ms();
    int dir = e < 0 ? -1 : 1, moves = clamp(e < 0 ? -e : e, 1, 8);
    if (ui.gate_dir != dir || now - ui.gate_t > 85u) ui.gate_run = 0;
    ui.gate_t = now;
    ui.gate_dir = (int8_t)dir;
    ui.gate_run = (uint8_t)clamp(ui.gate_run + moves, 0, 3);
    if (ui.gate_run < 3) return clamp(gate + e, 1, GATE_MAX);
    {
        int bar = per_bar[rate];
        int snap = dir > 0 ? (gate / bar + moves) * bar : ((gate - 1) / bar - moves + 1) * bar;
        return clamp(snap, 1, GATE_MAX);
    }
}

static void iheld_knob(int k, int32_t e)  /* a step held: K1 its notes (the chord moves), K2 velocity, K3 gate */
{
    int tr = ui.trk, i, lo = 127, hi = 0;
    nstep_t *s = &ipart(tr)->st[ui.held_step];
    char t[16];
    uint32_t f;
    if (!s->note[0] || k > 2)
        return;
    if (!ui.held_used)
        undo_mark(0);
    if (!ui.held_used || k != 2) ui.gate_run = 0;
    ui.held_used = 1;
    f = plat_lock();
    if (k == 0) {
        for (i = 0; i < NCHORD && s->note[i]; i++) {
            if (s->note[i] < lo) lo = s->note[i];
            if (s->note[i] > hi) hi = s->note[i];
        }
        e = clamp(e, 1 - lo, 127 - hi);
        for (i = 0; i < NCHORD && s->note[i]; i++)
            s->note[i] = (uint8_t)(s->note[i] + e);
    } else if (k == 1) {                  /* (past 127: ACCENT, a 303's) */
        int v = clamp(ns_vel(s) + ((s->vel & NS_ACC) ? 1 : 0) + e, 1, 128);
        s->vel = (uint8_t)(v > 127 ? 127u | NS_ACC : (uint32_t)v);
    } else {
        s->gate = (uint16_t)gate_edit(s->gate, ipart(tr)->rate, e);
    }
    plat_unlock(f);
    if (k == 0) {
        ui.last_note[tr] = s->note[0];
        note_text(s->note[0], t);
        app_say("NOTE", t);
        preview(tr, s);
    } else if (k == 1) {
        int_text(ns_vel(s), t);
        app_say("VELOCITY", (s->vel & NS_ACC) ? "127 ACCENT" : t);
    } else {
        gate_text(s->gate, t);
        app_say("GATE", t);
    }
}

static void iheld_chance(int32_t e)       /* a step held + SELECT: its chance, 100 % down to 5 % */
{
    nstep_t *s = &ipart(ui.trk)->st[ui.held_step];
    char t[12];
    uint32_t f;
    if (!s->note[0])
        return;
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    s->prob = (uint8_t)((s->prob & NS_SLIDE) | (uint32_t)clamp(ns_prob(s) + 5 * e, 5, 100));
    plat_unlock(f);
    int_text(ns_prob(s), t);
    text_cat(t, "%");
    app_say("CHANCE", t);
}

static int whole_gate(const ipart_t *p, int s)   /* a WHOLE note at step s: held to just before the next note */
{
    int k = 1;
    while (s + k < p->len && !p->st[s + k].note[0])
        k++;
    return 8 * k - 1 > GATE_MAX ? GATE_MAX : 8 * k - 1;
}

static void iheld_len(void)               /* a step held + OCT+: its length as a 303's: HALF, WHOLE, SLIDE, round */
{
    static const char *const N[3] = {"HALF", "WHOLE", "SLIDE"};
    ipart_t *p = ipart(ui.trk);
    nstep_t *s = &p->st[ui.held_step];
    int m;
    uint32_t f;
    if (!s->note[0])
        return;
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    m = ((s->prob & NS_SLIDE) ? 2 : s->gate >= 7 ? 1 : 0) + 1;
    f = plat_lock();
    s->prob &= (uint8_t)~NS_SLIDE;
    if (m == 3 || m == 0) {
        s->gate = GATE_HALF;
        m = 0;
    } else if (m == 1) {
        s->gate = (uint16_t)whole_gate(p, ui.held_step);
    } else {
        s->gate = GATE_SLIDE;
        s->prob |= NS_SLIDE;
    }
    plat_unlock(f);
    app_say("LENGTH", N[m]);
}

static void iheld_clear(void)             /* a step held + OCT-: its chance back to 100 %, its locks gone */
{
    ipart_t *p = ipart(ui.trk);
    nstep_t *s = &p->st[ui.held_step];
    uint32_t f;
    int n;
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (s->note[0])
        s->prob = (uint8_t)((s->prob & NS_SLIDE) | 100u);
    n = ilock_clear_step(p, ui.held_step);
    plat_unlock(f);
    app_say(n ? "LOCKS CLEARED" : "CHANCE", n ? "CHANCE 100%" : "100%");
}

static void ilock_turn(int id, int32_t e)   /* a step held, a sound knob on EDIT or ENV: its value for that step */
{
    int tr = ui.trk, s = ui.held_step, j, v, r;
    ipart_t *p = ipart(tr);
    const syn_param_t *d = syn_param(id);
    char t[16];
    uint32_t f;
    if (!d || id == SP_VOICES || id == SP_LEGATO || (id >= SP_XM && (id - SP_XM) % 3 != 2) || !p->st[s].note[0]) {
        app_say("NOT LOCKABLE", "");      /* (the matrix's routing: only its amounts lock) */
        return;
    }
    j = ilock_find(p, s, id);
    v = clamp((j >= 0 ? p->lk[j].val : tset(tr)->snd[id]) + e, d->min, d->max);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    r = ilock_put(p, s, id, v);
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    syn_param_text(id, v, t);
    app_say(d->name, t);
}

static int inst_epages(int tr) { return tset(tr)->type == TY_SAMPLER ? NSPAGE : NEPAGE; }
static int epage_of(int tr) { return ui.epage < inst_epages(tr) ? ui.epage : 0; }
static const int8_t *epage_p(int tr) { return tset(tr)->type == TY_SAMPLER ? SPAGE_P[epage_of(tr)] : EPAGE_P[epage_of(tr)]; }
static const char *epage_name(int tr)
{
    return tset(tr)->type == TY_SAMPLER ? SPAGE_NAME[epage_of(tr)] : EPAGE_NAME[epage_of(tr)];
}

static void seconds_text(uint32_t ms, char *t)   /* 1.23 S */
{
    char u[4];
    int_text((int32_t)(ms / 1000u), t);
    text_cat(t, ".");
    two_digits((int)(ms % 1000u / 10u), u);
    text_cat(t, u);
    text_cat(t, " S");
}

static const char *smp_label(int id)
{
    switch (id) {
    case K_START: return "START";
    case K_END: return "END";
    case K_LOOP: return "LOOP";
    case K_LSTART: return "LOOP START";
    case K_ROOT: return "ROOT";
    case K_TUNE: return "TUNE";
    default: return "RECORD AT";
    }
}

static int smp_value(int tr, int id, int *max)   /* a SAMPLER knob's value and its top */
{
    const track_t *ts = tset(tr);
    *max = 1000;
    switch (id) {
    case K_START: return ts->s_start;
    case K_END: return ts->s_end;
    case K_LSTART: return ts->s_lstart;
    case K_LOOP: *max = 1; return ts->s_loop;
    case K_ROOT: *max = 127; return ts->s_root;
    case K_TUNE: *max = 100; return ts->s_tune + 50;
    default: *max = 1; return ts->s_q;
    }
}

static void smp_text(int tr, int id, char *t)
{
    const track_t *ts = tset(tr);
    int max, v = smp_value(tr, id, &max);
    t[0] = 0;
    if (id == K_START || id == K_END || id == K_LSTART) {
        if (smp_frames[tr] && smp_rate[tr]) {   /* as a time in the sample */
            seconds_text(smp_frames[tr] * (uint32_t)v / smp_rate[tr], t);   /* (at most 132300 x 1000) */
        } else {
            int_text(v / 10, t);
            text_cat(t, "%");
        }
    } else if (id == K_LOOP) {
        text_cat(t, v ? "ON" : "OFF");
    } else if (id == K_ROOT) {
        note_text(v, t);
    } else if (id == K_TUNE) {
        if (ts->s_tune > 0)
            text_cat(t, "+");
        int_text(ts->s_tune, t + (ts->s_tune > 0));
        text_cat(t, " CT");
    } else {
        text_cat(t, v ? "22 KHZ" : "44 KHZ");
    }
}

static void smp_turn(int id, int32_t e)   /* a SAMPLER knob (positions move 0.5 % a detent) */
{
    int tr = ui.trk;
    track_t *ts = tset(tr);
    char t[16];
    switch (id) {
    case K_START: ts->s_start = (uint16_t)clamp(ts->s_start + e * 5, 0, 1000); break;
    case K_END: ts->s_end = (uint16_t)clamp(ts->s_end + e * 5, 0, 1000); break;
    case K_LSTART: ts->s_lstart = (uint16_t)clamp(ts->s_lstart + e * 5, 0, 1000); break;
    case K_LOOP: ts->s_loop = (uint8_t)clamp(ts->s_loop + e, 0, 1); break;
    case K_ROOT: ts->s_root = (uint8_t)clamp(ts->s_root + e, 0, 127); break;
    case K_TUNE: ts->s_tune = (int8_t)clamp(ts->s_tune + e, -50, 50); break;
    default: ts->s_q = (uint8_t)clamp(ts->s_q + e, 0, 1); break;
    }
    track_shape(tr);
    dirty = 1;
    smp_text(tr, id, t);
    app_say(smp_label(id), t);
}

static int snd_max(int tr, int id)        /* on a MONO track VOICES is the unison stack: 4 at most */
{
    return id == SP_VOICES && tset(tr)->type == TY_MONO ? 4 : syn_param(id)->max;
}

static const char *snd_label(int tr, int id)
{
    return id == SP_VOICES && tset(tr)->type == TY_MONO ? "UNISON" : syn_param(id)->name;
}

static int snd_value(int tr, int id) { return clamp(tset(tr)->snd[id], syn_param(id)->min, snd_max(tr, id)); }

static void snd_turn(int id, int32_t e)   /* a sound's knob, on the EDIT or ENV pages */
{
    int tr = ui.trk, v = clamp(snd_value(tr, id) + e, syn_param(id)->min, snd_max(tr, id));
    track_t *ts = tset(tr);
    char t[16];
    uint32_t f;
    if (v != ts->snd[id]) {
        ts->snd[id] = (int8_t)v;
        f = plat_lock();
        ilk_mask[tr] &= ~((uint64_t)1 << id);   /* turned while a lock holds it: the knob's value wins (as the drums') */
        syn_set(tr, id, v);
        plat_unlock(f);
        ts->edited = 1;
        dirty = 1;
        if (ui.rec && sq.playing && sq.t[tr].step >= 0) {   /* motion: on the step playing */
            int r;
            undo_mark(4);
            f = plat_lock();
            r = ilock_put(ipart(tr), sq.t[tr].step, id, v);
            plat_unlock(f);
            if (r) {
                app_say("NO LOCK LEFT", "16 A TRACK");
                return;
            }
        }
    }
    syn_param_text(id, v, t);
    app_say(snd_label(tr, id), t);
}

/* the presets: an instrument track's (original factory sounds, USER 1..16, then expansion) and the drums' (909, 808, USER KIT 1..8) */
static int npresets(void) { return syn_presets() + NUSER; }
static int preset_ok(int i)
{
    int s = user_preset_slot(i);
    return i >= 0 && i < npresets() && (s < 0 || user.snd[s].used);
}
static void preset_name(int i, char *t)
{
    char u[4];
    int s = user_preset_slot(i);
    t[0] = 0;
    if (s < 0) {
        text_cat(t, syn_preset_name(factory_preset_index(i)));
        return;
    }
    text_cat(t, "USER ");
    int_text(s + 1, u);
    text_cat(t, u);
}
static int dpreset_ok(int display)
{
    int i = dpreset_id(display);
    return dpreset_factory(i) >= 0 || (dpreset_user(i) && user.kit[i - 2].used && user.kit[i - 2].kit < KIT_NUM);
}
static void dpreset_name(int i, char *t)
{
    char u[4];
    t[0] = 0;
    if (dpreset_factory(i) >= 0) {
        text_cat(t, drums_kit_name(dpreset_factory(i)));
        return;
    }
    text_cat(t, "USER KIT ");
    int_text(i - 1, u);
    text_cat(t, u);
}

static int preset_step(int i, int32_t e, int n, int (*ok)(int))   /* e presets on from i, past the empty slots */
{
    int dir = e > 0 ? 1 : -1, k = e > 0 ? e : -e, j;
    while (k--) {
        for (j = i + dir; j >= 0 && j < n && !ok(j); j += dir) {
        }
        if (j < 0 || j >= n)
            break;
        i = j;
    }
    return i;
}

static void preset_turn(int32_t e)        /* PRESETS on an instrument track: its sound (and MONO / POLY) */
{
    int tr = ui.trk, i = preset_step(tset(tr)->preset, e, npresets(), preset_ok), k, mono;
    track_t *ts = tset(tr);
    char t[16];
    uint32_t f;
    int s = user_preset_slot(i);
    if (s < 0) {
        mono = syn_preset(factory_preset_index(i), ts->snd);
    } else {
        const usound_t *u = &user.snd[s];
        for (k = 0; k < SP_NUM; k++)
            ts->snd[k] = u->v[k];
        mono = u->mono;
    }
    ts->preset = (uint8_t)i;
    ts->edited = 0;
    ts->type = (uint8_t)(mono ? TY_MONO : TY_POLY);
    f = plat_lock();
    syn_release(tr, 0);
    syn_source(tr, 0);
    syn_sound(tr, ts->snd);
    plat_unlock(f);
    dirty = 1;
    preset_name(i, t);
    app_say(t, TYPE_NAME[ts->type]);
}

static void kit_turn(int32_t e)
{
    const int from = dpreset_display(proj.dpreset);
    const int i = dpreset_id(preset_step(from, e, DPRESET_COUNT, dpreset_ok));
    char t[16];
    uint32_t f = plat_lock();
    locks_release();
    if (dpreset_user(i))
        pots_from_engines();
    kit_raw(i, 0, 1);
    plat_unlock(f);
    dirty = 1;
    ui.epage = 0;
    dpreset_name(i, t);
    app_say(t, dpreset_user(i) ? drum_kit_title() : "");
}

static void save_target(int32_t e)        /* SAVE held + PRESETS: the user slot that letting go of SAVE saves to */
{
    int inst = ui.trk != TR_DRUM, n = inst ? NUSER : NUKIT, s = ui.save_slot, k, used;
    char t[16], u[4];
    int current = inst ? user_preset_slot(tset(ui.trk)->preset) : -1;
    if (s < 0) {                          /* the first turn: where the sound came from, else the first empty slot */
        if (current >= 0)
            s = current;
        else if (!inst && dpreset_user(proj.dpreset))
            s = proj.dpreset - 2;
        else
            for (s = 0, k = n - 1; k >= 0; k--)
                if (!(inst ? user.snd[k].used : user.kit[k].used))
                    s = k;
    } else {
        s = clamp(s + e, 0, n - 1);
    }
    ui.save_slot = (int8_t)s;
    used = inst ? user.snd[s].used : user.kit[s].used;
    t[0] = 0;
    text_cat(t, inst ? "SAVE AS USER " : "SAVE AS KIT ");
    int_text(s + 1, u);
    text_cat(t, u);
    app_say(t, used ? "REPLACE" : "EMPTY");
}

static void save_user(void)               /* SAVE let go after choosing a slot: the sound (or the kit) kept there */
{
    int s = ui.save_slot, k, r, c;
    char t[16];
    uint32_t f;
    ui.save_slot = -1;
    if (sq.playing) {
        app_say("STOP FIRST", "TO SAVE A PRESET");
        return;
    }
    if (ui.trk != TR_DRUM) {
        track_t *ts = tset(ui.trk);
        usound_t *u = &user.snd[s];
        u->used = 1;
        u->mono = (uint8_t)(ts->type == TY_MONO);
        for (k = 0; k < SP_NUM; k++)
            u->v[k] = ts->snd[k];
        ts->preset = (uint8_t)(PRESET_USER_FIRST + s);
        ts->edited = 0;
        preset_name(ts->preset, t);
    } else {
        ukit_t *u = &user.kit[s];
        f = plat_lock();
        locks_release();
        plat_unlock(f);
        pots_from_engines();
        u->used = 1;
        u->kit = proj.kit;
        for (r = 0; r <= NPADS; r++) {
            if (r < NPADS)
                user.model[s][r] = proj.model[r];
            for (c = 0; c < NPOTS; c++)
                u->pots[r][c] = kit_pots(drum_model(r))[r][c];
        }
        proj.dpreset = (uint8_t)(2 + s);
        dpreset_name(proj.dpreset, t);
    }
    dirty = 1;
    app_say(user_save() ? "SAVE FAILED" : "SAVED AS", t);
}

static void ienv_knob(int k, int32_t e)   /* an instrument track's ENV pages: A D S R of the amp, the filter, MOD ENV */
{
    if (ui.held_step >= 0)
        ilock_turn(ENVPAGE_P[ui.envpage % NENVPAGE][k], e);
    else
        snd_turn(ENVPAGE_P[ui.envpage % NENVPAGE][k], e);
}

static void iedit_knob(int k, int32_t e)  /* an instrument track's EDIT pages: VOICE .. MORE, OSC 2, SHAPER, MACRO */
{
    int tr = ui.trk, id;
    track_t *ts = tset(tr);
    char t[8];
    uint32_t f;
    if (ui.epage >= inst_epages(tr))
        ui.epage = 0;
    id = epage_p(tr)[k];
    if (ui.held_step >= 0) {              /* a step held (on the steps, then EDIT): a lock */
        if (id >= 0)
            ilock_turn(id, e);
        else
            app_say("NOT LOCKABLE", "");
    } else if (id >= 0) {
        snd_turn(id, e);
    } else if (id == K_TYPE) {
        int ty = clamp(ts->type + e, 0, NTYPES - 1);
        if (ty != ts->type) {
            f = plat_lock();
            ts->type = (uint8_t)ty;
            syn_release(tr, 0);
            syn_source(tr, ty == TY_SAMPLER);
            plat_unlock(f);
            dirty = 1;
        }
        app_say("TYPE", TYPE_NAME[ts->type]);
    } else if (id != K_LEVEL) {
        smp_turn(id, e);
    } else {
        ts->level = (uint8_t)clamp(ts->level + e, 0, 127);
        f = plat_lock();
        syn_level(tr, (float)ts->level / 127.0f);
        plat_unlock(f);
        dirty = 1;
        int_text(ts->level, t);
        app_say("LEVEL", t);
    }
}

static void octave(int up)
{
    track_t *ts = tset(ui.trk);
    char t[8] = "+";
    ts->oct = (int8_t)clamp(ts->oct + (up ? 1 : -1), -3, 3);
    dirty = 1;
    if (ts->oct > 0)
        int_text(ts->oct, t + 1);
    else
        int_text(ts->oct, t);
    app_say("OCTAVE", t);
}

/* ---------------------------------------------------------------- sampling --- */
static void rec_arm(void)                 /* REC held + SEL on a SAMPLER track: the next sound is recorded */
{
    int t = ui.trk;
    const track_t *ts = tset(t);
    uint32_t f;
    if (t == TR_DRUM || ts->type != TY_SAMPLER) {
        app_say("SAMPLING", "ON A SAMPLER TRACK");
        return;
    }
    if (rec.pending && rec.trk != t) {    /* (only while playing: a stop stores it) */
        app_say("STOP FIRST", "TO KEEP THE LAST ONE");
        return;
    }
    if (rec.pending) {                    /* this track's own, not stored: its sample in the flash again */
        rec.pending = 0;
        bank_use(t);
    }
    f = plat_lock();
    fx_delay_line(0);                     /* the delay's line is the recording's now */
    rec.trk = (uint8_t)t;
    rec.half = ts->s_q;
    rec.pos = 0;
    rec.odd = 0;
    rec.state = RS_ARMED;
    plat_unlock(f);
    app_say("SAMPLING", "PLAY A SOUND");
}

static void rec_stop(void)                /* SEL while sampling: done (or given up while still waiting) */
{
    uint32_t f = plat_lock();
    rec.state = rec.pos >= 2u ? RS_DONE : RS_IDLE;
    plat_unlock(f);
    if (rec.state == RS_IDLE)
        app_say("SAMPLING", "CANCELLED");
}

static void rec_store(void)               /* the recording into the bank (main loop, stopped) */
{
    smp_head_t *h = (smp_head_t *)(void *)rec_buf;
    int t = rec.trk, trim = 0;
    uint32_t size, room = bank_room(t), off, n;
    if (!rec.pending)
        return;
    if (room < SMP_HEAD + s12_bytes(h->frames)) {   /* no room for all of it: as much as fits */
        uint32_t fr = room > SMP_HEAD ? (room - SMP_HEAD) * 2u / 3u : 0;
        if (fr < 2u) {                    /* none: the recording goes (the track's stored sample, if any, again) */
            rec.pending = 0;
            bank_use(t);
            app_say("BANK FULL", "NOT KEPT");
            return;
        }
        h->frames = fr;
        trim = 1;
    }
    size = SMP_HEAD + s12_bytes(h->frames);
    if (bank_begin(t, size) != 0)
        goto fail;
    for (off = 0; off < size; off += n) {
        n = size - off < 4096u ? size - off : 4096u;
        if (bank_write(t, off, rec_buf + off, n) != 0)
            goto fail;
    }
    if (bank_commit(t, size, crc32_of(rec_buf, size)) != 0)
        goto fail;
    rec.pending = 0;                      /* (bank_commit: it plays from the flash now) */
    app_say(trim ? "STORED, CUT" : "SAMPLE STORED", TRK_NAME[t]);
    return;
fail:
    rec.pending = 0;
    bank_use(t);
    app_say("STORE FAILED", TRK_NAME[t]);
}

static void rec_done(void)                /* main loop: a recording finished */
{
    smp_head_t *h = (smp_head_t *)(void *)rec_buf;
    int t = rec.trk;
    track_t *ts = tset(t);
    char s[16];
    h->magic = SMP_MAGIC;
    h->frames = rec.pos;
    h->rate = rec.half ? 22050u : 44100u;
    h->root = 0;
    h->rsv[0] = h->rsv[1] = h->rsv[2] = 0;
    rec.state = RS_IDLE;
    ts->s_start = 0;                      /* a new sample: all of it */
    ts->s_end = 1000;
    ts->s_lstart = 0;
    track_shape(t);
    use_sample(t, rec_buf + SMP_HEAD, h->frames, h->rate);
    rec.pending = 1;
    dirty = 1;
    seconds_text(h->frames * 1000u / h->rate, s);
    app_say("SAMPLED", s);
    if (!sq.playing)
        rec_store();
}

/* ------------------------------------------------------------- LFO and ARP --- */
static const char *const LFO_LABEL[2][4] = {{"SHAPE", "RATE", "DEPTH", "DEST"}, {"SYNC", "RETRIG", "FADE IN", "PHASE"}};
static const char *const LFO_PAGE_NAME[4] = {"LFO 1", "LFO 1 MORE", "LFO 2", "MATRIX"};
#define LP_LFO2 2
#define LP_MATRIX 3
static int lfo_pages(int tr) { return tr == TR_DRUM ? 2 : 4; }   /* the drums': LFO, LFO MORE */
static int lfo_page(int tr) { return ui.lfopage < lfo_pages(tr) ? ui.lfopage : 0; }
static int mod_id(int page, int k)        /* LFO 2's and MATRIX's knobs: their sound parameter (-1: MATRIX's SLOT) */
{
    if (page == LP_LFO2)
        return SP_L2SHAPE + k;
    return k == 0 ? -1 : SP_XM + 3 * ui.mslot + k - 1;
}
static void slot_text(int tr, int slot, char *t)   /* MATRIX: a slot, "LFO 2 > PITCH +4" (or "OFF") */
{
    const int8_t *v = tset(tr)->snd + SP_XM + 3 * slot;
    char u[16];
    t[0] = 0;
    if (!v[0]) {
        text_cat(t, "OFF");
        return;
    }
    syn_param_text(SP_XM + 3 * slot, v[0], u);
    text_cat(t, u);
    text_cat(t, " > ");
    syn_param_text(SP_XM + 3 * slot + 1, v[1], u);
    text_cat(t, u);
    text_cat(t, " ");
    syn_param_text(SP_XM + 3 * slot + 2, v[2], u);
    text_cat(t, u);
}
static void snd_turn(int id, int32_t e);
static void ilock_turn(int id, int32_t e);
static void mod_knob(int k, int32_t e)    /* an instrument's LFO 2 and MATRIX pages (sound parameters: lockable amounts) */
{
    int id = mod_id(ui.lfopage, k);
    if (id < 0) {                         /* MATRIX's SLOT */
        char t[8] = "MOD ", u[32];
        ui.mslot = (uint8_t)clamp(ui.mslot + e, 0, SYN_SLOTS - 1);
        int_text(ui.mslot + 1, t + 4);
        slot_text(ui.trk, ui.mslot, u);
        app_say(t, u);
    } else if (ui.held_step >= 0) {
        ilock_turn(id, e);
    } else {
        snd_turn(id, e);
    }
}

static int lfo_drum_knobs(int pad, int *idx)   /* the knobs the drums' LFO can move on a pad (not switches or sends) */
{
    int i, n = 0, np = drums_nparams(drum_model(pad), pad);
    for (i = 0; i < np && i < NPOTS; i++) {
        const x0x_param_t *d = drums_param(drum_model(pad), pad, i);
        if (d && !d->names && !param_is_send(d))
            idx[n++] = i;
    }
    return n;
}

static void lfo_text(int tr, int page, int k, char *t)
{
    const lfo_t *l = &proj.lfo[tr];
    t[0] = 0;
    if (page == 0 && k == 0) {
        text_cat(t, LSHAPE_NAME[l->shape]);
    } else if (page == 0 && k == 1) {
        if (l->sync) {
            text_cat(t, DIV_NAME[l->div]);
        } else {                          /* Hz: two decimals below 1, one below 10 */
            float hz = LFO_HZ[l->rate];
            int c = (int)(hz * 100.0f + 0.5f);
            char u[4];
            if (hz < 10.0f) {
                int_text(c / 100, t);
                text_cat(t, ".");
                two_digits(c % 100, u);
                if (hz >= 1.0f)
                    u[1] = 0;
                text_cat(t, u);
            } else {
                int_text((c + 50) / 100, t);
            }
            text_cat(t, " HZ");
        }
    } else if (page == 0 && k == 2) {
        if (l->depth > 0)
            text_cat(t, "+");
        int_text(l->depth, t + (l->depth > 0));
    } else if (page == 0) {
        if (tr != TR_DRUM) {
            text_cat(t, LDEST_NAME[l->dest]);
        } else if (l->dest == LFO_OFF || (l->rsv[0] && l->rsv[0] != drum_model(l->pad) + 1) || !drums_param(drum_model(l->pad), l->pad, l->dest)) {
            text_cat(t, "OFF");
        } else {
            text_cat(t, drums_pad_name(drum_model(l->pad), l->pad));
            text_cat(t, " ");
            text_cat(t, drums_param(drum_model(l->pad), l->pad, l->dest)->name);
        }
    } else if (k == 0) {
        text_cat(t, l->sync ? "TEMPO" : "FREE");
    } else if (k == 1) {
        text_cat(t, LRETRIG_NAME[l->retrig]);
    } else if (k == 2) {
        if (tr == TR_DRUM)
            text_cat(t, "-");
        else if (!l->fade)
            text_cat(t, "OFF");
        else {
            uint32_t ms = (uint32_t)time_ms(l->fade);
            int_text((int32_t)ms, t);
            text_cat(t, " MS");
        }
    } else {
        int_text(l->phase * 360 / 128, t);
        text_cat(t, " DEG");
    }
}

static int lfo_value(int tr, int page, int k, int *max)   /* for the bar under the value */
{
    const lfo_t *l = &proj.lfo[tr];
    *max = 127;
    if (page == 0) {
        switch (k) {
        case 0: *max = NLSHAPES - 1; return l->shape;
        case 1: if (l->sync) { *max = NDIV - 1; return l->div; } return l->rate;
        case 2: return l->depth + 64;
        default: *max = tr == TR_DRUM ? 1 : SM_NUM - 1; return tr == TR_DRUM ? l->dest != LFO_OFF : l->dest;
        }
    }
    switch (k) {
    case 0: *max = 1; return l->sync;
    case 1: *max = NLRETRIG - 1; return l->retrig;
    case 2: return tr == TR_DRUM ? 0 : l->fade;
    default: return l->phase;
    }
}

static void lfo_knob(int k, int32_t e)
{
    int tr = ui.trk, inst = tr != TR_DRUM;
    lfo_t *l = &proj.lfo[tr];
    char t[24];
    uint32_t f;
    if (ui.lfopage >= lfo_pages(tr))
        ui.lfopage = 0;
    if (inst && ui.lfopage >= LP_LFO2) {
        mod_knob(k, e);
        return;
    }
    f = plat_lock();
    if (ui.lfopage == 0) {
        switch (k) {
        case 0: l->shape = (uint8_t)clamp(l->shape + e, 0, NLSHAPES - 1); break;
        case 1:
            if (l->sync)
                l->div = (uint8_t)clamp(l->div + e, 0, NDIV - 1);
            else
                l->rate = (uint8_t)clamp(l->rate + e, 0, 127);
            break;
        case 2: l->depth = (int8_t)clamp(l->depth + e, -64, 63); break;
        default:
            if (inst) {
                l->dest = (uint8_t)clamp(l->dest + e, 0, SM_NUM - 1);
            } else {                      /* the picked pad's knobs, OFF before them */
                int idx[NPOTS], n = lfo_drum_knobs(ui.pad, idx), pos = -1, i;
                if (l->dest != LFO_OFF && l->pad == ui.pad && (!l->rsv[0] || l->rsv[0] == drum_model(ui.pad) + 1))
                    for (i = 0; i < n; i++)
                        if (idx[i] == l->dest)
                            pos = i;
                pos = clamp(pos + e, -1, n - 1);
                l->pad = ui.pad;
                l->rsv[0] = (uint8_t)(drum_model(ui.pad) + 1);
                l->dest = (uint8_t)(pos < 0 ? LFO_OFF : idx[pos]);
            }
            break;
        }
    } else {
        switch (k) {
        case 0: l->sync = (uint8_t)clamp(l->sync + e, 0, 1); break;
        case 1:
            if (inst)
                l->retrig = (uint8_t)clamp(l->retrig + e, 0, NLRETRIG - 1);
            else                          /* the drums: FREE or BAR */
                l->retrig = (uint8_t)(e > 0 ? LR_BAR : LR_FREE);
            break;
        case 2:
            if (inst)
                l->fade = (uint8_t)clamp(l->fade + e, 0, 127);
            break;
        default: l->phase = (uint8_t)clamp(l->phase + e, 0, 127); break;
        }
    }
    plat_unlock(f);
    dirty = 1;
    lfo_text(tr, ui.lfopage, k, t);
    app_say(LFO_LABEL[ui.lfopage][k], t);
}

static const char *const ARP_LABEL[2][4] = {{"MODE", "RATE", "OCTAVES", "GATE"}, {"LATCH", "CHORD", "INVERT", ""}};

static int arp_value(int tr, int page, int k, int *max, char *t)   /* its value, its top, and its text */
{
    const track_t *ts = tset(tr);
    t[0] = 0;
    if (page == 1) {
        switch (k) {
        case 0: *max = 1; text_cat(t, ts->arp_latch ? "ON" : "OFF"); return ts->arp_latch;
        case 1: *max = NCHORDS - 1; text_cat(t, CHORD_NAME[ts->chord % NCHORDS]); return ts->chord;
        case 2: *max = 3; text_cat(t, INV_NAME[ts->chord_inv & 3u]); return ts->chord_inv;
        default: *max = 0; return -1;
        }
    }
    switch (k) {
    case 0: *max = NARPMODES - 1; text_cat(t, ARP_MODE_NAME[ts->arp_mode]); return ts->arp_mode;
    case 1: *max = NARPRATES - 1; text_cat(t, ARP_RATE_NAME[ts->arp_rate]); return ts->arp_rate;
    case 2: *max = 4; int_text(ts->arp_oct, t); return ts->arp_oct;
    default: *max = 100; int_text(ts->arp_gate, t); text_cat(t, "%"); return ts->arp_gate;
    }
}

static void arp_knob(int k, int32_t e)
{
    int tr = ui.trk, max;
    track_t *ts = tset(tr);
    char t[16];
    uint32_t f = plat_lock();
    if (ui.arppage == 0) {
        switch (k) {
        case 0: {
            int m = clamp(ts->arp_mode + e, 0, NARPMODES - 1);
            if ((m == AM_OFF) != (ts->arp_mode == AM_OFF)) {   /* on or off: nothing left hanging */
                arp_clear(tr);
                syn_release(tr, 0);
            }
            ts->arp_mode = (uint8_t)m;
            break;
        }
        case 1: ts->arp_rate = (uint8_t)clamp(ts->arp_rate + e, 0, NARPRATES - 1); break;
        case 2: ts->arp_oct = (uint8_t)clamp(ts->arp_oct + e, 1, 4); break;
        default: ts->arp_gate = (uint8_t)clamp(ts->arp_gate + e * 5, 5, 100); break;
        }
    } else if (k == 1) {                  /* (a key held keeps its chord till it's let go: key_chord) */
        ts->chord = (uint8_t)clamp(ts->chord + e, 0, NCHORDS - 1);
    } else if (k == 2) {
        ts->chord_inv = (uint8_t)clamp(ts->chord_inv + e, 0, 3);
    } else if (k == 0) {
        ts->arp_latch = (uint8_t)clamp(ts->arp_latch + e, 0, 1);
        if (!ts->arp_latch && !arp[tr].keys) {   /* LATCH off with no key down: the latched notes go */
            int i;
            for (i = arp[tr].n - 1; i >= 0; i--)
                if (arp[tr].life[i] < 0)
                    arp_drop(tr, i);
        }
    }
    plat_unlock(f);
    dirty = 1;
    if (arp_value(tr, ui.arppage, k, &max, t) >= 0)
        app_say(ARP_LABEL[ui.arppage][k], t);
}

/* ----------------------------------------------------------------- MIXER and FX --- */
static const char *const MIX_NAME[4] = {"LEVEL", "PAN", "REVERB", "DELAY"};
#define NFXPAGE 4
static const char *const FX_PAGE_NAME[NFXPAGE] = {"REVERB", "DELAY", "DELAY 2", "FILTER"};
static const int8_t FX_PAGE_P[NFXPAGE][4] = {
    {FXP_RV_DECAY, FXP_RV_TONE, FXP_RV_HPF, FXP_RV_LEVEL},
    {FXP_DL_TIME, FXP_DL_FDBK, FXP_DL_TONE, FXP_DL_LEVEL},
    {FXP_DL_TYPE, FXP_DL_WEAR, FXP_DL_WIDTH, FXP_DL_HPF},
    {FXP_FILTER, FXP_F_RESO, -1, -1},    /* the DJ filter: on the master bus, after the compressor */
};
/* the master bus (GLO after the mixer's pages, or SEL + GLO): its pages and their knobs */
#define NMSTPAGE 8
static const char *const MST_PAGE_NAME[NMSTPAGE] = {"CHAIN", "EQ", "EQ 2", "COMP", "COMP 2", "TAPE", "STEREO", "LIMIT"};
static const int8_t MST_PAGE_P[NMSTPAGE][4] = {
    {FXP_M_PRESET, FXP_L_GAIN, FXP_L_CEIL, FXP_M_BYPASS},
    {FXP_EQ_LOW, FXP_EQ_MID, FXP_EQ_HIGH, FXP_EQ_CUT},
    {FXP_EQ_LFREQ, FXP_EQ_MFREQ, FXP_EQ_MQ, FXP_EQ_HFREQ},
    {FXP_C_THRESH, FXP_C_RATIO, FXP_C_ATTACK, FXP_C_RELEASE},
    {FXP_C_MAKEUP, FXP_C_MIX, FXP_PUMP, -1},   /* (the fourth box: the reduction now) */
    {FXP_TP_DRIVE, FXP_TP_WARM, FXP_TP_TONE, FXP_TP_MIX},
    {FXP_ST_WIDTH, FXP_ST_BASS, FXP_ST_BAL, -1},   /* (the fourth: the correlation) */
    {FXP_L_GAIN, FXP_L_CEIL, FXP_L_REL, FXP_LIMIT},
};
static const char *const FX_LABEL[FXP_NUM] = {"DECAY", "TONE", "LOW CUT", "LEVEL", "TIME", "FEEDBACK", "TONE", "LEVEL",
                                              "TYPE", "WEAR", "WIDTH", "LOW CUT", "THRESHOLD", "RATIO", "ATTACK",
                                              "RELEASE", "MAKEUP", "MIX", "PUMP", "FILTER", "RESO", "LIMIT",
                                              "LOW", "MID", "HIGH", "LOW CUT", "LOW FREQ", "MID FREQ", "MID Q",
                                              "HIGH FREQ", "DRIVE", "WARMTH", "TONE", "MIX", "WIDTH", "BASS MONO",
                                              "BALANCE", "GAIN", "CEILING", "RELEASE", "PRESET", "BYPASS"};
_Static_assert(sizeof FX_LABEL / sizeof FX_LABEL[0] == FXP_NUM, "a label for each FX knob");

static int mix_value(int t, int page)
{
    switch (page) {
    case 0: return t == TR_DRUM ? proj.mix[0].level : tset(t)->level;
    case 1: return proj.mix[t].pan;
    case 2: return proj.mix[t].rev;
    default: return proj.mix[t].dly;
    }
}

static void mix_text(int t, int page, char *s)
{
    int v = mix_value(t, page);
    s[0] = 0;
    if (page != 1) {
        int_text(v, s);
    } else if (v == 64) {
        text_cat(s, "C");
    } else {
        text_cat(s, v < 64 ? "L" : "R");
        int_text(v < 64 ? 64 - v : v - 64, s + 1);
    }
}

static void mix_knob(int k, int32_t e)    /* the mixer: K1..K4 are the drums, T2, T3 and T4 */
{
    int t = k, page = ui.mixpage;
    char s[12], u[24];
    uint32_t f = plat_lock();
    if (page == 0 && t == TR_DRUM) {
        proj.mix[0].level = (uint8_t)clamp(proj.mix[0].level + e, 0, 127);
    } else if (page == 0) {
        tset(t)->level = (uint8_t)clamp(tset(t)->level + e, 0, 127);
        syn_level(t, (float)tset(t)->level / 127.0f);
    } else {
        uint8_t *v = page == 1 ? &proj.mix[t].pan : page == 2 ? &proj.mix[t].rev : &proj.mix[t].dly;
        *v = (uint8_t)clamp(*v + e, 0, 127);
        if (t != TR_DRUM)
            syn_mixer(t, proj.mix[t].pan, (float)proj.mix[t].rev / 127.0f, (float)proj.mix[t].dly / 127.0f);
    }
    plat_unlock(f);
    dirty = 1;
    mix_text(t, page, s);
    u[0] = 0;
    text_cat(u, TRK_NAME[t]);
    text_cat(u, " ");
    text_cat(u, MIX_NAME[page]);
    app_say(u, s);
}

static void fx_text(int i, char *s)
{
    const x0x_param_t *p = fx_param(i);
    int v = proj.fx[i];
    s[0] = 0;
    if (i == FXP_PUMP && !v) {
        text_cat(s, "OFF");
    } else if (i == FXP_M_PRESET) {      /* the chain preset, * once a knob it sets has moved */
        text_cat(s, fx_preset_name(v));
        if (fx_preset_edited())
            text_cat(s, "*");
    } else {
        fx_format(i, s);                  /* the master bus's: in their units (the filter: LP / HP and how far) */
        if (s[0])
            ;
        else if (p && p->names && v <= p->max)
            text_cat(s, p->names[v]);
        else
            int_text(v, s);
    }
}

static void fx_knob_i(int i, int32_t e)
{
    const x0x_param_t *p = fx_param(i);
    char s[16];
    uint32_t f;
    if (!p)
        return;
    proj.fx[i] = (uint8_t)clamp(proj.fx[i] + e, 0, p->max);
    f = plat_lock();
    fx_set(i, proj.fx[i]);
    plat_unlock(f);
    dirty = 1;
    fx_text(i, s);
    app_say(FX_LABEL[i], s);
}

static void fx_knob(int k, int32_t e)
{
    if (FX_PAGE_P[ui.fxpage][k] >= 0)
        fx_knob_i(FX_PAGE_P[ui.fxpage][k], e);
}

/* a chain preset: its knobs, all at once */
static void chain_preset(int pr)
{
    int i;
    uint32_t f = plat_lock();
    proj.fx[FXP_M_PRESET] = (uint8_t)pr;
    fx_set(FXP_M_PRESET, pr);
    for (i = 0; i < FXP_NUM; i++) {
        int v = fx_preset_value(pr, i);
        if (v >= 0) {
            proj.fx[i] = (uint8_t)v;
            fx_set(i, v);
        }
    }
    plat_unlock(f);
    dirty = 1;
    app_say("MASTER", fx_preset_name(pr));
}

static void mst_knob(int k, int32_t e)
{
    int i = MST_PAGE_P[ui.mstpage][k];
    if (i == FXP_M_PRESET)
        chain_preset(clamp(proj.fx[FXP_M_PRESET] + e, 0, fx_presets() - 1));
    else if (i >= 0)
        fx_knob_i(i, e);
}

static uint8_t fx_meter_shown;           /* the master bus's meters on (the pages that show them) */

/* --------------------------------------------------------------------- SONG --- */
/* the song: up to 32 entries, each a pattern for 1..16 bars; with SONG on, PLAY plays it, round again after the last */
static void song_key(int k)               /* a white key on the SONG page: the cursor's entry that pattern, then on */
{
    int c = ui.scur;
    char t[8], u[8];
    uint32_t f = plat_lock();
    if (c < proj.song_n) {
        proj.song[c].pat = (uint8_t)k;
    } else if (proj.song_n < NSONG) {
        proj.song[proj.song_n].pat = (uint8_t)k;
        proj.song[proj.song_n].bars = 1;
        proj.song_n++;
    } else {
        plat_unlock(f);
        app_say("SONG", "FULL");
        return;
    }
    plat_unlock(f);
    ui.scur = (uint8_t)(c + 1 <= proj.song_n ? c + 1 : proj.song_n);
    dirty = 1;
    int_text(c + 1, u);
    t[0] = 'P';
    two_digits(k + 1, t + 1);
    app_say(u, t);
}

static void song_knob(int k, int32_t e)   /* K1 the entry's PATTERN, K2 its BARS, K4 SONG on / off */
{
    int c = ui.scur;
    char t[8];
    uint32_t f;
    if (k == 3) {
        proj.song_on = (uint8_t)clamp(proj.song_on + e, 0, 1);
        dirty = 1;
        app_say("SONG", proj.song_on ? "ON" : "OFF");
        return;
    }
    if (k == 2) {                         /* LAUNCH: when a clip or scene starts */
        static const char *const LN[3] = {"BAR", "BEAT", "NOW"};
        int v = clamp(launch_mode() + e, 0, 2);
        proj.recq = (uint8_t)((proj.recq & ~0x30u) | (uint32_t)v << 4);
        dirty = 1;
        app_say("LAUNCH", LN[v]);
        return;
    }
    if (c >= proj.song_n) {
        app_say("THE END", "PRESS A PATTERN");
        return;
    }
    f = plat_lock();
    if (k == 0)
        proj.song[c].pat = (uint8_t)clamp(proj.song[c].pat + e, 0, NPAT - 1);
    else
        proj.song[c].bars = (uint8_t)clamp(proj.song[c].bars + e, 1, 16);
    plat_unlock(f);
    dirty = 1;
    if (k == 0) {
        t[0] = 'P';
        two_digits(proj.song[c].pat + 1, t + 1);
        app_say("PATTERN", t);
    } else {
        int_text(proj.song[c].bars, t);
        app_say("BARS", t);
    }
}

static void song_oct(int up)              /* OCT-: the cursor's entry gone; OCT+: a copy of it after it */
{
    int c = ui.scur, i;
    uint32_t f;
    if (c >= proj.song_n)
        return;
    f = plat_lock();
    if (!up) {
        for (i = c; i + 1 < proj.song_n; i++)
            proj.song[i] = proj.song[i + 1];
        proj.song_n--;
    } else if (proj.song_n < NSONG) {
        for (i = proj.song_n; i > c + 1; i--)
            proj.song[i] = proj.song[i - 1];
        proj.song[c + 1] = proj.song[c];
        proj.song_n++;
        ui.scur = (uint8_t)(c + 1);
    }
    plat_unlock(f);
    if (ui.scur > proj.song_n)
        ui.scur = proj.song_n;
    dirty = 1;
    app_say(up ? "COPIED" : "DELETED", "");
}

/* ------------------------------------------------------------------ common --- */
static void track_pick(int tr)
{
    tr = clamp(tr, 0, NTRK - 1);
    if (tr != ui.trk) {
        if (tr == TR_DRUM && ui.page == PG_INSERT) ui.page = PG_SEQ;
        if (ui.page == PG_PICK)
            pick_leave();
        /* between the drums and an instrument track: the track's own page (its keys play), unless on EDIT or
         * HOME; between instrument tracks the page stays */
        if ((tr == TR_DRUM) != (ui.trk == TR_DRUM) && ui.page != PG_EDIT && ui.page != PG_HOME && ui.page != PG_LFO &&
            ui.page != PG_MIX && ui.page != PG_FX && ui.page != PG_COLOR && ui.page != PG_SONG && ui.page != PG_MST && ui.page != PG_SETUP)
            ui.page = (uint8_t)home_page(tr);
        keys_let_go();
        ui.trk = (uint8_t)tr;
        sel_trk = (uint8_t)tr;
        ui.held_step = -1;
        ui.spage = 0;
        ui.epage = 0;
    }
    app_say(TRK_NAME[tr], tr == TR_DRUM ? drum_kit_title() : TYPE_NAME[tset(tr)->type]);
}

static int edit_pat(void) { return clip_of(ui.trk); }

static void pf_press(int i)               /* a perform key down (HOME): its effect while held */
{
    uint32_t f;
    if ((i == PF_STUT || i == PF_TAPE) && (rec.state != RS_IDLE || rec.pending)) {
        app_say(PF_NAME[i], "NOT WHILE SAMPLING");
        return;
    }
    f = plat_lock();
    if (i == PF_STUT || i == PF_TAPE)     /* the delay's memory, lent (as for sampling) */
        fx_delay_line(0);
    if (i == PF_FILT)
        fx_set(FXP_FILTER, perf.filt);
    if (i == PF_WASH)
        fx_set(FXP_RV_DECAY, perf.wash);
    if (i == PF_THROW)
        fx_set(FXP_DL_FDBK, perf.throw_fb);
    if (i == PF_FILL)
        fill_on = 1;
    perf.want |= (uint8_t)(1u << i);
    plat_unlock(f);
    ui.pf_key = (int8_t)i;
    app_say(PF_NAME[i], i == PF_FILL ? "NEXT PATTERN'S DRUMS" : "");
    ui.redraw = 1;
}

static void pf_release(int i)             /* let go: as it was (the delay's memory back with the next frame) */
{
    uint32_t f = plat_lock();
    perf.want &= (uint8_t)~(1u << i);
    if (i == PF_FILT)
        fx_set(FXP_FILTER, proj.fx[FXP_FILTER]);
    if (i == PF_WASH)
        fx_set(FXP_RV_DECAY, proj.fx[FXP_RV_DECAY]);
    if (i == PF_THROW)
        fx_set(FXP_DL_FDBK, proj.fx[FXP_DL_FDBK]);
    if (i == PF_FILL)
        fill_on = 0;
    plat_unlock(f);
    if (ui.pf_key == i)
        ui.pf_key = -1;
    ui.redraw = 1;
}

static void pf_knob(int i, int32_t e)     /* K1 with a perform key held: how it does it, live */
{
    char t[8];
    uint32_t f = plat_lock();
    switch (i) {
    case PF_ROLL:
        perf.roll_rate = (uint8_t)clamp(perf.roll_rate + e, 0, 4);
        perf.roll_len = bar_samples() * (1.0f / 16.0f) * ROLL_F[perf.roll_rate];
        break;
    case PF_STUT: perf.stut_len = (uint8_t)clamp(perf.stut_len + e, 0, 2); break;
    case PF_FILT:
        perf.filt = (uint8_t)clamp(perf.filt + e, 0, 127);
        fx_set(FXP_FILTER, perf.filt);
        break;
    case PF_THROW:
        perf.throw_fb = (uint8_t)clamp(perf.throw_fb + e, 0, 127);
        fx_set(FXP_DL_FDBK, perf.throw_fb);
        break;
    case PF_WASH:
        perf.wash = (uint8_t)clamp(perf.wash + e, 0, 127);
        fx_set(FXP_RV_DECAY, perf.wash);
        break;
    case PF_CRUSH: perf.crush = (uint8_t)clamp(perf.crush + e, 0, 127); break;
    default: break;
    }
    plat_unlock(f);
    t[0] = 0;
    if (i == PF_ROLL)
        text_cat(t, ROLL_NAME[perf.roll_rate]);
    else if (i == PF_STUT)
        text_cat(t, STUT_NAME[perf.stut_len]);
    else
        int_text(i == PF_FILT ? perf.filt : i == PF_THROW ? perf.throw_fb : i == PF_WASH ? perf.wash : perf.crush, t);
    app_say(PF_NAME[i], t);
}

static void follow_knob(int p, int k, int32_t e)   /* HOME, a scene key held: K1 its follow action, K2 after how many bars */
{
    char t[8];
    int a = proj.follow[p] & 7, b = proj.follow[p] >> 3;
    if (k == 0)
        a = clamp(a + e, 0, 4);
    else if (k == 1)
        b = clamp(b + e, 0, 15);
    else
        return;
    proj.follow[p] = (uint8_t)(a | b << 3);
    dirty = 1;
    if (k == 0) {
        app_say("FOLLOW", FOLLOW_NAME[a]);
    } else {
        int_text(b + 1, t);
        text_cat(t, b ? " BARS" : " BAR");
        app_say("AFTER", t);
    }
}

static void setup_knob(int k, int32_t e)  /* SETUP: K1 MIDI CLOCK, K2 SOUNDS */
{
    static const char *const CL[3] = {"OFF", "SEND", "FOLLOW"};
    uint32_t f;
    int v;
    if (k == 0) {
        v = clamp((int)(proj.recq >> 2 & 3u) + e, 0, 2);
        proj.recq = (uint8_t)((proj.recq & ~0x0Cu) | (uint32_t)v << 2);
        app_say("MIDI CLOCK", CL[v]);
    } else if (k == 1) {
        v = clamp(sounds_pp() + e, 0, 1);
        f = plat_lock();
        if (v && !sounds_pp()) {          /* on: each track's sound now is the pattern it's on */
            proj.recq |= 0x40u;
            psnd_store_all();
        } else if (!v) {
            proj.recq &= (uint8_t)~0x40u;
        }
        plat_unlock(f);
        app_say("SOUNDS", v ? "PER PATTERN" : "SHARED");
    } else {
        return;
    }
    dirty = 1;
}

static void reload_sound(void)            /* SAVE + OCT-: the track's sound (the drums: their knobs) as last saved */
{
    int tr = ui.trk, sz = plat_file_size(&PROJ_FILE), fmt = proj_file_format();
    uint32_t f;
    if (!fmt) {
        app_say("RELOAD", "NOTHING SAVED");
        return;
    }
    if (tr == TR_DRUM) {
        static uint8_t pots[2][NPADS + 1][NPOTS], saved606[NPADS + 1][NPOTS], saved707[NPADS + 1][NPOTS];
        uint8_t saved_map[NPADS] = {0}, saved_kit, saved_preset;
        const uint32_t off707 = PROJ_PREFIX + (uint32_t)offsetof(project_t, pots707) - PROJ_TAIL;
        const uint32_t offmap = PROJ_PREFIX + (uint32_t)offsetof(project_t, model) - PROJ_TAIL;
        if (plat_file_read(&PROJ_FILE, offsetof(project_t, kit), &saved_kit, 1) ||
            plat_file_read(&PROJ_FILE, offsetof(project_t, dpreset), &saved_preset, 1) || saved_kit >= KIT_NUM)
            return;
        if (fmt >= PROJ_VERSION && (plat_file_read(&PROJ_FILE, off707, saved707, sizeof saved707) ||
                        plat_file_read(&PROJ_FILE, offmap, saved_map, sizeof saved_map)))
            return;
        const uint32_t fmt4_606_off = PROJ_PREFIX + (uint32_t)offsetof(project_t, pots606) - PROJ_TAIL;
        if (fmt >= 4 && plat_file_read(&PROJ_FILE, fmt4_606_off, saved606, sizeof saved606) != 0)
            return;
        if (plat_file_read(&PROJ_FILE, fmt >= 4 ? PROJ_PREFIX : fmt >= 2 ? OLD_TAIL : V1_OFF(pots), pots,
                           sizeof pots) != 0)
            return;
        f = plat_lock();
        locks_release();
        {
            int k, r, c;
            for (k = 0; k < 2; k++)
                for (r = 0; r <= NPADS; r++)
                    for (c = 0; c < NPOTS; c++)
                        kit_pots(k)[r][c] = pots[k][r][c];
            if (fmt >= 4) {
                for (r = 0; r <= NPADS; r++)
                    for (c = 0; c < NPOTS; c++)
                        kit_pots(KIT_606)[r][c] = saved606[r][c];
            } else {
                model_defaults(KIT_606);
            }
            if (fmt >= PROJ_VERSION) {
                for (r = 0; r <= NPADS; r++)
                    for (c = 0; c < NPOTS; c++)
                        kit_pots(KIT_707)[r][c] = saved707[r][c];
            } else {
                model_defaults(KIT_707);
            }
        }
        if (fmt >= PROJ_VERSION) {
            drum_map_apply(saved_kit, saved_map);
            proj.dpreset = saved_preset >= DPRESET_COUNT ? (uint8_t)kit_factory(saved_kit) : saved_preset;
        }
        pots_apply();
        plat_unlock(f);
    } else {
        static track_t t;
        static psound_t q;
        track_t *ts = tset(tr);
        int k;
        if (fmt >= 2 ? plat_file_read(&PROJ_FILE, (uint32_t)((uint8_t *)&proj.tr[tr - 1] - (uint8_t *)&proj), &t,
                                      sizeof t) != 0
                     : track_from_v1(tr - 1, &t) != 0)
            return;
        q.used = 0;
        if (sounds_pp() && fmt >= 2)
            plat_file_read(&PROJ_FILE, (fmt >= 4 ? PROJ_PREFIX : OLD_TAIL) + (uint32_t)((uint8_t *)&proj.psnd[clip_of(tr)][tr - 1] - (uint8_t *)proj.pots), &q,
                           sizeof q);
        else if (sounds_pp() && sz == (int)sizeof(project_v1_t) && psound_from_v1(clip_of(tr), tr - 1, &q) != 0)
            q.used = 0;
        f = plat_lock();
        for (k = 0; k < SP_NUM; k++)
            ts->snd[k] = q.used ? q.snd[k] : t.snd[k];
        ts->type = q.used ? q.type : t.type;
        ts->level = q.used ? q.level : t.level;
        ts->preset = q.used ? q.preset : t.preset;
        ts->edited = q.used ? q.edited : t.edited;
        syn_sound(tr, ts->snd);
        syn_level(tr, (float)ts->level / 127.0f);
        syn_source(tr, ts->type == TY_SAMPLER);
        plat_unlock(f);
    }
    dirty = 1;
    app_say(TRK_NAME[tr], "RELOADED");
}

static void tap_tempo(void)               /* HOME, OCT-: taps; from the second, the tempo follows them */
{
    uint32_t now = plat_ms(), d = now - ui.tap_t[0], avg;
    int i, n = 0;
    char t[8];
    if (d > 2000u)
        ui.tap_n = 0;
    for (i = 3; i > 0; i--)
        ui.tap_t[i] = ui.tap_t[i - 1];
    ui.tap_t[0] = now;
    if (ui.tap_n < 4)
        ui.tap_n++;
    if (ui.tap_n < 2) {
        app_say("TAP", "AGAIN");
        return;
    }
    n = ui.tap_n - 1;
    avg = (ui.tap_t[0] - ui.tap_t[n]) / (uint32_t)n;
    if (avg < 250u || avg > 1000u) {      /* 60..240 BPM */
        app_say("TAP", "TOO FAR APART");
        return;
    }
    proj.bpm10 = (uint16_t)clamp((int32_t)(600000u / avg), 600, 2400);
    dirty = 1;
    int_text(proj.bpm10 / 10, t);
    app_say("TEMPO", t);
}

static void nudge(int32_t e)              /* HOME, SEL + SELECT while playing: the whole groove 5 ms later / earlier */
{
    float d = (float)e * 220.5f;
    int t;
    char s[12];
    uint32_t f = plat_lock();
    sq.bar_left += d;
    for (t = 0; t < NTRK; t++)
        if (sq.t[t].to_next < 1e8f)
            sq.t[t].to_next += d;
    if (sq.bar_left < 0.0f)
        sq.bar_left = 0.0f;
    plat_unlock(f);
    s[0] = 0;
    text_cat(s, e > 0 ? "+" : "-");
    int_text(e > 0 ? e * 5 : -e * 5, s + 1);
    text_cat(s, " MS");
    app_say("NUDGE", s);
}

/* Set under the audio lock. Future notes follow the mask; old voices, arp
 * material and the unused portion of the current synth block are removed.
 * The shared FX/output history is intentionally not flushed. */
static void set_track_mutes(uint8_t mask)
{
    uint32_t f = plat_lock();
    uint8_t cut = mask & (uint8_t)~proj.tmute;
    int t;
    proj.tmute = mask;
    roll_inhibit_tracks |= cut;
    sync_drum_mutes();
    if (cut & 1u) {
        drum_pk = 0.0f;
        color_reset(&color_rt[TR_DRUM]);
    }
    for (t = 1; t < NTRK; t++)
        if (cut >> t & 1u) {
            arp_clear(t);
            sq.slid[t] = 0;
            syn_cut_track(t);
            track_insert_reset(&insert_rt[t-1]);
            color_reset(&color_rt[t]);
        }
    for (t = 1; t < NTRK; t++) insert_route(t);
    if ((cut & 0x0Eu) && syn_at < SYN_CTL)
        syn_mix_repack(syn_l, syn_r, syn_rv, syn_dl);
    plat_unlock(f);
}

static void solo(int tr)                  /* HOME, a track key + OCT+: only it heard; again: as before */
{
    if (ui.solo_trk == tr) {
        set_track_mutes(ui.solo_save);
        ui.solo_trk = -1;
        app_say(TRK_NAME[tr], "SOLO OFF");
    } else {
        if (ui.solo_trk < 0)
            ui.solo_save = proj.tmute;
        set_track_mutes((uint8_t)(0x0Fu & ~(1u << tr)));
        ui.solo_trk = (int8_t)tr;
        app_say(TRK_NAME[tr], "SOLO");
    }
    dirty = 1;
}

static int home_trkkey(void)              /* HOME: the track key held (the lowest), or -1 */
{
    int k;
    if (ui.page != PG_HOME)
        return -1;
    for (k = 0; k < NTRK; k++)
        if (ui.tkey_down >> k & 1u)
            return k;
    return -1;
}

static void track_mute(int tr)            /* HOME + pad keys 1-4 */
{
    set_track_mutes((uint8_t)(proj.tmute ^ (1u << tr)));
    dirty = 1;
    app_say(TRK_NAME[tr], (proj.tmute >> tr & 1u) ? "MUTED" : "ON");
}

static int scene_whole(int n)             /* every track on pattern n, none waiting */
{
    int t;
    for (t = 0; t < NTRK; t++)
        if (clip[t] != n || qclip[t] != -1)
            return 0;
    return 1;
}

static void pattern_pick(int n)           /* a white key on HOME: the whole pattern, a scene: every track on it */
{
    char t[4];
    uint32_t f;
    two_digits(n + 1, t);
    ui.held_step = -1;
    if (sq.playing) {
        f = plat_lock();
        if (n == proj.pat && scene_whole(n)) {
            sq.queued = -1;
        } else if (launch_mode() == LN_NOW) {   /* at once: every track from step 1, the bar from here */
            proj.pat = (uint8_t)n;
            sq.queued = -1;
            clips_all(n);
            seq_reset_tracks();
            sq.bar_left = 0.0f;
            sq.fbars = 0;
            sq.fskip = 1;
        } else {
            sq.queued = (int8_t)n;
        }
        plat_unlock(f);
        app_say(n == proj.pat && sq.queued < 0 ? "PATTERN" : "NEXT", t);
    } else {
        f = plat_lock();
        proj.pat = (uint8_t)n;
        clips_all(n);
        plat_unlock(f);
        ui.spage = 0;
        app_say("PATTERN", t);
    }
    dirty = 1;
}

static void clip_launch(int tr, int n)    /* a track key held + a white key on HOME: that track's part of pattern n */
{
    char t[16];
    uint32_t f = plat_lock();
    if (!sq.playing) {
        set_clip(tr, n);
        qclip[tr] = -1;
    } else if (clip[tr] == n && qclip[tr] == -1) {
        ;                                 /* playing it already */
    } else {
        qclip[tr] = (int8_t)n;
        if (launch_mode() == LN_NOW)
            clips_start();
    }
    plat_unlock(f);
    t[0] = 'P';
    two_digits(n + 1, t + 1);
    text_cat(t, sq.playing && qclip[tr] == n ? " NEXT" : "");
    app_say(TRK_NAME[tr], t);
    ui.held_step = -1;
    ui.redraw = 1;
}

static void clip_stop(int tr)             /* a track key held + OCT- on HOME: the track stops (a clip starts it) */
{
    uint32_t f = plat_lock();
    if (!sq.playing) {
        clip[tr] = -1;
        qclip[tr] = -1;
    } else {
        qclip[tr] = -2;
        if (launch_mode() == LN_NOW)
            clips_start();
    }
    plat_unlock(f);
    app_say(TRK_NAME[tr], sq.playing && qclip[tr] == -2 ? "STOPS NEXT" : "STOPPED");
    ui.redraw = 1;
}

static void pattern_copy_to(int n)        /* the whole pattern, all four tracks (and its parts' generators) */
{
    char t[4];
    uint32_t f;
    int i;
    if (n == proj.pat)
        return;
    undo_push((uint8_t)n);
    f = plat_lock();
    proj.p[n] = *cur();
    for (i = 0; i < NINST; i++)
        proj.gen[n][i] = proj.gen[proj.pat][i];
    plat_unlock(f);
    dirty = 1;
    two_digits(n + 1, t);
    app_say("COPIED TO", t);
}

static void do_save(void)
{
    if (sq.playing) {
        ui.save_pending = 1;
        app_say("SAVE", "WHEN STOPPED");
        return;
    }
    {
        int r = project_save();
        app_say(r == -2 ? "PROJECT TOO LARGE" : r ? "SAVE FAILED" : "SAVED", r == -2 ? "OLD SAVE KEPT" : "");
    }
}

static void page_button(int page)         /* a page's button: its page, or (pressed again) the track's own */
{
    ui.page = (uint8_t)(ui.page == page ? home_page(ui.trk) : page);
    ui.redraw = 1;
}

/* ------------------------------------------------------- the 303 generator --- */
/* The ACID pages (SEQ, after STEPS; T2..T4, 0.4): a part's notes made the 303's way. Each step has its own random
 * numbers, from the seed: whether it plays, which of the scale's notes, an octave up or down, a slide into the next,
 * an accent. The knobs only move where those numbers are cut, so DENSITY up adds notes and keeps the ones there,
 * SLIDE and ACCENT add to the same line, ROOT and SCALE move it; OCT+ NEW takes the next seed, OCT- MUTATE re-rolls an
 * eighth of the steps. A part keeps its settings (proj.gen); its steps are what plays, is saved and can be edited.
 * A step: one note, velocity GEN_VEL (an accent GEN_ACC and the NS_ACC mark); HALF (GATE_HALF), WHOLE (held to just
 * before the next note, over the rests between: a 303's tie) or a SLIDE (GATE_SLIDE and the NS_SLIDE mark: the next
 * note glides from it on any sound, as a 303's) */
static const char *const GS_NAME[NGSCALES] = {"MINOR", "PHRYGIAN", "DORIAN", "PENTA", "BLUES", "HARM MIN", "MAJOR",
                                              "MIXOLYD"};
static const int8_t GS_DEG[NGSCALES][7] = {   /* each scale from the root, a bass line's favourites first (-1: none) */
    {0, 7, 3, 10, 5, 8, 2}, {0, 1, 7, 3, 10, 5, 8},   {0, 7, 3, 10, 5, 9, 2}, {0, 7, 3, 10, 5, -1, -1},
    {0, 7, 3, 10, 5, 6, -1}, {0, 7, 3, 8, 11, 5, 2}, {0, 7, 4, 5, 9, 2, 11}, {0, 7, 4, 10, 5, 2, 9}};
#define GEN_VEL 90
#define GEN_ACC 127
static gen_t gen_last[NTRK];              /* each track's settings used last: a part's first turn starts from them */
static void seq_knob(int k, int32_t e);
static nstep_t gen_buf[2][NSTEPS];

static gen_t *gen_of(int tr) { return &proj.gen[clip_of(tr)][tr - 1]; }

static void gen_default(gen_t *g)
{
    uint32_t k;
    for (k = 0; k < sizeof *g; k++)
        ((uint8_t *)g)[k] = 0;
    g->seed = 1;
    g->root = 36;                         /* C2 */
    g->scale = GS_MINOR;
    g->notes = 4;
    g->dens = 75;
    g->slide = 25;
    g->accent = 30;
    g->oct = 20;
    g->whole = 20;
}

static gen_t gen_view(int tr)             /* what the ACID page shows: the part's, else what its first turn starts from */
{
    gen_t g;
    if (gen_of(tr)->seed)
        return *gen_of(tr);
    if (gen_last[tr].seed)
        return gen_last[tr];
    gen_default(&g);
    return g;
}

static int gen_scale_n(int sc)            /* the scale's notes */
{
    int n = 0;
    while (n < 7 && GS_DEG[sc % NGSCALES][n] >= 0)
        n++;
    return n;
}

static int gen_ndeg(const gen_t *g)       /* the scale's notes in use */
{
    int n = 0;
    while (n < g->notes && n < 7 && GS_DEG[g->scale % NGSCALES][n] >= 0)
        n++;
    return n ? n : 1;
}

static uint32_t gen_hash(uint32_t a, uint32_t b)
{
    uint32_t x = a * 0x9E3779B9u ^ (b + 0x7F4A7C15u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static void gen_make(const gen_t *g, int len, nstep_t *out)   /* the part's first len steps, from the settings */
{
    uint8_t ep[NSTEPS], on[NSTEPS], acc[NSTEPS], sl[NSTEPS], wh[NSTEPS];
    int s, m, j, k = len / 8 > 1 ? len / 8 : 1, nd = gen_ndeg(g);
    for (s = 0; s < len; s++)
        ep[s] = 0;
    for (m = 1; m <= g->mut; m++)         /* each MUTATE: the steps it re-rolled (the last one's numbers win) */
        for (j = 0; j < k; j++)
            ep[gen_hash(g->seed * 7919u + (uint32_t)m, (uint32_t)j) % (uint32_t)len] = (uint8_t)m;
    for (s = 0; s < len; s++) {
        uint32_t base = (uint32_t)g->seed * 65537u + (uint32_t)ep[s] * 1009u, r[7];
        int i, note, w;
        for (i = 0; i < 7; i++)
            r[i] = gen_hash(base, (uint32_t)(s * 8 + i)) % 1000u;
        on[s] = (uint8_t)(g->dens && (s == 0 || r[0] < g->dens * 10u));   /* (the first step always, as a line's anchor) */
        w = (int)(r[1] * (uint32_t)(nd + 2) / 1000u);   /* the root three times as likely as each other note */
        note = g->root + GS_DEG[g->scale % NGSCALES][w < 3 ? 0 : w - 2];
        if (r[2] < g->oct * 10u)
            note += r[3] < 700u ? 12 : -12;
        if (note < 24)
            note += 12;
        out[s].note[0] = (uint8_t)note;
        sl[s] = (uint8_t)(r[4] < g->slide * 10u);
        acc[s] = (uint8_t)(r[5] < g->accent * 10u);
        wh[s] = (uint8_t)(r[6] < g->whole * 10u);
    }
    for (s = 0; s < len; s++) {
        nstep_t *st = &out[s];
        int note = st->note[0], nx = s + 1 < len ? s + 1 : 0;
        nstep_clear(st);
        if (!on[s])
            continue;
        st->note[0] = (uint8_t)note;
        st->vel = (uint8_t)(acc[s] ? GEN_ACC | NS_ACC : GEN_VEL);
        st->prob = 100;
        if (sl[s] && len > 1 && on[nx]) {
            st->gate = GATE_SLIDE;
            st->prob |= NS_SLIDE;
        } else if (wh[s]) {
            int k = 1;
            while (s + k < len && !on[s + k])
                k++;
            st->gate = (uint16_t)(8 * k - 1 > GATE_MAX ? GATE_MAX : 8 * k - 1);
        } else {
            st->gate = GATE_HALF;
        }
    }
}

static void gen_write(int tr)             /* the part's steps from its settings (its steps past its length untouched) */
{
    ipart_t *p = ipart(tr);
    int s, len = p->len;
    uint32_t f;
    gen_make(gen_of(tr), len, gen_buf[0]);
    f = plat_lock();
    for (s = 0; s < len; s++)
        p->st[s] = gen_buf[0][s];
    plat_unlock(f);
    gen_last[tr] = *gen_of(tr);
    dirty = 1;
}

static gen_t *gen_begin(int tr)           /* a part's generator, set up the first time: from the track's last */
{
    gen_t *g = gen_of(tr);
    if (!g->seed)
        *g = gen_view(tr);
    return g;
}

static void seed_text(const gen_t *g, char *t)   /* "SEED 42", "SEED 42 M3" */
{
    char u[8];
    t[0] = 0;
    text_cat(t, "SEED ");
    int_text(g->seed, u);
    text_cat(t, u);
    if (g->mut) {
        text_cat(t, " M");
        int_text(g->mut, u);
        text_cat(t, u);
    }
}

static void gen_new(void)                 /* OCT+: the next seed's line */
{
    int tr = ui.trk;
    gen_t *g;
    char t[16];
    undo_mark(0);
    g = gen_begin(tr);
    g->seed = (uint16_t)(g->seed % 999u + 1u);
    g->mut = 0;
    gen_write(tr);
    seed_text(g, t);
    app_say("NEW", t);
}

static void gen_mutate(void)              /* OCT-: a few steps re-rolled (until something changes, eight tries) */
{
    int tr = ui.trk, len = ipart(tr)->len, s, n, same = 1;
    gen_t *g;
    char t[16];
    undo_mark(0);
    g = gen_begin(tr);
    gen_make(g, len, gen_buf[1]);
    for (n = 0; n < 8 && same && g->mut < 255; n++) {
        g->mut++;
        gen_make(g, len, gen_buf[0]);
        for (s = 0; s < len && same; s++) {
            const nstep_t *a = &gen_buf[0][s], *b = &gen_buf[1][s];
            same = a->note[0] == b->note[0] && a->vel == b->vel && a->gate == b->gate && a->prob == b->prob;
        }
    }
    gen_write(tr);
    seed_text(g, t);
    app_say("MUTATE", t);
}

/* the pages: ACID (DENSITY WHOLE SLIDE ACCENT), NOTES (ROOT SCALE NOTES OCTAVE), LINE (SEED, and the part's LENGTH,
 * RATE and the SWING, as on STEPS) */
static const char *const GEN_LABEL[12] = {"DENSITY", "WHOLE", "SLIDE", "ACCENT", "ROOT", "SCALE", "NOTES", "OCTAVE",
                                          "SEED", "LENGTH", "RATE", "SWING"};
static int gen_value(int tr, const gen_t *g, int i, int *lo, int *hi, char *t)   /* knob i (page * 4 + k): value, text */
{
    int v;
    t[0] = 0;
    *lo = 0;
    *hi = 100;
    switch (i) {
    case 0: v = g->dens; break;
    case 1: v = g->whole; break;
    case 2: v = g->slide; break;
    case 3: v = g->accent; break;
    case 7: v = g->oct; break;
    case 4:
        *lo = 24;
        *hi = 71;
        note_text(g->root, t);
        return g->root;
    case 5:
        *hi = NGSCALES - 1;
        text_cat(t, GS_NAME[g->scale % NGSCALES]);
        return g->scale;
    case 6:
        *lo = 1;
        *hi = gen_scale_n(g->scale);
        int_text(gen_ndeg(g), t);
        return gen_ndeg(g);
    case 8:
        *lo = 1;
        *hi = 999;
        int_text(g->seed, t);
        return g->seed;
    case 9:
        *lo = 1;
        *hi = NSTEPS;
        int_text(*part_len(tr), t);
        return *part_len(tr);
    case 10:
        *hi = NRATES - 1;
        text_cat(t, RATE_NAME[*part_rate(tr) % NRATES]);
        return *part_rate(tr);
    default:
        *lo = 50;
        *hi = 75;
        v = proj.swing;
        break;
    }
    int_text(v, t);
    text_cat(t, "%");
    return v;
}

static void gen_knob(int k, int32_t e)    /* the ACID pages' knobs: the part made again at once */
{
    int tr = ui.trk, i = ui.genpage * 4 + k, lo, hi;
    gen_t *g;
    char t[16];
    if (i >= 10) {                        /* RATE, SWING: as on STEPS, the line as it is */
        seq_knob(i - 9, e);
        return;
    }
    if (i == 9) {                         /* LENGTH: as on STEPS, then a line made for it (if there is one) */
        seq_knob(0, e);
        if (gen_of(tr)->seed)
            gen_write(tr);
        return;
    }
    undo_mark(10u + (uint32_t)i);         /* (a turn of one knob: one undo) */
    g = gen_begin(tr);
    switch (i) {
    case 0: g->dens = (uint8_t)clamp(g->dens + e, 0, 100); break;
    case 1: g->whole = (uint8_t)clamp(g->whole + e, 0, 100); break;
    case 2: g->slide = (uint8_t)clamp(g->slide + e, 0, 100); break;
    case 3: g->accent = (uint8_t)clamp(g->accent + e, 0, 100); break;
    case 4: g->root = (uint8_t)clamp(g->root + e, 24, 71); break;
    case 5: g->scale = (uint8_t)clamp(g->scale + e, 0, NGSCALES - 1); break;
    case 6: g->notes = (uint8_t)clamp(gen_ndeg(g) + e, 1, gen_scale_n(g->scale)); break;
    case 7: g->oct = (uint8_t)clamp(g->oct + e, 0, 100); break;
    default:
        g->seed = (uint16_t)clamp(g->seed + e, 1, 999);
        g->mut = 0;
        break;
    }
    gen_write(tr);
    gen_value(tr, g, i, &lo, &hi, t);
    app_say(GEN_LABEL[i], t);
}

static void seq_knob(int k, int32_t e)    /* the step and KEYS pages' knobs: LENGTH, RATE (the track's), SWING, TEMPO */
{
    int tr = ui.trk;
    uint8_t *len = part_len(tr), *rate = part_rate(tr);
    char t[16];
    uint32_t f;
    if (k == 0) {
        undo_mark(1);
        f = plat_lock();
        *len = (uint8_t)clamp(*len + e, 1, NSTEPS);
        if (sq.t[tr].step >= *len)
            sq.t[tr].step = *len - 1;
        plat_unlock(f);
        if (ui.spage >= npages())
            ui.spage = (uint8_t)(npages() - 1);
        if (ui.cursor[tr] >= *len)
            ui.cursor[tr] = 0;
        int_text(*len, t);
        app_say("LENGTH", t);
    } else if (k == 1) {
        undo_mark(2);
        *rate = (uint8_t)clamp(*rate + e, 0, NRATES - 1);
        app_say("RATE", RATE_NAME[*rate]);
    } else if (k == 2) {
        proj.swing = (uint8_t)clamp(proj.swing + e, 50, 75);
        dirty = 1;
        int_text(proj.swing, t);
        text_cat(t, "%");
        app_say("SWING", t);
    } else {
        proj.bpm10 = (uint16_t)clamp(proj.bpm10 + e * 10, 600, 2400);
        dirty = 1;
        int_text(proj.bpm10 / 10, t);
        app_say("TEMPO", t);
    }
}

static const char *const CLICK_NAME[3] = {"OFF", "ON", "REC"};
static const char *const COUNT_NAME[3] = {"OFF", "1 BAR", "2 BARS"};
static const char *const GRID_NAME[NRATES][3] = {
    {"1/16", "1/8", "1/4"}, {"1/8", "1/4", "1/2"}, {"1/32", "1/16", "1/8"}, {"1/16T", "1/8T", "1/4T"}, {"1/8T", "1/4T", "1/2T"}};

static void rec_knob(int k, int32_t e)    /* REC held: K1 the metronome, K2 the count-in, K3 the grid, K4 the click's level */
{
    char t[8];
    int v;
    switch (k) {
    case 0:
        v = clamp(click_mode() + e, 0, 2);
        proj.click = (uint8_t)((proj.click & ~3u) | (uint32_t)v);
        app_say("CLICK", CLICK_NAME[v]);
        break;
    case 1:
        v = clamp(countin_bars() + e, 0, 2);
        proj.click = (uint8_t)((proj.click & ~12u) | (uint32_t)v << 2);
        app_say("COUNT-IN", COUNT_NAME[v]);
        break;
    case 2:
        proj.recq = (uint8_t)((proj.recq & ~3u) | (uint32_t)clamp(recq_grid() + e, 0, 2));
        app_say("QUANTIZE", GRID_NAME[*part_rate(ui.trk) % NRATES][recq_grid()]);
        break;
    default:
        v = clamp(click_level() + e, 1, 15);
        proj.click = (uint8_t)((proj.click & 15u) | (uint32_t)v << 4);
        int_text(v, t);
        app_say("CLICK LEVEL", t);
        break;
    }
    dirty = 1;
}

static void insert_select(int tr, int e)
{
    track_insert_param_t *p = &proj.insert[tr-1];
    int mode = clamp(p->mode + e, TI_OFF, TI_MODES - 1);
    if (mode != p->mode) {
        uint32_t f = plat_lock();
        track_insert_default(p, mode);
        track_insert_reset(&insert_rt[tr-1]);
        insert_route(tr);
        plat_unlock(f);
        dirty = 1;
    }
    app_say("INSERT", track_insert_name(mode));
}

static void insert_text(const track_insert_param_t *p, int k, char *text)
{
    int n = p->v[k];
    if (p->mode == TI_SHIFT && k < 3) {
        static const int scale[3] = {900, 25, 15};
        int_text((n-64)*scale[k]/64, text);
        text_cat(text, " HZ");
    } else if (k == 3) {
        int_text(n*100/127, text);
        text_cat(text, "%");
    } else {
        int_text(n, text);
    }
}

static void insert_knob(int tr, int k, int e)
{
    track_insert_param_t *p = &proj.insert[tr-1];
    char value[8];
    int v = clamp(p->v[k] + e, 0, 127);
    if (!p->mode || v == p->v[k])
        return;
    {
        uint32_t f = plat_lock();
        if (k == 3 && (!p->v[3] || !v)) track_insert_reset(&insert_rt[tr-1]);
        p->v[k] = (uint8_t)v;
        insert_route(tr);
        plat_unlock(f);
    }
    dirty = 1;
    insert_text(p, k, value);
    app_say(track_insert_label(p->mode, k), value);
}

static const char *const COLOR_LABEL[COL_NUM] = {"AMOUNT", "WEAR", "WOW", "DIGITAL", "DROPOUT", "COLOR"};
static int color_index(int k)
{
    return ui.color_page ? COL_WEAR + k : k == 0 ? COL_AMOUNT : k == 3 ? COL_ON : -1;
}
static void color_knob(int k, int32_t e)
{
    int id = color_index(k), v, was;
    int tr = ui.trk;
    color_params_t *p = &proj.color[tr];
    color_state_t *rt = &color_rt[tr];
    char t[8];
    uint32_t f;
    if (id < 0) return;
    v = clamp(p->v[id] + e, 0, id == COL_ON ? 1 : 127);
    if (v == p->v[id]) return;
    f = plat_lock();
    was = color_active(p);
    if (id == COL_ON || ((!v || !p->v[id]) && id == COL_AMOUNT))
        color_reset(rt);
    if (id == COL_WOW && (!v || !p->v[id]))
        rt->valid = rt->pos = 0;
    if (id == COL_DIGITAL && (!v || !p->v[id]))
        rt->clock = 0;
    if (id == COL_DROP && (!v || !p->v[id])) {
        rt->dropout = 1.f;
        rt->remain = rt->countdown = 0;
    }
    p->v[id] = (uint8_t)v;
    if (was != color_active(p)) color_reset(rt);
    if (tr != TR_DRUM) insert_route(tr);
    plat_unlock(f);
    dirty = 1;
    int_text(v*100/127,t);
    text_cat(t,"%");
    app_say(COLOR_LABEL[id], id == COL_ON ? (v ? "ON" : "BYPASS") : t);
}

static void knob(int k, int32_t e)        /* KNOB 1..4 (k 0..3) on the page shown */
{
    int inst = ui.trk != TR_DRUM;
    ui.knob_k = (int8_t)k;
    ui.knob_t = plat_ms();
    if ((ui.btn >> B_REC & 1u) && rec.state == RS_IDLE) {
        ui.rec_used = ui.rec_shown = 1;
        rec_knob(k, e);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PG_HOME && ui.scene_key >= 0) {   /* a scene key held: its follow action */
        follow_knob(ui.scene_key, k, e);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PG_SETUP) {
        setup_knob(k, e);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PG_HOME && ui.pf_key >= 0 && (perf.want >> ui.pf_key & 1u)) {
        if (k == 0)
            pf_knob(ui.pf_key, e);
        ui.redraw = 1;
        return;
    }
    if (!inst && ui.page == PG_EDIT && drum_model_page()) {
        if (k == 0) {
            if (ui.held_step >= 0)
                app_say("NOT LOCKABLE", "");
            else
                drum_model_turn(ui.pad, e);
        }
    } else if (!inst && ui.page == PG_EDIT && drum_kit_page()) {
        if (ui.held_step >= 0)
            app_say("NOT LOCKABLE", "");
        else if (k < drums_nparams(proj.kit, DRUMS_PAD_KIT))
            param_turn(DRUMS_PAD_KIT, k, e);
    } else if (!inst && ui.page == PG_EDIT) {
        int idx[NPOTS], n = edit_params(drum_model(ui.pad), ui.pad, idx), i = ui.epage * 4 + k;
        if (i < n && ui.held_step >= 0)
            held_lock(idx[i], e);
        else if (i < n)
            param_turn(ui.pad, idx[i], e);
    } else if (!inst && ui.page == PG_KIT) {
        if (k < drums_nparams(proj.kit, DRUMS_PAD_KIT))
            param_turn(DRUMS_PAD_KIT, k, e);
    } else if (inst && ui.page == PG_EDIT) {
        iedit_knob(k, e);
    } else if (inst && ui.page == PG_ENV) {
        ienv_knob(k, e);
    } else if (ui.page == PG_SONG) {
        song_knob(k, e);
    } else if (ui.page == PG_MIX) {
        mix_knob(k, e);
    } else if (inst && ui.page == PG_INSERT) {
        insert_knob(ui.trk, k, e);
    } else if (ui.page == PG_COLOR) {
        color_knob(k,e);
    } else if (ui.page == PG_FX) {
        fx_knob(k, e);
    } else if (ui.page == PG_MST) {
        mst_knob(k, e);
    } else if (ui.page == PG_LFO) {
        lfo_knob(k, e);
    } else if (inst && ui.page == PG_ARP) {
        arp_knob(k, e);
    } else if (inst && ui.page == PG_GEN) {
        gen_knob(k, e);
    } else if (inst && ui.page == PG_SEQ && ui.held_step >= 0) {
        iheld_knob(k, e);
    } else {
        seq_knob(k, e);
    }
    ui.redraw = 1;
}

/* SEQ + octave doubles the selected clip's active loop or truncates it.
 * Preflight locks: never repeat the notes but silently drop their automation. */
static void part_resize(int up)
{
    int tr = ui.trk, i, k, count = 0;
    pattern_t *pat = &proj.p[clip_of(tr)];
    dpart_t *d = &pat->d;
    ipart_t *p = tr == TR_DRUM ? 0 : &pat->t[tr - 1];
    uint8_t *len = p ? &p->len : &d->len;
    int old = *len, newlen = up ? old * 2 : old / 2;
    uint32_t f;
    char t[16];
    if (up && newlen > NSTEPS) {
        app_say("DOUBLE PART", "MAX 128 STEPS");
        return;
    }
    if (!up && old == 1) {
        app_say("HALVE PART", "MIN 1 STEP");
        return;
    }
    if (up) {
        int locks = p ? p->nlk : d->nlk;
        for (i = 0; i < locks; i++)
            count += (p ? p->lk[i].step : d->lk[i].step) < old;
        if (count * 2 > (p ? NILOCKS : NLOCKS)) {
            app_say("DOUBLE PART", "TOO MANY LOCKS");
            return;
        }
    }
    undo_mark(0);
    f = plat_lock();
    if (p) {
        for (i = 0, k = 0; i < p->nlk; i++)
            if (p->lk[i].step < (up ? old : newlen)) p->lk[k++] = p->lk[i];
        p->nlk = (uint8_t)k;
        for (i = 0; up && i < k; i++) {
            ilock_t c = p->lk[i];
            c.step = (uint8_t)(c.step + old);
            p->lk[p->nlk++] = c;
        }
        for (i = up ? old : newlen; i < NSTEPS; i++)
            if (i < newlen) p->st[i] = p->st[i - old];
            else nstep_clear(&p->st[i]);
    } else {
        for (i = 0, k = 0; i < d->nlk; i++)
            if (d->lk[i].step < (up ? old : newlen)) d->lk[k++] = d->lk[i];
        d->nlk = (uint8_t)k;
        for (i = 0; up && i < k; i++) {
            lock_t c = d->lk[i];
            c.step = (uint8_t)(c.step + old);
            d->lk[d->nlk++] = c;
        }
        for (k = 0; k <= NPADS; k++) {
            uint32_t *row = k < NPADS ? d->hit[k] : d->acc;
            for (i = up ? old : newlen; i < NSTEPS; i++)
                if (i < newlen && bit_get(row, i - old)) bit_set(row, i);
                else bit_clear(row, i);
        }
    }
    *len = (uint8_t)newlen;
    if (ui.spage * 16 >= newlen) ui.spage = (uint8_t)((newlen - 1) / 16);
    if (ui.cursor[tr] >= newlen) ui.cursor[tr] = (uint8_t)(newlen - 1);
    plat_unlock(f);
    int_text(newlen, t);
    text_cat(t, " STEPS");
    app_say(up ? "DOUBLE PART" : "HALVE PART", t);
    ui.redraw = 1;
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, pressed, released, kp, kr;
    int32_t e;
    int inst;
    touch_poll();
    keys = touch_keys();
    pressed = btn & ~ui.btn;
    released = ~btn & ui.btn;
    ui.btn = btn;                         /* (step_down reads SEQ from it) */
    inst = ui.trk != TR_DRUM;
    if (ui.page == PG_PICK && clip_of(ui.trk) != ui.pick_clip)
        pick_leave();
    if (pressed)                          /* a button: the knob card turned last stands out no more */
        ui.knob_k = -1;

    /* buttons */
    if (pressed >> B_EDIT & 1u)
        ui.edit_used = 0;
    if (pressed >> B_SAVE & 1u) {
        ui.save_used = 0;
        ui.save_slot = -1;
    }
    if (pressed >> B_SEL & 1u) {          /* SEL: stops sampling; REC held + SEL samples; held + pad keys 1-4: tracks */
        ui.sel_used = 0;
        if (rec.state == RS_ARMED || rec.state == RS_REC) {
            ui.sel_used = 1;
            rec_stop();
        } else if (btn >> B_REC & 1u) {
            ui.sel_used = ui.rec_used = 1;
            rec_arm();
        }
    }
    if (pressed >> B_SEQ & 1u) {
        ui.seq_prev = ui.page;
        ui.seq_used = 0;
    }
    if ((pressed >> B_OCTDN & 1u) || (pressed >> B_OCTUP & 1u)) {
        int up = (int)(pressed >> B_OCTUP & 1u);
        if ((btn >> B_SAVE & 1u) && !up) {   /* SAVE + OCT-: the track's sound as last saved */
            ui.save_used = 1;
            reload_sound();
        } else if (ui.page == PG_PICK && !(btn & ((1u << B_EDIT) | (1u << B_REC)))) {
            ui.pick_oct = (int8_t)clamp(ui.pick_oct + (up ? 1 : -1), -3, 3);
        } else if ((btn >> B_SEQ & 1u) && ui.seq_prev != PG_PICK && !(btn >> B_REC & 1u) &&
                   !(btn >> B_EDIT & 1u) && !(btn >> B_SAVE & 1u) && ui.held_step < 0) {
            ui.seq_used = 1;
            ui.page = ui.seq_prev;
            part_resize(up);
        } else if (ui.page == PG_HOME && home_trkkey() >= 0) {   /* HOME, a track key held: OCT- stops it, OCT+ solo */
            int tk = home_trkkey();
            ui.tkey_used |= (uint8_t)(1u << tk);
            if (!up)
                clip_stop(tk);
            else
                solo(tk);
        } else if (ui.page == PG_HOME && !(btn >> B_EDIT & 1u) && !(btn >> B_REC & 1u)) {
            if (up)                       /* TAPE STOP while held */
                pf_press(PF_TAPE);
            else
                tap_tempo();
        } else if ((btn >> B_REC & 1u) && rec.state == RS_IDLE) {   /* REC held: OCT- QUANTIZE, OCT+ LEGATO */
            ui.rec_used = ui.rec_shown = 1;
            if (up)
                legato();
            else
                quantize();
            ui.redraw = 1;
        } else if (ui.held_step >= 0) {          /* a step held + OCT-: its chance (and locks) cleared; OCT+ (T2..T4)
                                                  * its length as a 303's */
            if (!up) {
                if (inst)
                    iheld_clear();
                else
                    held_clear();
            } else if (inst) {
                iheld_len();
            }
        } else if (btn >> B_EDIT & 1u) {  /* EDIT + OCT- / OCT+: undo / redo */
            ui.edit_used = 1;
            undo_step(up);
        } else if (ui.page == PG_SONG) {
            song_oct(up);
        } else if (!inst && ui.page == PG_MUTE && up) {
            uint32_t f;
            undo_mark(0);
            f = plat_lock();
            dcur()->mute = 0;
            sync_drum_mutes();
            plat_unlock(f);
            app_say("ALL PADS", "ON");
        } else if (inst && ui.page == PG_GEN) {   /* the 303 generator: OCT+ a new line, OCT- a few steps changed */
            if (up)
                gen_new();
            else
                gen_mutate();
        } else if (inst && (ui.page == PG_KEYS || ui.page == PG_EDIT || ui.page == PG_ENV || ui.page == PG_LFO ||
                            ui.page == PG_ARP || ui.page == PG_MIX || ui.page == PG_FX || ui.page == PG_MST || ui.page == PG_INSERT)) {
            octave(up);
        } else if (ui.page == PG_SEQ || ui.page == PG_EDIT) {
            ui.spage = (uint8_t)clamp(ui.spage + (up ? 1 : -1), 0, npages() - 1);
            ui.spage_t = plat_ms();
        }
    }
    if ((released >> B_EDIT & 1u) && !ui.edit_used) {   /* EDIT tapped: its page, or its next page (or back) */
        if (ui.page != PG_EDIT) {
            ui.page = PG_EDIT;
        } else if (inst) {                /* the next page; after the last, the keys */
            if (ui.epage + 1 < inst_epages(ui.trk)) {
                ui.epage++;
            } else {
                ui.epage = 0;
                ui.page = PG_KEYS;
            }
        } else {
            ui.epage = (uint8_t)(ui.epage + 1 < drum_epages() ? ui.epage + 1 : 0);
        }
    }
    if ((pressed >> B_SEQ & 1u) && !ui.seq_used) {      /* the steps; on an instrument track, again: the 303 generator (the page of it
                                           * shown last; SELECT its other), then back to the keys */
        if (ui.page == PG_PICK)
            pick_leave();
        else if (!inst || (ui.page != PG_SEQ && ui.page != PG_GEN))
            ui.page = PG_SEQ;
        else
            ui.page = (uint8_t)(ui.page == PG_SEQ ? PG_GEN : PG_KEYS);
        ui.held_step = -1;
    }
    if (pressed >> B_ENV & 1u) {          /* the drums' MUTE page; an instrument's envelopes (amp, filter, then the keys) */
        if (!inst) {
            page_button(PG_MUTE);
        } else if (ui.page != PG_ENV) {
            ui.page = PG_ENV;
        } else if (ui.envpage + 1 < NENVPAGE) {
            ui.envpage++;
        } else {
            ui.envpage = 0;
            ui.page = PG_KEYS;
        }
    }
    if (pressed >> B_HOME & 1u)           /* HOME: the patterns, then the song, then back */
        ui.page = (uint8_t)(ui.page == PG_HOME ? PG_SONG : ui.page == PG_SONG ? PG_SETUP
                            : ui.page == PG_SETUP ? home_page(ui.trk) : PG_HOME);
    if (pressed >> B_GLO & 1u) {          /* the mixer: LEVEL, PAN, REVERB, DELAY; the master bus's pages; then back */
        if ((btn >> B_REC & 1u) && rec.state == RS_IDLE) {   /* REC + GLO: the track's part erased, to record again */
            ui.rec_used = ui.rec_shown = 1;
            part_erase();
            ui.redraw = 1;
        } else if (btn >> B_SEL & 1u) {   /* SEL + GLO: the master bus, at once */
            ui.sel_used = 1;
            ui.page = PG_MST;
            ui.mstpage = 0;
        } else if (ui.page == PG_MIX) {
            if (ui.mixpage < 3) {
                ui.mixpage++;
            } else {
                ui.mixpage = 0;
                ui.page = PG_MST;
                ui.mstpage = 0;
            }
        } else if (ui.page == PG_MST) {
            if (ui.mstpage < NMSTPAGE - 1) {
                ui.mstpage++;
            } else {
                ui.mstpage = 0;
                ui.page = (uint8_t)home_page(ui.trk);
            }
        } else {
            ui.page = PG_MIX;
        }
    }
    if (pressed >> B_PLAY & 1u) {
        if (sq.playing) {
            seq_stop();
            if (rec.pending)              /* a recording made while playing goes into the bank now */
                rec_store();
            if (ui.save_pending) {
                ui.save_pending = 0;
                do_save();
            }
        } else if (ck.count > 0) {        /* PLAY in the count-in: no start */
            ck.count = 0;
            ck.pre = 0;
            ck.pre_pads = 0;
            app_say("COUNT-IN", "OFF");
        } else if (ui.rec && countin_bars()) {   /* recording: the count-in, then the sequencer (app_render) */
            uint32_t f = plat_lock();
            song_from = (int8_t)(ui.page == PG_SONG ? ui.scur : 0);
            sq.clock = 0;
            ck.to_next = 0.0f;
            ck.beat = 0;
            ck.pre = 0;
            ck.pre_pads = 0;
            ck.count = (int32_t)(bar_samples() * (float)countin_bars());
            plat_unlock(f);
        } else {
            song_from = (int8_t)(ui.page == PG_SONG ? ui.scur : 0);
            seq_start();
        }
    }
    if ((released >> B_OCTUP & 1u) && (perf.want >> PF_TAPE & 1u))
        pf_release(PF_TAPE);
    if (pressed >> B_REC & 1u) {
        ui.rec_used = 0;
        ui.rec_t = plat_ms();
    }
    if (released >> B_REC & 1u) {
        if (ui.rec_shown)
            ui.redraw = 1;
        ui.rec_shown = 0;
    }
    if ((released >> B_REC & 1u) && !ui.rec_used) {   /* REC tapped: recording on / off (held + SEL: sampling) */
        ui.rec ^= 1u;
        ui.srec_held = ui.srec_n = 0;
        app_say("RECORD", ui.rec ? "ON" : "OFF");
    }
    if ((released >> B_SEL & 1u) && !ui.sel_used)
        app_say("SEL + PAD KEYS 1-4", "PICK A TRACK");
    if ((released >> B_SAVE & 1u) && !ui.save_used)
        do_save();
    else if ((released >> B_SAVE & 1u) && ui.save_slot >= 0)   /* SAVE held + PRESETS, let go: a user preset */
        save_user();
    if (pressed >> B_LFO & 1u) {          /* every track's LFO: its pages (an instrument's: LFO 2, MATRIX too), then back */
        if (ui.page != PG_LFO) {
            ui.page = PG_LFO;
            if (ui.lfopage >= lfo_pages(ui.trk))
                ui.lfopage = 0;
        } else if (ui.lfopage + 1 < lfo_pages(ui.trk)) {
            ui.lfopage++;
        } else {
            ui.lfopage = 0;
            ui.page = (uint8_t)home_page(ui.trk);
        }
    }
    if (pressed >> B_ARP & 1u) {          /* T2..T4's arpeggiator */
        if (!inst) {
            app_say("ARP", "ON T2-T4");
        } else if (ui.page == PG_PICK) {
            pick_leave();
        } else if (ui.page == PG_SEQ && ui.held_step >= 0) {
            pick_open();
        } else if (ui.page != PG_ARP) {
            ui.page = PG_ARP;
        } else if (!ui.arppage) {
            ui.arppage = 1;
        } else {
            ui.arppage = 0;
            ui.page = PG_KEYS;
        }
    }
    if (pressed >> B_FX & 1u) {           /* the FX: REVERB, DELAY, DELAY 2, FILTER, then back */
        if ((btn >> B_SEL & 1u) && inst) {
            ui.sel_used = 1;
            ui.page = ui.page == PG_INSERT ? (uint8_t)home_page(ui.trk) : PG_INSERT;
        } else if (ui.page == PG_INSERT || ui.page == PG_COLOR) {
            ui.page = (uint8_t)home_page(ui.trk);
        } else if (ui.page != PG_FX) {
            ui.page = PG_FX;
        } else if (ui.fxpage < NFXPAGE - 1) {
            ui.fxpage++;
        } else {
            ui.fxpage = 0;
            ui.color_page = 0;
            ui.page = PG_COLOR;
        }
    }

    /* keys */
    kp = keys & ~ui.keys;
    kr = ~keys & ui.keys;
    for (k = 0; k < 16u; k++) {           /* white keys */
        uint32_t key = WK[k], bit = 1u << key;
        if (kr & bit) {
            if (ui.scene_key == (int)k) {
                ui.scene_key = -1;
                ui.redraw = 1;
            }
            if (ui.held_step >= 0 && (int)k == ui.held_key) {
                if (inst)
                    istep_up((int)k);
                else
                    step_up((int)k);      /* a held step let go */
            }
            note_up((int)key);
            continue;
        }
        if (!(kp & bit))
            continue;
        if (ui.btn >> B_SAVE & 1u) {      /* SAVE held: the pattern copied to this slot */
            ui.save_used = 1;
            pattern_copy_to((int)k);
        } else if (ui.page == PG_SONG) {
            song_key((int)k);
        } else if (ui.page == PG_HOME) {
            int tk = home_trkkey();
            if (tk >= 0) {                /* a track key held: that track's clip */
                ui.tkey_used |= (uint8_t)(1u << tk);
                clip_launch(tk, (int)k);
            } else {
                pattern_pick((int)k);
                ui.scene_key = (int8_t)k; /* (held: K1, K2 its follow action) */
            }
        } else if (!inst) {
            step_down((int)k);
        } else if (ui.page == PG_PICK) {
            pick_toggle((int)key);
        } else if (ui.page == PG_SEQ) {
            istep_down((int)k);
        } else {
            note_down((int)key);
        }
    }
    for (k = 0; k < 11u; k++) {           /* black keys: the pads, tracks, step pages or notes */
        uint32_t key = BK[k], bit = 1u << key;
        if (kr & bit) {
            if (k >= NTRK && (perf.want >> (k - NTRK) & 1u) && k - NTRK < NPF) {   /* a perform key let go */
                pf_release((int)k - NTRK);
                continue;
            }
            if (k < NTRK && (ui.tkey_down >> k & 1u)) {   /* HOME: a track key let go: a tap mutes */
                ui.tkey_down &= (uint8_t)~(1u << k);
                if (!(ui.tkey_used >> k & 1u) && ui.page == PG_HOME)
                    track_mute((int)k);
                ui.tkey_used &= (uint8_t)~(1u << k);
                continue;
            }
            note_up((int)key);
            continue;
        }
        if (!(kp & bit))
            continue;
        if (!inst && (ui.btn >> B_ENV & 1u) &&
            (ui.page == PG_SEQ || ui.page == PG_EDIT || ui.page == PG_MUTE)) {
            /* ENV + a black key directly mutes that lane, including when
             * ENV was pressed from MUTE and has just returned to the editor. */
            pad_pick((int)k);
            mute_toggle((int)k);
        } else if ((ui.btn >> B_SEL & 1u) && k < NTRK) {   /* SEL held + pad keys 1-4: the track */
            ui.sel_used = 1;
            track_pick((int)k);
            inst = ui.trk != TR_DRUM;
        } else if (ui.page == PG_HOME) {  /* a track key: held, a modifier (a clip, OCT- stop); tapped, mute */
            if (k < NTRK) {
                ui.tkey_down |= (uint8_t)(1u << k);
                ui.tkey_used &= (uint8_t)~(1u << k);
            } else if (k - NTRK < NPF) {  /* black 5-11: the perform keys, while held */
                pf_press((int)k - NTRK);
            }
        } else if (ui.page == PG_SONG) {
            if (k < NTRK)
                track_mute((int)k);
        } else if (!inst) {
            pad_pick((int)k);
            if (ui.page == PG_MUTE)
                mute_toggle((int)k);
            else
                pad_play((int)k, touch_vel(key));
        } else if (ui.page == PG_PICK) {
            pick_toggle((int)key);
        } else if (ui.page == PG_SEQ) {   /* black keys 1-8: the step page */
            if ((int)k < npages()) {
                ui.spage = (uint8_t)k;
                ui.spage_t = plat_ms();
            }
        } else {
            note_down((int)key);
        }
    }

    /* encoders */
    if ((e = plat_enc(EN_PRESET)) != 0) { /* PRESETS: the kit, or the track's sound; SAVE held: where to save it */
        if (ui.btn >> B_SAVE & 1u) {
            ui.save_used = 1;
            save_target(e);
        } else if (inst) {
            preset_turn(e);
        } else {
            kit_turn(e);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0)     /* ALGORITHM: the track */
        track_pick(ui.trk + e);
    inst = ui.trk != TR_DRUM;
    if ((e = plat_enc(EN_SELECT)) != 0 && ui.page != PG_PICK) { /* SELECT: a held step's chance, the cursor, or the page within the page */
        if (ui.page == PG_COLOR) {
            ui.color_page = (uint8_t)clamp(ui.color_page+e,0,1);
        } else if (inst && ui.page == PG_INSERT) {
            insert_select(ui.trk, e);
        } else if (ui.held_step >= 0 && !(inst && (ui.page == PG_EDIT || ui.page == PG_ENV || ui.page == PG_LFO))) {
            /* (there: pages to lock on) */
            if (inst)
                iheld_chance(e);
            else
                held_chance(e);
        } else if (!inst && ui.page == PG_EDIT) {
            ui.epage = (uint8_t)clamp(ui.epage + e, 0, drum_epages() - 1);
        } else if (inst && ui.page == PG_EDIT) {
            ui.epage = (uint8_t)clamp(ui.epage + e, 0, inst_epages(ui.trk) - 1);
        } else if (inst && ui.page == PG_ENV) {
            ui.envpage = (uint8_t)clamp(ui.envpage + e, 0, NENVPAGE - 1);
        } else if (ui.page == PG_SONG) {
            ui.scur = (uint8_t)clamp(ui.scur + e, 0, proj.song_n);
        } else if (ui.page == PG_MIX || ui.page == PG_MST) {   /* one row of pages: the mixer's, the master bus's */
            int g = clamp((ui.page == PG_MIX ? ui.mixpage : 4 + ui.mstpage) + e, 0, 4 + NMSTPAGE - 1);
            ui.page = g < 4 ? PG_MIX : PG_MST;
            if (g < 4)
                ui.mixpage = (uint8_t)g;
            else
                ui.mstpage = (uint8_t)(g - 4);
        } else if (ui.page == PG_FX) {
            ui.fxpage = (uint8_t)clamp(ui.fxpage + e, 0, NFXPAGE - 1);
        } else if (ui.page == PG_LFO) {
            ui.lfopage = (uint8_t)clamp(ui.lfopage + e, 0, lfo_pages(ui.trk) - 1);
        } else if (inst && ui.page == PG_ARP) {
            ui.arppage = (uint8_t)clamp(ui.arppage + e, 0, 1);
        } else if (inst && ui.page == PG_GEN) {
            ui.genpage = (uint8_t)clamp(ui.genpage + e, 0, NGENPAGE - 1);
        } else if (ui.page == PG_HOME && (ui.btn >> B_SEL & 1u) && sq.playing) {
            ui.sel_used = 1;
            nudge(e);
        } else if (ui.page == PG_HOME) {
            pattern_pick(clamp(proj.pat + e, 0, NPAT - 1));
        } else if (inst && ui.page == PG_KEYS) {
            char t[8];
            ui.cursor[ui.trk] = (uint8_t)clamp(ui.cursor[ui.trk] + e, 0, *part_len(ui.trk) - 1);
            ui.srec_held = ui.srec_n = 0;
            int_text(ui.cursor[ui.trk] + 1, t);
            app_say("STEP", t);
        } else {
            ui.spage = (uint8_t)clamp(ui.spage + e, 0, npages() - 1);
            ui.spage_t = plat_ms();
        }
        ui.redraw = 1;
    }
    for (k = 0; k < 4u; k++)
        if ((e = plat_enc(EN_K1 + (int)k)) != 0 && ui.page != PG_PICK)
            knob((int)k, e);

    if (keys != ui.keys || pressed || released)
        ui.redraw = 1;
    ui.keys = keys;
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------- lights --- */
#ifdef PLAT_HOST
static uint16_t lit_white, dim_white;     /* the white keys as last lit and glowing, a bit a key (the tests' wlit, wdim) */
#endif

static void lights(void)
{
    uint32_t bk = 0, wk = 0, dimk = 0, b = 0, k;
    int tr = ui.trk;
    if (ui.page == PG_SONG) {             /* the cursor's entry's pattern lit, the song's others dim */
        for (k = 0; k < proj.song_n; k++)
            dimk |= 1u << WK[proj.song[k].pat];
        if (ui.scur < proj.song_n)
            wk |= 1u << WK[proj.song[ui.scur].pat];
        for (k = 0; k < NTRK; k++)
            if (!(proj.tmute >> k & 1u))
                bk |= 1u << BK[k];
    } else if (ui.page == PG_HOME) {
        int tk = home_trkkey();
        for (k = 0; k < 16u; k++) {       /* a track key held: its clips (the one playing lit); else the scenes */
            int lit = tk >= 0 ? clip[tk] == (int)k : (int)k == proj.pat;
            int q = tk >= 0 ? qclip[tk] == (int)k : sq.queued == (int)k;
            int has = tk < 0 ? !pattern_empty(&proj.p[k]) : tk == TR_DRUM ? !dpart_empty(&proj.p[k].d)
                                                                       : !ipart_empty(&proj.p[k].t[tk - 1]);
            if (lit)
                wk |= 1u << WK[k];
            else if (q && (plat_ms() / 150u & 1u))
                wk |= 1u << WK[k];
            else if (has)
                dimk |= 1u << WK[k];
        }
        for (k = 0; k < NTRK; k++)       /* tracks playing lit, muted ones dark */
            if (!(proj.tmute >> k & 1u))
                bk |= 1u << BK[k];
        for (k = 0; k < NPF; k++)        /* the perform keys: dim, lit while held */
            if (perf.want >> k & 1u)
                bk |= 1u << BK[NTRK + k];
            else
                dimk |= 1u << BK[NTRK + k];
    } else if (tr == TR_DRUM) {
        dpart_t *p = dcur();
        int st = sq.t[TR_DRUM].step, accent = (int)(ui.btn >> B_SEQ & 1u);   /* SEQ held: the accents */
        const uint32_t *row = accent ? p->acc : p->hit[ui.pad];
        for (k = 0; k < 16u; k++) {
            int s = ui.spage * 16 + (int)k;
            if (s >= p->len)
                continue;
            if (bit_get(row, s))
                wk |= 1u << WK[k];
            else if (!(k & 3u))          /* the beats (steps 1, 5, 9, 13 of the bank) glow when empty */
                dimk |= 1u << WK[k];
            if (sq.playing && s == st)
                wk ^= 1u << WK[k];      /* the playhead: a step lit goes out, a dark one lights */
        }
        for (k = 0; k < 11u; k++) {
            if (ui.page == PG_MUTE) {
                if (!(p->mute >> k & 1u))
                    bk |= 1u << BK[k];  /* playing pads lit, muted ones dark */
            } else if ((int)k == ui.pad) {
                bk |= 1u << BK[k];
            } else if (drum_row_used(p->hit[k])) {
                dimk |= 1u << BK[k];
            }
        }
    } else if (ui.page == PG_SEQ) {
        ipart_t *p = ipart(tr);
        int st = sq.t[tr].step;
        for (k = 0; k < 16u; k++) {
            int s = ui.spage * 16 + (int)k;
            if (s >= p->len)
                continue;
            if (p->st[s].note[0])
                wk |= 1u << WK[k];
            if (sq.playing && s == st)
                wk ^= 1u << WK[k];
        }
        for (k = 0; k < NSTEPS / 16u; k++) /* black keys 1-8: the step pages */
            if ((int)k == ui.spage)
                bk |= 1u << BK[k];
            else if ((int)k < npages())
                dimk |= 1u << BK[k];
    } else if (ui.page == PG_PICK) {      /* the physical keys and drawing both show selected pitches */
        const nstep_t *s = &proj.p[ui.pick_clip].t[tr - 1].st[ui.pick_step];
        for (k = 0; k < NKEYS; k++)
            if (pick_is_selected(s, pick_pitch((int)k)))
                wk |= 1u << k;
        dimk = 1u << 7 | 1u << 19;        /* the Cs anchor the pitch window */
    } else {                              /* the keys: held ones lit, the Cs dim */
        wk = ui.keys & 0x7FFFFFFu;
        dimk = 1u << 7 | 1u << 19;
    }
    if (ui.page != PG_KEYS)
        b |= 1u << (ui.page == PG_SEQ || ui.page == PG_GEN || ui.page == PG_PICK ? B_SEQ : ui.page == PG_EDIT ? B_EDIT : ui.page == PG_MUTE || ui.page == PG_ENV ? B_ENV
                    : ui.page == PG_LFO ? B_LFO : ui.page == PG_ARP ? B_ARP : ui.page == PG_FX || ui.page == PG_INSERT || ui.page == PG_COLOR ? B_FX
                    : ui.page == PG_HOME || ui.page == PG_SONG || ui.page == PG_SETUP ? B_HOME : B_GLO);
    if (sq.playing)
        b |= LED_PLAY_GREEN;
    if (ui.rec)
        b |= 1u << B_REC;
    if (rec.state == RS_ARMED || rec.state == RS_REC) {   /* sampling: SEL and REC blink (fast while recording) */
        if (plat_ms() / (rec.state == RS_REC ? 80u : 250u) & 1u)
            b |= 1u << B_SEL | 1u << B_REC;
        else
            b &= ~(1u << B_REC);
    }
    if (dirty)
        b |= 1u << B_SAVE;
    b |= ui.btn;
    plat_lights(b, wk | bk, 0, dimk & ~(wk | bk), 1);
#ifdef PLAT_HOST
    lit_white = dim_white = 0;
    for (k = 0; k < 16u; k++) {
        if (wk >> WK[k] & 1u)
            lit_white |= (uint16_t)(1u << k);
        else if (dimk >> WK[k] & 1u)
            dim_white |= (uint16_t)(1u << k);
    }
#endif
}

/* ------------------------------------------------------------------- screen --- */
/* GhettoMachine's look (0.2): bright colour blocks, clear controls, meters everywhere. The page and the knob strip are
 * the section's colour (the track's; cream for HOME, SONG and the mixer; cyan for FX; rose for the master bus), drawn
 * on in ink; the band above and the knob cards are dark, in that colour. Top to bottom:
 *   the band (240 x 44, y 0): big, what this is; then the page and what it's on; the pattern and the tempo; the
 *     page's dots; the output's two meters (before MASTER) at the right edge. A change shows here, large, a moment.
 *   the page (240 x 132, y 44): its picture: steps, curves, shapes, meters.
 *   the knob strip (240 x 64, y 176): four cards, K1..K4 as the knobs sit: the name, the value, a gauge (from the
 *     middle for a two-way knob, in steps for a switch). The card of the knob just turned is ringed in white; a
 *     lock's card is cream; a card that only reads (a meter) is grey.
 * (attic/screen_dark.c keeps the first look: dark pages, cream cards.)
 * draw() draws all three; draw_live() ~30 times a second the band (its meters) and, on a page that moves, the page. */
/* Bong Cult's Dungeon BBS look (since 0.5; github.com/siddholdenried-del/bongcult-dungeon-bbs): an ink-dark canvas,
 * bone text and 1-pixel rules, square corners. Violet marks what is selected (the knob being turned, the page you are
 * on, the selected row) with dark ink on it; pale violet a lock; acid green is only for live readings (the meters).
 * Each section keeps its own colour for its name and for what it draws (hits, notes, curves): the guide keeps
 * instrument colours that mean something. Condensed capitals (Barlow) label; a monospace face (IBM Plex Mono) reads
 * the counters and the line under the title. (attic/screen_dark.c and git keep the earlier looks.) */
#define C_CANVAS RGB(16, 16, 15)
#define C_FIELD RGB(30, 29, 24)               /* a raised field: cards, boxes */
#define C_BONE RGB(222, 217, 185)             /* text, strong rules */
#define C_MUTED RGB(187, 183, 159)            /* secondary text, labels */
#define C_RULE RGB(98, 95, 80)                /* a 1-pixel rule */
#define C_DUSK RGB(50, 48, 41)                 /* the grid, empty steps, a gauge's track */
#define C_VIOLET RGB(141, 83, 237)            /* selected (dark ink on it) */
#define C_FOCUS RGB(194, 154, 252)            /* pale violet: a lock, emphasis */
#define C_METER RGB(163, 237, 88)             /* live readings */
#define C_INK C_CANVAS                        /* dark ink: on violet, bone or a section's colour */
#define C_CREAM C_BONE                        /* HOME, SONG, SETUP and the mixer */
#define C_BANDBG C_CANVAS
#define C_TILE C_FIELD
#define C_TILET C_BONE
#define C_TILET2 C_MUTED
#define C_WELL C_DUSK
#define C_WHITE2 C_BONE
#define C_RECCOL RGB(238, 64, 64)             /* hold REC: recording's page */
#define C_FXCOL RGB(64, 200, 222)
#define C_MSTCOL RGB(255, 98, 128)
#define BODY_Y 44
#define BODY_H 132
#define STRIP_Y 176

static uint16_t mix565(uint16_t a, uint16_t b, int t)   /* a toward b by t / 256 */
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    t = t < 0 ? 0 : t > 256 ? 256 : t;
    return (uint16_t)(((ar + (br - ar) * t / 256) << 11) | ((ag + (bg - ag) * t / 256) << 5) | (ab + (bb - ab) * t / 256));
}

/* the page's colours, from the section's (theme(), before each drawing) */
static struct {
    uint16_t col;                         /* the section's colour: the page's ground */
    uint16_t panel;                       /* a box on it, a little darker */
    uint16_t grid;                        /* lines, empty steps */
    uint16_t txt2;                        /* labels */
    uint16_t fg;                          /* what's drawn: ink */
    uint16_t hi;                          /* what stands out: bone */
    uint16_t red;                         /* an alarm that reads on it */
    uint16_t txt;                         /* text on the page: bone */
    uint16_t ink;                         /* what a shade of the ground leans to (mix565(C_BODY, TH_INK, n)): bone */
} th;
#define C_BODY th.col
#define C_PANEL th.panel
#define C_GRID th.grid
#define C_TXT2 th.txt2
#define TH_FG th.fg
#define TH_HI th.hi
#define TH_RED th.red
#define TH_TXT th.txt
#define TH_INK th.ink

static int lum565(uint16_t c)             /* 0..255 */
{
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    return (r * 54 + g * 183 + b * 19) >> 8;
}

static void theme(uint16_t col)          /* the page: the canvas, drawn on in the section's colour, written in bone */
{
    th.col = C_CANVAS;
    th.panel = C_FIELD;
    th.grid = C_DUSK;
    th.txt2 = C_MUTED;
    th.fg = col;
    th.hi = C_BONE;
    th.red = RGB(240, 84, 72);
    th.txt = C_BONE;
    th.ink = C_BONE;
}

static int rec_page(void)                 /* REC held a moment (or used as a modifier): recording's page */
{
    return ui.rec_shown && (ui.btn >> B_REC & 1u) && rec.state == RS_IDLE;
}

static uint16_t sec_col(void)             /* the section's colour */
{
    if (rec_page())
        return C_RECCOL;
    switch (ui.page) {
    case PG_HOME: case PG_SONG: case PG_MIX: case PG_SETUP: return C_CREAM;
    case PG_FX: return C_FXCOL;
    case PG_MST: return C_MSTCOL;
    default: return trk_rgb(ui.trk);
    }
}

static void rrect(int x, int y, int w, int h, int r, uint16_t c)   /* a box (square corners: r is the old look's) */
{
    (void)r;
    if (w > 0 && h > 0)
        cv_rect(x, y, w, h, c);
}

static void frame(int x, int y, int w, int h, uint16_t c)   /* a 1-pixel rule round a box */
{
    if (w <= 0 || h <= 0)
        return;
    cv_rect(x, y, w, 1, c);
    cv_rect(x, y + h - 1, w, 1, c);
    cv_rect(x, y, 1, h, c);
    cv_rect(x + w - 1, y, 1, h, c);
}

static void fit_(const felucca_font_t *f, const char *s, int maxw, char *out, int cap)   /* s, cut to fit maxw */
{
    int n = 0;
    while (s[n] && n < cap - 1) {
        out[n] = s[n];
        n++;
    }
    out[n] = 0;
    while (n > 0 && text_w(f, out) > maxw)
        out[--n] = 0;
}

#define fit(f, s, w, o) fit_(f, s, w, o, (int)sizeof(o))   /* (o: an array) */

static void upper(char *s)                /* for the big faces, which have no small letters */
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - 32);
}

static int32_t text_r(int32_t xr, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return cv_text(xr - text_w(f, s), y, f, s, c);
}
static int32_t text_c(int32_t xc, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return cv_text(xc - text_w(f, s) / 2, y, f, s, c);
}

static void thick(int x0, int y0, int x1, int y1, uint16_t c)   /* a 2-pixel line */
{
    cv_line(x0, y0, x1, y1, c);
    cv_line(x0, y0 + 1, x1, y1 + 1, c);
}

static void db1(float db, char *s)        /* "-12.3" */
{
    int32_t t = (int32_t)(db * 10.0f + (db < 0.0f ? -0.5f : 0.5f));
    char u[12];
    s[0] = 0;
    if (t < 0) {
        text_cat(s, "-");
        t = -t;
    }
    int_text(t / 10, u);
    text_cat(s, u);
    text_cat(s, ".");
    int_text(t % 10, u);
    text_cat(s, u);
}

static int db_h(float lin, int h, float range)   /* a meter's height: lin over -range..0 dBFS */
{
    float db = um_db(lin);
    int v = db <= -range ? 0 : (int)((db + range) * (float)h / range);
    return v > h ? h : v;
}

/* ---- the band */
static struct {
    uint16_t col;                         /* the section's colour */
    char id[8], title[24], ctx[32];
    uint8_t npg, pg;                      /* the page's dots */
} bd;
static float band_l, band_r;                /* the band's meters, falling */
static uint32_t clip_t;                   /* when the output last reached its top */

static void band_set(uint16_t col, const char *id, const char *title, const char *ctx, int npg, int pg)
{
    bd.col = col;
    bd.id[0] = bd.title[0] = bd.ctx[0] = 0;
    fit(&FONT_L, id, 120, bd.id);
    fit(&FONT_B, title, 200, bd.title);
    fit(&FONT_S, ctx, 200, bd.ctx);
    bd.npg = (uint8_t)npg;
    bd.pg = (uint8_t)pg;
}

static void band(void)
{
    uint16_t col = bd.col, ink = C_BONE, ink2 = C_MUTED, well = C_DUSK;
    uint32_t now = plat_ms();
    char t[32], u[32];
    int32_t x;
    float pl, pr;
    cv_begin(240, 44, C_BANDBG);
    cv_rect(0, 43, 240, 1, C_RULE);       /* the rule under the masthead */
    if (now < ui.msg_until) {             /* a change: what, and its value, large */
        const char *val = ui.msg[1][0] ? ui.msg[1] : ui.msg[0];
        const felucca_font_t *f;
        if (ui.msg[1][0]) {
            t[0] = 0;
            text_cat(t, ui.msg[0]);
            upper(t);
            cv_text(8, 1, &FONT_MS, t, ink2);
        }
        u[0] = 0;
        text_cat(u, val);
        upper(u);
        f = text_w(&FONT_L, u) <= 168 ? &FONT_L : text_w(&FONT_M, u) <= 168 ? &FONT_M : &FONT_B;
        fit(f, u, 168, t);
        cv_text(7, f == &FONT_L ? 11 : f == &FONT_M ? 16 : 22, f, t, col);
    } else {
        int longid = bd.id[0] && bd.id[1] && bd.id[2] && bd.id[3] && bd.id[4];   /* (no libc: 5 letters or more) */
        x = (longid ? cv_text(7, 10, &FONT_M, bd.id, col) : cv_text(7, 5, &FONT_L, bd.id, col)) + 8;
        fit(&FONT_B, bd.title, 170 - x, t);
        cv_text(x, 4, &FONT_B, t, ink);
        {   /* the line under it in the monospace face; the condensed one where that would cut it */
            const felucca_font_t *cf = text_w(&FONT_MS, bd.ctx) <= 170 - x ? &FONT_MS : &FONT_S;
            fit(cf, bd.ctx, 170 - x, t);
            cv_text(x, cf == &FONT_MS ? 23 : 21, cf, t, ink2);
        }
        if (bd.npg > 1) {                 /* the section's pages: the one shown violet */
            int i;
            for (i = 0; i < bd.npg; i++)
                cv_rect(8 + i * 9, 37, 6, 4, i == bd.pg ? C_VIOLET : C_DUSK);
        }
    }
    /* the pattern (and the next, the song's entry), the tempo, the transport */
    {
        int home = ui.page == PG_HOME || ui.page == PG_SONG, bp = home ? proj.pat : clip_of(ui.trk);
        int q = sq.queued >= 0 ? sq.queued : (!home && qclip[ui.trk] >= 0) ? qclip[ui.trk] : -1;
        t[0] = 'P';
        two_digits(bp + 1, t + 1);
        if (!home && clip[ui.trk] < 0) {
            t[0] = '-';
            t[1] = '-';
            t[2] = 0;
        }
        if (q >= 0 && (now / 250u & 1u)) {
            t[0] = '>';
            two_digits(q + 1, t + 1);
        }
    }
    text_r(220, 3, &FONT_MB, t, ink);
    int_text(proj.bpm10 / 10, t);
    if (srt.pos >= 0 && sq.playing) {
        u[0] = 'S';
        two_digits(srt.pos + 1, u + 1);
        text_cat(u, " ");
        text_cat(u, t);
        text_r(220, 23, &FONT_MS, u, ink2);
    } else {
        text_r(220, 23, &FONT_MS, t, ink2);
    }
    if (sq.playing) {                     /* a triangle */
        int j;
        for (j = 0; j < 9; j++)
            cv_rect(176, 25 + j, j < 5 ? j + 1 : 9 - j, 1, col);
    }
    if (ui.rec)
        cv_rect(176, 8, 8, 8, C_RED);
    /* the output, each side, -48..0 dBFS */
    fx_out_sides(&pl, &pr);
    band_l = pl > band_l ? pl : band_l * 0.93f;
    band_r = pr > band_r ? pr : band_r * 0.93f;
    if (pl > 0.97f || pr > 0.97f)
        clip_t = now;
    {
        int hl = db_h(band_l, 36, 48.0f), hr = db_h(band_r, 36, 48.0f), clip = now - clip_t < 1500u && clip_t;
        cv_rect(225, 4, 5, 36, well);
        cv_rect(232, 4, 5, 36, well);
        cv_rect(225, 40 - hl, 5, hl, C_METER);   /* (live: acid green) */
        cv_rect(232, 40 - hr, 5, hr, C_METER);
        cv_rect(224, 4 + 36 * 6 / 48, 14, 1, C_BANDBG);   /* -6 dB */
        if (clip) {
            cv_rect(225, 4, 5, 3, C_RED);
            cv_rect(232, 4, 5, 3, C_RED);
        }
    }
    cv_blit(0, 0);
}

/* ---- the knob strip */
typedef struct {
    char label[12], text[16];
    int16_t v, lo, hi, mid;               /* the gauge: v in lo..hi; mid > lo: drawn from mid */
    uint16_t acc;                         /* its colour */
    uint8_t on, lock, ro;                 /* there; a lock's value; a reading, not a knob */
} card_t;
static card_t cards[4];

static void card_put(card_t *c, const char *label, const char *text, int v, int lo, int hi, int mid)
{
    int i;
    for (i = 0; label[i] && i < (int)sizeof c->label - 1; i++)
        c->label[i] = label[i];
    c->label[i] = 0;
    upper(c->label);
    c->text[0] = 0;
    fit(&FONT_B, text, 200, c->text);
    c->v = (int16_t)v;
    c->lo = (int16_t)lo;
    c->hi = (int16_t)hi;
    c->mid = (int16_t)mid;
    c->on = 1;
    c->lock = c->ro = 0;
    c->acc = bd.col;
}

static void strip(void)                   /* four cells in one hard-edged grid, K1..K4 */
{
    int k;
    uint32_t now = plat_ms();
    cv_begin(240, 64, C_CANVAS);
    cv_rect(0, 0, 240, 1, C_RULE);
    for (k = 1; k < 4; k++)
        cv_rect(k * 60 - 1, 0, 1, 64, C_RULE);
    for (k = 0; k < 4; k++) {
        const card_t *c = &cards[k];
        int x = k * 60, y = 1, w = 59, h = 63, hot = ui.knob_k == k && now - ui.knob_t < 1300u && !c->ro;
        int gx = x + 5, gy = y + 46, gw = w - 10, gh = 7, range = c->hi - c->lo;
        /* the knob just turned: violet, dark ink on it; a lock's value: pale violet; a reading: on the canvas */
        uint16_t bg = hot ? C_VIOLET : c->ro ? C_CANVAS : C_FIELD;
        uint16_t lab = hot ? C_INK : c->lock ? C_FOCUS : C_MUTED, val = hot ? C_INK : c->lock ? C_FOCUS : C_BONE;
        uint16_t fill = hot ? C_INK : c->lock ? C_FOCUS : c->ro ? C_METER : c->acc;
        uint16_t well = hot ? mix565(C_VIOLET, C_INK, 90) : C_DUSK;
        char t[32];
        const felucca_font_t *f;
        if (!c->on)
            continue;
        cv_rect(x, y, w, h, bg);
        f = text_w(&FONT_XS, c->label) <= w - (c->lock ? 16 : 9) ? &FONT_XS : &FONT_T;   /* a long name: the small face */
        fit(f, c->label, w - (c->lock ? 16 : 9), t);
        cv_text(x + 5, y + (f == &FONT_T ? 4 : 2), f, t, lab);
        if (c->lock) {                    /* L: this step's own value */
            cv_rect(x + w - 12, y + 3, 9, 11, hot ? C_INK : C_FOCUS);
            cv_text(x + w - 11, y + 1, &FONT_XS, "L", hot ? C_VIOLET : C_INK);
        }
        f = text_w(&FONT_B, c->text) <= w - 8   ? &FONT_B   /* a long value: smaller faces before it is cut */
            : text_w(&FONT_S, c->text) <= w - 8  ? &FONT_S
            : text_w(&FONT_XS, c->text) <= w - 8 ? &FONT_XS
                                                 : &FONT_T;
        fit(f, c->text, w - 8, t);
        cv_text(x + 5, y + (f == &FONT_T ? 21 : 18), f, t, val);
        if (range <= 0)
            continue;
        if (range <= 7 && c->mid <= c->lo) {   /* a switch: a step each */
            int i, sw = (gw - (range) * 2) / (range + 1);
            for (i = 0; i <= range; i++)
                cv_rect(gx + i * (sw + 2), gy, sw, gh, c->lo + i == c->v ? fill : well);
            continue;
        }
        cv_rect(gx, gy, gw, gh, well);
        {
            int v = c->v < c->lo ? c->lo : c->v > c->hi ? c->hi : c->v, px = gx + (v - c->lo) * gw / range;
            if (c->mid > c->lo) {             /* two-way: from the middle */
                int mx = gx + (c->mid - c->lo) * gw / range;
                if (px >= mx)
                    cv_rect(mx, gy, px - mx + 1, gh, fill);
                else
                    cv_rect(px, gy, mx - px + 1, gh, fill);
                cv_rect(mx, gy - 2, 1, gh + 4, hot ? C_INK : C_BONE);
            } else if (px > gx) {
                cv_rect(gx, gy, px - gx < 2 ? 2 : px - gx, gh, fill);
            }
        }
    }
    cv_blit(0, STRIP_Y);
}

/* ---- each kind of page's four knobs */
enum { KD_SEQ, KD_HELD, KD_DRUM, KD_KIT, KD_MODEL, KD_INST, KD_ENV, KD_LFO, KD_ARP, KD_MIX, KD_FX, KD_MST, KD_SONG, KD_REC, KD_PF,
       KD_SETUP, KD_FOLLOW, KD_GEN, KD_INSERT, KD_COLOR };

static int fx_mid(int i)                  /* a two-way FX knob's middle, or -1 */
{
    return i == FXP_EQ_LOW || i == FXP_EQ_MID || i == FXP_EQ_HIGH ? 24 : i == FXP_ST_BAL || i == FXP_FILTER ? 64 : -1;
}

static int kinfo(int kind, int page, int k, card_t *c)   /* knob k on page page of that kind: 0 if none */
{
    int tr = ui.trk, max, v;
    char t[32];
    t[0] = 0;
    c->on = 0;
    switch (kind) {
    case KD_COLOR: {
        int id = color_index(k);
        if (id < 0) return 0;
        v = proj.color[tr].v[id];
        int_text(v * 100 / 127,t);
        text_cat(t, "%");
        card_put(c, COLOR_LABEL[id], id == COL_ON ? (v ? "ON" : "BYPASS") : t,
                 v, 0, id == COL_ON ? 1 : 127, -1);
        return 1;
    }
    case KD_INSERT: {
        const track_insert_param_t *p = &proj.insert[tr-1];
        if (!p->mode) return 0;
        insert_text(p, k, t);
        card_put(c, track_insert_label(p->mode, k), t, p->v[k], 0, 127, p->mode == TI_SHIFT && k < 3 ? 64 : -1);
        return 1;
    }
    case KD_SEQ: {
        static const char *const N[4] = {"LENGTH", "RATE", "SWING", "TEMPO"};
        if (k == 0) {
            int_text(*part_len(tr), t);
            card_put(c, N[0], t, *part_len(tr), 1, NSTEPS, -1);
        } else if (k == 1) {
            card_put(c, N[1], RATE_NAME[*part_rate(tr)], *part_rate(tr), 0, NRATES - 1, -1);
        } else if (k == 2) {
            int_text(proj.swing, t);
            text_cat(t, "%");
            card_put(c, N[2], t, proj.swing, 50, 75, -1);
        } else {
            int_text(proj.bpm10 / 10, t);
            card_put(c, N[3], t, proj.bpm10 / 10, 60, 240, -1);
        }
        return 1;
    }
    case KD_HELD: {
        const nstep_t *s = &ipart(tr)->st[ui.held_step];
        static const char *const N[4] = {"NOTE", "VELOCITY", "GATE", "CHANCE"};
        if (k == 0) {
            note_text(s->note[0] ? s->note[0] : 60, t);
            if (nstep_count(s) > 1)
                text_cat(t, "+");
            card_put(c, N[0], t, s->note[0], 24, 108, -1);
        } else if (k == 1) {
            int_text(ns_vel(s), t);
            card_put(c, N[1], (s->vel & NS_ACC) ? "ACCENT" : t, ns_vel(s) + ((s->vel & NS_ACC) ? 1 : 0), 1, 128, -1);
        } else if (k == 2) {
            gate_text(s->gate, t);
            card_put(c, N[2], (s->prob & NS_SLIDE) ? "SLIDE" : t, s->gate, 1, GATE_MAX, -1);
        } else {
            int_text(ns_prob(s), t);
            text_cat(t, "%");
            card_put(c, N[3], t, ns_prob(s), 5, 100, -1);
        }
        return 1;
    }
    case KD_DRUM: {
        int idx[NPOTS], n = edit_params(drum_model(ui.pad), ui.pad, idx), i = page * 4 + k, j = -1;
        const x0x_param_t *p;
        const dpart_t *pat = dcur();
        if (i >= n)
            return 0;
        p = drums_param(drum_model(ui.pad), ui.pad, idx[i]);
        v = idx[i] < NPOTS && (ovr_mask[ui.pad] >> idx[i] & 1u) ? ovr_base[ui.pad][idx[i]]
                                                               : drums_get(drum_model(ui.pad), ui.pad, idx[i]);
        if (ui.held_step >= 0 && (j = lock_find(pat, ui.held_step, ui.pad, lk_param(drum_model(ui.pad), idx[i]))) >= 0)
            v = pat->lk[j].val;
        param_text(p, v, t);
        card_put(c, p->name, t, v, 0, p->max, -1);
        c->lock = (uint8_t)(j >= 0);
        return 1;
    }
    case KD_MODEL:
        if (k != 0)
            return 0;
        v = drum_model(ui.pad);
        card_put(c, "MODEL", drums_has_pad(v, ui.pad) ? drums_kit_name(v) : "--", v, 0, KIT_NUM - 1, -1);
        return 1;
    case KD_KIT: {
        const x0x_param_t *p;
        if (k >= drums_nparams(proj.kit, DRUMS_PAD_KIT))
            return 0;
        p = drums_param(proj.kit, DRUMS_PAD_KIT, k);
        v = drums_get(proj.kit, DRUMS_PAD_KIT, k);
        param_text(p, v, t);
        card_put(c, p->name, t, v, 0, p->max, -1);
        return 1;
    }
    case KD_INST:
    case KD_ENV: {
        const track_t *ts = tset(tr);
        int id = kind == KD_ENV ? ENVPAGE_P[page % NENVPAGE][k]
                                : (ts->type == TY_SAMPLER ? SPAGE_P[page % NSPAGE][k] : EPAGE_P[page % NEPAGE][k]);
        if (id >= 0 && ui.held_step >= 0) {
            const ipart_t *ip = ipart(tr);
            int j = ilock_find(ip, ui.held_step, id), mn = syn_param(id)->min;
            v = j >= 0 ? ip->lk[j].val : snd_value(tr, id);
            syn_param_text(id, v, t);
            card_put(c, snd_label(tr, id), t, v, mn, snd_max(tr, id), mn < 0 ? 0 : -1);
            c->lock = (uint8_t)(j >= 0);
        } else if (id == K_TYPE) {
            card_put(c, "TYPE", TYPE_NAME[ts->type], ts->type, 0, NTYPES - 1, -1);
        } else if (id == K_LEVEL) {
            int_text(ts->level, t);
            card_put(c, "LEVEL", t, ts->level, 0, 127, -1);
        } else if (id < 0) {
            v = smp_value(tr, id, &max);
            smp_text(tr, id, t);
            card_put(c, smp_label(id), t, v, 0, max, id == K_TUNE ? max / 2 : -1);
        } else {
            int mn = syn_param(id)->min;
            v = snd_value(tr, id);
            syn_param_text(id, v, t);
            card_put(c, snd_label(tr, id), t, v, mn, snd_max(tr, id), mn < 0 ? 0 : -1);
        }
        return 1;
    }
    case KD_LFO:
        if (tr != TR_DRUM && page >= LP_LFO2) {   /* LFO 2, MATRIX: sound parameters (a held step's locks shown) */
            static const char *const ML[4] = {"SLOT", "SOURCE", "DEST", "AMOUNT"};
            int id = mod_id(page, k), mn, j = -1;
            if (id < 0) {
                t[0] = 0;
                text_cat(t, "MOD ");
                int_text(ui.mslot + 1, t + 4);
                card_put(c, ML[0], t, ui.mslot, 0, SYN_SLOTS - 1, -1);
                return 1;
            }
            mn = syn_param(id)->min;
            v = snd_value(tr, id);
            if (ui.held_step >= 0 && (j = ilock_find(ipart(tr), ui.held_step, id)) >= 0)
                v = ipart(tr)->lk[j].val;
            syn_param_text(id, v, t);
            card_put(c, page == LP_MATRIX ? ML[k] : syn_param(id)->name, t, v, mn, syn_param(id)->max, mn < 0 ? 0 : -1);
            c->lock = (uint8_t)(j >= 0);
            return 1;
        }
        v = lfo_value(tr, page, k, &max);
        lfo_text(tr, page, k, t);
        card_put(c, LFO_LABEL[page & 1][k], t, v, 0, max, page == 0 && k == 2 ? 64 : -1);
        return 1;
    case KD_ARP:
        if (tr == TR_DRUM || (v = arp_value(tr, page, k, &max, t)) < 0)
            return 0;
        card_put(c, ARP_LABEL[page][k], t, v, 0, max, -1);
        return 1;
    case KD_MIX:
        mix_text(k, page, t);
        card_put(c, k == TR_DRUM ? "DRUMS" : TRK_NAME[k], t, mix_value(k, page), 0, 127, page == 1 ? 64 : -1);
        c->acc = trk_rgb(k);
        return 1;
    case KD_FX:
    case KD_MST: {
        int i = kind == KD_FX ? FX_PAGE_P[page][k] : MST_PAGE_P[page][k];
        const x0x_param_t *p = i >= 0 ? fx_param(i) : 0;
        if (!p) {
            if (kind == KD_MST && page == 4) {   /* the compressor's reduction, read */
                float gr = fx_gr();
                db1(-gr, t);
                text_cat(t, "dB");
                card_put(c, "REDUCTION", t, (int)(gr * 10.0f), 0, 120, -1);
                c->ro = 1;
                c->acc = C_RED;
                return 1;
            }
            if (kind == KD_MST && page == 6) {   /* the correlation, read */
                float cr = fx_corr();
                int32_t q = (int32_t)(cr * 100.0f + (cr < 0.0f ? -0.5f : 0.5f));
                t[0] = 0;
                text_cat(t, q < 0 ? "-" : "+");
                int_text(q < 0 ? -q / 100 : q / 100, t + 1);
                text_cat(t, ".");
                {
                    char u[4];
                    int a = q < 0 ? -q : q;
                    u[0] = (char)('0' + a / 10 % 10);
                    u[1] = (char)('0' + a % 10);
                    u[2] = 0;
                    text_cat(t, u);
                }
                card_put(c, "PHASE", t, q, -100, 100, 0);
                c->ro = 1;
                c->acc = cr < 0.0f ? C_RED : C_GREEN;
                return 1;
            }
            return 0;
        }
        fx_text(i, t);
        card_put(c, FX_LABEL[i], t, proj.fx[i], 0, p->max, fx_mid(i));
        return 1;
    }
    case KD_SETUP: {
        static const char *const CL[3] = {"OFF", "SEND", "FOLLOW"};
        if (k == 0) {
            card_put(c, "MIDI CLOCK", CL[proj.recq >> 2 & 3u], (int)(proj.recq >> 2 & 3u), 0, 2, -1);
            return 1;
        }
        if (k == 1) {
            card_put(c, "SOUNDS", sounds_pp() ? "OWN" : "SHARED", sounds_pp(), 0, 1, -1);
            return 1;
        }
        return 0;
    }
    case KD_GEN: {                        /* the 303 generator: page 0 ACID, 1 NOTES, 2 LINE */
        gen_t g = gen_view(tr);
        int lo, hi, i = (page % NGENPAGE) * 4 + k;
        if (tr == TR_DRUM)
            return 0;
        v = gen_value(tr, &g, i, &lo, &hi, t);
        card_put(c, GEN_LABEL[i], t, v, lo, hi, -1);
        return 1;
    }
    case KD_FOLLOW:                       /* a scene key held on HOME: its follow action */
        if (k == 0) {
            card_put(c, "FOLLOW", FOLLOW_NAME[proj.follow[page] & 7u], proj.follow[page] & 7, 0, 4, -1);
            return 1;
        }
        if (k == 1) {
            int_text((proj.follow[page] >> 3) + 1, t);
            card_put(c, "BARS", t, (proj.follow[page] >> 3) + 1, 1, 16, -1);
            return 1;
        }
        return 0;
    case KD_PF:                           /* a perform key held: K1 plays it */
        if (k != 0 || page == PF_FILL)
            return 0;
        switch (page) {
        case PF_ROLL: card_put(c, "RATE", ROLL_NAME[perf.roll_rate % 5], perf.roll_rate, 0, 4, -1); break;
        case PF_STUT: card_put(c, "LENGTH", STUT_NAME[perf.stut_len % 3], perf.stut_len, 0, 2, -1); break;
        case PF_FILT:
            t[0] = 0;
            text_cat(t, perf.filt < 60 ? "LP" : perf.filt > 68 ? "HP" : "OFF");
            card_put(c, "FILTER", t, perf.filt, 0, 127, 64);
            break;
        case PF_THROW: int_text(perf.throw_fb, t); card_put(c, "FEEDBACK", t, perf.throw_fb, 0, 127, -1); break;
        case PF_WASH: int_text(perf.wash, t); card_put(c, "SIZE", t, perf.wash, 0, 127, -1); break;
        default: int_text(perf.crush, t); card_put(c, "AMOUNT", t, perf.crush, 0, 127, -1); break;
        }
        return 1;
    case KD_REC:
        if (k == 0) {
            card_put(c, "CLICK", CLICK_NAME[click_mode()], click_mode(), 0, 2, -1);
        } else if (k == 1) {
            card_put(c, "COUNT-IN", COUNT_NAME[countin_bars()], countin_bars(), 0, 2, -1);
        } else if (k == 2) {
            card_put(c, "QUANTIZE", GRID_NAME[*part_rate(tr) % NRATES][recq_grid()], recq_grid(), 0, 2, -1);
        } else {
            int_text(click_level(), t);
            card_put(c, "LEVEL", t, click_level(), 1, 15, -1);
        }
        return 1;
    case KD_SONG: {
        static const char *const N[4] = {"PATTERN", "BARS", "LAUNCH", "SONG"};
        static const char *const LN[3] = {"BAR", "BEAT", "NOW"};
        if (k == 2) {
            card_put(c, N[2], LN[launch_mode()], launch_mode(), 0, 2, -1);
            return 1;
        }
        if (k == 3) {
            card_put(c, N[3], proj.song_on ? "ON" : "OFF", proj.song_on, 0, 1, -1);
            return 1;
        }
        if (ui.scur >= proj.song_n)
            return 0;
        if (k == 0) {
            t[0] = 'P';
            two_digits(proj.song[ui.scur].pat + 1, t + 1);
            card_put(c, N[0], t, proj.song[ui.scur].pat, 0, NPAT - 1, -1);
        } else {
            int_text(proj.song[ui.scur].bars, t);
            card_put(c, N[1], t, proj.song[ui.scur].bars, 1, 16, -1);
        }
        return 1;
    }
    }
    return 0;
}

static void cards_of(int kind, int page)
{
    int k;
    for (k = 0; k < 4; k++)
        if (!kinfo(kind, page, k, &cards[k]))
            cards[k].on = 0;
}

/* every page's knobs of a kind as rows of four small gauges, the page the knobs are on now lit: the sound at a glance */
static void page_map(int kind, int npg, int cur, const char *const *names, int y0, int h)
{
    int p, k, rh = h / (npg ? npg : 1);
    card_t c;
    if (rh > 28)
        rh = 28;
    for (p = 0; p < npg; p++) {
        int y = y0 + p * rh, me = p == cur;
        char t[8];
        if (me) {                         /* the page the knobs are on: a field, marked violet */
            rrect(2, y, 236, rh - 2, 4, C_PANEL);
            cv_rect(2, y, 3, rh - 2, C_VIOLET);
        }
        if (names)
            fit(&FONT_XS, names[p], 40, t);
        else {
            t[0] = 'P';
            int_text(p + 1, t + 1);
        }
        cv_text(6, y + (rh - 2) / 2 - 8, &FONT_XS, t, me ? TH_TXT : C_TXT2);
        for (k = 0; k < 4; k++) {
            int x = 48 + k * 47, w = 42, gy = y + (rh - 2) / 2 - 2, range;
            if (!kinfo(kind, p, k, &c))
                continue;
            if (rh >= 26) {                   /* room: each knob's name over its gauge */
                char nm[12];
                fit(&FONT_T, c.label, w, nm);
                cv_text(x, y + 2, &FONT_T, nm, me ? TH_TXT : C_TXT2);
                gy = y + rh - 10;
            }
            range = c.hi - c.lo;
            rrect(x, gy, w, 5, 2, me ? C_GRID : mix565(C_BODY, TH_INK, 50));
            if (range > 0) {
                int v = c.v < c.lo ? c.lo : c.v > c.hi ? c.hi : c.v, px = x + (v - c.lo) * w / range;
                uint16_t col = me ? TH_FG : C_TXT2;
                if (c.mid > c.lo) {
                    int mx = x + (c.mid - c.lo) * w / range;
                    cv_rect(px < mx ? px : mx, gy, (px < mx ? mx - px : px - mx) + 1, 5, col);
                } else {
                    rrect(x, gy, px - x < 4 ? 4 : px - x, 5, 2, col);
                }
            }
        }
    }
}

/* ---- what a track is playing */
static void sample_status(int tr, char *t)   /* a SAMPLER track: its sample, or what sampling is doing */
{
    char u[16];
    t[0] = 0;
    if (rec.trk == tr && rec.state == RS_ARMED) {
        text_cat(t, "WAITING FOR SOUND");
    } else if (rec.trk == tr && rec.state == RS_REC) {
        text_cat(t, "RECORDING ");
        seconds_text(rec.pos * 1000u / (rec.half ? 22050u : 44100u), u);
        text_cat(t, u);
    } else if (!smp_frames[tr]) {
        text_cat(t, "EMPTY: REC + SEL");
    } else {
        seconds_text(smp_frames[tr] * 1000u / smp_rate[tr], t);
        text_cat(t, smp_rate[tr] == 22050u ? "  22K" : "  44K");
        if (rec.pending && rec.trk == tr)
            text_cat(t, "  NOT STORED");
    }
}

static void sound_name(int tr, char *t)   /* the track's sound: the preset it started from (* if changed), or the sample */
{
    const track_t *ts = tset(tr);
    t[0] = 0;
    if (ts->type == TY_SAMPLER) {
        sample_status(tr, t);
        return;
    }
    preset_name(ts->preset, t);
    if (ts->edited)
        text_cat(t, " *");
}

static void draw_wave(int tr, int x0, int y0, int w, int h)   /* the sample: what plays lit, its start, end, loop */
{
    const track_t *ts = tset(tr);
    const uint8_t *d = smp_data[tr];
    uint32_t n = smp_frames[tr], c;
    int mid = y0 + h / 2, xs = x0 + ts->s_start * (w - 1) / 1000, xe = x0 + ts->s_end * (w - 1) / 1000;
    if (!d || n < 2u)
        return;
    cv_rect(x0, mid, w, 1, C_GRID);
    for (c = 0; c < (uint32_t)w; c++) {   /* the loudest of 8 frames in each column */
        uint32_t a = c * n / (uint32_t)w, b = (c + 1u) * n / (uint32_t)w, k, step = (b - a) / 8u + 1u;
        int pk = 0, x = x0 + (int)c;
        for (k = a; k < b; k += step) {
            const uint8_t *p = d + (k >> 1) * 3u;
            int v = (k & 1u) ? (p[1] >> 4 | p[2] << 4) : (p[0] | (p[1] & 0x0F) << 8);
            if (v & 0x800)
                v -= 4096;
            if (v < 0)
                v = -v;
            if (v > pk)
                pk = v;
        }
        pk = pk * (h / 2) / 2048;
        if (pk)
            cv_rect(x, mid - pk, 1, 2 * pk, x >= xs && x <= xe ? TH_FG : C_GRID);
    }
    cv_rect(xs, y0, 2, h, TH_HI);
    cv_rect(xe - 1, y0, 2, h, TH_HI);
    if (ts->s_loop) {
        int xl = x0 + ts->s_lstart * (w - 1) / 1000, y;
        for (y = y0; y < y0 + h; y += 5)
            cv_rect(xl, y, 2, 3, TH_HI);
    }
}

/* ---- page pictures */
static int x_of_hz(float hz, int x0, int w)   /* 20 Hz .. 20 kHz, evenly in octaves */
{
    return x0 + (int)(um_log2(hz / 20.0f) * (float)(w - 1) / 9.966f);
}

static void grid_hz(int x0, int y0, int w, int h)   /* the frequency grid: 100, 1k, 10k */
{
    static const char *const N[3] = {"100", "1k", "10k"};
    static const float F[3] = {100.0f, 1000.0f, 10000.0f};
    int i;
    for (i = 0; i < 3; i++) {
        int x = x_of_hz(F[i], x0, w);
        cv_rect(x, y0, 1, h, C_GRID);
        cv_text(x + 2, y0 + h - 14, &FONT_XS, N[i], C_GRID);
    }
}

/* a 12 dB/oct filter's response (LP 0, BP 1, HP 2) at fc and q, as a filled curve over -36..+18 dB */
static void svf_curve(int type, float fc, float q, int x0, int y0, int w, int h, uint16_t col, int fill)
{
    int i, py = 0;
    for (i = 0; i < w; i++) {
        float f = 20.0f * um_exp2((float)i * 9.966f / (float)(w - 1)), x = f / fc, x2 = x * x;
        float d = (1.0f - x2) * (1.0f - x2) + x2 / (q * q), num = type == 0 ? 1.0f : type == 1 ? x2 / (q * q) : x2 * x2;
        float db = 10.0f * um_log2(num / (d > 1e-9f ? d : 1e-9f)) * 0.30103f;
        int y;
        if (db > 18.0f)
            db = 18.0f;
        if (db < -36.0f)
            db = -36.0f;
        y = y0 + (int)((18.0f - db) * (float)(h - 1) / 54.0f);
        if (fill)
            cv_rect(x0 + i, y, 1, y0 + h - y, mix565(C_BODY, col, 70));
        if (i)
            thick(x0 + i - 1, py, x0 + i, y, col);
        py = y;
    }
    cv_rect(x0, y0 + (int)(18.0f * (float)(h - 1) / 54.0f), w, 1, C_GRID);   /* 0 dB */
}

static float cut_hz(int v) { return 30.0f * um_exp2((float)v * 0.0713295f); }   /* the engine's: 30 Hz .. 16 kHz */

static void pic_filter(int tr)
{
    float fc = cut_hz(snd_value(tr, SP_CUT)), q = 0.6f + (float)snd_value(tr, SP_RES) * (7.0f / 127.0f);
    int fenv = snd_value(tr, SP_FENV), xc;
    uint16_t col = TH_FG;
    char t[16];
    rrect(4, 4, 232, 124, 6, C_PANEL);
    grid_hz(10, 8, 220, 116);
    svf_curve(snd_value(tr, SP_FTYPE), fc, q, 10, 10, 220, 112, col, 1);
    xc = x_of_hz(fc, 10, 220);
    {
        int y;
        for (y = 10; y < 122; y += 6)
            cv_rect(xc, y, 1, 3, TH_HI);
    }
    if (fenv) {                           /* the envelope's sweep: an arrow from the cutoff */
        int xe = x_of_hz(fc * um_exp2((float)fenv * 0.07f), 10, 220);
        if (xe > 228)
            xe = 228;
        if (xe < 12)
            xe = 12;
        thick(xc, 18, xe, 18, TH_HI);
        cv_rect(xe + (xe > xc ? -3 : 0), 15, 4, 8, TH_HI);
    }
    syn_param_text(SP_CUT, snd_value(tr, SP_CUT), t);
    cv_text(xc + 4 > 180 ? xc - 50 : xc + 4, 24, &FONT_S, t, TH_TXT);
}

static void pic_env(int tr, int pg)       /* the envelope's shape: attack, decay, sustain, release (ENV page pg) */
{
    const int8_t *ids = ENVPAGE_P[pg % NENVPAGE];
    int a = snd_value(tr, ids[0]), d = snd_value(tr, ids[1]), s = snd_value(tr, ids[2]), r = snd_value(tr, ids[3]);
    int wa = 6 + a * 56 / 127, wd = 6 + d * 56 / 127, ws = 36, wr = 6 + r * 56 / 127, tot = wa + wd + ws + wr;
    int x0 = 10, w = 220, top = 14, bot = 112, ys = bot - s * (bot - top) / 127, x1, x2, x3, x4, x;
    uint16_t col = TH_FG;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    x1 = x0 + wa * w / tot;
    x2 = x1 + wd * w / tot;
    x3 = x2 + ws * w / tot;
    x4 = x0 + w;
    for (x = x0; x <= x4; x++) {          /* filled under it */
        int y = x <= x1 ? bot - (x - x0) * (bot - top) / (x1 - x0 ? x1 - x0 : 1)
                : x <= x2 ? top + (x - x1) * (ys - top) / (x2 - x1 ? x2 - x1 : 1)
                : x <= x3 ? ys : ys + (x - x3) * (bot - ys) / (x4 - x3 ? x4 - x3 : 1);
        cv_rect(x, y, 1, bot - y, mix565(C_PANEL, col, 60));
    }
    thick(x0, bot, x1, top, col);
    thick(x1, top, x2, ys, col);
    thick(x2, ys, x3, ys, col);
    thick(x3, ys, x4, bot, col);
    cv_rect(x0, bot, w, 1, C_GRID);
    {
        static const char *const N[4] = {"A", "D", "S", "R"};
        int xs[5] = {x0, x1, x2, x3, x4}, i;
        for (i = 0; i < 4; i++) {
            cv_rect(xs[i + 1], top - 4, 1, bot - top + 8, C_GRID);
            text_c((xs[i] + xs[i + 1]) / 2, bot + 1, &FONT_XS, N[i], C_TXT2);
        }
    }
}

static void pic_osc(int tr)               /* the oscillator's wave, two cycles; the second, by MIX */
{
    int wave = snd_value(tr, SP_WAVE), mixv = snd_value(tr, SP_MIX), i, py = 0, py2 = 0;
    uint16_t col = TH_FG;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_rect(10, 66, 220, 1, C_GRID);
    for (i = 0; i < 220; i++) {
        float ph = (float)(i % 110) / 110.0f, v, v2, ph2 = (float)((i * 3 / 2) % 110) / 110.0f;
        switch (wave) {
        case 0: v = 1.0f - 2.0f * ph; break;                                   /* SAW */
        case 1: v = ph < 0.5f ? 0.8f : -0.8f; break;                           /* SQUARE */
        case 2: v = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;    /* TRI */
        case 3: v = um_sin(ph * 6.2831853f); break;                           /* SINE */
        case 4: v = ph < 0.25f ? 0.8f : -0.8f; break;                          /* PWM */
        case 5: v = 1.0f - 2.0f * (ph * 1.6f - (float)(int)(ph * 1.6f)); break;   /* SYNC */
        default: v = (ph < 0.5f ? 0.8f : -0.8f) * 0.5f + (1.0f - 2.0f * ph) * 0.5f; break;   /* SUB */
        }
        v2 = 1.0f - 2.0f * ph2;
        {
            int y = 66 - (int)(v * 50.0f), y2 = 66 - (int)(v2 * 50.0f);
            if (i && mixv > 0 && (i & 1))
                cv_line(10 + i - 1, py2, 10 + i, y2, mix565(C_PANEL, col, 40 + mixv));
            if (i)
                thick(10 + i - 1, py, 10 + i, y, col);
            py = y;
            py2 = y2;
        }
    }
}

static float wave_f(int w, float ph)      /* WAVE 0..4 at a phase (cycles), -1..1, as pic_osc draws them */
{
    ph -= (float)(int)ph;
    if (ph < 0.0f)
        ph += 1.0f;
    switch (w) {
    case 1: return ph < 0.5f ? 0.8f : -0.8f;
    case 2: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case 3: return um_sin(ph * 6.2831853f);
    case 4: return ph < 0.25f ? 0.8f : -0.8f;
    default: return 1.0f - 2.0f * ph;
    }
}

static void pic_osc2(int tr)              /* OSC 2 at its interval (faint) and what MIX, FM and RING make with OSC 1 */
{
    int w1 = snd_value(tr, SP_WAVE), w2 = snd_value(tr, SP_WAVE2), fm = snd_value(tr, SP_FM);
    int i, py = 0, py2 = 0;
    float r = um_exp2((float)snd_value(tr, SP_INTV) / 12.0f), dev = 1.5f * (float)(fm * fm) / 16129.0f;
    float mx = (float)snd_value(tr, SP_MIX) / 127.0f, rg = (float)snd_value(tr, SP_RING) / 127.0f;
    uint16_t col = TH_FG;
    char t[24], u[8];
    rrect(4, 4, 232, 124, 6, C_PANEL);
    if (w1 > 4) {                         /* SYNC and SUB are pairs of their own */
        text_c(120, 50, &FONT_S, "SYNC AND SUB:", C_TXT2);
        text_c(120, 70, &FONT_S, "OSC 2 IS THEIR OWN", C_TXT2);
        return;
    }
    w2 = w2 ? w2 - 1 : w1;
    cv_rect(10, 66, 220, 1, C_GRID);
    for (i = 0; i < 220; i++) {
        float ph = (float)i / 110.0f, b = wave_f(w2, ph * r), a = wave_f(w1, ph + dev * b), s = a + (b - a) * mx;
        int y, y2;
        s += (a * b - s) * rg;
        s = s > 1.1f ? 1.1f : s < -1.1f ? -1.1f : s;
        y = 66 - (int)(s * 48.0f);
        y2 = 66 - (int)(b * 48.0f);
        if (i && (i & 1))
            cv_line(10 + i - 1, py2, 10 + i, y2, mix565(C_PANEL, col, 70));
        if (i)
            thick(10 + i - 1, py, 10 + i, y, col);
        py = y;
        py2 = y2;
    }
    t[0] = 0;                             /* OSC 2's ratio to OSC 1 */
    text_cat(t, "OSC 2 X");
    int_text((int32_t)r, u);
    text_cat(t, u);
    text_cat(t, ".");
    two_digits((int)((r - (float)(int)r) * 100.0f + 0.5f) % 100, u);
    text_cat(t, u);
    cv_text(12, 8, &FONT_XS, t, C_TXT2);
}

static float shaper_f(int type, float x, float amt, float bias)   /* the SHAPER on -1..1 (eng_analog.c shp_run's) */
{
    float u = x + bias, y;
    switch (type) {
    case 0: return um_sin(1.5707963f * u * (0.64f + amt * 7.36f));
    case 1:
        y = u * (1.0f + amt * 15.0f);
        return y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;
    case 2:
        y = 2.0f * (u < 0.0f ? -u : u) - 1.0f;
        y = y > 1.0f ? 1.0f : y;
        return x + (y - x) * amt;
    default: {
        float st = (float)(1 << (int)(amt * 13.0f)) / 32768.0f, q = u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
        return (float)(int)((q + 1.0f) / st) * st - 1.0f;
    }
    }
}

static void pic_shaper(int tr)            /* the SHAPER: its curve (in across, out up), and a sine through it */
{
    int type = snd_value(tr, SP_SHTYPE), on = snd_value(tr, SP_SHAPE) > 0, i, py = 0;
    float amt = (float)snd_value(tr, SP_SHAPE) / 127.0f, bias = (float)snd_value(tr, SP_BIAS) / 128.0f;
    uint16_t col = on ? TH_FG : C_GRID;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_rect(14, 66, 100, 1, C_GRID);      /* the curve, in a square */
    cv_rect(64, 16, 1, 100, C_GRID);
    for (i = 0; i <= 100; i++) {
        float x = (float)i / 50.0f - 1.0f, y = on ? shaper_f(type, x, amt, bias) : x;
        int yy = 66 - (int)(y * 48.0f);
        if (i)
            thick(14 + i - 1, py, 14 + i, yy, col);
        py = yy;
    }
    cv_rect(126, 66, 104, 1, C_GRID);     /* a sine, shaped */
    for (i = 0; i < 104; i++) {
        float x = um_sin((float)i * (6.2831853f / 52.0f)) * 0.9f, y = on ? shaper_f(type, x, amt, bias) : x;
        int yy = 66 - (int)((y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y) * 48.0f);
        if (i)
            thick(126 + i - 1, py, 126 + i, yy, col);
        py = yy;
    }
    if (!on)
        text_c(120, 104, &FONT_XS, "AMOUNT 0: OFF", C_TXT2);
}

static void pic_macro(int tr)             /* the four macros: where each is, and what the matrix has it move */
{
    const int8_t *v = tset(tr)->snd;
    int m, k;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    for (m = 0; m < 4; m++) {
        int x = 8 + m * 57, val = v[SP_M1 + m], h = val * 56 / 127, n = 0;
        char t[4] = {'M', (char)('1' + m), 0, 0};
        text_c(x + 27, 7, &FONT_XS, t, C_TXT2);
        rrect(x + 17, 22, 20, 58, 3, C_GRID);
        if (h > 0)
            rrect(x + 17, 80 - h, 20, h, 3, TH_FG);
        for (k = 0; k < SYN_SLOTS && n < 3; k++) {
            const int8_t *q = v + SP_XM + 3 * k;
            if (q[0] == MS_M1 + m && q[2]) {
                char u[16], w[16];
                syn_param_text(SP_XM + 3 * k + 1, q[1], u);
                fit(&FONT_XS, u, 54, w);
                text_c(x + 27, 86 + n * 12, &FONT_XS, w, TH_TXT);
                n++;
            }
        }
        if (!n)
            text_c(x + 27, 86, &FONT_XS, "-", C_TXT2);
    }
}

static void pic_matrix(int tr)            /* MATRIX: its six slots, the one on the knobs lit */
{
    int k;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    for (k = 0; k < SYN_SLOTS; k++) {
        int y = 8 + k * 19, me = k == ui.mslot, on = tset(tr)->snd[SP_XM + 3 * k] && tset(tr)->snd[SP_XM + 3 * k + 2];
        char t[32], u[32], n[4] = {(char)('1' + k), 0, 0, 0};
        if (me)                           /* the slot on the knobs: selected */
            rrect(8, y, 224, 18, 4, C_VIOLET);
        cv_text(14, y + 3, &FONT_XS, n, me ? C_INK : C_TXT2);
        slot_text(tr, k, t);
        fit(&FONT_XS, t, 196, u);
        cv_text(30, y + 3, &FONT_XS, u, me ? C_INK : on ? TH_TXT : C_TXT2);
    }
}

static void pic_wave(int tr)              /* a SAMPLER track's sample, its start, end and loop */
{
    char t[24];
    rrect(4, 4, 232, 124, 6, C_PANEL);
    if (!smp_frames[tr]) {
        sample_status(tr, t);
        text_c(120, 58, &FONT_B, t, C_TXT2);
        return;
    }
    draw_wave(tr, 10, 10, 220, 112);
}

static void voices_meter(int tr, int x, int y, int cell)   /* the 16 voices: this track's, the others', free */
{
    int mine = tr > 0 ? syn_voices(tr) : 0, all = syn_voices(-1), i;
    for (i = 0; i < 16; i++) {
        uint16_t c = i < mine ? TH_FG : i < all ? C_RULE : C_GRID;
        rrect(x + (i % 8) * (cell + 3), y + (i / 8) * (cell + 3), cell, cell, 2, c);
    }
}

static float lvl[NTRK];                   /* the tracks' meters, falling */
static void levels_take(void)
{
    int t;
    for (t = 0; t < NTRK; t++) {
        float p = (t == TR_DRUM ? drum_pk : syn_peak(t)) * FX_BUS_TRIM;   /* as it comes to the master bus */
        if (t == TR_DRUM)
            drum_pk = 0.0f;
        lvl[t] = p > lvl[t] ? p : lvl[t] * 0.9f;
    }
}

static float hit[NPADS];                  /* the pads' flashes, fading */
static void hits_take(void)
{
    int p;
    for (p = 0; p < NPADS; p++) {
        float v = pad_hit[p];
        pad_hit[p] = 0.0f;
        hit[p] = v > hit[p] ? v : hit[p] * 0.72f;
    }
}

/* ---- the pages */
static void kit_band(const char *title, const char *ctx, int npg, int pg)
{
    band_set(KIT_RGB[proj.kit], drum_kit_title(), title, ctx, npg, pg);
}

static void steps_text(char *t)           /* "STEPS 17-32" */
{
    char u[8];
    t[0] = 0;
    text_cat(t, "STEPS ");
    int_text(ui.spage * 16 + 1, u);
    text_cat(t, u);
    text_cat(t, "-");
    int_text(ui.spage * 16 + 16, u);
    text_cat(t, u);
}

static void page_steps(void)              /* the drums' grid: 11 pads and the accents, 16 steps of the page */
{
    dpart_t *p = dcur();
    int row, k, st = sq.t[TR_DRUM].step;
    char t[24], hint[28];
    steps_text(t);
    hint[0] = 0;
    text_cat(hint, drums_pad_name(drum_model(ui.pad), ui.pad));
    text_cat(hint, drum_lane_muted(ui.pad) ? " MUTED / ENV+PAD" : " / MUTE: ENV+PAD");
    kit_band(t, hint, npages(), ui.spage);
    hits_take();
    for (row = 0; row < NPADS + 1; row++) {
        int y = row * 11, muted = row < NPADS && (p->mute >> row & 1u), sel = row == ui.pad;
        const uint32_t *bits = row < NPADS ? p->hit[row] : p->acc;
        uint16_t on = row == NPADS ? TH_HI : muted ? C_GRID : sel ? TH_FG : mix565(C_BODY, TH_INK, 118);
        if (sel) {                        /* the pad picked: its row a field, its name on violet */
            rrect(0, y, 240, 11, 3, C_PANEL);
            cv_rect(1, y + 1, 22, 9, C_VIOLET);
        }
        if (row < NPADS && hit[row] > 0.05f)   /* the pad sounding: its name lights */
            rrect(1, y + 1, 22, 9, 2, mix565(sel ? C_VIOLET : C_BODY, C_WHITE2, (int)(hit[row] * 230.0f)));
        cv_text(3, y - 2, &FONT_XS, row < NPADS ? drums_pad_name(drum_model(row), row) : "AC",
                muted ? TH_RED : sel || (row < NPADS && hit[row] > 0.4f) ? C_INK : C_TXT2);
        for (k = 0; k < 16; k++) {
            int s = ui.spage * 16 + k, x = 27 + k * 13;
            if (s >= p->len)
                continue;
            if (!(k & 3))                 /* the beats (steps 1, 5, 9, 13 of the page): a column of their own */
                cv_rect(x - 1, y, 13, 11, mix565(sel ? C_PANEL : C_BODY, TH_INK, 22));
            if (sq.playing && s == st)
                cv_rect(x - 1, y, 13, 11, C_GRID);
            if (bit_get(bits, s)) {
                rrect(x, y + 1, 11, 9, 2, on);
                if (row < NPADS && step_locked(p, s, row))
                    cv_rect(x + 7, y + 1, 4, 3, C_WHITE2);
                if (row == ui.pad && s == ui.held_step)
                    cv_rect(x + 3, y + 3, 5, 5, C_WHITE2);
            } else {
                cv_rect(x + 4, y + 4, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            }
        }
    }
    cards_of(KD_SEQ, 0);
}

static void page_roll(void)               /* an instrument part: its notes, pitch up, 16 steps of the page */
{
    int tr = ui.trk, st = sq.t[tr].step, k, i, s, lo = 127, hi = 0, rows, rh, top, n, H = BODY_H - 6;
    ipart_t *p = ipart(tr);
    uint16_t col = TRK_RGB[tr];
    char t[24], u[40], note[8];
    const track_t *ts = tset(tr);
    steps_text(t);
    if (ui.held_step >= 0 && ui.held_step < p->len) {
        u[0] = 0;
        text_cat(u, "HOLD + ARP: PICK NOTES");
    } else {
        note_text(track_note(tr), note);
        upper(note);
        u[0] = 0;
        text_cat(u, "STAMP ");
        text_cat(u, note);
        if (ts->chord) {
            text_cat(u, " ");
            text_cat(u, CHORD_NAME[ts->chord]);
            text_cat(u, " ");
            text_cat(u, INV_NAME[ts->chord_inv & 3u]);
        }
    }
    band_set(col, TRK_NAME[tr], t, u, npages(), ui.spage);
    for (s = 0; s < p->len; s++)
        for (i = 0; i < NCHORD && p->st[s].note[i]; i++) {
            if (p->st[s].note[i] < lo) lo = p->st[s].note[i];
            if (p->st[s].note[i] > hi) hi = p->st[s].note[i];
        }
    if (lo > hi) {
        lo = 54 + 12 * tset(tr)->oct;
        hi = lo + 12;
    }
    if (hi - lo + 1 < 13) {
        int mid = (lo + hi) / 2;
        lo = mid - 6;
        hi = mid + 6;
    }
    if (hi - lo + 1 > 36)
        hi = lo + 35;
    rows = hi - lo + 1;
    rh = H / rows;
    top = H - rows * rh;
    for (n = lo; n <= hi; n++) {
        int y = top + (hi - n) * rh;
        int b = n % 12 == 1 || n % 12 == 3 || n % 12 == 6 || n % 12 == 8 || n % 12 == 10;
        if (b)
            cv_rect(26, y, 214, rh, mix565(C_BODY, TH_INK, 16));
        if (n % 12 == 0) {
            cv_rect(26, y + rh - 1, 214, 1, C_GRID);
            note_text(n, t);
            cv_text(1, y + rh - 13 < 0 ? 0 : y + rh - 13, &FONT_XS, t, C_TXT2);
        }
    }
    for (k = 0; k < 16; k++) {
        const nstep_t *ns;
        int x = 28 + k * 13;
        s = ui.spage * 16 + k;
        if (s >= p->len)
            continue;
        ns = &p->st[s];
        if (sq.playing && s == st)
            cv_rect(x - 1, 0, 13, H, C_GRID);
        if (!ns->note[0]) {
            cv_rect(x + 4, H + 1, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            continue;
        }
        for (i = 0; i < NCHORD && ns->note[i]; i++) {
            int nn = ns->note[i], w = ns->gate * 13 / 8 - 1;
            if (nn < lo || nn > hi)
                continue;
            if (w < 3)
                w = 3;
            if (x + w > 240)
                w = 240 - x;
            rrect(x, top + (hi - nn) * rh, w, rh > 2 ? rh - 1 : rh, 2,
                  s == ui.held_step ? TH_HI : mix565(C_BODY, TH_FG, 110 + ns_vel(ns)));
        }
        if (istep_locked(p, s))
            cv_rect(x + 3, 0, 5, 3, TH_FG);
        if (ns_prob(ns) < 100)
            cv_rect(x + 3, H + 1, 5, 4, TH_FG);
        if (ns->vel & NS_ACC)             /* a 303's accent: a bar over it; a slide: a mark at its end */
            cv_rect(x, 0, 11, 2, C_WHITE2);
        if (ns->prob & NS_SLIDE)
            cv_rect(x + 9, H - 3, 4, 3, TH_FG);
    }
    if (ui.held_step >= 0)
        cards_of(KD_HELD, 0);
    else
        cards_of(KD_SEQ, 0);
}

static void page_gen(void)                /* the 303 generator: the line (16 steps of the page), its accents and slides */
{
    int tr = ui.trk, st = sq.t[tr].step, k, s, n, lo = 127, hi = 0, span, H = 78;
    const ipart_t *p = ipart(tr);
    gen_t g = gen_view(tr);
    uint16_t col = TRK_RGB[tr];
    char t[32], u[24];
    t[0] = 0;
    if (gen_of(tr)->seed) {
        seed_text(&g, t);
        text_cat(t, "  ");
        note_text(g.root, u);
        text_cat(t, u);
        text_cat(t, " ");
        text_cat(t, GS_NAME[g.scale % NGSCALES]);
    } else {
        text_cat(t, "OCT+ OR A KNOB: MAKE A LINE");
    }
    band_set(col, TRK_NAME[tr], ui.genpage == 0 ? "ACID" : ui.genpage == 1 ? "ACID: NOTES" : "ACID: LINE", t, NGENPAGE,
             ui.genpage);
    for (s = 0; s < p->len; s++)
        if (p->st[s].note[0]) {
            if (p->st[s].note[0] < lo) lo = p->st[s].note[0];
            if (p->st[s].note[0] > hi) hi = p->st[s].note[0];
        }
    if (lo > hi)
        lo = hi = g.root;
    if (hi - lo < 12) {                   /* an octave at least, the line in its middle */
        int mid = (lo + hi) / 2;
        lo = mid - 6;
        hi = mid + 6;
    }
    span = hi - lo;
#define GEN_Y(nn) (2 + (hi - clamp((nn), lo, hi)) * (H - 8) / span)   /* a note's block's top: 6 high */
    for (n = lo; n <= hi; n++)            /* the root's octaves: a line and the name */
        if ((n - g.root) % 12 == 0) {
            int y = GEN_Y(n) + 3;
            cv_rect(26, y, 214, 1, mix565(C_BODY, TH_INK, 60));
            note_text(n, t);
            cv_text(1, y - 8 < 0 ? 0 : y - 8 > H - 14 ? H - 14 : y - 8, &FONT_XS, t, C_TXT2);
        }
    cv_text(3, 84, &FONT_XS, "AC", C_TXT2);
    cv_text(3, 98, &FONT_XS, "SL", C_TXT2);
    for (k = 0; k < 16; k++) {           /* (playhead columns first: notes held over them stay on top) */
        s = ui.spage * 16 + k;
        if (s < p->len && sq.playing && s == st)
            cv_rect(28 + k * 13 - 1, 0, 13, 110, C_GRID);
    }
    for (k = 0; k < 16; k++) {
        const nstep_t *ns;
        int x = 28 + k * 13, y, nx, slid, w;
        s = ui.spage * 16 + k;
        if (s >= p->len)
            continue;
        ns = &p->st[s];
        if (!ns->note[0]) {
            cv_rect(x + 4, 89, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            cv_rect(x + 4, 103, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            continue;
        }
        nx = s + 1 < p->len ? s + 1 : 0;
        slid = (ns->prob & NS_SLIDE) && p->st[nx].note[0];
        y = GEN_Y(ns->note[0]);
        w = slid ? 11 : ns->gate * 13 / 8 - 1;   /* HALF short, WHOLE as long as it's held */
        w = w < 5 ? 5 : x + w > 239 ? 239 - x : w;
        if (slid && k < 15)               /* the slide: on to the next note's pitch */
            thick(x + 10, y + 2, x + 13, GEN_Y(p->st[nx].note[0]) + 2, TH_FG);
        rrect(x, y, w, 6, 2, (ns->vel & NS_ACC) ? C_WHITE2 : TH_FG);
        if (ns->vel & NS_ACC)
            rrect(x + 1, 85, 9, 10, 2, C_WHITE2);
        else
            cv_rect(x + 4, 89, 3, 3, C_GRID);
        if (slid)
            rrect(x + 1, 99, 12, 10, 2, TH_FG);
        else
            cv_rect(x + 4, 103, 3, 3, C_GRID);
    }
#undef GEN_Y
    cv_text(6, 116, &FONT_XS, "OCT+ NEW LINE", TH_TXT);
    cv_text(100, 116, &FONT_XS, "OCT- MUTATE", TH_TXT);
    if (p->len > 16) {
        steps_text(t);
        text_r(236, 116, &FONT_XS, t, C_TXT2);
    }
    cards_of(KD_GEN, ui.genpage);
}

static void page_keys(void)               /* an instrument track's KEYS page: what the keys hold, the voices, the keys */
{
    int tr = ui.trk, i, k;
    const track_t *ts = tset(tr);
    uint16_t col = TRK_RGB[tr];
    char t[40], u[12];
    t[0] = 0;
    text_cat(t, "OCT ");
    if (ts->oct > 0)
        text_cat(t, "+");
    int_text(ts->oct, u);
    text_cat(t, u);
    u[0] = 0;
    {
        char ctx[32];
        ctx[0] = 0;
        if (ts->type == TY_SAMPLER && (!smp_frames[tr] || (rec.trk == tr && rec.state != RS_IDLE))) {
            sample_status(tr, ctx);
        } else if (ui.rec && sq.playing) {
            text_cat(ctx, "RECORDING");
        } else if (ui.rec) {
            text_cat(ctx, "STEP REC ");
            int_text(ui.cursor[tr] + 1, u);
            text_cat(ctx, u);
            text_cat(ctx, "/");
            int_text(*part_len(tr), u);
            text_cat(ctx, u);
        } else {
            sound_name(tr, ctx);
        }
        {
            char ttl[24];
            ttl[0] = 0;
            text_cat(ttl, TYPE_NAME[ts->type]);
            text_cat(ttl, "  ");
            text_cat(ttl, t);
            if (ts->chord) {              /* chord mode: which */
                text_cat(ttl, "  ");
                text_cat(ttl, CHORD_NAME[ts->chord % NCHORDS]);
            }
            band_set(col, TRK_NAME[tr], ttl, ctx, 0, 0);
        }
    }
    t[0] = 0;                             /* the notes the keys hold now */
    for (k = 0; k < NKEYS; k++)
        if (ui.key_note[k] && ui.key_trk[k] == tr) {
            note_text(ui.key_note[k], u);
            if (t[0])
                text_cat(t, " ");
            text_cat(t, u);
        }
    upper(t);
    {
        char f[40];
        fit(&FONT_L, t[0] ? t : "-", 140, f);
        cv_text(8, 4, &FONT_L, f, t[0] ? TH_TXT : C_GRID);
    }
    voices_meter(tr, 152, 8, 7);
    cv_text(152, 30, &FONT_XS, "VOICES", C_TXT2);
    for (i = 0; i < 16; i++) {            /* the keyboard: 16 white keys, 11 black */
        int key = WK[i], on = (int)(ui.keys >> key & 1u);
        rrect(i * 15, 56, 14, 74, 2, on ? col : C_TILET);
    }
    for (i = 0; i < 11; i++) {
        int key = BK[i], on = (int)(ui.keys >> key & 1u), w;
        for (w = 0; w < 16 && WK[w] != key - 1; w++) {
        }
        rrect(w * 15 + 9, 56, 11, 44, 2, on ? col : C_INK);
    }
    cards_of(KD_SEQ, 0);
}

static void page_pick(void)
{
    int tr = ui.trk, i, w;
    const nstep_t *s = &proj.p[ui.pick_clip].t[tr - 1].st[ui.pick_step];
    uint16_t col = TRK_RGB[tr];
    char t[24] = "STEP ", u[8], notes[32] = "";
    int_text(ui.pick_step + 1, u);
    text_cat(t, u);
    band_set(col, TRK_NAME[tr], "PICK NOTES", t, 0, 0);
    for (i = 0; i < NCHORD && s->note[i]; i++) {
        if (i)
            text_cat(notes, " ");
        note_text(s->note[i], u);
        upper(u);
        text_cat(notes, u);
    }
    cv_text(6, 3, &FONT_S, notes[0] ? notes : "REST", TH_FG);
    t[0] = 0;
    int_text(i, t);
    text_cat(t, "/4");
    text_r(234, 3, &FONT_S, t, TH_FG);
    cv_text(6, 24, &FONT_XS, "TAP KEY: ADD / REMOVE", TH_FG);
    text_r(234, 24, &FONT_XS, "OCT: SHIFT", C_TXT2);
    for (i = 0; i < 16; i++) {
        int pitch = pick_pitch(WK[i]), on = pick_is_selected(s, pitch);
        rrect(i * 15, 43, 14, 55, 2, on ? col : C_TILET);
        if (on)
            cv_rect(i * 15 + 5, 88, 4, 4, C_WHITE2);
        if (i == 0 || pitch % 12 == 0 || i == 15) {
            note_text(pitch, u);
            upper(u);
            cv_text(i * 15 + 1, 100, &FONT_T, u, C_TXT2);
        }
    }
    for (i = 0; i < 11; i++) {
        int key = BK[i], on = pick_is_selected(s, pick_pitch(key));
        for (w = 0; w < 16 && WK[w] != key - 1; w++) {}
        rrect(w * 15 + 9, 43, 11, 35, 2, on ? col : C_INK);
        if (on)
            cv_rect(w * 15 + 12, 69, 5, 4, C_WHITE2);
    }
    cv_text(6, 114, &FONT_T, "ARP / SEQ: BACK TO STEPS", TH_FG);
    for (i = 0; i < NCHORD; i++)
        cards[i].on = 0;                 /* the player is tapping musical keys, not adjusting knobs */
}

static void page_dedit(void)              /* the drums' EDIT page: the pad, its steps and hits, all its knobs */
{
    int idx[NPOTS], n = edit_params(drum_model(ui.pad), ui.pad, idx), np = (n + 3) / 4, k, hs = ui.held_step;
    const dpart_t *pat = dcur();
    uint16_t kc = KIT_RGB[proj.kit];
    int st = sq.t[TR_DRUM].step;
    char t[24], u[12];
    if (ui.epage >= drum_epages())
        ui.epage = 0;
    if (drum_model_page()) {
        kit_band("DRUM MODEL", "K1: CHANGE", drum_epages(), ui.epage);
        cv_text(7, 75, &FONT_B, drums_pad_name(drum_model(ui.pad), ui.pad), TH_FG);
        cv_text(7, 94, &FONT_S, "SAVE + PRESETS: STORE KIT", TH_FG);
        cards_of(KD_MODEL, 0);
        return;
    }
    if (drum_kit_page()) {
        kit_band("KIT", "THE KIT'S OWN KNOBS", drum_epages(), ui.epage);
        page_map(KD_KIT, 1, 0, 0, 8, 28);
        cards_of(KD_KIT, 0);
        return;
    }
    t[0] = 0;
    text_cat(t, "EDIT ");
    text_cat(t, drums_pad_name(drum_model(ui.pad), ui.pad));
    u[0] = 0;
    if (hs >= 0) {
        int c = lock_find(pat, hs, ui.pad, LK_PROB);
        char v[24];
        v[0] = 0;
        text_cat(v, "STEP ");
        int_text(hs + 1, u);
        text_cat(v, u);
        text_cat(v, " LOCKS  ");
        int_text(c >= 0 ? pat->lk[c].val : 100, u);
        text_cat(v, u);
        text_cat(v, "%");
        kit_band(t, v, drum_epages(), ui.epage);
    } else {
        kit_band(t, "MODEL: LAST PAGE", drum_epages(), ui.epage);
    }
    hits_take();
    {   /* the pad: its name, lit as it sounds */
        float h = hit[ui.pad];
        rrect(4, 2, 66, 40, 6, h > 0.05f ? mix565(C_TILE, C_WHITE2, (int)(h * 230.0f)) : C_TILE);
        text_c(37, 6, &FONT_L, drums_pad_name(drum_model(ui.pad), ui.pad), h > 0.5f ? C_INK : kc);
        text_c(37, 28, &FONT_XS, drums_kit_name(drum_model(ui.pad)), h > 0.5f ? C_INK : kc);
    }
    for (k = 0; k < 16; k++) {            /* its steps on this page */
        int s = ui.spage * 16 + k, x = 76 + k * 10;
        if (s >= pat->len)
            continue;
        if (sq.playing && s == st)
            cv_rect(x - 1, 8, 10, 28, C_GRID);
        if (bit_get(pat->hit[ui.pad], s))
            rrect(x, 10, 8, 24, 2, s == hs ? TH_HI : TH_FG);
        else
            cv_rect(x + 3, 20, 2, 4, (k & 3) ? C_GRID : C_TXT2);
    }
    page_map(KD_DRUM, np, ui.epage, 0, 48, BODY_H - 50);
    cards_of(KD_DRUM, ui.epage);
}

static void page_iedit(int env)           /* an instrument track's EDIT (or ENV) page */
{
    int tr = ui.trk, page = env ? ui.envpage % NENVPAGE : ui.epage, smp = tset(tr)->type == TY_SAMPLER;
    uint16_t col = TRK_RGB[tr];
    char t[32];
    if (!env && ui.epage >= inst_epages(tr))
        ui.epage = page = 0;
    if (ui.held_step >= 0) {
        char u[8];
        t[0] = 0;
        text_cat(t, "STEP ");
        int_text(ui.held_step + 1, u);
        text_cat(t, u);
        text_cat(t, " LOCKS");
    } else {
        sound_name(tr, t);
    }
    band_set(col, TRK_NAME[tr], env ? ENVPAGE_NAME[page] : (smp ? SPAGE_NAME[page] : EPAGE_NAME[page]), t,
             env ? NENVPAGE : inst_epages(tr), page);
    if (env) {
        pic_env(tr, page);
    } else if (page == 0) {               /* VOICE: the type, the voices, the level */
        float lv;
        char u[16];
        levels_take();
        lv = lvl[tr];
        rrect(4, 4, 232, 124, 6, C_PANEL);
        u[0] = 0;
        text_cat(u, TYPE_NAME[tset(tr)->type]);
        cv_text(12, 8, &FONT_L, u, TH_TXT);
        voices_meter(tr, 12, 52, 20);
        cv_text(12, 102, &FONT_XS, "VOICES", C_TXT2);
        cv_text(200, 8, &FONT_XS, "LEVEL", C_TXT2);
        rrect(208, 28, 14, 90, 3, C_GRID);
        {
            int h = db_h(lv, 90, 48.0f);
            if (h > 0)
                rrect(208, 118 - h, 14, h, 3, TH_FG);
        }
    } else if (smp && page == 1) {
        pic_wave(tr);
    } else if (!smp && page == 1) {
        pic_osc(tr);
    } else if (page == 3) {
        pic_filter(tr);
    } else if (!smp && page == EP_OSC2) {
        pic_osc2(tr);
    } else if (page == (smp ? 5 : EP_SHAPER)) {
        pic_shaper(tr);
    } else if (page == (smp ? 6 : EP_MACRO)) {
        pic_macro(tr);
    } else {
        page_map(KD_INST, inst_epages(tr), page, smp ? SPAGE_NAME : EPAGE_NAME, 8, 112);
    }
    cards_of(env ? KD_ENV : KD_INST, page);
}

static void page_lfo(void)                /* the track's LFO: its shape, where it is, what it moves */
{
    static const float RND[6] = {0.6f, -0.3f, 0.9f, -0.7f, 0.6f, -0.2f};
    int tr = ui.trk, c, py = 0, pg = lfo_page(tr);
    const lfo_t *l = &proj.lfo[tr];
    uint16_t col = trk_rgb(tr), wc = l->depth ? TH_FG : C_GRID;
    float amp = (float)(l->depth < 0 ? -l->depth : l->depth) / 64.0f;
    char t[32], d[24];
    ui.lfopage = (uint8_t)pg;
    if (tr != TR_DRUM && pg >= LP_LFO2) { /* LFO 2 (each note's own), MATRIX */
        const int8_t *v = tset(tr)->snd;
        int k, n = 0;
        t[0] = 0;
        if (pg == LP_MATRIX) {
            text_cat(t, "MOD ");
            int_text(ui.mslot + 1, d);
            text_cat(t, d);
        } else {
            for (k = 0; k < SYN_SLOTS; k++)
                n += v[SP_XM + 3 * k] == MS_LFO2 && v[SP_XM + 3 * k + 2];
            text_cat(t, n ? "EACH NOTE ITS OWN" : "ROUTE ON MATRIX");
        }
        band_set(col, TRK_NAME[tr], LFO_PAGE_NAME[pg], t, lfo_pages(tr), pg);
        if (pg == LP_MATRIX) {
            pic_matrix(tr);
        } else {
            int sh = v[SP_L2SHAPE];
            rrect(4, 4, 232, 124, 6, C_PANEL);
            cv_rect(10, 64, 220, 1, C_GRID);
            for (c = 0; c < 220; c++) {
                float ph = (float)(c % 110) / 110.0f, a = RND[c / 44], b = RND[c / 44 + 1];
                float w = (sh == LS_SH || sh == LS_SMOOTH) ? lfo_wave(sh, (float)(c % 44) / 44.0f, a, b)
                                                           : lfo_wave(sh, ph, 0.0f, 0.0f);
                int y = 64 - (int)(w * 50.0f);
                if (c)
                    thick(10 + c - 1, py, 10 + c, y, n ? TH_FG : C_GRID);
                py = y;
            }
            if (v[SP_L2FADE]) {           /* FADE IN: a ramp under the first cycle */
                int fw = 10 + v[SP_L2FADE] * 100 / 127;
                thick(10, 118, 10 + fw, 104, mix565(C_PANEL, TH_FG, 120));
            }
        }
        cards_of(KD_LFO, pg);
        return;
    }
    lfo_text(tr, 0, 3, d);
    t[0] = 0;
    text_cat(t, "TO ");
    text_cat(t, d);
    band_set(col, tr == TR_DRUM ? drum_kit_title() : TRK_NAME[tr],
             tr == TR_DRUM ? (pg ? "LFO MORE" : "LFO") : LFO_PAGE_NAME[pg], t, lfo_pages(tr), pg);
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_rect(10, 64, 220, 1, C_GRID);
    if (amp < 0.25f)
        amp = 0.25f;
    for (c = 0; c < 220; c++) {
        float ph = (float)(c % 110) / 110.0f, a = RND[c / 44], b = RND[c / 44 + 1], v;
        int y;
        v = (l->shape == LS_SH || l->shape == LS_SMOOTH) ? lfo_wave(l->shape, (float)(c % 44) / 44.0f, a, b)
                                                        : lfo_wave(l->shape, ph, 0.0f, 0.0f);
        if (l->depth < 0)
            v = -v;
        y = 64 - (int)(v * amp * 52.0f);
        if (c)
            thick(10 + c - 1, py, 10 + c, y, wc);
        py = y;
    }
    {   /* where it is now */
        float v = lfo_rt[tr].out * (l->depth < 0 ? -1.0f : 1.0f);
        int x = 10 + (int)(lfo_rt[tr].ph * 109.0f), y = 64 - (int)(v * amp * 52.0f);
        rrect(x - 5, y - 5, 11, 11, 5, TH_FG);
        rrect(x - 3, y - 3, 7, 7, 3, C_WHITE2);
        rrect(x - 4 + 110, y - 4, 9, 9, 4, mix565(C_PANEL, TH_FG, 90));
    }
    cards_of(KD_LFO, ui.lfopage);
}

static void page_arp(void)                /* an instrument track's arpeggiator: its notes, its order */
{
    int tr = ui.trk, i, mode = tset(tr)->arp_mode, n = arp[tr].n;
    uint16_t col = TRK_RGB[tr];
    char t[32], u[8];
    t[0] = 0;
    if (!mode) {
        text_cat(t, "OFF: THE KEYS AS THEY ARE");
    } else {
        int_text(n, u);
        text_cat(t, u);
        text_cat(t, n == 1 ? " NOTE" : " NOTES");
        if (tset(tr)->arp_latch)
            text_cat(t, "  LATCHED");
    }
    if (ui.arppage) {                     /* ARP 2: LATCH, and chord mode: the chord drawn on two octaves from C */
        const track_t *ts = tset(tr);
        int spec = ts->chord | ts->chord_inv << 4, cn[4], nc = chord_of(spec, 48, cn), w, k;
        static const uint8_t WN[7] = {0, 2, 4, 5, 7, 9, 11}, BN[5] = {1, 3, 6, 8, 10}, BX[5] = {0, 1, 3, 4, 5};
        band_set(col, TRK_NAME[tr], "LATCH, CHORD", ts->chord ? "ONE KEY, A CHORD" : "CHORD MODE OFF", 2, 1);
        rrect(4, 4, 232, 124, 6, C_PANEL);
        t[0] = 0;
        text_cat(t, ts->chord ? CHORD_NAME[ts->chord % NCHORDS] : "OFF");
        cv_text(12, 6, &FONT_L, t, ts->chord ? TH_TXT : C_GRID);
        if (ts->chord) {
            t[0] = 0;
            text_cat(t, INV_NAME[ts->chord_inv & 3u]);
            text_cat(t, ts->chord_inv ? " INVERSION" : " POSITION");
            cv_text(120, 10, &FONT_S, t, TH_TXT);
            if (ts->type == TY_MONO)
                cv_text(120, 26, &FONT_XS, "MONO: ONE NOTE. POLY PLAYS IT", C_TXT2);
        }
        for (w = 0; w < 14; w++) {        /* 14 white keys, C3..B4 */
            int n = 48 + w / 7 * 12 + WN[w % 7], on = 0;
            for (k = 0; k < nc && ts->chord; k++)
                on |= cn[k] == n;
            rrect(10 + w * 16, 46, 15, 76, 2, on ? col : C_TILET);
        }
        for (w = 0; w < 10; w++) {        /* 10 black */
            int n = 48 + w / 5 * 12 + BN[w % 5], on = 0, x = 10 + (w / 5 * 7 + BX[w % 5]) * 16 + 10;
            for (k = 0; k < nc && ts->chord; k++)
                on |= cn[k] == n;
            rrect(x, 46, 11, 46, 2, on ? col : C_INK);
        }
        cards_of(KD_ARP, 1);
        return;
    }
    band_set(col, TRK_NAME[tr], ui.arppage ? "ARP 2" : "ARP", t, 2, ui.arppage);
    rrect(4, 4, 232, 124, 6, C_PANEL);
    t[0] = 0;
    text_cat(t, ARP_MODE_NAME[mode]);
    cv_text(12, 8, &FONT_L, t, mode ? TH_TXT : C_GRID);
    for (i = 0; i < 12; i++) {            /* its order, drawn: rising, falling, both, scattered, as played */
        static const uint8_t RN[12] = {3, 7, 1, 5, 2, 6, 4, 0, 6, 2, 5, 1}, PL[12] = {0, 4, 2, 6, 0, 4, 2, 6, 0, 4, 2, 6};
        int v = mode == AM_UP ? i % 4 * 2 : mode == AM_DOWN ? 6 - i % 4 * 2 : mode == AM_UPDOWN ? (i % 6 < 4 ? i % 6 : 6 - i % 6) * 2
              : mode == AM_RANDOM ? RN[i] : PL[i];
        int x = 14 + i * 18, h = 12 + v * 7;
        rrect(x, 120 - h, 14, h, 3, mode ? (i == (int)(arp[tr].idx % 12u) ? TH_HI : mix565(C_PANEL, TH_INK, 150)) : C_GRID);
    }
    for (i = 0; i < n && i < 6; i++) {    /* the notes it has */
        note_text(arp[tr].note[i], u);
        rrect(130 + (i % 3) * 34, 8 + (i / 3) * 20, 31, 17, 3, C_TILE);
        text_c(130 + (i % 3) * 34 + 15, 8 + (i / 3) * 20, &FONT_S, u, C_TILET);
    }
    cards_of(KD_ARP, ui.arppage);
}

static void page_mute(void)               /* the drums' pads: on or muted in this pattern, each lit as it sounds */
{
    dpart_t *p = dcur();
    int k;
    uint16_t kc = KIT_RGB[proj.kit];
    kit_band("LANE MUTE", track_is_muted(TR_DRUM) ? "DRUM TRACK MUTED" : "PAD KEYS: MUTE  OCT+: ALL ON", 0, 0);
    hits_take();
    for (k = 0; k < NPADS; k++) {
        int x = 3 + (k % 4) * 59, y = 2 + (k / 4) * 44, muted = (int)(p->mute >> k & 1u);
        uint16_t bg = muted ? C_PANEL : hit[k] > 0.05f ? mix565(C_TILE, C_WHITE2, (int)(hit[k] * 200.0f)) : C_TILE;
        rrect(x, y, 56, 40, 6, bg);
        cv_text(x + 7, y + 3, &FONT_B, drums_pad_name(drum_model(k), k), muted ? C_TXT2 : hit[k] > 0.5f ? C_INK : C_TILET);
        cv_text(x + 7, y + 21, &FONT_S, muted ? "MUTE" : "ON", muted ? TH_RED : C_TILET2);
        if (k == ui.pad)
            rrect(x + 36, y + 6, 12, 12, 6, kc);
    }
    cards_of(KD_SEQ, 0);
}

static void page_home(void)               /* the scene grid: 16 patterns across (the white keys), a track a row */
{
    int k, t, tk = home_trkkey(), blink = (int)(plat_ms() / 250u & 1u);
    char c[32], u[8];
    c[0] = 0;
    if (tk >= 0) {
        text_cat(c, TRK_NAME[tk]);
        text_cat(c, ": KEY = CLIP");
    } else {
        text_cat(c, "CPU ");
        int_text((int32_t)plat_cpu_pct(), u);
        text_cat(c, u);
        text_cat(c, "%  V");
        int_text(syn_voices(-1), u);
        text_cat(c, u);
    }
    band_set(C_CREAM, "HOME", "SCENES", c, 0, 0);
    levels_take();
    rrect(32 + proj.pat * 13, 0, 14, 132, 4, C_PANEL);   /* the focused scene's column */
    for (k = 0; k < NPAT; k++) {          /* the scene numbers */
        two_digits(k + 1, u);
        text_c(39 + k * 13, 0, &FONT_T, u[0] == '0' ? u + 1 : u, k == proj.pat ? TH_TXT : C_TXT2);
    }
    for (t = 0; t < NTRK; t++) {
        int y = 14 + t * 29, muted = (int)(proj.tmute >> t & 1u), h;
        uint16_t tc = trk_rgb(t);
        /* the track: its name, muted or stopped, its level */
        rrect(1, y, 29, 27, 4, muted || clip[t] < 0 ? C_PANEL : tc);
        text_c(15, y + 1, &FONT_S, t == TR_DRUM ? drum_kit_title() : TRK_NAME[t],
               muted ? TH_RED : C_INK);
        h = db_h(lvl[t], 23, 48.0f);
        cv_rect(4, y + 21, 23, 3, muted || clip[t] < 0 ? C_GRID : mix565(tc, C_INK, 90));
        if (h > 0)
            cv_rect(4, y + 21, h, 3, C_INK);
        if (tk == t)                      /* its key held: selected */
            rrect(1, y, 3, 27, 1, C_VIOLET);
        for (k = 0; k < NPAT; k++) {      /* its clips: the part of each pattern */
            int x = 33 + k * 13, has = t == TR_DRUM ? !dpart_empty(&proj.p[k].d) : !ipart_empty(&proj.p[k].t[t - 1]);
            int on = clip[t] == k, q = qclip[t] == k || (sq.queued == k && qclip[t] == -1);
            int stopping = on && qclip[t] == -2;
            uint16_t fill = has ? mix565(C_BODY, TH_INK, 64) : C_BODY;
            if (on)
                fill = (stopping && blink) ? mix565(C_BODY, TH_INK, 64) : tc;
            else if (q)
                fill = blink ? tc : mix565(C_BODY, TH_INK, 120);
            if (!has && !on && !q) {      /* empty: an outline */
                rrect(x, y, 12, 27, 3, C_GRID);
                rrect(x + 1, y + 1, 10, 25, 2, k == proj.pat ? C_PANEL : C_BODY);
            } else {
                rrect(x, y, 12, 27, 3, fill);
            }
            if (on && sq.playing && !muted) {   /* where it is: a bar up the clip */
                int len = t == TR_DRUM ? proj.p[k].d.len : proj.p[k].t[t - 1].len, st = sq.t[t].step;
                int ph = len > 0 && st >= 0 ? (st + 1) * 23 / len : 0;
                if (ph > 0)
                    cv_rect(x + 4, y + 25 - ph, 4, ph, C_INK);
            }
        }
    }
    if (ui.scene_key >= 0) {              /* a scene key held: its follow action */
        char t2[24];
        t2[0] = 'P';
        two_digits(ui.scene_key + 1, t2 + 1);
        text_cat(t2, " FOLLOW");
        band_set(C_CREAM, "HOME", "SCENES", t2, 0, 0);
        cards_of(KD_FOLLOW, ui.scene_key);
    } else if (ui.pf_key >= 0 && (perf.want >> ui.pf_key & 1u)) {   /* a perform key held: its knob */
        cards_of(KD_PF, ui.pf_key);
    } else {
        cards_of(KD_SEQ, 0);
    }
}

static void page_setup(void)              /* SETUP: MIDI CLOCK; SOUNDS shared or per pattern, and which have their own */
{
    static const char *const CL[3] = {"OFF: THE FM-1'S OWN TEMPO", "SENT: 24 A BEAT, START, STOP", "FOLLOWED: TEMPO, START, STOP"};
    int t, k, pp = sounds_pp();
    band_set(C_CREAM, "SETUP", "CLOCK, SOUNDS", "HOME: NEXT", 0, 0);
    rrect(3, 2, 234, 40, 6, C_PANEL);
    cv_text(10, 4, &FONT_XS, "MIDI CLOCK (USB)", C_TXT2);
    cv_text(10, 20, &FONT_S, CL[proj.recq >> 2 & 3u], TH_TXT);
    rrect(3, 46, 234, 84, 6, C_PANEL);
    cv_text(10, 48, &FONT_XS, pp ? "SOUNDS PER PATTERN: WHICH HAVE THEIR OWN" : "SOUNDS: ONE FOR ALL PATTERNS", C_TXT2);
    for (t = 0; t < NTRK; t++) {          /* a row a track, a box a pattern: dark where it keeps a sound */
        int y = 66 + t * 15;
        cv_text(8, y - 3, &FONT_XS, t == TR_DRUM ? "DR" : TRK_NAME[t], TH_TXT);
        for (k = 0; k < NPAT; k++) {
            int has = t == TR_DRUM ? proj.pkit[k] != 0 : proj.psnd[k][t - 1].used;
            rrect(30 + k * 13, y, 11, 11, 2, !pp ? C_GRID : has ? TH_FG : mix565(C_BODY, TH_INK, 40));
        }
    }
    cards_of(KD_SETUP, 0);
}

static void page_song(void)               /* the song: its entries, the cursor's, the one playing */
{
    int r, first, i;
    char t[24], u[8];
    int_text(proj.song_n, t);
    text_cat(t, proj.song_n == 1 ? " ENTRY" : " ENTRIES");
    band_set(C_CREAM, "SONG", proj.song_on ? "ON" : "OFF", t, 0, 0);
    first = clamp(ui.scur - 3, 0, proj.song_n + 1 > 7 ? proj.song_n + 1 - 7 : 0);
    for (r = 0; r < 7; r++) {
        int e = first + r, y = 2 + r * 18, me = e == ui.scur;
        if (e > proj.song_n)
            break;
        rrect(3, y, 234, 16, 4, me ? C_VIOLET : C_PANEL);   /* the cursor's entry: selected */
        if (e == proj.song_n) {
            cv_text(12, y, &FONT_S, e < NSONG ? "+ A PATTERN KEY ADDS ONE" : "(FULL)", me ? C_INK : C_TXT2);
            continue;
        }
        if (sq.playing && srt.pos == e)
            rrect(3, y, 8, 16, 3, me ? C_INK : TH_FG);
        two_digits(e + 1, u);
        cv_text(16, y, &FONT_S, u, me ? C_INK : C_TXT2);
        t[0] = 'P';
        two_digits(proj.song[e].pat + 1, t + 1);
        cv_text(46, y - 1, &FONT_B, t, me ? C_INK : TH_TXT);
        int_text(proj.song[e].bars, t);
        text_cat(t, proj.song[e].bars == 1 ? " BAR" : " BARS");
        cv_text(96, y, &FONT_S, t, me ? C_INK : C_TXT2);
        for (i = 0; i < proj.song[e].bars && i < 16; i++)   /* its bars as ticks */
            cv_rect(160 + i * 4, y + 5, 3, 6, me ? C_INK : C_GRID);
    }
    cards_of(KD_SONG, 0);
}

static void page_mix(void)                /* the mixer: a strip a track, its meter beside the page's value */
{
    int t;
    band_set(C_CREAM, "MIX", MIX_NAME[ui.mixpage], "K1-K4: TRACKS", 4 + NMSTPAGE, ui.mixpage);
    levels_take();
    for (t = 0; t < NTRK; t++) {
        int x = 3 + t * 59, v = mix_value(t, ui.mixpage), muted = (int)(proj.tmute >> t & 1u), h;
        uint16_t col = trk_rgb(t);
        rrect(x, 2, 56, 128, 6, t == ui.trk ? C_PANEL : mix565(C_BODY, TH_INK, 12));
        if (t == ui.trk)                  /* the track picked: selected */
            frame(x, 2, 56, 128, C_VIOLET);
        rrect(x + 4, 6, 48, 18, 4, muted ? C_GRID : col);
        text_c(x + 28, 7, &FONT_S, t == TR_DRUM ? drum_kit_title() : TRK_NAME[t],
               muted ? TH_RED : C_INK);
        h = db_h(lvl[t], 92, 48.0f);      /* the meter, -48..0 dBFS */
        rrect(x + 6, 30, 8, 92, 3, C_GRID);
        if (h > 0)
            rrect(x + 6, 122 - h, 8, h, 3, h > 90 ? TH_RED : h > 80 ? RGB(240, 190, 80) : C_METER);
        if (ui.mixpage == 1) {            /* PAN: a slider across */
            cv_rect(x + 20, 76, 30, 2, C_GRID);
            cv_rect(x + 35, 70, 1, 14, C_TXT2);
            rrect(x + 18 + v * 30 / 127, 68, 6, 18, 3, col);
        } else {                          /* a bar up */
            int b = v * 92 / 127;
            rrect(x + 20, 30, 30, 92, 4, C_GRID);
            if (b > 0)
                rrect(x + 20, 122 - b, 30, b, 4, col);
        }
    }
    cards_of(KD_MIX, ui.mixpage);
}

static void page_fx(void)                 /* the sends' FX: what they do, drawn */
{
    int pg = ui.fxpage, i;
    char t[32];
    t[0] = 0;
    if ((pg == 1 || pg == 2) && !fx_delay_on()) {
        text_cat(t, "PAUSED: SAMPLING");
    } else if (pg == 1 || pg == 2) {      /* the time, in notes and in ms */
        char u[12];
        fx_text(FXP_DL_TIME, t);
        text_cat(t, "  ");
        int_text((int32_t)(fx_delay_ms() + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " MS");
    } else {
        text_cat(t, pg == 3 ? "ON THE MASTER BUS" : "FROM THE SENDS");
    }
    band_set(C_FXCOL, "FX", FX_PAGE_NAME[pg], t, NFXPAGE, pg);
    rrect(4, 4, 232, 124, 6, C_PANEL);
    if (pg == 0) {                        /* the reverb: its tail, long by DECAY, bright by TONE, loud by LEVEL */
        float decay = 0.2f + (float)proj.fx[FXP_RV_DECAY] / 127.0f * 0.78f, lvlv = (float)proj.fx[FXP_RV_LEVEL] / 127.0f, a = 1.0f;
        uint16_t c = mix565(C_BODY, TH_INK, 120 + proj.fx[FXP_RV_TONE] / 2);
        cv_rect(10, 118, 220, 1, C_GRID);
        for (i = 0; i < 54; i++) {
            int h = (int)(a * (0.3f + 0.7f * lvlv) * 100.0f);
            if (h > 0)
                cv_rect(12 + i * 4, 118 - h, 3, h, c);
            a *= decay * 0.12f + 0.86f;
        }
    } else if (pg == 1 || pg == 2) {      /* the delay: the echoes over 2 s, each lower by FEEDBACK; DELAY 2 to the sides */
        float ms = fx_delay_ms(), fb = fx_delay_fb(), a = (float)proj.fx[FXP_DL_LEVEL] / 127.0f * 0.8f + 0.2f;
        int wide = proj.fx[FXP_DL_WIDTH] > 0, n;
        uint16_t c = fx_delay_on() ? TH_FG : C_GRID;
        cv_rect(10, 66, 220, 1, C_GRID);
        rrect(10, 18, 4, 96, 2, TH_HI);  /* the sound */
        for (n = 1; n < 24 && ms > 1.0f; n++) {
            int x = 12 + (int)((float)n * ms * 216.0f / 2000.0f), h = (int)(a * 46.0f);
            if (x > 228 || h < 1)
                break;
            if (wide)
                rrect(x, n & 1 ? 66 - h : 67, 4, h, 2, c);
            else
                rrect(x, 66 - h, 4, 2 * h, 2, c);
            a *= fb;
        }
        if (wide) {
            cv_text(220, 10, &FONT_XS, "L", C_TXT2);
            cv_text(220, 106, &FONT_XS, "R", C_TXT2);
        }
    } else {                              /* the DJ filter: low-pass to the left, high-pass to the right */
        int v = proj.fx[FXP_FILTER];
        float q = 0.5f + (float)proj.fx[FXP_F_RESO] * (11.5f / 127.0f);
        grid_hz(10, 8, 220, 116);
        if (v < 60)
            svf_curve(0, 30.0f * um_exp2((float)(8 + v * 119 / 59) * (9.23f / 127.0f)), q, 10, 10, 220, 112, TH_FG, 1);
        else if (v > 68)
            svf_curve(2, 30.0f * um_exp2((float)((v - 69) * 120 / 58) * (9.23f / 127.0f)), q, 10, 10, 220, 112, TH_FG, 1);
        else {
            cv_rect(10, 10 + 37, 220, 2, TH_FG);
            text_c(120, 70, &FONT_S, "OFF: TURN LEFT OR RIGHT", C_TXT2);
        }
    }
    cards_of(KD_FX, pg);
}

/* ---- the master bus's pages */
static float mtr_in, mtr_out;             /* the CHAIN page's peak meters, falling */

static void hmeter(int x, int y, int w, int h, const char *name, float lin, float range)   /* a peak meter across */
{
    int v = db_h(lin, w, range);
    char s[12];
    float db = um_db(lin);
    cv_text(x, y - 3, &FONT_XS, name, C_TXT2);
    rrect(x + 30, y, w, h, 3, C_GRID);
    if (v > 0)
        rrect(x + 30, y, v, h, 3, db > -1.0f ? TH_RED : db > -6.0f ? RGB(240, 190, 80) : C_METER);
    cv_rect(x + 30 + w - w * 6 / (int)range, y - 2, 1, h + 4, C_TXT2);   /* -6 dB */
    s[0] = 0;
    if (db <= -60.0f)
        text_cat(s, "-INF");
    else
        db1(db, s);
    text_r(236, y - 3, &FONT_XS, s, TH_TXT);
}

static void gr_bar(int x, int y, int w, int h, const char *name, float gr)   /* a reduction, 0..12 dB down */
{
    int v = (int)(gr * (float)w / 12.0f);
    char s[12], u[12];
    if (v > w)
        v = w;
    cv_text(x, y - 3, &FONT_XS, name, C_TXT2);
    rrect(x + 30, y, w, h, 3, C_GRID);
    if (v > 0)
        rrect(x + 30 + w - v, y, v, h, 3, TH_RED);
    u[0] = 0;
    text_cat(u, "-");
    db1(gr, s);
    text_cat(u, s);
    text_r(236, y - 3, &FONT_XS, gr >= 0.05f ? u : "0.0", gr >= 0.05f ? TH_RED : C_TXT2);
}

static void mst_chain(void)
{
    static const char *const ST[5] = {"EQ", "COMP", "TAPE", "STEREO", "LIMIT"};
    int k;
    float in = fx_in_peak(0), out = fx_out_peak(0), lu = fx_lufs(0), lm = fx_lufs(1);
    char s[24];
    mtr_in = in > mtr_in ? in : mtr_in * 0.85f;
    mtr_out = out > mtr_out ? out : mtr_out * 0.85f;
    for (k = 0; k < 5; k++) {             /* the chain, in its order: lit where it does something */
        int x = 3 + k * 47, on = fx_stage_on(k);
        rrect(x, 2, 44, 22, 5, on ? C_TILE : C_PANEL);
        text_c(x + 22, 4, &FONT_XS, ST[k], on ? C_TILET : C_TXT2);
        if (k < 4)
            cv_rect(x + 44, 12, 3, 2, C_GRID);
    }
    gr_bar(4, 34, 160, 7, "COMP", fx_gr());
    gr_bar(4, 48, 160, 7, "LIMIT", fx_lgr());
    hmeter(4, 66, 160, 8, "IN", mtr_in, 48.0f);
    hmeter(4, 82, 160, 8, "OUT", mtr_out, 48.0f);
    rrect(3, 98, 234, 32, 6, C_PANEL);
    cv_text(10, 106, &FONT_XS, "LUFS", C_TXT2);
    if (lu <= -69.0f) {
        cv_text(44, 99, &FONT_M, "--", C_GRID);
    } else {
        db1(lu, s);
        cv_text(44, 99, &FONT_M, s, TH_TXT);
        {   /* the momentary loudness, a bar over -36..0 */
            int w = lm <= -36.0f ? 0 : (int)((lm + 36.0f) * 100.0f / 36.0f);
            if (w > 100)
                w = 100;
            cv_text(124, 101, &FONT_XS, "M", C_TXT2);
            rrect(134, 106, 100, 7, 3, C_GRID);
            if (w > 0)
                rrect(134, 106, w, 7, 3, TH_FG);
            db1(lm, s);
            cv_text(134, 113, &FONT_XS, s, C_TXT2);
        }
    }
}

static void mst_comp(int pg)              /* COMP: the curve, in and out over -48..0 dB; COMP 2: the reduction, large */
{
    float thr = -48.0f + 48.0f * (float)proj.fx[FXP_C_THRESH] / 127.0f, gr = fx_gr();
    static const float RV[10] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 20.0f, 1e6f};
    float slope = 1.0f - 1.0f / RV[proj.fx[FXP_C_RATIO] % 10], mk = 24.0f * (float)proj.fx[FXP_C_MAKEUP] / 127.0f;
    int x0 = 8, y0 = 6, s = 120, i, py = 0;
    if (pg == 4) {
        int w = (int)(gr * 220.0f / 12.0f);
        char t[16], u[16];
        rrect(4, 4, 232, 124, 6, C_PANEL);
        cv_text(12, 8, &FONT_XS, "REDUCTION NOW", C_TXT2);
        u[0] = 0;
        text_cat(u, "-");
        db1(gr, t);
        text_cat(u, t);
        text_cat(u, " DB");
        cv_text(12, 24, &FONT_L, u, gr >= 0.05f ? TH_RED : C_GRID);
        rrect(10, 76, 220, 16, 4, C_GRID);
        if (w > 220)
            w = 220;
        if (w > 0)
            rrect(230 - w, 76, w, 16, 4, TH_RED);
        for (i = 0; i <= 12; i += 3)
            cv_rect(230 - i * 220 / 12, 94, 1, 5, C_TXT2);
        cv_text(10, 104, &FONT_XS, proj.fx[FXP_PUMP] ? "PUMP: DUCKS AT EACH KICK" : "PUMP OFF", C_TXT2);
        return;
    }
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_line(x0, y0 + s, x0 + s, y0, C_GRID);   /* 1:1 */
    for (i = 0; i <= s; i++) {
        float in = -48.0f + 48.0f * (float)i / (float)s, over = in - thr, g;
        int y;
        g = over <= -3.0f ? 0.0f : over >= 3.0f ? slope * over : slope * (over + 3.0f) * (over + 3.0f) / 12.0f;
        y = y0 + s - (int)((in - g + mk + 48.0f) * (float)s / 48.0f);
        if (y < y0)
            y = y0;
        if (i)
            thick(x0 + i - 1, py, x0 + i, y, TH_FG);
        py = y;
    }
    {
        int xt = x0 + (int)((thr + 48.0f) * (float)s / 48.0f), y;
        for (y = y0; y < y0 + s; y += 6)
            cv_rect(xt, y, 1, 3, C_TXT2);
    }
    {
        char t[16], u[16];
        int w = (int)(gr * 92.0f / 12.0f);
        cv_text(138, 8, &FONT_XS, "REDUCTION", C_TXT2);
        u[0] = 0;
        text_cat(u, "-");
        db1(gr, t);
        text_cat(u, t);
        text_cat(u, " DB");
        cv_text(138, 22, &FONT_B, u, gr >= 0.05f ? TH_RED : C_GRID);
        rrect(138, 42, 92, 8, 3, C_GRID);
        if (w > 92)
            w = 92;
        if (w > 0)
            rrect(230 - w, 42, w, 8, 3, TH_RED);
        cv_text(138, 60, &FONT_XS, proj.fx[FXP_C_RATIO] ? "THE MIX," : "OFF:", C_TXT2);
        cv_text(138, 74, &FONT_XS, proj.fx[FXP_C_RATIO] ? "COMPRESSED" : "RATIO 1:1", C_TXT2);
    }
}

static void mst_tape(void)                /* TAPE: its curve, in to out, -1..1 */
{
    float k = um_exp2((float)proj.fx[FXP_TP_DRIVE] * (12.0f / 127.0f) * 0.166096f), b = (float)proj.fx[FXP_TP_WARM] * (0.35f / 127.0f);
    float tb = um_tanh(b), nrm = 1.0f / (k * (1.0f - tb * tb));
    int x0 = 58, y0 = 6, s = 120, i, py = 0, on = proj.fx[FXP_TP_DRIVE] > 0;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_line(x0, y0 + s, x0 + s, y0, C_GRID);
    cv_rect(x0, y0 + s / 2, s, 1, C_GRID);
    cv_rect(x0 + s / 2, y0, 1, s, C_GRID);
    for (i = 0; i <= s; i++) {
        float x = -1.0f + 2.0f * (float)i / (float)s, y = on ? (um_tanh(k * x + b) - tb) * nrm : x;
        int py2;
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        py2 = y0 + s / 2 - (int)(y * (float)(s / 2));
        if (i)
            thick(x0 + i - 1, py, x0 + i, py2, on ? TH_FG : C_TXT2);
        py = py2;
    }
}

static void mst_stereo(void)              /* STEREO: the image's width as a fan, the lows in the middle */
{
    int w = proj.fx[FXP_ST_WIDTH] * 5, i;
    float spread = (float)w / 100.0f * 0.7854f;   /* 100 %: 45 degrees each way */
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_text(10, 8, &FONT_XS, "L", C_TXT2);
    cv_text(224, 8, &FONT_XS, "R", C_TXT2);
    for (i = -20; i <= 20; i++) {
        float a = spread * (float)i / 20.0f;
        int x = 120 + (int)(um_sin(a) * 100.0f), y = 120 - (int)(um_sin(a + 1.5708f) * 100.0f);
        if (a > 1.5f || a < -1.5f)
            continue;
        cv_line(120, 120, x, y, mix565(C_PANEL, TH_INK, 80 + (20 - (i < 0 ? -i : i)) * 8));
    }
    if (proj.fx[FXP_ST_BASS])             /* BASS MONO: the lows centred */
        rrect(114, 70, 13, 50, 4, TH_HI);
    cv_rect(120, 20, 1, 100, C_GRID);
}

static void mst_limit(void)               /* LIMIT: the reduction and the output against the ceiling */
{
    float out = fx_out_peak(0), ce = -0.1f * (float)proj.fx[FXP_L_CEIL];
    int xc;
    mtr_out = out > mtr_out ? out : mtr_out * 0.85f;
    rrect(4, 4, 232, 124, 6, C_PANEL);
    gr_bar(10, 20, 150, 12, "GR", fx_lgr());
    hmeter(10, 52, 150, 12, "OUT", mtr_out, 24.0f);
    xc = 40 + 150 + (int)(ce * 150.0f / 24.0f);
    cv_rect(xc, 46, 2, 24, TH_FG);
    cv_text(xc - 30, 74, &FONT_XS, "CEILING", TH_TXT);
    cv_text(10, 96, &FONT_XS, proj.fx[FXP_LIMIT] ? "GAIN IN, NOTHING PAST THE CEILING" : "OFF: IT CLIPS",
            proj.fx[FXP_LIMIT] ? C_TXT2 : TH_RED);
}

static void page_mst(void)
{
    int pg = ui.mstpage;
    char t[24];
    t[0] = 0;
    fx_text(FXP_M_PRESET, t);
    if (proj.fx[FXP_M_BYPASS]) {
        t[0] = 0;
        text_cat(t, "BYPASSED");
    }
    band_set(C_MSTCOL, "MASTER", MST_PAGE_NAME[pg], t, 4 + NMSTPAGE, 4 + pg);
    switch (pg) {
    case 0: mst_chain(); break;
    case 1:
    case 2: {
        float db[112];
        int i, x0 = 10, y0 = 8, w = 220, h = 112, mid = y0 + h / 2, py = mid;
        rrect(4, 4, 232, 124, 6, C_PANEL);
        grid_hz(x0, y0, w, h);
        for (i = -12; i <= 12; i += 6)
            cv_rect(x0, mid - i * h / 30, w, 1, i ? mix565(C_PANEL, TH_INK, 30) : C_GRID);
        fx_eq_curve(db, 112);
        for (i = 0; i < 112; i++) {
            float d = db[i] < -15.0f ? -15.0f : db[i] > 15.0f ? 15.0f : db[i];
            int x = x0 + i * 2, y = mid - (int)(d * (float)h / 30.0f + (d < 0.0f ? -0.5f : 0.5f));
            cv_rect(x, y < mid ? y : mid, 2, (y < mid ? mid - y : y - mid) + 1, mix565(C_PANEL, TH_INK, 50));
            if (i)
                thick(x - 2, py, x, y, proj.fx[FXP_M_BYPASS] ? C_TXT2 : TH_FG);
            py = y;
        }
        break;
    }
    case 3:
    case 4: mst_comp(pg); break;
    case 5: mst_tape(); break;
    case 6: mst_stereo(); break;
    default: mst_limit(); break;
    }
    cards_of(KD_MST, pg);
}

static void page_rec(void)                /* REC held: the metronome, the count-in, the grid; QUANTIZE and LEGATO */
{
    int tr = ui.trk, g = recq_steps(), k, i, inst = tr != TR_DRUM;
    const char *grid = GRID_NAME[*part_rate(tr) % NRATES][recq_grid()];
    char t[32];
    band_set(C_RECCOL, "REC", ui.rec ? "RECORDING ON" : "RECORDING OFF", "GLO: ERASE PART", 0, 0);   /* (REC + GLO) */
    /* the two things it does to the notes there now */
    rrect(3, 2, 115, 44, 6, C_TILE);
    cv_text(10, 3, &FONT_XS, "OCT-", C_TILET2);
    cv_text(10, 15, &FONT_B, "QUANTIZE", C_TILET);
    t[0] = 0;
    text_cat(t, g == 1 ? "PICK A GRID: K3" : "TO ");
    if (g > 1)
        text_cat(t, grid);
    cv_text(10, 30, &FONT_XS, t, g == 1 ? C_TILET2 : C_RECCOL);
    rrect(122, 2, 115, 44, 6, C_TILE);
    cv_text(129, 3, &FONT_XS, "OCT+", C_TILET2);
    cv_text(129, 15, &FONT_B, "LEGATO", inst ? C_TILET : C_TILET2);
    cv_text(129, 30, &FONT_XS, inst ? "TIE THE LOOP" : "ON T2-T4", inst ? C_RECCOL : C_TILET2);
    /* this page of steps, the grid's points marked: where notes go */
    rrect(3, 52, 234, 62, 6, C_PANEL);
    for (k = 0; k <= 16; k++) {
        int x = 9 + k * 14, s = ui.spage * 16 + k;
        cv_rect(x, 56, 1, 54, s % g == 0 ? TH_FG : C_GRID);
        if (s % g == 0 && k < 16)
            cv_rect(x, 56, 3, 4, TH_FG);
    }
    if (inst) {                           /* the notes, each as long as its gate: legato, they meet */
        const ipart_t *p = ipart(tr);
        for (k = 0; k < 16; k++) {
            int s = ui.spage * 16 + k, w, row;
            const nstep_t *ns;
            if (s >= p->len)
                break;
            ns = &p->st[s];
            if (!ns->note[0])
                continue;
            w = ns->gate * 14 / 8;
            if (9 + k * 14 + w > 233)
                w = 233 - 9 - k * 14;
            row = 0;
            for (i = 0; i < k; i++) {     /* a second row when the one before still sounds */
                const nstep_t *o = &p->st[ui.spage * 16 + i];
                if (o->note[0] && i * 14 + o->gate * 14 / 8 > k * 14 && row == 0)
                    row = 1;
            }
            rrect(10 + k * 14, 66 + row * 20, w > 3 ? w : 3, 16, 3, TH_FG);
        }
    } else {                              /* the drums: the picked pad's hits, and the accents */
        const dpart_t *p = dcur();
        for (k = 0; k < 16; k++) {
            int s = ui.spage * 16 + k;
            if (s >= p->len)
                break;
            if (bit_get(p->hit[ui.pad], s))
                rrect(11 + k * 14, 70, 11, 18, 3, TH_FG);
            if (bit_get(p->acc, s))
                rrect(11 + k * 14, 92, 11, 6, 2, TH_HI);
        }
    }
    t[0] = 0;
    text_cat(t, g == 1 ? "RECORDED TO THE NEAREST STEP" : "NOTES SNAP TO ");
    if (g > 1) {
        text_cat(t, grid);
        text_cat(t, " AS YOU RECORD");
    }
    cv_text(6, 116, &FONT_XS, t, TH_TXT);
    cards_of(KD_REC, 0);
}

static void page_color(void)
{
    int i;
    char title[24], amount[12];
    const color_params_t *p = &proj.color[ui.trk];
    band_set(trk_rgb(ui.trk), TRK_NAME[ui.trk], ui.color_page ? "COLOR: MODULES" : "COLOR: AMOUNT", "PRE-SEND + USB STEM", 2, ui.color_page);
    rrect(4,4,232,124,6,C_PANEL);
    title[0] = 0;
    text_cat(title,TRK_NAME[ui.trk]);
    text_cat(title,p->v[COL_ON] ? " COLOR ON" : " COLOR BYPASS");
    int_text(p->v[COL_AMOUNT]*100/127,amount);
    text_cat(amount,"%");
    cv_text(12,10,&FONT_B,title,TH_INK);
    cv_text(184,10,&FONT_B,amount,TH_FG);
    for (i=0;i<4;i++) {
        int v=p->v[COL_WEAR+i], y=35+i*15;
        cv_text(12,y,&FONT_XS,COLOR_LABEL[COL_WEAR+i],TH_FG);
        cv_rect(89,y+4,130,3,C_GRID);
        if (v) cv_rect(89,y+4,v*130/127,3,TH_FG);
    }
    cv_text(12,107,&FONT_XS,ui.color_page ? "SELECT LEFT: AMOUNT / BYPASS" : "SELECT RIGHT: MODULES",TH_INK);
    cards_of(KD_COLOR,0);
}

static void page_insert(void)
{
    const track_insert_param_t *p = &proj.insert[ui.trk-1];
    band_set(trk_rgb(ui.trk), TRK_NAME[ui.trk], "INSERT", track_insert_name(p->mode), 0, 0);
    rrect(4, 4, 232, 124, 6, C_PANEL);
    cv_text(12, 16, &FONT_M, track_insert_name(p->mode), C_INK);
    cv_text(12, 56, &FONT_S, "SELECT: EFFECT / OFF", C_INK);
    cv_text(12, 80, &FONT_XS, "PRE-SEND + USB STEM", TH_FG);
    cv_text(12, 101, &FONT_XS, "FX: BACK", TH_FG);
    cards_of(KD_INSERT, 0);
}

/* the page now: its band's text and its knobs set, its picture drawn (the canvas begun and blitted by the caller) */
static void page(void)
{
    int inst = ui.trk != TR_DRUM;
    if (rec_page()) {
        page_rec();
        return;
    }
    switch (ui.page) {
    case PG_EDIT: inst ? page_iedit(0) : page_dedit(); break;
    case PG_ENV: page_iedit(1); break;
    case PG_LFO: page_lfo(); break;
    case PG_MIX: page_mix(); break;
    case PG_FX: page_fx(); break;
    case PG_INSERT: page_insert(); break;
    case PG_COLOR: page_color(); break;
    case PG_MST: page_mst(); break;
    case PG_ARP: page_arp(); break;
    case PG_MUTE: page_mute(); break;
    case PG_HOME: page_home(); break;
    case PG_SONG: page_song(); break;
    case PG_SETUP: page_setup(); break;
    case PG_KIT:
        kit_band("KIT", "THE KIT'S OWN KNOBS", 0, 0);
        page_map(KD_KIT, 1, 0, 0, 8, 28);
        cards_of(KD_KIT, 0);
        break;
    case PG_KEYS: page_keys(); break;
    case PG_GEN: page_gen(); break;
    case PG_PICK: page_pick(); break;
    default: inst ? page_roll() : page_steps(); break;
    }
}

static void draw(void)
{
    theme(sec_col());
    cv_begin(240, BODY_H, C_BODY);
    page();
    cv_blit(0, BODY_Y);
    band();
    strip();
}

static int page_moves(void)               /* a page whose picture moves by itself: its meters, a flash, a dot */
{
    int inst = ui.trk != TR_DRUM;
    if (rec.state == RS_ARMED || rec.state == RS_REC)
        return 1;
    if (rec_page())
        return 0;
    switch (ui.page) {
    case PG_HOME: case PG_MIX: case PG_MUTE: case PG_LFO: case PG_KEYS: case PG_ARP: return 1;
    case PG_SEQ: return !inst;
    case PG_EDIT: return !inst || ui.epage == 0;
    case PG_SONG: return sq.playing;
    case PG_MST: return ui.mstpage == 0 || ui.mstpage == 3 || ui.mstpage == 4 || ui.mstpage == 6 || ui.mstpage == 7;
    default: return 0;
    }
}

static void draw_live(void)               /* the band's meters; the page too where it moves; a meter card */
{
    theme(sec_col());
    if (page_moves()) {
        cv_begin(240, BODY_H, C_BODY);
        page();
        cv_blit(0, BODY_Y);
        if (ui.page == PG_MST && (ui.mstpage == 4 || ui.mstpage == 6))
            strip();
    }
    band();
}

static void speak(void)                   /* a screen reader that has just started listening hears the screen */
{
    char t[96];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, TRK_NAME[ui.trk]);
        text_cat(t, ". ");
        text_cat(t, PAGE_NAME[ui.page]);
        if (ui.trk == TR_DRUM) {
            text_cat(t, ". PAD ");
            text_cat(t, drums_pad_name(drum_model(ui.pad), ui.pad));
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* Boot screen: two text masks and the supplied paletted art on paper. The
 * platform calls this at power-on before app_ui_init, drawn in 48-row strips. */
static void logo_part(const uint8_t *a, int32_t x0, int32_t y0, int32_t w, int32_t h, int32_t top, int32_t n,
                      uint16_t c)
{
    uint32_t r = c >> 11, g = (c >> 5) & 63u, b = c & 31u;
    int32_t y, x;
    for (y = top; y < top + n; y++) {
        const uint8_t *row;
        if (y < y0 || y >= y0 + h)
            continue;
        row = a + (y - y0) * ((w + 1) / 2);
        for (x = 0; x < w; x++) {
            uint32_t al = (x & 1) ? (row[x >> 1] & 15u) : (row[x >> 1] >> 4u), bg, br, bgg, bb;
            uint16_t *p;
            if (!al)
                continue;
            p = &cv_px[(uint32_t)(y - top) * 240u + (uint32_t)(x0 + x)];
            if (al == 15u) {
                *p = swap16(c);
                continue;
            }
            bg = swap16(*p);
            br = bg >> 11;
            bgg = (bg >> 5) & 63u;
            bb = bg & 31u;
            *p = swap16((uint16_t)((((r * al + br * (15u - al)) / 15u) << 11) |
                                   (((g * al + bgg * (15u - al)) / 15u) << 5) | ((b * al + bb * (15u - al)) / 15u)));
        }
    }
}

static void logo_art(int32_t top, int32_t n)
{
    int32_t y, x;
    for (y = top; y < top + n; y++) {
        const uint8_t *row;
        uint16_t *out;
        if (y < LOGO_ART_Y || y >= LOGO_ART_Y + LOGO_ART_H)
            continue;
        row = LOGO_ART + (y - LOGO_ART_Y) * (LOGO_ART_W / 2);
        out = cv_px + (y - top) * 240 + LOGO_ART_X;
        for (x = 0; x < LOGO_ART_W; x += 2) {
            uint8_t pair = row[x / 2];
            out[x] = swap16(LOGO_ART_RGB[pair >> 4]);
            out[x+1] = swap16(LOGO_ART_RGB[pair & 15u]);
        }
    }
}

static void logo_outline(const uint8_t *mask, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t top, int32_t n)
{
    /* One-pixel dark outline using the same compact mask: no extra bitmap. */
    logo_part(mask, x - 1, y, w, h, top, n, C_BLACK);
    logo_part(mask, x + 1, y, w, h, top, n, C_BLACK);
    logo_part(mask, x, y - 1, w, h, top, n, C_BLACK);
    logo_part(mask, x, y + 1, w, h, top, n, C_BLACK);
}

void app_splash(void)
{
    int32_t y;
    _Static_assert(LOGO_ART_X == 0 && LOGO_ART_Y == 0 &&
                   LOGO_ART_W == 240 && LOGO_ART_H == 240, "full-bleed boot art");
    for (y = 0; y < 240; y += 48) {
        const int32_t n = 48;
        cv_begin(240, (uint32_t)n, C_BLACK);
        logo_art(y, n);
        logo_outline(LOGO_NAME, LOGO_NAME_X, LOGO_NAME_Y, LOGO_NAME_W, LOGO_NAME_H, y, n);
        logo_outline(LOGO_TAG, LOGO_TAG_X, LOGO_TAG_Y, LOGO_TAG_W, LOGO_TAG_H, y, n);
        logo_part(LOGO_NAME, LOGO_NAME_X, LOGO_NAME_Y, LOGO_NAME_W, LOGO_NAME_H, y, n, APP_SPLASH_RGB);
        logo_part(LOGO_TAG, LOGO_TAG_X, LOGO_TAG_Y, LOGO_TAG_W, LOGO_TAG_H, y, n, C_FOCUS);
        cv_blit(0, (uint32_t)y);
    }
    lcd_sync();
}

void app_ui_init(void)
{
    ui.held_step = -1;
    ui.pf_key = -1;
    ui.solo_trk = -1;
    ui.scene_key = -1;
    ui.knob_k = -1;
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

static void shed(void)                    /* the CPU short: fewer voices; back up once there is room again */
{
    uint32_t pct = plat_cpu_pct(), now = plat_ms(), f;
    if (pct > 85u && pool_now > 4u && now - ui.shed_t > 40u)
        pool_now--;
    else if (pct < 65u && pool_now < pool_max && now - ui.shed_t > 1000u)
        pool_now++;
    else
        return;
    ui.shed_t = now;
    f = plat_lock();
    syn_pool(pool_now);
    plat_unlock(f);
}

void app_frame(void)
{
    uint32_t now = plat_ms(), t = sq.ticks;
    input();
    ck.rec = ui.rec;
    if (sq.ext_stop) {                    /* MIDI CLOCK followed: a Stop */
        sq.ext_stop = 0;
        seq_stop();
    }
    if ((ui.btn >> B_REC & 1u) && !ui.rec_shown && rec.state == RS_IDLE && now - ui.rec_t >= 300u) {
        ui.rec_shown = 1;                 /* REC held: its page */
        ui.redraw = 1;
    }
    if (ck.count > 0) {                   /* the count-in: the beats left, large */
        float bl = bar_samples() * 0.25f;
        int b = (int)(((float)ck.count + bl - 1.0f) / bl);
        if (b != ui.count_shown) {
            char c[4];
            int_text(b, c);
            app_say("COUNT-IN", c);
            ui.count_shown = (int8_t)b;
        }
    } else if (ui.count_shown) {
        ui.count_shown = 0;
        ui.msg_until = now;
        ui.redraw = 1;
    }
    shed();
    {   /* the delay's line back once sampling, STUTTER and TAPE STOP are done with it */
        int free = rec.state == RS_IDLE && !rec.pending && !(perf.want & (1u << PF_STUT | 1u << PF_TAPE));
        if (free != fx_delay_on()) {
            uint32_t f = plat_lock();
            fx_delay_line(free);
            plat_unlock(f);
        }
    }
    if (rec.state == RS_DONE)
        rec_done();
    {   /* the master bus's meters: measured only while a page shows them */
        int meters = ui.page == PG_MST && (ui.mstpage == 0 || ui.mstpage == 6);
        if (meters != fx_meter_shown) {
            uint32_t f = plat_lock();
            fx_meter(meters);
            plat_unlock(f);
            fx_meter_shown = (uint8_t)meters;
        }
    }
    speak();
    if (t != ui.last_ticks) {             /* a step went by: the playhead moves, the page follows it */
        int st = sq.t[ui.trk].step;
        ui.last_ticks = t;
        if (sq.playing && st >= 0 && now - ui.spage_t > 3000u &&
            (ui.page == PG_SEQ || ui.page == PG_GEN || (ui.trk == TR_DRUM && ui.page == PG_EDIT)))
            ui.spage = (uint8_t)(st / 16);
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (sq.queued >= 0)
        ui.redraw = 1;
    lights();
    if (ui.redraw) {
        ui.redraw = 0;
        ui.live_t = now;
        draw();
    } else if (now - ui.live_t >= 33u) {  /* the meters, ~30 times a second: the band, and the page if it moves */
        ui.live_t = now;
        draw_live();
    }
}

/* ------------------------------------------------------------ companion --- */
/* three files: T2..T4's samples, as the bank keeps them (tools/fm1_sample.py sends a WAV as one). A write is the bank's:
 * free sectors, then the head; a sample that came makes its track a SAMPLER, all of it playing */
static const char *const SMP_NAME[NINST] = {"sample-t2", "sample-t3", "sample-t4"};
int app_file(uint32_t i, plat_file_info_t *f)
{
    int t = (int)i + 1;
    if (i >= NINST)
        return -1;
    f->name = SMP_NAME[i];
    f->kind = PLAT_FK_SAMPLE;
    f->flags = PLAT_FF_READ | PLAT_FF_WRITE | PLAT_FF_DELETE;
    f->size = bank.at[t] ? bank.size[t] : 0u;
    f->room = bank_room(t);
    return 0;
}
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n)
{
    int t = (int)i + 1;
    const uint8_t *p;
    if (i >= NINST || !bank.at[t] || off > bank.size[t] || n > bank.size[t] - off ||
        !(p = plat_data(bank.at[t] + BANK_REC)))
        return -1;
    while (n--)
        *dst++ = p[off++];
    return 0;
}
int app_file_begin(uint32_t i, uint32_t size)
{
    return i < NINST ? bank_begin((int)i + 1, size) : -1;
}
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n)
{
    return i < NINST ? bank_write((int)i + 1, off, src, n) : -1;
}
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc)
{
    int t = (int)i + 1;
    track_t *ts;
    if (i >= NINST || bank_commit(t, size, crc) != 0)
        return -1;
    if (!smp_frames[t]) {                 /* written whole, but not a sample */
        bank_kill(t);
        app_say("NOT A SAMPLE", TRK_NAME[t]);
        return 0;
    }
    if (rec.pending && rec.trk == t)      /* the computer's wins over a recording not stored */
        rec.pending = 0;
    ts = tset(t);
    if (ts->type != TY_SAMPLER) {
        uint32_t f = plat_lock();
        ts->type = TY_SAMPLER;
        syn_release(t, 0);
        syn_source(t, 1);
        plat_unlock(f);
    }
    ts->s_start = 0;
    ts->s_end = 1000;
    ts->s_lstart = 0;
    track_shape(t);
    dirty = 1;
    app_say("SAMPLE IN", TRK_NAME[t]);
    return 0;
}
int app_file_delete(uint32_t i)
{
    if (i >= NINST)
        return -1;
    if (rec.pending && rec.trk == (int)i + 1)
        rec.pending = 0;
    return bank_kill((int)i + 1);
}
int app_file_busy(void) { return sq.playing || rec.state != RS_IDLE; }

#ifdef PLAT_HOST
static const nstep_t *expect_step(const char *what, int skip)   /* "XXXN_S...": track N (2..4), step S (1-based) */
{
    int tr = atoi(what + skip);
    const char *u = strchr(what + skip, '_');
    if (tr < 2 || tr > NTRK || !u)
        return 0;
    return &ipart(tr - 1)->st[(atoi(u + 1) - 1) & (NSTEPS - 1)];
}

int app_expect(const char *what, int *got)
{
    dpart_t *p = dcur();
    const nstep_t *ns;
    if (!strncmp(what,"color",5)) {
        int t = atoi(what+5)-1, j;
        const char *u = strchr(what,'_');
        if (t < 0 || t >= NTRK || !u) return 0;
        j = atoi(u+1);
        if (j < 0 || j >= COL_NUM) return 0;
        *got = proj.color[t].v[j];
    } else if (!strncmp(what, "insert", 6)) {
        int t = atoi(what + 6) - 2;
        const char *u = strchr(what, '_');
        if (t < 0 || t >= NINST) return 0;
        *got = u ? proj.insert[t].v[atoi(u+1) & 3] : proj.insert[t].mode;
    } else if (!strcmp(what, "projfmt")) {       /* saved format, or 0 if none */
        *got = proj_file_format();
    } else if (!strcmp(what, "projsize")) {   /* bytes in RAM (not the packed saved size) */
        *got = (int)sizeof proj;
    } else if (!strcmp(what, "seed_dense")) { /* simulator only: exercise an incompressible project through SAVE */
        uint32_t rng = 0x579C0219u;
        int i, j, k, m;
        for (i = 0; i < NPAT; i++) for (j = 0; j < NINST; j++) {
            ipart_t *d = &proj.p[i].t[j];
            uint8_t *b = (uint8_t *)d->st;
            for (k = 0; k < NSTEPS; k++) for (m = 0; m < (int)sizeof(nstep_t); m++) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                *b++ = (uint8_t)((rng & 0x7Fu) | 1u);
            }
            d->len = NSTEPS;
        }
        dirty = 1;
        *got = 1;
    } else if (!strcmp(what, "savedsize")) {
        *got = plat_file_size(&PROJ_FILE);
    } else if (!strcmp(what, "splash")) {     /* the boot screen drawn now (a shot straight after sees it) */
        app_splash();
        *got = 1;
    } else if (!strcmp(what, "kit"))
        *got = proj.kit;
    else if (!strcmp(what, "pat"))
        *got = proj.pat + 1;
    else if (!strcmp(what, "pad"))
        *got = ui.pad;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "trk"))        /* the track picked, 1..4 */
        *got = ui.trk + 1;
    else if (!strcmp(what, "tmute"))
        *got = proj.tmute;
    else if (!strcmp(what, "playing"))
        *got = sq.playing;
    else if (!strcmp(what, "step"))
        *got = sq.t[TR_DRUM].step;
    else if (!strcmp(what, "len"))
        *got = p->len;
    else if (!strcmp(what, "mute"))       /* the pattern's drum mute bits */
        *got = p->mute;
    else if (!strcmp(what, "dirty"))
        *got = dirty;
    else if (!strcmp(what, "rec"))
        *got = ui.rec;
    else if (!strcmp(what, "queued"))
        *got = sq.queued >= 0 ? sq.queued + 1 : 0;
    else if (!strcmp(what, "undo"))
        *got = undo_n;
    else if (!strcmp(what, "nlk"))        /* the drum part's locks in use */
        *got = p->nlk;
    else if (!strcmp(what, "epage"))
        *got = ui.epage;
    else if (!strcmp(what, "envpage"))
        *got = ui.envpage;
    else if (!strcmp(what, "pool"))       /* the voices allowed now */
        *got = pool_now;
    else if (!strcmp(what, "gr"))         /* the master compressor's reduction now, dB x 10 */
        *got = (int)(fx_gr() * 10.0f);
    else if (!strcmp(what, "songn"))
        *got = proj.song_n;
    else if (!strcmp(what, "songon"))
        *got = proj.song_on;
    else if (!strcmp(what, "spos"))       /* the song's entry playing (1-based), 0 none */
        *got = srt.pos + 1;
    else if (!strcmp(what, "scur"))
        *got = ui.scur + 1;
    else if (!strcmp(what, "dpreset"))
        *got = proj.dpreset;
    else if (!strcmp(what, "kitcustom"))
        *got = kit_is_custom(proj.model);
    else if (!strncmp(what, "model", 5)) {
        int pad = atoi(what + 5);
        if (pad < 0 || pad >= NPADS)
            return 0;
        *got = drum_model(pad);
    }
    else if (!strncmp(what, "songe", 5)) {   /* songeS_K: entry S (1-based), K 0 its pattern (1-based), 1 its bars */
        int e = atoi(what + 5) - 1;
        const char *u = strchr(what, '_');
        if (e < 0 || e >= NSONG || !u)
            return 0;
        *got = atoi(u + 1) ? proj.song[e].bars : proj.song[e].pat + 1;
    } else if (!strncmp(what, "usnd", 4) || !strncmp(what, "ukit", 4)) {   /* usndS, ukitS: user slot S (1-based) used */
        int i = atoi(what + 4) - 1;
        if (i < 0 || i >= (what[1] == 's' ? NUSER : NUKIT))
            return 0;
        *got = what[1] == 's' ? user.snd[i].used : user.kit[i].used;
    } else if (!strcmp(what, "mixpage"))
        *got = ui.mixpage;
    else if (!strcmp(what, "fxpage"))
        *got = ui.fxpage;
    else if (!strcmp(what, "mstpage"))
        *got = ui.mstpage;
    else if (!strncmp(what, "qclip", 5)) {   /* qclipN: track N's clip waiting (1-based; 0 none, -1 to stop) */
        int t = atoi(what + 5) - 1;
        if (t < 0 || t >= NTRK)
            return 0;
        *got = qclip[t] == -1 ? 0 : qclip[t] == -2 ? -1 : qclip[t] + 1;
    } else if (!strncmp(what, "clip", 4) && what[4] >= '1' && what[4] <= '4') {   /* clipN: the pattern track N plays
                                                                                    * (1-based; 0 stopped) */
        int t = atoi(what + 4) - 1;
        if (t < 0 || t >= NTRK)
            return 0;
        *got = clip[t] + 1;
    } else if (!strcmp(what, "pfon")) {   /* the perform keys the sound has on (bits); solo: the track soloed (1-based) */
        *got = perf.on;
    } else if (!strncmp(what, "follow", 6)) {   /* followP: scene P's action (0..4); fbarsP: its bars */
        int q = atoi(what + 6) - 1;
        if (q < 0 || q >= NPAT)
            return 0;
        *got = proj.follow[q] & 7;
    } else if (!strncmp(what, "fbars", 5)) {
        int q = atoi(what + 5) - 1;
        if (q < 0 || q >= NPAT)
            return 0;
        *got = (proj.follow[q] >> 3) + 1;
    } else if (!strcmp(what, "sounds")) { /* SOUNDS PER PATTERN; mclock: MIDI CLOCK 0 off 1 sent 2 followed */
        *got = sounds_pp();
    } else if (!strcmp(what, "mclock")) {
        *got = (int)(proj.recq >> 2 & 3u);
    } else if (!strcmp(what, "mclkerr")) {   /* MIDI CLOCK followed: the groove from the last 16th's clock, ms x 10 */
        *got = (int)(sq.mclk_err * (10000.0f / 44100.0f));
    } else if (!strncmp(what, "psnd", 4)) {   /* psndP_N: pattern P keeps track N's sound (the drums: their kit + 1) */
        int q = atoi(what + 4) - 1;
        const char *u = strchr(what, '_');
        int tr = u ? atoi(u + 1) - 1 : -1;
        if (q < 0 || q >= NPAT || tr < 0 || tr >= NTRK)
            return 0;
        *got = tr == TR_DRUM ? proj.pkit[q] : proj.psnd[q][tr - 1].used;
    } else if (!strcmp(what, "rolls")) {  /* ROLL's replays since the last look */
        *got = (int)perf.rolls;
        perf.rolls = 0;
    } else if (!strcmp(what, "solo")) {
        *got = ui.solo_trk + 1;
    } else if (!strcmp(what, "bpm")) {
        *got = proj.bpm10 / 10;
    } else if (!strcmp(what, "launch"))
        *got = launch_mode();
    else if (!strcmp(what, "click"))      /* the metronome: 0 OFF 1 ON 2 REC; the count-in's bars; the level; the grid */
        *got = click_mode();
    else if (!strcmp(what, "countin"))
        *got = countin_bars();
    else if (!strcmp(what, "clevel"))
        *got = click_level();
    else if (!strcmp(what, "recq"))
        *got = recq_grid();
    else if (!strcmp(what, "counting"))   /* in the count-in */
        *got = ck.count > 0;
    else if (!strcmp(what, "clicks")) {   /* clicks since the last look */
        *got = (int)ck.n;
        ck.n = 0;
    } else if (!strcmp(what, "recpage")) {
        *got = rec_page();
    } else if (!strncmp(what, "chord", 5)) {   /* chordN: track N's chord mode (0 off), chordinvN its inversion */
        int inv = what[5] == 'i', tr = atoi(what + (inv ? 8 : 5)) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = inv ? tset(tr)->chord_inv : tset(tr)->chord;
    }
    else if (!strcmp(what, "mpre"))       /* the chain preset loaded, and whether a knob it sets has moved since */
        *got = proj.fx[FXP_M_PRESET];
    else if (!strcmp(what, "medit"))
        *got = fx_preset_edited();
    else if (!strcmp(what, "lgr"))        /* the limiter's reduction, dB x 10 */
        *got = (int)(fx_lgr() * 10.0f);
    else if (!strcmp(what, "lufs") || !strcmp(what, "lufsm"))   /* short-term / momentary loudness, LUFS x 10 */
        *got = (int)(fx_lufs(what[4] == 'm') * 10.0f);
    else if (!strcmp(what, "corr"))       /* the correlation x 100 */
        *got = (int)(fx_corr() * 100.0f);
    else if (!strncmp(what, "eqdb", 4))   /* eqdbHZ: the EQ's response at HZ, dB x 10 */
        *got = (int)(fx_eq_db((float)atoi(what + 4)) * 10.0f + 0.5f * (fx_eq_db((float)atoi(what + 4)) < 0.0f ? -1 : 1));
    else if (!strcmp(what, "inpk") || !strcmp(what, "outpk"))   /* the bus's peak in / out since the last look, dB x 10 */
        *got = (int)(fx_db(what[0] == 'i' ? fx_in_peak(1) : fx_out_peak(1)) * 10.0f);
    else if (!strncmp(what, "stage", 5))  /* stageN: the chain's stage N (0 EQ .. 4 LIMIT) doing something */
        *got = fx_stage_on(atoi(what + 5));
    else if (!strcmp(what, "clips")) {    /* samples past the ceiling since the last look */
        *got = (int)clips;
        clips = 0;
    } else if (!strcmp(what, "stems"))    /* the USB stems written this block (the host: always taken) */
        *got = STEMS && stems_on;
    else if (!strcmp(what, "stemreset")) {   /* every stem's peak started again (1) */
        int s;
        for (s = 0; s < NTRK; s++)
            stem_pk[s][0] = stem_pk[s][1] = 0.0f;
        *got = 1;
    } else if (!strcmp(what, "wlit")) {   /* the white keys lit, a bit a key (white 1: 1) */
        *got = lit_white;
    } else if (!strcmp(what, "wdim")) {   /* the white keys glowing */
        *got = dim_white;
    } else if (!strcmp(what, "stemovers")) {   /* stem samples clipped since the last look */
        *got = (int)stem_overs;
        stem_overs = 0;
    } else if (!strncmp(what, "stem", 4) && what[4] >= '1' && what[4] < '1' + NTRK) {
        /* stemN, stemNl, stemNr: track N's stem's peak since the last look (both sides, or one), dB x 10; -1200 silent */
        int s = what[4] - '1', side = what[5] == 'l' ? 0 : what[5] == 'r' ? 1 : -1;
        float pk = side >= 0 ? stem_pk[s][side] : stem_pk[s][0] > stem_pk[s][1] ? stem_pk[s][0] : stem_pk[s][1];
        if (side != 1)
            stem_pk[s][0] = 0.0f;
        if (side != 0)
            stem_pk[s][1] = 0.0f;
        *got = (int)(fx_db(pk) * 10.0f);
    } else if (!strcmp(what, "dlyon"))    /* the delay in the chain (not lent to sampling) */
        *got = fx_delay_on();
    else if (!strcmp(what, "pkl") || !strcmp(what, "pkr")) {   /* a side's peak since the last look, dB x 10 */
        float p = what[2] == 'l' ? peak_l : peak_r;
        int db = -1000;
        if (p > 0.00001f) {               /* (20 log10 by halving: steps of 6 dB, then the rest by a line) */
            float d = 0.0f;
            while (p < 0.5f) {
                p *= 2.0f;
                d -= 6.0206f;
            }
            db = (int)((d + (p - 1.0f) * 8.6859f) * 10.0f);
        }
        if (what[2] == 'l')
            peak_l = 0.0f;
        else
            peak_r = 0.0f;
        *got = db;
    } else if (!strncmp(what, "mix", 3)) {   /* mixN_K: track N, K 0 level 1 pan 2 reverb 3 delay */
        int tr = atoi(what + 3) - 1;
        const char *u = strchr(what, '_');
        if (tr < 0 || tr >= NTRK || !u)
            return 0;
        *got = mix_value(tr, atoi(u + 1) & 3);
    } else if (!strncmp(what, "fx", 2)) {    /* fxI: the FX knob I (dsp/fx.h FXP_*) */
        int i = atoi(what + 2);
        if (i < 0 || i >= FXP_NUM)
            return 0;
        *got = proj.fx[i];
    } else if (!strcmp(what, "lfopage"))
        *got = ui.lfopage;
    else if (!strcmp(what, "mslot"))      /* MATRIX: the slot on the knobs (1-based) */
        *got = ui.mslot + 1;
    else if (!strcmp(what, "arppage"))
        *got = ui.arppage;
    else if (!strncmp(what, "lset", 4) || !strncmp(what, "arpset", 6)) {
        /* lsetN_K: track N's LFO, K 0 shape 1 rate 2 div 3 sync 4 retrig 5 fade 6 phase 7 dest 8 depth 9 pad;
         * arpsetN_K: K 0 mode 1 rate 2 octaves 3 gate 4 latch */
        int arpq = what[0] == 'a', tr = atoi(what + (arpq ? 6 : 4)) - 1, k;
        const char *u = strchr(what, '_');
        if (tr < 0 || tr >= NTRK || !u || (arpq && tr < 1))
            return 0;
        k = atoi(u + 1);
        if (arpq) {
            const track_t *ts = tset(tr);
            const uint8_t v[5] = {ts->arp_mode, ts->arp_rate, ts->arp_oct, ts->arp_gate, ts->arp_latch};
            *got = k >= 0 && k < 5 ? v[k] : -1;
        } else {
            const lfo_t *l = &proj.lfo[tr];
            const int v[10] = {l->shape, l->rate, l->div, l->sync, l->retrig, l->fade, l->phase,
                               l->dest == LFO_OFF ? -1 : l->dest, l->depth, l->pad};
            *got = k >= 0 && k < 10 ? v[k] : -1;
        }
    } else if (!strncmp(what, "lfo", 3)) {   /* lfoN: track N's LFO now, x 1000 */
        int tr = atoi(what + 3) - 1;
        if (tr < 0 || tr >= NTRK)
            return 0;
        *got = (int)(lfo_rt[tr].out * 1000.0f);
    } else if (!strncmp(what, "arpn", 4)) {  /* arpnN: the notes in track N's arpeggiator */
        int tr = atoi(what + 4) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = arp[tr].n;
    } else if (!strncmp(what, "probe", 5)) { /* probeN_W: the newest voice, W 0 pitch16, 1 cutoff (<< 8) */
        int tr = atoi(what + 5) - 1;
        const char *u = strchr(what, '_');
        if (tr < 1 || tr >= NTRK || !u)
            return 0;
        *got = syn_probe(tr, atoi(u + 1));
    } else if (!strncmp(what, "syn", 3)) {   /* synN_ID: the synth's value now (a lock's included) */
        int tr = atoi(what + 3) - 1;
        const char *u = strchr(what, '_');
        if (tr < 1 || tr >= NTRK || !u)
            return 0;
        *got = syn_get(tr, atoi(u + 1));
    } else if (!strncmp(what, "ilock", 5)) { /* ilockN_S_P: track N, step S, parameter P's lock (-1000 none) */
        int tr = atoi(what + 5) - 1, j;
        const char *u = strchr(what, '_'), *v = u ? strchr(u + 1, '_') : 0;
        if (tr < 1 || tr >= NTRK || !v)
            return 0;
        j = ilock_find(ipart(tr), (atoi(u + 1) - 1) & (NSTEPS - 1), atoi(v + 1));
        *got = j >= 0 ? ipart(tr)->lk[j].val : -1000;
    } else if (!strncmp(what, "inlk", 4)) {  /* inlkN: track N's locks in this pattern */
        int tr = atoi(what + 4) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = ipart(tr)->nlk;
    } else if (!strcmp(what, "rstate"))     /* sampling: 0 idle, 1 armed, 2 recording, 3 done */
        *got = rec.state;
    else if (!strcmp(what, "rpos"))
        *got = (int)rec.pos;
    else if (!strcmp(what, "rpending"))
        *got = rec.pending;
    else if (!strcmp(what, "bseq"))
        *got = (int)bank.seq;
    else if (!strncmp(what, "sframes", 7) || !strncmp(what, "srate", 5) || !strncmp(what, "bsize", 5) ||
             !strncmp(what, "bat", 3)) {   /* sframesN, srateN: what track N plays; bsizeN, batN: its bank record */
        int tr = atoi(what + (what[1] == 'f' ? 7 : what[1] == 'r' || what[1] == 's' ? 5 : 3)) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = what[1] == 'f' ? (int)smp_frames[tr] : what[1] == 'r' ? (int)smp_rate[tr]
             : what[1] == 's' ? (int)bank.size[tr] : (int)(bank.at[tr] ? bank.at[tr] - BANK_LO : -1);
    } else if (!strncmp(what, "sshape", 6)) {   /* sshapeN_K: K 0 start, 1 end, 2 loop, 3 loop start, 4 root, 5 tune, 6 rate */
        int tr = atoi(what + 6) - 1;
        const char *u = strchr(what, '_');
        const track_t *ts;
        if (tr < 1 || tr >= NTRK || !u)
            return 0;
        ts = tset(tr);
        switch (atoi(u + 1)) {
        case 0: *got = ts->s_start; break;
        case 1: *got = ts->s_end; break;
        case 2: *got = ts->s_loop; break;
        case 3: *got = ts->s_lstart; break;
        case 4: *got = ts->s_root; break;
        case 5: *got = ts->s_tune; break;
        default: *got = ts->s_q; break;
        }
    }
    else if (!strncmp(what, "pitch", 5))  /* pitchN: track N's newest voice, 1/16 semitone (-1: none) */
        *got = syn_pitch16(atoi(what + 5) - 1);
    else if (!strncmp(what, "preset", 6) || !strncmp(what, "edited", 6) || !strncmp(what, "snd", 3)) {
        int tr = atoi(what + (what[0] == 's' ? 3 : 6)) - 1;  /* presetN, editedN, sndN_ID */
        const char *u = strchr(what, '_');
        if (tr < 1 || tr >= NTRK)
            return 0;
        if (what[0] == 'p')
            *got = tset(tr)->preset;
        else if (what[0] == 'e')
            *got = tset(tr)->edited;
        else
            *got = u ? tset(tr)->snd[atoi(u + 1) % SP_NUM] : -1;
    } else if (!strcmp(what, "held"))
        *got = ui.held_step >= 0 ? ui.held_step + 1 : 0;
    else if (!strcmp(what, "voices"))     /* the test voice's voices sounding */
        *got = syn_voices(-1);
    else if (!strncmp(what, "voices", 6)) /* voicesN: track N's (2..4) */
        *got = syn_voices(atoi(what + 6) - 1);
    else if (!strncmp(what, "hits", 4)) { /* hitsN: pad N's steps in this pattern */
        int pad = atoi(what + 4), s, c = 0;
        for (s = 0; s < NSTEPS; s++)
            c += bit_get(p->hit[pad % NPADS], s);
        *got = c;
    } else if (!strncmp(what, "hit", 3)) { /* hitP_S: pad P, step S (1-based) */
        int pad = atoi(what + 3);
        const char *u = strchr(what, '_');
        *got = u ? bit_get(p->hit[pad % NPADS], (atoi(u + 1) - 1) & (NSTEPS - 1)) : -1;
    } else if (!strncmp(what, "prob", 4)) { /* probP_S: pad P's step S chance, % */
        int pad = atoi(what + 4), i;
        const char *u = strchr(what, '_');
        i = u ? lock_find(p, (atoi(u + 1) - 1) & (NSTEPS - 1), pad % NPADS, LK_PROB) : -1;
        *got = i >= 0 ? p->lk[i].val : 100;
    } else if (!strncmp(what, "lock", 4)) {   /* lockP_S_I: pad P's step S, knob I locked to (-1: none) */
        int pad = atoi(what + 4), s, k, i;
        const char *u = strchr(what, '_'), *v = u ? strchr(u + 1, '_') : 0;
        s = u ? (atoi(u + 1) - 1) & (NSTEPS - 1) : 0;
        k = v ? atoi(v + 1) : 0;
        i = lock_find(p, s, pad % NPADS, lk_param(drum_model(pad % NPADS), k));
        *got = i >= 0 ? p->lk[i].val : -1;
    } else if (!strncmp(what, "acc", 3))  /* accS */
        *got = bit_get(p->acc, (atoi(what + 3) - 1) & (NSTEPS - 1));
    else if (!strncmp(what, "pot", 3)) {  /* potP_I: the kit's pad P, parameter I */
        int pad = atoi(what + 3);
        const char *u = strchr(what, '_');
        *got = u ? drums_get(drum_model(pad), pad, atoi(u + 1)) : -1;
    } else if (!strcmp(what, "genpage")) {   /* the 303 generator's page: 0 ACID, 1 NOTES */
        *got = ui.genpage;
    } else if (!strncmp(what, "gen", 3)) {   /* genN_K: track N's part's generator, K 0 seed (0: never) 1 mutates 2 root
                                              * 3 scale 4 notes 5 density 6 slide 7 accent 8 octave 9 whole */
        int tr = atoi(what + 3) - 1, k;
        const char *u = strchr(what, '_');
        const gen_t *g;
        if (tr < 1 || tr >= NTRK || !u)
            return 0;
        g = gen_of(tr);
        k = atoi(u + 1);
        {
            const int v[10] = {g->seed, g->mut, g->root, g->scale, g->notes, g->dens, g->slide, g->accent, g->oct,
                               g->whole};
            *got = k >= 0 && k < 10 ? v[k] : -1;
        }
    } else if (!strncmp(what, "ilen", 4)) {  /* ilenN: track N's part's length */
        int tr = atoi(what + 4) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = ipart(tr)->len;
    } else if (!strncmp(what, "nacc", 4) || !strncmp(what, "nslide", 6) || !strncmp(what, "noffscale", 9) ||
               !strncmp(what, "nwhole", 6)) {
        /* naccN: track N's part's accents (NS_ACC); nslideN: its slides (NS_SLIDE, into a next note); noffscaleN: its
         * notes not in its generator's scale (from the root); nwholeN: its WHOLE notes (held most of a step or more) */
        int kind = what[1] == 'a' ? 0 : what[1] == 's' ? 1 : what[1] == 'o' ? 2 : 3;
        int tr = atoi(what + (kind == 0 ? 4 : kind == 2 ? 9 : 6)) - 1;
        int s, c = 0, len, sc;
        const ipart_t *ip;
        if (tr < 1 || tr >= NTRK)
            return 0;
        ip = ipart(tr);
        len = ip->len;
        sc = gen_of(tr)->scale % NGSCALES;
        for (s = 0; s < len; s++) {
            const nstep_t *a = &ip->st[s];
            if (!a->note[0])
                continue;
            if (kind == 0)
                c += (a->vel & NS_ACC) != 0;
            else if (kind == 1)
                c += (a->prob & NS_SLIDE) && ip->st[s + 1 < len ? s + 1 : 0].note[0] != 0;
            else if (kind == 3)
                c += !(a->prob & NS_SLIDE) && a->gate >= 7;
            else if (kind == 2) {
                int d = ((int)a->note[0] - gen_of(tr)->root + 120) % 12, j, in = 0;
                for (j = 0; j < gen_scale_n(sc); j++)
                    in |= GS_DEG[sc][j] == d;
                c += !in;
            }
        }
        *got = c;
    } else if (!strncmp(what, "linemark", 8) || !strncmp(what, "linesame", 8)) {
        /* linemarkN: track N's part's steps remembered (0); linesameN: 1 if they are as remembered */
        static uint32_t mark;
        int tr = atoi(what + 8) - 1, s, i;
        uint32_t h = 2166136261u;
        if (tr < 1 || tr >= NTRK)
            return 0;
        for (s = 0; s < NSTEPS; s++)
            for (i = 0; i < (int)sizeof(nstep_t); i++)
                h = (h ^ ((const uint8_t *)&ipart(tr)->st[s])[i]) * 16777619u;
        if (what[4] == 'm') {
            mark = h;
            *got = 0;
        } else {
            *got = h == mark;
        }
    } else if (!strncmp(what, "nsteps", 6)) {   /* nstepsN: track N's steps with notes */
        int tr = atoi(what + 6), s, c = 0;
        if (tr < 2 || tr > NTRK)
            return 0;
        for (s = 0; s < NSTEPS; s++)
            c += ipart(tr - 1)->st[s].note[0] != 0;
        *got = c;
    } else if (!strncmp(what, "note", 4)) {     /* noteN_S_I: track N, step S, the chord's note I (0: none) */
        const char *u, *v;
        if (!(ns = expect_step(what, 4)))
            return 0;
        u = strchr(what, '_');
        v = strchr(u + 1, '_');
        *got = ns->note[(v ? atoi(v + 1) : 0) & (NCHORD - 1)];
    } else if (!strncmp(what, "vel", 3)) {      /* velN_S */
        if (!(ns = expect_step(what, 3)))
            return 0;
        *got = ns_vel(ns);
    } else if (!strncmp(what, "gate", 4)) {     /* gateN_S, in eighths of a step */
        if (!(ns = expect_step(what, 4)))
            return 0;
        *got = ns->gate;
    } else if (!strncmp(what, "chance", 6)) {   /* chanceN_S */
        if (!(ns = expect_step(what, 6)))
            return 0;
        *got = ns_prob(ns);
    } else if (!strncmp(what, "iacc", 4)) {     /* iaccN_S: the step's accent mark; islideN_S its slide's */
        if (!(ns = expect_step(what, 4)))
            return 0;
        *got = (ns->vel & NS_ACC) != 0;
    } else if (!strncmp(what, "islide", 6)) {
        if (!(ns = expect_step(what, 6)))
            return 0;
        *got = (ns->prob & NS_SLIDE) != 0;
    } else if (!strncmp(what, "tlen", 4)) {     /* tlenN: track N's part length (1: the drums) */
        int tr = atoi(what + 4) - 1;
        if (tr < 0 || tr >= NTRK)
            return 0;
        *got = *part_len(tr);
    } else if (!strncmp(what, "tstep", 5)) {    /* tstepN: the step track N plays now (1-based, 0 none) */
        int tr = atoi(what + 5) - 1;
        if (tr < 0 || tr >= NTRK)
            return 0;
        *got = sq.t[tr].step + 1;
    } else if (!strncmp(what, "type", 4)) {     /* typeN */
        int tr = atoi(what + 4) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = tset(tr)->type;
    } else if (!strncmp(what, "oct", 3)) {      /* octN */
        int tr = atoi(what + 3) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = tset(tr)->oct;
    } else if (!strncmp(what, "level", 5)) {    /* levelN */
        int tr = atoi(what + 5) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = tset(tr)->level;
    } else if (!strncmp(what, "cursor", 6)) {   /* cursorN: step recording's step (1-based) */
        int tr = atoi(what + 6) - 1;
        if (tr < 1 || tr >= NTRK)
            return 0;
        *got = ui.cursor[tr] + 1;
    } else
        return 0;
    return 1;
}
#endif
