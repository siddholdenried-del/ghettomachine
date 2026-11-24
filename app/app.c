/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal (fm1-base's template, which this starts from) */
/* GhettoMachine: four tracks (PLAN.md, phase 1).
 *
 * T1 is the drum track, X0X-style: two kits (909 / 808, PRESETS), 11 pads on the black keys, accents (SEQ held +
 * steps), per-pad editing (EDIT), per-step chance (a step held + SELECT) and locks (EDIT, a step held + a knob), and a
 * MUTE page (ENV) whose mutes belong to the pattern.
 * T2..T4 are instrument tracks. Each has a TYPE: MONO or POLY, both playing the synth (dsp/synth.h: Felucca's ANALOG
 * voice, 16 voices shared, on both cores; the SAMPLER type went in 0.5). A step holds a chord of up to 4 notes, a velocity, a gate
 * and a chance. The KEYS page plays them from all 27 keys; REC records them, live while playing (to the nearest
 * step) or step by step while stopped (a chord per step, the cursor moving on when the keys are let go).
 * A pattern holds all four tracks; each track's part has its own length and rate. ALGORITHM (or SEL held + pad keys
 * 1-4) picks the track; HOME + pad keys 1-4 mute tracks. The project is a file in the data area (A / B copies).
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope.
 * The engines are behind dsp/drums.h and dsp/synth.h (their own units). */
#include "dsp/drums.h"
#ifdef PLAT_HOST
#include <stdlib.h>
#endif
#include "dsp/synth.h"
#include "dsp/track_insert.h"
#include "dsp/color.h"
#include "dsp/fx.h"
#include "dsp/uimath.h"
#include "logo.h"                       /* tools/gen_logo.py: the boot screen's logo */
#include "mono_font.h"                  /* tools/gen_mono.py: FONT_MS, FONT_MB (IBM Plex Mono) */
#include <stddef.h>

#include "v3/trial.h"
#include "v3/presets.h"
#include "v3_metrics.h"

/* The stock 808 / 909 engine's sound (mode 0, STOCK) is the simulator's only since 0.5: the FM-1 plays the DrumEngine
 * (1 dry, 2 through its bus), and the stock engine's render, triggers and cymbal samples (about 140 KB) are not in its
 * image. Its knobs' tables and values stay (saved projects, locks and kits hold them). */
#ifdef PLAT_HOST
#define V3_MODE_MIN 0
#else
#define V3_MODE_MIN 1
#define drums_render(k, l, r, rv, dl, n) ((void)0)
#define drums_trigger_mapped(k, m, p, v) ((void)0)
#define drums_cut_mapped(k, m, p) ((void)0)
#define drums_cut_pad(p) ((void)0)
#define drums_governor_tail_active(p) 0
#define drums_governor_release_tail(p) 0
#endif
static volatile uint8_t v3_mode = 1;
#ifdef PLAT_HOST
#define V3ON v3_mode                      /* the DrumEngine plays (the simulator's older suites run the stock kits: 0) */
#else
#define V3ON 1                            /* the FM-1: always (0.5). Written so, the stock 808 / 909's pages, knobs and
                                           * locks compile away there */
#endif
static void v3_set_mode(unsigned mode)
{
    if (mode < V3_MODE_MIN || mode > 2 || v3_mode == mode) return;
    if (!mode || !V3ON) trial_quiet();
    if (mode && !V3ON)                 /* (the stock kits stop where they are: not to ring on when they come back) */
        for (int r = 0; r < DRUMS_NPADS; r++)
            drums_cut_pad(r);
    v3_mode = (uint8_t)mode;
    trial_metrics_reset();
}

static const char *trial_pad_name(int kit, int pad)
{
    static const char *const names[] = {"BD","SD","CP","LT","MT","HT","CH","OH","CY","P1","P2"};
    return V3ON && (unsigned)pad < 11u ? names[pad] : drums_pad_name(kit, pad);
}

#define NPADS DRUMS_NPADS
#define NPAT 16
#define NSTEPS 128
#define OLD_STEPS 64
#define STEP_WORDS (NSTEPS / 32)
#define NPOTS 12                          /* knob positions kept per pad (the engines have at most 11) */
#define OLD_LOCKS 16                      /* a part's locks up to format 10, drums and instruments alike */
#define NLOCKS 32                         /* per drum part: a step's chance or a knob locked for one pad's step */
#define LK_KIT606 0x40u                    /* 606 locks keep their own pots */
#define LK_KIT808 0x80u                   /* lock_t.param: the 808's knob (else the 909's) */
#define LK_KIT707 (LK_KIT606 | LK_KIT808) /* both bits unused by legacy locks */
#define LK_707_CB 0xD0u                   /* no historic 707 knob used bit 4 or 5 */
#define LK_707_TB 0xE0u
#define LK_PROB 0x7Fu                     /* lock_t.param: the step's chance, either kit */
#define LK_VEL 0x7Du                      /* lock_t.param: the hit's velocity (0.5), 1..127 (none: normal, or its accent) */
#define LK_SUB 0x7Eu                      /* lock_t.param: the step's substeps (0.5), 2..4: it plays that many times */
#define LK_NUDGE 0x7Cu                    /* lock_t.param: the hit late (0.5), 1..15 sixteenths of its step */
#define IL_NUDGE 0x7Eu                    /* ilock_t.param: an instrument step late, the same */
#define IL_X0 0x70u                       /* ilock_t.param IL_X0 + XL_*: the step's LFO 1 RATE (its division, synced) and
                                           * DEPTH, the arpeggiator's RATE and GATE (0.5; not sound parameters) */
enum { XL_LRATE, XL_LDEPTH, XL_ARATE, XL_AGATE, XL_NUM };
#define IL_SUB 0x7Fu                      /* ilock_t.param: an instrument step's substeps (not a sound parameter) */
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
#define PROJ_VERSION 12                   /* 12: MIX PER PATTERN (pmix, pfx) appended (11's layout before them); 11: 32 locks a part (10's patterns had 16: pattern_v10_t); 10: the DrumEngine's patch (the drums' edits) appended (9's layout before
                                           * it); 9: BongVerb's and BongDelay's knobs in the sends' slots (fx_migrate; the
                                           * layout is 8's); 8: each clip its own LFO (plfo); 7: combined Color and mixed
                                           * kits; 1-5 and 7-9 readable, private divergent v6 rejected */
#define EIGHT_VERSION "0.4"
#define NSONG 32                          /* the song chain's entries */
#define NUSER 16                          /* user sounds (T2..T4) */
/* IDs are persisted in tracks and per-pattern sounds: old factory 0..23, USER 1..16 at 24..39,
 * new factory from 40. The physical PRESETS order follows those IDs (skipping empty user slots).
 * The DSP factory table itself stays contiguous; never use its growing count as a user offset. */
#define PRESET_USER_FIRST SYN_FACTORY_LEGACY_COUNT
#define PRESET_NEW_FIRST (PRESET_USER_FIRST + NUSER)
#define PRESET_EXTRA_FIRST (PRESET_NEW_FIRST + SYN_MOTOR_CITY_COUNT)
#define PRESET_LIMIT (PRESET_EXTRA_FIRST + SYN_NEW_BANKS * SYN_BANK_SIZE)
_Static_assert(PRESET_LIMIT <= 256, "project and per-pattern preset IDs are unsigned bytes");
static int user_preset_slot(int id) { return id >= PRESET_USER_FIRST && id < PRESET_NEW_FIRST ? id - PRESET_USER_FIRST : -1; }
static int factory_preset_index(int id) { return id >= PRESET_NEW_FIRST ? id - NUSER : id; }
#define NUKIT 8                           /* user kits (the drums) */
/* the drums' presets PRESET steps through: the 909 (0), the 808 (1), the user kits (2 ..). The 606 and the 707 came
 * after them (DPRESET_606, _707) until they were cut (21 Nov 2026, dsp/drums.h): a project saved on one loads on the
 * 808 (a 606) or the 909 (a 707), project_check */
#define DPRESET_606 (2 + NUKIT)
#define DPRESET_707 (DPRESET_606 + 1)
/* the DrumEngine's genre kits after them (0.5: presets.h KIT_GENRES, ELECTRO first). With the DrumEngine, PRESETS on
 * the drums goes through these; the stock 808 / 909 and the user kits (their knobs) only with the stock engine. A kit
 * of either kind is the other engine's too: a genre's stock kit (GENRE_STOCK); a stock or user kit's genre: ELECTRO (the
 * DrumEngine's factory kit, which every project played until 0.5's genre kits) */
#define DPRESET_GENRE (DPRESET_707 + 1)
#define DPRESET_COUNT (DPRESET_GENRE + KIT_GENRES)
static const uint8_t GENRE_STOCK[KIT_GENRES] = {KIT_808, KIT_909, KIT_909, KIT_808, KIT_808, KIT_909,
                                                KIT_909, KIT_909, KIT_808, KIT_808, KIT_909, KIT_909};
static const char *const GENRE_SHORT[KIT_GENRES] = {"ELC", "HSE", "TEC", "TRP", "BMB", "DNB", "GRG", "DSC", "DUB", "LOF",
                                                    "IND", "MIN"};   /* (the small track labels) */
static int kit_now(int kit) { return kit == KIT_606 ? KIT_808 : kit == KIT_707 ? KIT_909 : kit; }   /* a cut kit's heir */
static int dpreset_factory(int i) { return i >= 0 && i < 2 ? i : -1; }
static int kit_factory(int i) { return kit_now(i); }
static int dpreset_user(int i) { return i >= 2 && i < DPRESET_606; }
static int dpreset_genre(int i) { return i >= DPRESET_GENRE && i < DPRESET_COUNT ? i - DPRESET_GENRE : -1; }
static int dpreset_valid(int i) { return dpreset_factory(i) >= 0 || dpreset_user(i) || dpreset_genre(i) >= 0; }
static int dpreset_genre_of(int i);       /* the DrumEngine's kit for a kit id (below, by the user kits) */
static int dpreset_display(int i) { return i; }
static int dpreset_id(int i) { return i; }


/* ------------------------------------------------------------------ project --- */
enum { RATE_16, RATE_8, RATE_32, RATE_16T, RATE_8T, NRATES };
static const char *const RATE_NAME[NRATES] = {"1/16", "1/8", "1/32", "1/16T", "1/8T"};
static const float RATE_MUL[NRATES] = {1.0f, 2.0f, 0.5f, 2.0f / 3.0f, 4.0f / 3.0f};
enum { TY_MONO, TY_POLY, NTYPES };           /* (2 was SAMPLER until 0.5: a saved one loads POLY) */

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

#define NILOCKS 32                        /* per instrument part: a sound parameter locked for one step */
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
    uint16_t s_start, s_end, s_lstart;    /* (s_*: the SAMPLER's until 0.5, unused: kept so saves keep their layout) */
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
#define LFO_V3 (DRUM_MODEL_NUM + 1)        /* the drums' LFO's rsv[0]: its DEST is one of the DrumEngine's controls of the
                                           * pad (v3/presets.h); else the model + 1 whose knob it is (0: the pad's own) */

enum { GS_MINOR, GS_PHRYG, GS_DORIAN, GS_PENTA, GS_BLUES, GS_HARM, GS_MAJOR, GS_MIXO, NGSCALES };   /* its scales */
typedef struct {                          /* a part's 303 generator (the ACID pages, 0.4) */
    uint16_t seed;                        /* the line, 1..65535 (0.4: 1..999); 0: never generated (it starts from the
                                           * track's last) */
    uint8_t mut;                          /* MUTATEs since the seed */
    uint8_t root;                         /* the root, MIDI 24..71 */
    uint8_t scale;                        /* GS_* */
    uint8_t notes;                        /* the scale's notes it uses, 1..7 (its favourites first) */
    uint8_t dens, slide, accent, oct;     /* chances, 0..100 %: a step plays; slides into the next; accented; an octave */
    uint8_t whole;                        /* the chance a note is WHOLE (held to the next note), else HALF, 0..100 % */
    uint8_t rsv;
} gen_t;

/* a drum part's generator (the BEAT pages, 0.5: SEQ again on the drums): a beat in a style, from a seed */
enum { DS_HOUSE, DS_TECHNO, DS_ELECTRO, DS_BOOMBAP, DS_TRAP, DS_BREAKS, NDSTYLES };
typedef struct {
    uint16_t seed;                        /* 1..65535; 0: never generated */
    uint8_t mut;                          /* MUTATEs since the seed */
    uint8_t style;                        /* DS_* */
    uint8_t kick, snare, hats, perc;      /* how busy each is, 0..100 (50: the style as it comes) */
    uint8_t fill, accent;                 /* chances, 0..100 %: the part's last steps fill; a hit accented */
    uint8_t rsv[2];
} dgen_t;

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
    uint8_t model[NPADS];                /* 0 inherits, otherwise a pad model ID plus one */
    uint8_t pattern_model[NPAT][NPADS];  /* saved with the pattern when Sounds per Pattern is active */
    color_params_t color[NTRK];          /* format 7: independent Color per track */
    /* format 8: the LFO is a clip's, not a track's: each pattern's part keeps its own (lfo above is the LFO of the
     * clip each track plays now: set_clip swaps them as SOUNDS PER PATTERN swaps sounds; a format-7 file loads with
     * every clip given its track's) */
    lfo_t plfo[NPAT][NTRK];
    /* format 10: the DrumEngine's patch as SAVE found it: the drums' edits on top of their kit (dpatch_kit: the kit
     * they belong to; dpatch_size: sizeof(KitPatch) then, 0 none: another size or kit and the kit plays as it comes) */
    KitPatch dpatch;
    uint16_t dpatch_size;
    uint8_t dpatch_kit;
    uint8_t mflags;                       /* SETUP > MIDI: bit 0 SEQ OUT (the sequencer's notes sent), 1 BLUETOOTH on
                                           * (a spare byte until 0.5's MIDI page: 0 in every older save) */
    uint8_t fx2[8];                       /* format 10: the FX's second pages (fx.h FXP_DL_WIDTH ..: FXP_NUM on) */
    /* format 12: MIX PER PATTERN (SETUP K4; recq bit 7): a track's mixer strip is its clip's, as its sound is (pmix:
     * set_clip), and the FX's and the master bus's knobs are the focused pattern's (pfx: pfx_follow). used 0: none
     * yet, it takes what is playing when it starts */
    struct { uint8_t used, level, pan, rev, dly; } pmix[NPAT][NTRK];
    struct { uint8_t used, v[FXP_ALL]; } pfx[NPAT];
    uint8_t gkeep[NPAT][NINST][NSTEPS / 8];   /* format 12: the 303 generator's KEEP: a bit a step it leaves alone */
    dgen_t dgen[NPAT];                    /* format 12: each drum part's generator */
} project_t;
static uint8_t *fxv(int i);               /* an FX knob's place in the project: fx, or fx2 past FXP_NUM */
/* format 4..10's 128-step pattern, 16 locks a part: a record of such a file is decoded as this, then spread */
typedef struct {
    uint32_t hit[NPADS][STEP_WORDS], acc[STEP_WORDS];
    uint16_t mute;
    uint8_t len, rate, nlk, rsv[3];
    lock_t lk[OLD_LOCKS];
} dpart_v10_t;
typedef struct {
    nstep_t st[NSTEPS];
    uint8_t len, rate, nlk, rsv;
    ilock_t lk[OLD_LOCKS];
} ipart_v10_t;
typedef struct { dpart_v10_t d; ipart_v10_t t[NINST]; } pattern_v10_t;
_Static_assert(offsetof(dpart_v10_t, lk) == offsetof(dpart_t, lk) && offsetof(ipart_v10_t, lk) == offsetof(ipart_t, lk),
               "format 11 only lengthens the parts' lock tables");
#define PROJ_PREFIX ((uint32_t)offsetof(project_t, p))
#define PROJ_TAIL ((uint32_t)offsetof(project_t, pots))
#define PROJ_TAIL_BYTES (sizeof(project_t) - PROJ_TAIL)
#define PROJ_LEGACY_TAIL_BYTES ((uint32_t)offsetof(project_t, pots606) - PROJ_TAIL)
#define PROJ_CAPACITY (8u * 4096u - PLAT_FILE_HEAD)
#define PROJ_META (PROJ_PREFIX + PROJ_TAIL_BYTES)
#define PROJ_V5_META_BYTES (offsetof(project_t, insert) + sizeof(((project_t *)0)->insert) - PROJ_TAIL)
#define PROJ_V5_TAIL_BYTES ((PROJ_V5_META_BYTES + 3u) & ~3u)
#define PROJ_V4_TAIL_BYTES (((offsetof(project_t, insert) + 3u) & ~3u) - PROJ_TAIL)
#define PROJ_V7_META_BYTES (offsetof(project_t, plfo) - PROJ_TAIL)   /* format 7 ended before the clips' LFOs */
#define PROJ_V7_TAIL_BYTES ((PROJ_V7_META_BYTES + 3u) & ~3u)
#define PROJ_V9_TAIL_BYTES (offsetof(project_t, dpatch) - PROJ_TAIL)   /* formats 8 and 9 ended before the drums' patch */
#define PROJ_V11_TAIL_BYTES (offsetof(project_t, pmix) - PROJ_TAIL)   /* formats 10 and 11 ended before MIX PER PATTERN */
_Static_assert((offsetof(project_t, pmix) & 3u) == 0, "formats 10/11 ended on a word");
_Static_assert((offsetof(project_t, dpatch) & 3u) == 0, "formats 8/9 ended on a word");


/* the project's file: two copies of 32 KiB at the start of the data area (plat_file_*) */
static const plat_file_t PROJ_FILE = {0x97000u, 0x9F000u, 8u * 4096u};

static project_t proj PLAT_POOL;
static uint8_t *fxv(int i) { return i < FXP_NUM ? &proj.fx[i] : &proj.fx2[(i - FXP_NUM) & 7]; }
static color_state_t drum_color_rt;
#ifdef PLAT_HOST
#define DRUM_COLOR_RESET() color_reset(&drum_color_rt)
#else
#define DRUM_COLOR_RESET() ((void)0)      /* (the drums' Color is the stock kits': not on the FM-1) */
#endif
static color_sends_t color_drums_send;
static uint8_t (*kit_pots(int k))[NPOTS] { return k == KIT_707 ? proj.pots707 : k == KIT_606 ? proj.pots606 : proj.pots[k]; }
static int drum_model(int pad) { return drums_model(proj.kit, proj.model, pad); }

static void drum_map_clean(int base, uint8_t *map)
{
    for (int pad = 0; pad < NPADS; ++pad) {
        const int model = drums_model(base, map, pad);
        map[pad] = (uint8_t)(model == base ? 0 : model + 1);
    }
}
static int kit_is_custom(const uint8_t *map)
{
    if (map)
        for (int pad = 0; pad < NPADS; pad++)
            if (map[pad])
                return 1;
    return 0;
}
static const char *drum_kit_title(void)   /* the drums' kit: the DrumEngine's genre kit, or the stock engine's */
{
    static char t[16];
    if (V3ON) {
        const char *n = kit_genre_name(dpreset_genre_of(proj.dpreset));
        int i;
        for (i = 0; n && n[i] && i < 15; i++)
            t[i] = (char)(n[i] >= 'a' && n[i] <= 'z' ? n[i] - 32 : n[i]);
        t[i] = 0;
        return t;
    }
    return kit_is_custom(proj.model) ? "CUSTOM" : drums_kit_name(proj.kit);
}
static const char *drum_kit_short(void)   /* the same in three letters (the small track labels) */
{
    return V3ON ? GENRE_SHORT[dpreset_genre_of(proj.dpreset)] : drum_kit_title();
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
/* a pad's hits held back: the drums' track muted; a lane muted, with the stock engine only (with the DrumEngine a lane's
 * mute is its audio: its hits go on underneath, so letting the mute go brings back a tail still ringing: trial_mute) */
static int drum_hits_off(int pad)
{
    return track_is_muted(TR_DRUM) || (!V3ON && (dcur()->mute >> pad & 1u));
}

/* Same lock as the DSP: refresh after a pattern/FILL change as well as the UI.
 * Only rising edges cut a voice. Turning a mute off cannot resume a tail. */
static void sync_drum_mutes(void)
{
    uint16_t mask = track_is_muted(TR_DRUM) ? (uint16_t)((1u << NPADS) - 1u) : dcur()->mute;
    uint16_t cut;
    int pad;
    if (V3ON) {                        /* the DrumEngine: the lanes' mutes are their audio; only the track's cuts */
        for (pad = 0; pad < NPADS; pad++)
            trial_mute((unsigned)pad + 1u, (int)(dcur()->mute >> pad & 1u));
        mask = track_is_muted(TR_DRUM) ? (uint16_t)((1u << NPADS) - 1u) : 0u;
    }
    cut = mask & (uint16_t)~drum_mutes_applied;
    if (mask == drum_mutes_applied)
        return;
    drum_mutes_applied = mask;
    roll_inhibit_pads |= cut;
    if (cut) DRUM_COLOR_RESET(); /* no history from a newly cut lane */
    for (pad = 0; cut && pad < NPADS; pad++)
        if (cut >> pad & 1u) {
            drums_cut_mapped(proj.kit, proj.model, pad);
            trial_cut((unsigned)pad + 1u);   /* (the DrumEngine's pad too) */
            pad_hit[pad] = 0.0f;
        }
}

/* The sections' budgets (0.5; govern(), before app_render): each section a share of a block's time, kept by giving
 * up its least important work: the drums BUD_DRUMS % (release an aged hat tail first, then other percussion;
 * the kick and snare keep their full decay even if these two alone exceed the target), the synth BUD_SYNTH %
 * (its voices capped at what that buys, at the cost a voice is measured to have; a voice over the cap fades out in
 * about 10 ms), and the effects BUD_FX % (drum Color and the shared returns together). Drum Color can be bypassed
 * until there is room again. The reverb, delay and master bus continue to run. */
#define BUD_DRUMS 35                      /* (25 while the drums always took the first core's time; 35 since they
                                           * go to the second core when both they and the synth are heavy: drum_job) */
#define BUD_SYNTH 40                      /* (30 until 0.5's CPU pass: a voice about 250 us of work a block, half of it
                                           * on each core, so 30 bought 6; 40 buys 8 at that cost, more when cheaper,
                                           * fewer when dearer. BUD_GUARD still sheds when core 0 runs hot) */
#define BUD_FX 25
#define BUD_GUARD 90                      /* one eligible drum tail and up to two synth voices when core 0 runs hot */
static struct {
    uint32_t clock;                       /* blocks rendered */
    uint32_t pad_at[NPADS];               /* each pad's last hit (clock) */
    uint8_t cap;                          /* the synth's voices its budget allows */
    uint8_t fx_shed;                      /* drum Color bypassed by the budget */
    uint16_t fx_wait;                     /* blocks before the effects' next move */
    float per_voice, syn_base;            /* the synth: a voice's time a block and its time with none, us (measured) */
    float d_prev, fx_mean;                /* the drums' last block; the effects' running mean, us */
    uint32_t act[3];                      /* when each last gave something up (clock: the CPU view's red) */
    uint32_t cuts, caps, sheds, guards;   /* drum sounds cut, voices shed, effects taken out, the guard's blocks, so far */
} gov;

static float lfo_out_drums(void);
static int drum_lfo_moved;                /* the last hit's move by the drums' LFO, thousandths of the range, its size
                                           * (0: not moved; tests) */
/* MIDI out (0.5): the keys' notes always; with SEQ OUT (SETUP > MIDI) the sequencer's too, the drums on channel 10
 * (General MIDI notes) and T2..T4 on 2..4, each with its note-off when its gate is up (mo_tick, a block at a time).
 * To USB and, where the build has it, Bluetooth */
#define MIDI_TO (TO_USB | TO_BLE)
#define MO_N 24
static struct { uint8_t on, ch, note; int32_t left; } mo[MO_N];
static const uint8_t PAD_GM[NPADS];
static int seq_out(void) { return (int)(proj.mflags & 1u); }

static void mo_off(int i)
{
    plat_midi_send(0x08u | (0x80u | mo[i].ch) << 8 | (uint32_t)mo[i].note << 16, MIDI_TO);
    mo[i].on = 0;
}

static void mo_note(int ch, int note, int vel, int32_t gate)   /* audio interrupt, or the lock held */
{
    int i, at = -1, old = 0;
    if (!seq_out() || note < 1 || note > 127)
        return;
    for (i = 0; i < MO_N; i++) {
        if (mo[i].on && mo[i].ch == ch && mo[i].note == note) {   /* the same note again: its off first */
            mo_off(i);
            at = i;
        } else if (!mo[i].on) {
            if (at < 0)
                at = i;
        } else if (mo[i].left < mo[old].left || !mo[old].on) {
            old = i;
        }
    }
    if (at < 0) {                         /* full: the one nearest its end goes early */
        mo_off(old);
        at = old;
    }
    plat_midi_send(0x09u | (0x90u | (uint32_t)ch) << 8 | (uint32_t)note << 16 | (uint32_t)clamp(vel, 1, 127) << 24, MIDI_TO);
    mo[at].on = 1;
    mo[at].ch = (uint8_t)ch;
    mo[at].note = (uint8_t)note;
    mo[at].left = gate > 0 ? gate : 2205;
}

static void mo_tick(int32_t n)
{
    int i;
    for (i = 0; i < MO_N; i++)
        if (mo[i].on && (mo[i].left -= n) <= 0)
            mo_off(i);
}

static void mo_flush(void)                /* the sequencer stopped (or SEQ OUT turned off): every note let go */
{
    int i;
    for (i = 0; i < MO_N; i++)
        if (mo[i].on)
            mo_off(i);
}
static void mo_pad(int pad, float v) { mo_note(9, PAD_GM[pad], (int)(v * 100.0f), 2205); }

static void pad_fire(int pad, float vel)
{
    if (pad < 0 || pad >= NPADS || drum_hits_off(pad))
        return;
    drum_lfo_moved = 0;
    if (V3ON) {                        /* the drums' LFO on one of this pad's controls: moved for this hit */
        const lfo_t *lf = &proj.lfo[TR_DRUM];
        if (lf->rsv[0] == LFO_V3 && lf->dest != LFO_OFF && lf->depth && lf->pad == pad) {
            const float move = lfo_out_drums() * (float)lf->depth * (1.0f / 127.0f);
            drum_lfo_moved = (int)((move < 0.0f ? -move : move) * 1000.0f);
            trial_hit_moved((unsigned)pad + 1u, vel, lf->dest, move);
        } else {
            trial_hit((unsigned)pad + 1u, vel);
        }
    } else {
        if (!drums_has_pad(drum_model(pad), pad)) return;
        drums_trigger_mapped(proj.kit, proj.model, pad, vel);
    }
    gov.pad_at[pad] = gov.clock;
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

static int lk_param(int kit, int i) { return (kit == DRUM_MODEL_707_CB ? (int)LK_707_CB : kit == DRUM_MODEL_707_TB ? (int)LK_707_TB : kit == KIT_707 ? (int)LK_KIT707 : kit == KIT_606 ? (int)LK_KIT606 : kit == KIT_808 ? (int)LK_KIT808 : 0) | i; }
static int lk_model(uint8_t code)
{
    if ((code & 0xF0u) == LK_707_CB) return DRUM_MODEL_707_CB;
    if ((code & 0xF0u) == LK_707_TB) return DRUM_MODEL_707_TB;
    const int id = code & LK_KIT707;
    return id == LK_KIT707 ? KIT_707 : id == LK_KIT606 ? KIT_606 : id == LK_KIT808 ? KIT_808 : KIT_909;
}

/* the default project: a beat in pattern 1, the rest empty; T2 a mono bass an octave down, T3 a pluck, T4 a pad */
/* The LFO is a clip's (0.5, format 8): proj.lfo[t] is the one track t plays now; each pattern's part keeps its own in
 * proj.plfo. A track onto another clip leaves its LFO with the one it was on and takes the new one's (set_clip);
 * saving puts the ones playing back with their clips; a pattern copied takes its LFOs along. */
static void plfo_fill(void)               /* every clip its track's LFO (a new project; a file from before format 8) */
{
    int p, t;
    for (p = 0; p < NPAT; p++)
        for (t = 0; t < NTRK; t++)
            proj.plfo[p][t] = proj.lfo[t];
}

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
        for (i = 0; i < FXP_ALL; i++)
            *fxv((int)i) = (uint8_t)fx_get((int)i);   /* (fx_init's: X0X's defaults, the delay centred) */
    }
    for (i = 0; i < NTRK; i++) {
        lfo_t *l = &proj.lfo[i];
        l->rate = 64;                     /* about 1.2 Hz */
        l->div = 5;                       /* 1/4 */
        l->dest = i == TR_DRUM ? LFO_OFF : 0;   /* the drums: no knob yet; the others: CUTOFF (at depth 0) */
    }
    plfo_fill();                          /* every clip: its track's */
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
        for (pad = 0; drums_kit_present(k) && pad <= NPADS; pad++)   /* (a cut kit's saved knobs: left as they are) */
            for (i = 0; i < NPOTS; i++)
                kit_pots(k)[pad][i] = (uint8_t)(i < drums_nparams(k, pad) ? drums_get(k, pad, i) : 0);
}

/* one pad's knobs (model's pad) into its engine: a switch (808 tom / conga...) first, the knobs after it edit its
 * sound. (The lock held, or the interrupt) */
static void pots_apply_pad(int model, int pad)
{
    int i, pass;
    if (!drums_kit_present(model))
        return;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < drums_nparams(model, pad) && i < NPOTS; i++)
            if (param_is_switch_sound(drums_param(model, pad, i)) == (pass == 0) &&
                drums_get(model, pad, i) != kit_pots(model)[pad][i])   /* (0.5: only a knob that differs: a scene's
                                                                        * kit loaded in the interrupt sets a few knobs,
                                                                        * not every one, each a filter worked out) */
                drums_set(model, pad, i, kit_pots(model)[pad][i]);
}

/* every kit's knobs into the engines, from the main loop, a pad at a time under the lock (0.5, the second CPU pass):
 * all four kits' in one hold took about a millisecond on the FM-1 with the audio held off, a sixth of a block; a pad's
 * are a few dozen microseconds */
static void pots_to_engines(void)
{
    int k, pad;
    for (k = 0; k < KIT_NUM; k++)
        for (pad = 0; drums_kit_present(k) && pad <= NPADS; pad++) {
            uint32_t f = plat_lock();
            pots_apply_pad(k, pad);
            plat_unlock(f);
        }
}

static void tracks_to_voices(void)        /* (a track, then an FX knob, at a time under the lock: short holds) */
{
    int t;
    for (t = 1; t < NTRK; t++) {
        const track_t *ts = tset(t);
        uint32_t f = plat_lock();
        syn_sound(t, ts->snd);
        syn_level(t, (float)ts->level / 127.0f);
        syn_mixer(t, proj.mix[t].pan, (float)proj.mix[t].rev / 127.0f, (float)proj.mix[t].dly / 127.0f);
        plat_unlock(f);
    }
    for (t = 0; t < FXP_ALL; t++) {
        uint32_t f = plat_lock();
        fx_set(t, *fxv(t));
        plat_unlock(f);
    }
}

static uint32_t crc32_more(uint32_t c, const uint8_t *d, uint32_t n)   /* (project_codec.h's check) */
{
    uint32_t k;
    while (n--) {
        c ^= *d++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

/* the render's time by stage (0.5, CPU): plat_ticks24 (24 a microsecond: one register read on the FM-1) around each,
 * kept as a running mean in microseconds per block: SETUP shows it, the tests read it (cpuN). DRUMS: the kits and their
 * mix; SYNTH: the voices on both cores (waiting for the second included) and their mix; FX IN: drum Color;
 * FX: the reverb and the delay; MASTER: the master bus; REST: the sequencer, the perform effects, the
 * metronome, the stems and the output */
enum { CS_DRUMS, CS_SYNTH, CS_FXIN, CS_FX, CS_MASTER, CS_REST, CS_TOTAL, CS_NUM };
static uint32_t cs_t[CS_NUM];             /* this block's ticks */
static uint32_t fxin_t[2];                /* the inserts' and Color's ticks this block on each core (the tracks' mix is
                                           * shared between them: mix_job): FX IN is both, the first core's alone is
                                           * part of its block */
static float cs_us[CS_NUM];               /* the running means, us per block */
static float cs_pk[CS_NUM];               /* the worst block lately, us: held, falling by half in about 1.3 s (the spikes
                                           * a running mean hides: a pattern starting, a switch) */
/* the worst block of the last 8 to 10 s, in buckets of 2 s (speak: read remotely; the reader's HELLO and its WHERE
 * both ask, 0.2 s apart, so a maximum since the last asking covered only that) */
#define CS_MXB 5
static float cs_mx[CS_MXB][CS_NUM];
static uint8_t cs_mxi;
static uint16_t cs_mxn;
/* OTHER (CS_REST) in parts, the first core's alone (speak: OTHER US, each part's mean and its worst block of the last
 * 10 s, us): IN, the block's start up to its chunks (the second core's tail handed over, the stems, MIDI in, the tempo,
 * the perform keys, the clock out, the count-in); SEQ, the sequencer's steps (each chunk's start and end); DRUM, a
 * chunk's drums outside their own time (the DrumEngine's glue, the track's mix, its stem and its level); JOIN, the
 * drums from the second core waited for and added (drums2_mode); WAIT, the block before's tail waited for; OUT, the
 * output; LEFT, what is left (THROW and WASH, the tail's note, the timers) */
enum { OT_IN, OT_SEQ, OT_DRUM, OT_JOIN, OT_WAIT, OT_OUT, OT_LEFT, OT_NUM };
static uint32_t ot_t[OT_NUM];
static float ot_us[OT_NUM], ot_mx[CS_MXB][OT_NUM];
/* the synth's stage in three parts (speak): the control tick (the LFOs, the arpeggiators, syn_prepare), the voices
 * (both halves, the second core's waited for) and the dry track mix */
enum { SY_PREP, SY_VOICES, SY_MIX, SY_NUM };
static uint32_t sy_t[SY_NUM];             /* this block's ticks */
static float sy_us[SY_NUM];               /* the running means, us per block */
#ifdef PLAT_HOST
static float gov_fake[CS_NUM];            /* the simulator: the stages' times the budgets see, us (a test sets them:
                                           * its own clock is the computer's, which says nothing of the FM-1) */
#endif
#define CS_MARK(v) uint32_t v = plat_ticks24()

/* Sleep the drum Color strip when both the dry signal and sends have gone quiet. */
#define FXS_LEVEL 1.0e-6f
#define FXS_AFTER 4096u
static uint32_t drum_color_quiet;          /* samples the chain has given silence for silence */
static float fxs_peak(const float *a, const float *b, unsigned n)
{
    float m = 0.0f;
    unsigned i;
    for (i = 0; i < n; i++) {
        float x = a[i] < 0.0f ? -a[i] : a[i], y = b[i] < 0.0f ? -b[i] : b[i];
        if (x > m) m = x;
        if (y > m) m = y;
    }
    return m;
}
static int fxs_asleep(int in_quiet)  /* before the chain: 1 to skip it */
{
    if (!in_quiet)
        drum_color_quiet = 0;
    return in_quiet && drum_color_quiet >= FXS_AFTER;
}
static void fxs_after(int in_quiet, int out_quiet, unsigned n)   /* after it: the quiet counted on */
{
    drum_color_quiet = in_quiet && out_quiet ? drum_color_quiet + n : 0u;
}

static void project_check(void)           /* a loaded project's values kept in range */
{
    int i, t, s, n;
    /* Synth insert and Color settings are opaque compatibility data for older builds. */
    for (i = 0; i < COL_NUM; i++)
        proj.color[TR_DRUM].v[i] = (uint8_t)clamp(proj.color[TR_DRUM].v[i], 0, i == COL_ON ? 1 : 127);

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
    for (n = 0; n <= NPAT; n++)           /* the LFOs playing, then each clip's */
    for (t = 0; t < NTRK; t++) {
        lfo_t *l = n ? &proj.plfo[n - 1][t] : &proj.lfo[t];
        l->shape = (uint8_t)(l->shape < 7u ? l->shape : 0u);
        l->rate &= 127u;
        l->div = (uint8_t)(l->div < 13u ? l->div : 5u);
        l->sync &= 1u;
        l->retrig = (uint8_t)(l->retrig < 3u ? l->retrig : 0u);
        l->fade &= 127u;
        l->phase &= 127u;
        l->depth = (int8_t)clamp(l->depth, -64, 63);
        if (t == TR_DRUM) {
            if (l->rsv[0] > LFO_V3)
                l->rsv[0] = 0;
            if (l->pad >= NPADS || (l->dest != LFO_OFF && l->dest >= NPOTS))
                l->dest = LFO_OFF, l->pad = 0;
        } else if (l->dest >= SM_NUM) {   /* (START, the sampler's until 0.5, or a bad one: CUTOFF, still) */
            l->dest = 0;
            l->depth = 0;
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
    for (t = FXP_NUM; t < FXP_ALL; t++)
        *fxv(t) = (uint8_t)clamp(*fxv(t), 0, fx_param(t)->max);
    for (i = 0; i < NPAT; i++)
        for (t = 0; t < NINST; t++) {
            ipart_t *p = &proj.p[i].t[t];
            if (p->nlk > NILOCKS)
                p->nlk = 0;
            for (s = 0; s < p->nlk; s++)
                if ((p->lk[s].param >= SP_NUM && p->lk[s].param < IL_X0) || p->lk[s].step >= NSTEPS)
                    p->lk[s].param = 0, p->lk[s].step = 0, p->lk[s].val = (int8_t)syn_param(0)->def;
        }
    for (i = 0; i < NPAT; i++)            /* the generators: one never used (seed 0) all zero (every other seed, 1..65535,
                                           * is OCT+'s since 0.5; 0.4 drew 1..999) */
        for (t = 0; t < NINST; t++) {
            gen_t *g = &proj.gen[i][t];
            if (!g->seed) {
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
    for (i = 0; i < NPAT; i++) {          /* the patterns' own sounds, kits and follow actions */
        for (t = 0; t < NINST; t++) {
            psound_t *q = &proj.psnd[i][t];
            int k;
            q->used &= 1u;
            if (q->type >= NTYPES)        /* (a SAMPLER's, until 0.5: POLY, as the track's own) */
                q->type = TY_POLY;
            q->level &= 127u;
            q->edited &= 1u;
            if (q->preset >= syn_presets() + NUSER)
                q->preset = 0;
            for (k = 0; k < SP_NUM; k++)
                q->snd[k] = (int8_t)clamp(q->snd[k], syn_param(k)->min, syn_param(k)->max);
        }
        if (proj.pkit[i] == DPRESET_606 + 1)   /* (a pattern's kit: a cut kit's heir) */
            proj.pkit[i] = KIT_808 + 1;
        else if (proj.pkit[i] == DPRESET_707 + 1)
            proj.pkit[i] = KIT_909 + 1;
        if (proj.pkit[i] && !dpreset_valid(proj.pkit[i] - 1))
            proj.pkit[i] = 0;
        drum_map_clean(-1, proj.pattern_model[i]);
        if ((proj.follow[i] & 7u) > 4u)
            proj.follow[i] = 0;
        proj.follow[i] &= 0x7Fu;
    }
    proj.kit = (uint8_t)kit_now(proj.kit);   /* (saved on the 606 or the 707: the 808 or the 909 now) */
    drum_map_clean(proj.kit, proj.model);
    proj.version = PROJ_VERSION;
    if (!dpreset_valid(proj.dpreset))
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
static uint8_t fx_pat;
static void pmix_clear(void);
static void pmix_fetch_all(void);
static void pmix_store_all(void);

/* Identify the persisted layout without treating a legacy project as a current RAM struct. */
static int proj_file_format(void)
{
    int sz = plat_file_size(&PROJ_FILE);
    uint8_t ver = 0;
    if (sz < (int)offsetof(project_t, tr) || plat_file_read(&PROJ_FILE, offsetof(project_t, version), &ver, 1) != 0)
        return 0;
    if (sz >= (int)PROJ_META && sz <= (int)PROJ_CAPACITY && ver == PROJ_VERSION)
        return PROJ_VERSION;
    if (sz >= (int)(PROJ_PREFIX + PROJ_V11_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 11)
        return 11;                        /* (12's layout without MIX PER PATTERN) */
    if (sz >= (int)(PROJ_PREFIX + PROJ_V11_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 10)
        return 10;                        /* (11's layout, its patterns with 16 locks a part) */
    if (sz >= (int)(PROJ_PREFIX + PROJ_V9_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 9)
        return 9;                         /* (10's layout without the drums' patch) */
    if (sz >= (int)(PROJ_PREFIX + PROJ_V9_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 8)
        return 8;                         /* (9's layout; the sends' knobs meant ROOM's and DIGI's) */
    if (sz >= (int)(PROJ_PREFIX + PROJ_V7_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 7)
        return 7;
    if (sz >= (int)(PROJ_PREFIX + PROJ_V5_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 5)
        return 5;
    if (sz >= (int)(PROJ_PREFIX + PROJ_V4_TAIL_BYTES) && sz <= (int)PROJ_CAPACITY && ver == 4)
        return 4;
    return 0;                             /* (0.2, 0.3 and 0.4's files, formats 1 to 3: their converters went on 24 Nov
                                           * 2026; such a file starts a fresh project and stays in the flash until SAVE) */
}

#include "project_codec.h"

/* a project saved before BongFX (format 8 or older): its sends' knobs, BongVerb's and BongDelay's. DECAY, the delay's
 * TIME (a division as before), FEEDBACK (REGEN), TONE and both LEVELs keep their places and meaning; ROOM's TONE turns
 * the other way as DAMP; its LOW CUT, the delay's TYPE, WEAR, WIDTH and LOW CUT have no counterpart: PRE-DELAY, the
 * reverb's MODE and DRIVE take their defaults */
static void fx_migrate(void)
{
    proj.fx[FXP_RV_DAMP] = (uint8_t)(127 - clamp(proj.fx[FXP_RV_DAMP], 0, 127));
    proj.fx[FXP_RV_PREDELAY] = (uint8_t)fx_param(FXP_RV_PREDELAY)->def;
    proj.fx[FXP_RV_MODE] = (uint8_t)fx_param(FXP_RV_MODE)->def;
    proj.fx[FXP_DL_DRIVE] = (uint8_t)fx_param(FXP_DL_DRIVE)->def;
    proj.fx[FXP_RSV10] = proj.fx[FXP_RSV11] = 0;
}

static int project_load(void)
{
    int fmt = proj_file_format(), r = -1;
    uint32_t i;
    dirty = 0;
    for (i = 0; i < sizeof proj; i++)
        ((uint8_t *)&proj)[i] = 0;
    if (fmt >= 4) {
        r = project_decode(plat_file_data(&PROJ_FILE), (uint32_t)plat_file_size(&PROJ_FILE), fmt);
    }
    if (r != 0 || proj.magic != PROJ_MAGIC || proj.kit >= KIT_NUM || proj.pat >= NPAT || proj.bpm10 < 600 ||
        proj.bpm10 > 2400) {
        project_default();
        fx_pat = proj.pat;
        pots_from_engines();
        tracks_to_voices();
        clips_raw(proj.pat);
        trial_kit(dpreset_genre_of(proj.dpreset));   /* the DrumEngine's kit: the project's */
        return 0;
    }
    if (fmt < 7) {
        proj.lfo[TR_DRUM].rsv[0] = 0;
        for (i = 0; i < NTRK; i++) color_defaults(&proj.color[i]);
    }
    if (fmt < 8)                          /* before each clip had its own LFO: every clip its track's */
        plfo_fill();
    if (fmt < 12)                         /* before MIX PER PATTERN: no pattern has a mix of its own, the switch off */
        pmix_clear();
    if (fmt < 10) {                       /* before the FX's second pages: theirs as the effects were (fx.c's defaults) */
        for (i = FXP_NUM; i < FXP_ALL; i++)
            *fxv((int)i) = (uint8_t)fx_param((int)i)->def;
    }
    if (fmt < 9)                          /* the ROOM reverb's and DIGI delay's knobs: BongVerb's and BongDelay's now */
        fx_migrate();
    project_check();
    clips_raw(proj.pat);
    for (i = 0; i < NTRK; i++)            /* every track on the focused pattern: its clip's LFO */
        proj.lfo[i] = proj.plfo[proj.pat][i];
    psnd_fetch_all();                     /* SOUNDS PER PATTERN: the focused pattern's, before they reach the engines */
    pmix_fetch_all();                     /* MIX PER PATTERN: the same */
    trial_kit(dpreset_genre_of(proj.dpreset));   /* the DrumEngine's kit: the project's (an older one's: ELECTRO) */
    if (fmt >= 10 && proj.dpatch_size == sizeof(KitPatch) && proj.dpatch_kit == proj.dpreset)
        trial_restore(&proj.dpatch);      /* (format 10) and the edits saved on top of it */
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
    pmix_store_all();                     /* MIX PER PATTERN: each track's strip with its clip, the FX with the pattern */
    {   /* each track's LFO with the clip it's on */
        int t;
        for (t = 0; t < NTRK; t++)
            proj.plfo[clip_of(t)][t] = proj.lfo[t];
    }
    trial_snapshot(1, &proj.dpatch);      /* the drums' edits, with the kit they're on */
    proj.dpatch_size = (uint16_t)sizeof(KitPatch);
    proj.dpatch_kit = proj.dpreset;
    result = project_encode();
    if (result != 0)
        return result;
    dirty = 0;
    return 0;
}

/* ------------------------------------------------------------- user presets --- */
/* sounds for T2..T4 and kits for the drums, saved by hand (SAVE held + PRESETS, let go), kept in a store object of their
 * own (OBJ_USER, A / B: a power cut keeps the old one) apart from any project */
#define USER_MAGIC 0x32525355u            /* "USR2": the synth update's (64 parameters a sound) */
typedef struct {
    uint8_t used, mono, rsv[2];
    int8_t v[SP_NUM];
    uint8_t rsv2[2];
} usound_t;
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
_Static_assert(sizeof(user_t) <= 3840u, "the user presets fit one store object");
static user_t user PLAT_POOL;

static int dpreset_genre_of(int i)        /* a genre kit itself; a stock or user kit: ELECTRO, the DrumEngine's factory
                                           * kit, which every project played until the genre kits (none sounds new) */
{
    return dpreset_genre(i) >= 0 ? dpreset_genre(i) : 0;
}

static void user_load(void)
{
    uint8_t *b = (uint8_t *)&user;
    uint32_t i;
    int n = plat_store_load(OBJ_USER, &user, sizeof user), k;
    if ((n == (int)sizeof user || n == (int)offsetof(user_t, model)) && user.magic == USER_MAGIC) {
        for (k = 0; k < NUKIT; k++) {
            if (n == (int)sizeof user) drum_map_clean(user.kit[k].kit, user.model[k]);
            else for (i = 0; i < NPADS; i++) user.model[k][i] = 0;
        }
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
    if (base != proj.kit) {               /* another kit: the one leaving stops sounding (0.5). Its tails used to ring on
                                           * under the new kit's hits: two kits to render at a scene's change, the drums
                                           * at 44-48 % of a block on the FM-1, and the late blocks there. Its kick and
                                           * its snare ring out (they are never cut short: two voices at most) */
        for (int r = 2; r < NPADS; r++) {
            drums_cut_pad(r);
            pad_hit[r] = 0.0f;
        }
    }
    for (int r = 0; r < NPADS; r++) {
        const int model = drums_model(base, map, r);
        if (mixed && drum_model(r) != model) {
            /* A change of model routes the next kick or snare to its new owner.
             * The old physical voice keeps rendering its existing tail. */
            if (r >= 2)
                drums_cut_mapped(proj.kit, proj.model, r);
            roll_inhibit_pads |= (uint16_t)(1u << r);
            pad_hit[r] = 0.0f;
        }
    }
    /* Release eligible old owners before publishing new aliases. Earlier lanes can
     * otherwise mask a later lane's old owner during a live scene change. */
    for (int r = 0; r < NPADS; r++) {
        const int model = drums_model(base, map, r);
        proj.model[r] = model == base ? 0 : (uint8_t)(model + 1);
    }
    proj.kit = (uint8_t)base;
}

static void kit_raw(int i, const uint8_t *scene, int use_slot_pots)
{
    int base = dpreset_factory(i), r, c;
    const uint8_t *map = 0;
    if (dpreset_genre(i) >= 0)            /* a genre kit: its stock kit for the stock engine */
        base = GENRE_STOCK[dpreset_genre(i)];
    if (dpreset_user(i)) {
        const ukit_t *u = &user.kit[i - 2];
        if (!u->used || !drums_kit_present(u->kit))   /* (a user kit made on a cut kit: not there any more) */
            return;
        base = u->kit;
        map = user.model[i - 2];
        if (use_slot_pots)
            for (r = 0; r <= NPADS; r++) {
                uint8_t *pot = kit_pots(drums_model(base, map, r))[r];
                for (c = 0; c < NPOTS; c++)
                    pot[c] = u->pots[r][c];
            }
    }
    if (base < 0)
        return;
    drum_map_apply(base, scene ? scene : map);
    if (dpreset_genre_of(i) != dpreset_genre_of(proj.dpreset) || dpreset_genre(i) >= 0)
        trial_kit(dpreset_genre_of(i));   /* the DrumEngine's kit with it (its edits since gone, as a kit's knobs) */
    proj.dpreset = (uint8_t)i;
    if (use_slot_pots && dpreset_user(i))  /* the pads the kit set (above), not every kit's: a pattern switch in the
                                            * interrupt (SOUNDS PER PATTERN) takes a quarter of the time */
        for (r = 0; r <= NPADS; r++)
            pots_apply_pad(drums_model(base, map, r), r);
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
    }
}

/* MIX PER PATTERN (SETUP K4, format 12): a track's mixer strip (level, pan, sends) goes with its clip, as its sound
 * does; the FX's and the master bus's knobs go with the focused pattern (proj.pat: pfx_follow, a look a block) */
static int mix_pp(void) { return (int)(proj.recq >> 7 & 1u); }
static uint8_t fx_pat;                    /* the pattern whose FX are in proj.fx now */

static uint8_t *mix_level(int t) { return t == TR_DRUM ? &proj.mix[0].level : &tset(t)->level; }

static void pmix_store(int t, int p)      /* track t's strip now, kept with pattern p */
{
    if (p < 0 || p >= NPAT)
        return;
    proj.pmix[p][t].used = 1;
    proj.pmix[p][t].level = *mix_level(t);
    proj.pmix[p][t].pan = proj.mix[t].pan;
    proj.pmix[p][t].rev = proj.mix[t].rev;
    proj.pmix[p][t].dly = proj.mix[t].dly;
}

static void pmix_load(int t, int p, int engine)   /* pattern p's strip into track t (engine: and its voices; the lock
                                                   * held / interrupt); none yet: it takes the one playing */
{
    if (!proj.pmix[p][t].used) {
        pmix_store(t, p);
        return;
    }
    *mix_level(t) = proj.pmix[p][t].level;
    proj.mix[t].pan = proj.pmix[p][t].pan;
    proj.mix[t].rev = proj.pmix[p][t].rev;
    proj.mix[t].dly = proj.pmix[p][t].dly;
    if (engine && t != TR_DRUM) {
        syn_level(t, (float)tset(t)->level / 127.0f);
        syn_mixer(t, proj.mix[t].pan, (float)proj.mix[t].rev / 127.0f, (float)proj.mix[t].dly / 127.0f);
    }
}

static void pfx_store(int p)              /* the FX and the master bus now, kept with pattern p */
{
    int i;
    proj.pfx[p].used = 1;
    for (i = 0; i < FXP_ALL; i++)
        proj.pfx[p].v[i] = *fxv(i);
}

static void pfx_follow(void)              /* the focused pattern changed: its FX (the lock held / interrupt). Only the
                                           * knobs that differ are told to the effects */
{
    int i, p = proj.pat;
    if (!mix_pp() || p == fx_pat)
        return;
    pfx_store(fx_pat);
    fx_pat = (uint8_t)p;
    if (!proj.pfx[p].used) {
        pfx_store(p);
        return;
    }
    for (i = 0; i < FXP_ALL; i++)
        if (*fxv(i) != proj.pfx[p].v[i] && fx_param(i) && proj.pfx[p].v[i] <= fx_param(i)->max) {
            *fxv(i) = proj.pfx[p].v[i];
            fx_set(i, *fxv(i));
        }
}

static void pmix_clear(void)
{
    memset(proj.pmix, 0, sizeof proj.pmix);
    memset(proj.pfx, 0, sizeof proj.pfx);
    memset(proj.gkeep, 0, sizeof proj.gkeep);   /* (format 12's other news: no step kept, no beat generated) */
    memset(proj.dgen, 0, sizeof proj.dgen);
    proj.recq &= 0x7Fu;
}

static void pmix_store_all(void)          /* saving, or the switch turned on: every strip with its clip */
{
    int t;
    if (!mix_pp())
        return;
    for (t = 0; t < NTRK; t++)
        pmix_store(t, clip_of(t));
    pfx_store(fx_pat);
}

static void pmix_fetch_all(void)          /* loading: the focused pattern's (before they reach the engines) */
{
    int t, i;
    fx_pat = proj.pat;
    if (!mix_pp())
        return;
    for (t = 0; t < NTRK; t++)
        pmix_load(t, proj.pat, 0);
    if (proj.pfx[fx_pat].used)
        for (i = 0; i < FXP_ALL; i++)
            if (fx_param(i) && proj.pfx[fx_pat].v[i] <= fx_param(i)->max)
                *fxv(i) = proj.pfx[fx_pat].v[i];
}

static void set_clip(int t, int n)        /* track t onto pattern n's part (-1: stopped); its sound with it, and its
                                           * LFO (the clip's own: the one it leaves keeps the LFO it had) */
{
    if (n >= 0 && n != clip_of(t)) {
        proj.plfo[clip_of(t)][t] = proj.lfo[t];
        proj.lfo[t] = proj.plfo[n][t];
        if (sounds_pp()) {
            psnd_store(t, clip_of(t));
            psnd_load(t, n);
        }
        if (mix_pp()) {
            pmix_store(t, clip_of(t));
            pmix_load(t, n, 1);
        }
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
                                               "NOISE", "SUB"};
static float LFO_HZ[128];                 /* the free rates: 0.05 Hz .. 30 Hz */
static struct {
    float ph, out;                        /* where in its cycle; its value now, -1..1 */
    float a, b;                           /* S&H, SMOOTH: the value this cycle, and the next */
    float fade;                           /* 0..1 after a note (FADE) */
} lfo_rt[NTRK];
static float lfo_out_drums(void) { return lfo_rt[TR_DRUM].out; }
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

static int8_t xlk[NTRK][XL_NUM];         /* the step's LFO 1 / arpeggiator locks now, and which are on */
static uint8_t xlk_on[NTRK];
#define XL(tr, i, base) ((xlk_on[tr] >> (i) & 1u) ? xlk[tr][i] : (base))
static float lfo_hz(int t)
{
    const lfo_t *l = &proj.lfo[t];
    const int r = XL(t, XL_LRATE, l->sync ? l->div : l->rate);   /* (a step's lock: the division, or the rate) */
    return l->sync ? (float)proj.bpm10 / 600.0f / DIV_BEATS[r < NDIV ? r : NDIV - 1] : LFO_HZ[r & 127];
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
    float step = 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * ARP_BEATS[XL(t, XL_ARATE, ts->arp_rate) % NARPRATES];
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
    syn_note_on(t, note, (float)vel / 127.0f, (int32_t)(step * (float)XL(t, XL_AGATE, ts->arp_gate) * 0.01f), ts->type == TY_MONO);
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
    step = 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * ARP_BEATS[XL(t, XL_ARATE, tset(t)->arp_rate) % NARPRATES];
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
        int k = l->param & (lk_model(l->param) >= KIT_NUM ? 0x0F : 0x3F);
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
    uint8_t xon = 0;
    int i;
    for (i = 0; i < p->nlk; i++)
        if (p->lk[i].step == step && p->lk[i].param < SP_NUM) {
            syn_set(tr, p->lk[i].param, p->lk[i].val);
            want |= (uint64_t)1 << p->lk[i].param;
        } else if (p->lk[i].step == step && p->lk[i].param >= IL_X0 && p->lk[i].param < IL_X0 + XL_NUM) {
            xlk[tr][p->lk[i].param - IL_X0] = p->lk[i].val;
            xon |= (uint8_t)(1u << (p->lk[i].param - IL_X0));
        }
    xlk_on[tr] = xon;
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
    /* the QUICK MIXER's track moves (0.5): asked by its keys (qtrk the track, qroll ROLL's rate + 1, qstut STUTTER's
     * slice in steps), taken up by the interrupt (rmask the tracks rolling; st_* the track stuttering: its slice's
     * length, first step, place in it and the step it played last) */
    volatile int8_t qtrk;
    volatile uint8_t qroll, qstut;
    uint8_t rmask, qs_n, qs_trk;
    int16_t qs_start, qs_k, qs_ps;
} perf = {0, 0, 1, 1, 30, 90, 127, 70, 0.0f, 0.0f, {0}, 0, 0, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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

static void pump_key(int kit, float vel); /* (a kick for PUMP: at the block's tail, post_job) */

/* substeps (0.5): a step that plays 2, 3 or 4 times in its own length (a held step + ALGORITHM): a drum's hit again,
 * an instrument step's notes again (each half a substep long). One slot a pad and a track; the audio interrupt's */
static struct {
    float to, len;                        /* samples to the next, between them */
    uint8_t left, a;                      /* still to play; a pad's velocity (sub_vel), an instrument's step */
    int32_t gate;                         /* an instrument step played late, alone: its own gate (0: half a substep) */
} sub[NPADS + NINST];
static uint32_t sub_on;                   /* a bit per slot with some left */

static float sub_vel(int a)               /* a hit's velocity: 0 normal, 255 accented, else 1..127 of 127 */
{
    return a == 255 ? 1.0f : a ? (float)a * (1.0f / 127.0f) : DRUMS_VEL_NORMAL;
}

static void sub_arm(int i, int n, float dur, int a)
{
    if (n < 2 || n > 4)
        return;
    sub[i].len = dur / (float)n;
    sub[i].to = sub[i].len;
    sub[i].left = (uint8_t)(n - 1);
    sub[i].a = (uint8_t)a;
    sub[i].gate = 0;
    sub_on |= 1u << i;
}

/* microtiming (0.5: a held step + PRESETS): the step plays late16 sixteenths of its length late; with substeps, they
 * follow from there. Nothing sounds at the step's own time: the slot plays all of it */
static void sub_late(int i, int n, float dur, int a, int late16, int32_t gate)
{
    if (n < 1 || n > 4)
        n = 1;
    sub[i].len = dur / (float)n;
    sub[i].to = dur * (float)late16 * (1.0f / 16.0f);
    sub[i].left = (uint8_t)n;
    sub[i].a = (uint8_t)a;
    sub[i].gate = n == 1 ? gate : 0;
    sub_on |= 1u << i;
}

#ifdef PLAT_HOST
static int sub_fires;
#endif
static void sub_fire(int i)
{
#ifdef PLAT_HOST
    sub_fires++;
#endif
    if (i < NPADS) {
        float v = sub_vel(sub[i].a);
        if ((perf.rmask | proj.tmute) & 1u)
            return;
        pad_fire(i, v);
        mo_pad(i, v);
        if (i == 0)
            pump_key(drum_model(0), v);
    } else {
        int tr = i - NPADS + 1, k;
        const nstep_t *s = &ipart(tr)->st[sub[i].a & (NSTEPS - 1)];
        if (((perf.rmask | proj.tmute) >> tr & 1u) || clip[tr] < 0)
            return;
        for (k = 0; k < NCHORD && s->note[k]; k++) {
            const int32_t g = sub[i].gate ? sub[i].gate : (int32_t)(sub[i].len * 0.5f);
            syn_note_on_f(tr, s->note[k], (float)ns_vel(s) / 127.0f, g,
                          tset(tr)->type == TY_MONO, (s->vel & NS_ACC) ? SN_ACCENT : 0);
            mo_note(tr, s->note[k], ns_vel(s), g);
        }
    }
}

static void click_stretch(float r);       /* (the click's beat stretched by r: at the block's tail) */
static void seq_drum(void)                /* audio interrupt: the drum part's next step, and its hits */
{
    dpart_t *p = dcur();
    int next = sq.t[TR_DRUM].step + 1, pad, muted = (int)(proj.tmute & 1u);
    if (next >= p->len)
        next = 0;
    sq.t[TR_DRUM].step = next;
    sq.t[TR_DRUM].dur = step_samples(TR_DRUM, next);
    for (pad = 0; pad < NPADS && !(perf.rmask & 1u); pad++) {   /* (ROLL: silent underneath) */
        if (!bit_get(p->hit[pad], next) || (!V3ON && (p->mute >> pad & 1u)))   /* (DrumEngine: audio, drum_hits_off) */
            continue;
        if (sq.skip_once[pad] == next) {
            sq.skip_once[pad] = -1;
            continue;
        }
        if (muted || chance_skips(p, next, pad))
            continue;
        locks_apply(p, next, pad);
        {
            int j = lock_find(p, next, pad, LK_VEL), a = j >= 0 ? p->lk[j].val & 127 : bit_get(p->acc, next) ? 255 : 0;
            float v = sub_vel(a);         /* (its own velocity, else its accent's) */
            int late = lock_find(p, next, pad, LK_NUDGE);
            if (late >= 0) {              /* late: the pad's slot plays it, its substeps after */
                j = lock_find(p, next, pad, LK_SUB);
                sub_late(pad, j >= 0 ? p->lk[j].val : 1, sq.t[TR_DRUM].dur, a, p->lk[late].val, 0);
                continue;
            }
            pad_fire(pad, v);
            mo_pad(pad, v);
            if (pad == 0)                 /* the bass drum: the master's PUMP ducks to it */
                pump_key(drum_model(0), v);
            j = lock_find(p, next, pad, LK_SUB);
            if (j >= 0)
                sub_arm(pad, p->lk[j].val, sq.t[TR_DRUM].dur, a);
        }
    }
    sq.t[TR_DRUM].to_next += sq.t[TR_DRUM].dur;
}

static void seq_inst(int tr)              /* audio interrupt: an instrument part's next step, and its notes */
{
    ipart_t *p = ipart(tr);
    const track_t *ts = tset(tr);
    const nstep_t *s;
    int next = sq.t[tr].step + 1, i, slide_in = sq.slid[tr], marks, ps;
    int32_t gate;
    if (next >= p->len)
        next = 0;
    sq.t[tr].step = next;
    sq.t[tr].dur = step_samples(tr, next);
    sq.t[tr].to_next += sq.t[tr].dur;
    sq.slid[tr] = 0;
    ps = next;
    if (perf.qs_n && perf.qs_trk == tr) { /* the QUICK MIXER's STUTTER: its slice's next step plays, the part goes on
                                           * underneath (let go: it's where it would have been) */
        int n = perf.qs_n < p->len ? perf.qs_n : p->len;
        perf.qs_k = (int16_t)((perf.qs_k + 1) % (n > 0 ? n : 1));
        ps = (perf.qs_start + perf.qs_k) % (p->len ? p->len : 1);
        perf.qs_ps = (int16_t)ps;
    }
    s = &p->st[ps];
    if (!s->note[0] || (perf.rmask >> tr & 1u))   /* (ROLL: silent underneath) */
        return;
    if (sq.iskip[tr] == next) {
        sq.iskip[tr] = -1;
        return;
    }
    if ((proj.tmute >> tr & 1u) || (ns_prob(s) < 100 && (int)rnd100() >= ns_prob(s)))
        return;
    ilocks_apply(tr, p, ps);
    gate = note_gate_samples(tr, ps, p->len, s->gate);
    if (!ts->arp_mode && (i = ilock_find(p, ps, IL_NUDGE)) >= 0) {   /* late: the track's slot plays it */
        const int j = ilock_find(p, ps, IL_SUB);
        sub_late(NPADS + tr - 1, j >= 0 ? p->lk[j].val : 1, sq.t[tr].dur, ps, p->lk[i].val, gate);
        sq.slid[tr] = 0;
        return;
    }
    if (!ts->arp_mode && (i = ilock_find(p, ps, IL_SUB)) >= 0) {   /* substeps: this note as short as theirs */
        sub_arm(NPADS + tr - 1, p->lk[i].val, sq.t[tr].dur, ps);
        if (sub_on >> (NPADS + tr - 1) & 1u)
            gate = (int32_t)(sub[NPADS + tr - 1].len * 0.5f);
    }
    marks = (s->vel & NS_ACC) ? SN_ACCENT : 0;   /* a 303's marks: on any sound (a slide: the chord's first note) */
    for (i = 0; i < NCHORD && s->note[i]; i++) {
        if (ts->arp_mode)                 /* arpeggiated: the notes are the arpeggiator's, for the gate */
            arp_feed(tr, s->note[i], ns_vel(s), gate);
        else
            syn_note_on_f(tr, s->note[i], (float)ns_vel(s) / 127.0f, gate, ts->type == TY_MONO,
                          marks | (slide_in && i == 0 ? SN_SLIDE : 0));
        mo_note(tr, s->note[i], ns_vel(s), gate);   /* (arpeggiated: the step's own notes, not the arpeggio) */
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
    if ((perf.rmask & 1u) && clip[TR_DRUM] >= 0 && !((proj.tmute | roll_inhibit_tracks) & 1u))
        for (pad = 0; pad < NPADS; pad++)
            if (bit_get(d->hit[pad], st) && !((d->mute | roll_inhibit_pads) >> pad & 1u)) {
                float v = bit_get(d->acc, st) ? 1.0f : DRUMS_VEL_NORMAL;
                pad_fire(pad, v);
                if (pad == 0)
                    pump_key(drum_model(0), v);
            }
    for (t = 1; t < NTRK; t++) {
        const ipart_t *p = ipart(t);
        const nstep_t *s = &p->st[perf.roll_step[t] % (p->len ? p->len : 1)];
        int i;
        if (!(perf.rmask >> t & 1u) || clip[t] < 0 || ((proj.tmute | roll_inhibit_tracks) >> t & 1u) || !s->note[0])
            continue;
        for (i = 0; i < NCHORD && s->note[i]; i++) {
            syn_note_on(t, s->note[i], (float)ns_vel(s) / 127.0f, (int32_t)(perf.roll_len * 0.6f), tset(t)->type == TY_MONO);
            mo_note(t, s->note[i], ns_vel(s), (int32_t)(perf.roll_len * 0.6f));
        }
    }
}

/* FILL (0.5): the drums play the next pattern's part from where they are, by its own LENGTH and RATE. That used to
 * leave the drum clip wherever the fill's part had got to: a fill of another length or rate, and the drums came back
 * out of step with the bar and the other clips. Now the clip's own place goes on silently underneath while FILL is
 * held (fill_pos: its step and the time to its next, counted with the clip's own part), and the drums take it up
 * again when FILL is let go: the clip is where it would have been */
static struct {
    uint8_t on;                           /* FILL held (as the interrupt last saw it) */
    int16_t step;                         /* the clip's own step now */
    float to_next, dur;                   /* samples to its next, and that step's length */
} fill_pos;

static void fill_pos_restart(void)        /* the drum clip from its step 1 (a scene, a clip started): its place too */
{
    fill_pos.step = -1;
    fill_pos.to_next = 0.0f;
    fill_pos.dur = 1.0f;
}

static void fill_pos_run(uint32_t n)      /* audio interrupt, playing: n samples on, the clip's own part's steps */
{
    const dpart_t *m;
    if (!fill_pos.on || clip[TR_DRUM] < 0)
        return;
    m = &proj.p[clip_of(TR_DRUM)].d;
    fill_pos.to_next -= (float)n;
    while (fill_pos.to_next < 0.5f) {
        int next = fill_pos.step + 1;
        float base = 44100.0f * 60.0f / ((float)proj.bpm10 * 0.1f) * 0.25f * RATE_MUL[m->rate % NRATES];
        float sw = (float)proj.swing * 0.01f;
        if (next >= m->len)
            next = 0;
        fill_pos.step = (int16_t)next;
        fill_pos.dur = (next & 1) ? 2.0f * base * (1.0f - sw) : 2.0f * base * sw;   /* (step_samples, the clip's) */
        if (fill_pos.dur < 1.0f)
            fill_pos.dur = 1.0f;
        fill_pos.to_next += fill_pos.dur;
    }
}

static void fill_pos_follow(void)         /* audio interrupt, a block's start: FILL pressed or let go since the last */
{
    int f = fill_on != 0;
    if (f && !fill_pos.on) {              /* pressed: the clip's place from here, underneath */
        fill_pos.step = (int16_t)sq.t[TR_DRUM].step;
        fill_pos.to_next = sq.t[TR_DRUM].to_next;
        fill_pos.dur = sq.t[TR_DRUM].dur;
        fill_pos.on = 1;
    } else if (!f && fill_pos.on) {       /* let go: the drums back on the clip, where it has got to */
        fill_pos.on = 0;
        if (sq.playing && clip[TR_DRUM] >= 0 && fill_pos.to_next < 1e8f) {
            sq.t[TR_DRUM].step = fill_pos.step;
            sq.t[TR_DRUM].to_next = fill_pos.to_next;
            sq.t[TR_DRUM].dur = fill_pos.dur;
        }
    }
}

static void seq_reset_tracks(void)
{
    int t;
    sub_on = 0;
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
    fill_pos_restart();
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
        fill_pos_restart();
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
        plat_midi_send(0x0Fu | 0xFAu << 8, MIDI_TO);
    lfo_play();
    sq.playing = 1;
}

/* MIDI Continue (followed clock): from where the last Stop left every track, the bar and the song */
static struct {
    uint8_t ok, pat;
    __typeof__(sq.t[0]) t[NTRK];
    float bar_left;
    uint32_t clock;
    int8_t spos;
    uint8_t sleft, sstarted;
} cont;

static void seq_continue(void)            /* (as seq_begin) */
{
    int t, song = proj.song_on && proj.song_n;
    seq_begin();
    if (!cont.ok || (!song && cont.pat != proj.pat) || (song && (cont.spos < 0 || cont.spos >= proj.song_n)))
        return;                           /* nothing kept, or another pattern since: from the top */
    if (song) {
        srt.pos = cont.spos;
        srt.left = cont.sleft;
        srt.started = cont.sstarted;
        if (proj.pat != proj.song[srt.pos].pat) {
            proj.pat = proj.song[srt.pos].pat;
            clips_all(proj.pat);
        }
    }
    for (t = 0; t < NTRK; t++) {
        sq.t[t].step = cont.t[t].step;
        sq.t[t].frac = cont.t[t].frac;
        sq.t[t].to_next = cont.t[t].to_next;
        sq.t[t].dur = cont.t[t].dur;
    }
    sq.bar_left = cont.bar_left;
    sq.clock = cont.clock;
}

static void seq_start(void)
{
    uint32_t f = plat_lock();
    ck.count = 0;
    seq_begin();
    plat_unlock(f);
}

static void qm_clear(void);              /* (the QUICK MIXER's held moves: below) */
static void seq_stop(void)
{
    uint32_t f;
    int t;
    qm_clear();                           /* (a stop lets the QUICK MIXER's held moves go) */
    f = plat_lock();
    if (sq.playing && (proj.recq >> 2 & 3u) == 1u)   /* MIDI CLOCK sent: Stop */
        plat_midi_send(0x0Fu | 0xFCu << 8, MIDI_TO);
    if (sq.playing) {                     /* where it stopped: a MIDI Continue goes on from there */
        cont.ok = 1;
        cont.pat = proj.pat;
        for (t = 0; t < NTRK; t++) {
            cont.t[t].step = sq.t[t].step;
            cont.t[t].frac = sq.t[t].frac;
            cont.t[t].to_next = sq.t[t].to_next;
            cont.t[t].dur = sq.t[t].dur;
        }
        cont.bar_left = sq.bar_left;
        cont.clock = sq.clock;
        cont.spos = srt.pos;
        cont.sleft = srt.left;
        cont.sstarted = srt.started;
    }
    mo_flush();
    sq.playing = 0;
    sub_on = 0;
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
        click_stretch(r);                 /* (the click's beat: at this block's tail, post_job) */
        if (fill_pos.on && fill_pos.to_next < 1e8f)
            fill_pos.to_next *= r;        /* (the drum clip's own place under a FILL) */
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

static void drums2_force(unsigned v);     /* (below, by the drums' second-core state) */

#if FM1B_USB_HOST
#include "midi_host_owner.h"

static volatile uint8_t usb_host_selected;
static uint8_t mh_usb_start;

static void mh_note(int tr, unsigned note, unsigned vel, unsigned source, int on)
{
    int arp_on = tset(tr)->arp_mode;
    int repeat = on && source != MH_PANEL && (mh_notes[tr-1][note] & source);
    int event = mh_own(tr, note, source, on);
    if (on)
        mh_latch_set(tr, note, 0);
    if (event < 0 && arp_on && tset(tr)->arp_latch && source <= MH_USB_FIXED)
        mh_latch_set(tr, note, 1);
    if (!event && !repeat)
        return;
    if (arp_on) {
        if (repeat) {
            int i;
            for (i = 0; i < arp[tr].n; i++)
                if (arp[tr].note[i] == note && arp[tr].life[i] < 0) {
                    arp[tr].vel[i] = (uint8_t)vel;
                    return;
                }
        }
        arp_key(tr, (int)note, (int)vel, on);
    } else if (on) {
        syn_note_on(tr, (int)note, (float)vel / 127.0f, -1, tset(tr)->type == TY_MONO);
        lfo_note(tr);
    } else
        syn_note_off(tr, (int)note);
}

static void mh_usb_disconnected(void)
{
    int tr, note, i;
    if (mh_usb_start) {
        sq.mclk_go = 0;                   /* do not carry an unfulfilled Start across USB sessions */
        mh_usb_start = 0;
    }
    for (tr = 1; tr < NTRK; tr++) {
        for (note = 1; note < 128; note++) {
            uint8_t *p = &mh_notes[tr - 1][note];
            if (!(*p & 3u))
                continue;
            *p &= (uint8_t)~3u;
            if (*p)
                continue;
            syn_note_off(tr, note);
            if (tset(tr)->arp_mode) {
                arp_key(tr, note, 0, 0);   /* remove the held key even when latch was enabled */
                mh_latch_set(tr, (unsigned)note, 1);
            }
        }
        for (i = arp[tr].n - 1; i >= 0; i--) {
            note = arp[tr].note[i];
            if (arp[tr].life[i] < 0 && (mh_usb_arp[tr - 1][note >> 3] & (1u << (note & 7))))
                arp_drop(tr, i);
        }
        for (i = 0; i < 16; i++)
            mh_usb_arp[tr - 1][i] = 0;
    }
    for (i = 0; i < 32; i++)
        mh_route[0][i] = 0;
}
#endif

static void midi_in(void)                 /* audio interrupt */
{
    uint32_t pkt;
    int port;
#if FM1B_USB_HOST
    if (plat_usb_lost())
        mh_usb_disconnected();
#endif
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu, ch = st & 0x0Fu;
#if FM1B_USB_HOST
        if (usb_host_selected && port == PORT_USB &&
            ((st == 0xBFu && d1 >= 109u && d1 <= 119u) || (st == 0x9Fu && d1 >= 36u && d1 <= 46u)))
            continue;                     /* app/device diagnostic controls are not for external USB controllers */
#endif
        if (ch == 15u && st == 0xBFu && d1 >= 110u && d1 <= 119u) {
            if (d1 == 110u) v3_set_mode(d2);
            else if (d1 == 111u) { if (d2 == 1u) trial_metrics_reset(); }
            else if (d1 <= 115u) trial_patch((int)d1 - 111, (int)d2);
            else trial_macro(d1 - 116u, d2);
            continue;
        }
        if (ch == 15u && st == 0xBFu && d1 == 109u) {   /* measuring: the drums' core, 0 as built (the second whenever it
                                                         * runs), 1 the second, 2 the first, 3 drums2_pick's choice */
            drums2_force(d2);
            continue;
        }
        if (ch == 15u && st == 0x9Fu && d1 >= 36u && d1 <= 46u) {
            if (V3ON && d2) pad_fire((int)(d1 - 36u), (float)d2 / 127.0f);
            continue;
        }
        if (st >= 0xF8u) {                /* real time: MIDI CLOCK followed (SETUP) */
            if ((proj.recq >> 2 & 3u) != 2u)
                continue;
            if (st == 0xF8u) {            /* a clock: the first after a Start starts; every 6th times the tempo */
                if (sq.mclk_go) {
                    const int go = sq.mclk_go;
                    sq.mclk_go = 0;
#if FM1B_USB_HOST
                    mh_usb_start = 0;
#endif
                    if (!sq.playing) {
                        if (go == 2)
                            seq_continue();
                        else
                            seq_begin();
                    }
                }
                if (++sq.mclk_n >= 24u)
                    sq.mclk_n = 0;
                if (sq.mclk_n % 6u == 0u)
                    mclk_tick();
            } else if ((st == 0xFAu || st == 0xFBu) && !sq.playing) {   /* Start; Continue: from where it stopped */
                sq.mclk_go = st == 0xFBu ? 2 : 1;
#if FM1B_USB_HOST
                mh_usb_start = (uint8_t)(port == PORT_USB);
#endif
                sq.mclk_n = 23;           /* (the next clock is the first beat's) */
                sq.mclk_nt = 0;           /* (the 6th clocks counted from it) */
            } else if (st == 0xFCu) {
                sq.mclk_go = 0;
#if FM1B_USB_HOST
                mh_usb_start = 0;
#endif
                if (sq.playing)
                    sq.ext_stop = 1;
            }
            continue;
        }
        int on = (st & 0xF0u) == 0x90u && d2, off = (st & 0xF0u) == 0x80u || ((st & 0xF0u) == 0x90u && !d2);
        int tr = ch == 9u ? TR_DRUM : ch == 0u ? (int)sel_trk : ch <= 3u ? (int)ch : -1;
        if ((st & 0xF0u) == 0xD0u || (st & 0xF0u) == 0xA0u) {   /* pressure (channel, or a key's): the matrix's PRESSURE */
            if (tr > 0)
                syn_pressure(tr, (float)((st & 0xF0u) == 0xD0u ? d1 : d2) / 127.0f);
            continue;
        }
#if FM1B_USB_HOST
        int trs = port != PORT_USB;
        if (ch == 0u && d1 && (on || off)) {
            int was = mh_route_get(trs, d1);
            if (was && (off || was != tr))
                mh_note(was, d1, 0, (unsigned)(trs ? MH_TRS_SEL : MH_USB_SEL), 0);
            if (off) {
                mh_route_set(trs, d1, 0);
                continue;
            }
            mh_route_set(trs, d1, (unsigned)(tr > 0 && !track_is_muted(tr) ? tr : 0));
        }
#else
        (void)port;
#endif
        if (tr == TR_DRUM) {
            int pad = gm_pad(d1);
            if (on && pad >= 0)
                pad_fire(pad, d2 >= 127u ? 1.0f : (float)d2 / 127.0f);
        } else if (tr > 0 && d1 > 0u) {
            if (on && track_is_muted(tr))
                continue;
#if FM1B_USB_HOST
            if (on || off)
                mh_note(tr, d1, d2, (unsigned)(ch ? (trs ? MH_TRS_FIXED : MH_USB_FIXED)
                                                   : (trs ? MH_TRS_SEL : MH_USB_SEL)), on);
#else
            if (tset(tr)->arp_mode) {
                if (on || off)
                    arp_key(tr, (int)d1, (int)d2, on);
            } else if (on) {
                syn_note_on(tr, (int)d1, (float)d2 / 127.0f, -1, tset(tr)->type == TY_MONO);
                lfo_note(tr);
            } else if (off) {
                syn_note_off(tr, (int)d1);
            }
#endif
        }
    }
}

/* the synth, a block of SYN_CTL samples at a time (its voices dealt between the two cores), taken out as the render
 * needs it: a note starts at the next block, within 0.73 ms */
static float syn_l[SYN_CTL], syn_r[SYN_CTL], syn_rv[SYN_CTL], syn_dl[SYN_CTL];
static uint32_t syn_at = SYN_CTL;
static uint8_t cpu2;                      /* the second core takes half the voices */
static uint8_t pool_max, pool_now;        /* the voices allowed: 16 on two cores, 8 on one; fewer while the CPU is short */

static void syn_job(uint32_t half) { PLAT_FAR(syn_half)(half); }   /* (RAM code, 0.5: dsp/gm_ram.h) */
static volatile uint32_t syn_t2;          /* the second core's half: the ticks it took (syn_job2) */
static void syn_job2(uint32_t half)
{
    CS_MARK(a);
    PLAT_FAR(syn_half)(half);
    syn_t2 = plat_ticks24() - a;
}
static uint32_t syn_work;                 /* this block: the voices' rendering on both cores, ticks */
/* GhettoMachine 0.5 (CPU): the voices dealt between the cores by time (synth.c syn_split). Each synth block that both
 * render: the first core waited for the second (wait): more voices to the first; the second had done before the first
 * (no wait, the first's half the longer): more to the second. A step of 4 / 256 a block either way: it settles where
 * both finish together, a voice's worth either side. split_fixed: a test's share, held */
static int split_q8 = 128;
static uint8_t split_fixed;
static float split_wait_us, split_share;  /* running means (the readout): the first core's wait a synth block, us; the
                                           * first core's share, % */
static void drum_offer(void);             /* (the drums to the second core: below, by app_render) */

/* the second core takes its half of the voices only from SYN_SPLIT_MIN voices up: handing a job over and waiting for
 * it costs the first core about what a voice or two does (the synth with no voices took 8 % of a block on the FM-1,
 * eight hand-offs a block), so with fewer the first core renders them all */
#define SYN_SPLIT_MIN 2                   /* (4 at first; measured since: a hand-off about 18 us, a voice about 33 a
                                           * synth block, so even one voice handed over pays) */

/* The tracks' mix over both cores (0.5): each track's voices summed, its insert and Color, its pan, sends and meter
 * are its own work on its own buffers (dsp/synth.c syn_mix_track), so the second core takes some tracks while the
 * first does the others, then the first makes the mix of them. Which: by what each track's mix cost the block before
 * (mixc, ticks), the split of the three whose longer side is shortest, a hand-off's time on the second core's side;
 * all here when that saves less than MIX_GAIN, or the second core is busy (a block's tail) */
#define MIX_HAND (18u * PLAT_TICKS_PER_US)
#define MIX_GAIN (8u * PLAT_TICKS_PER_US)
static uint32_t mixc[NTRK];

static void mix_job(uint32_t mask)        /* the tracks in mask (bit t), each mixed and timed; either core */
{
    int t;
    for (t = 1; t < NTRK; t++)
        if (mask >> t & 1u) {
            CS_MARK(t0);
            PLAT_FAR(syn_mix_track)(t);
            mixc[t] = plat_ticks24() - t0;
        }
}

static uint32_t mix_split(void)           /* the tracks for the second core (bit t); 0: none */
{
    const uint32_t all = mixc[1] + mixc[2] + mixc[3];
    uint32_t best = all, pick = 0, m;
    for (m = 2u; m < 16u; m += 2u) {      /* every set of the tracks 1..3 but none and all */
        uint32_t there = MIX_HAND, here, longer;
        int t;
        if (m == 14u)
            continue;
        for (t = 1; t < NTRK; t++)
            if (m >> t & 1u)
                there += mixc[t];
        here = all + MIX_HAND - there;
        longer = here > there ? here : there;
        if (longer < best) {
            best = longer;
            pick = m;
        }
    }
    return best + MIX_GAIN <= all ? pick : 0u;
}

static void syn_block(void)
{
    int t;
    drum_offer();
    CS_MARK(t0);
    for (t = 0; t < NTRK; t++)
        lfo_tick(t, SYN_CTL);
    for (t = 1; t < NTRK; t++) {
        arp_tick(t, SYN_CTL);
        syn_mod(t, proj.lfo[t].dest, lfo_rt[t].out, (float)XL(t, XL_LDEPTH, proj.lfo[t].depth) / 64.0f);
    }
    syn_prepare();
    {
        CS_MARK(t1);
        uint32_t work;                    /* the voices' rendering, both cores' */
        sy_t[SY_PREP] += t1 - t0;
        if (cpu2 && syn_voices(-1) >= SYN_SPLIT_MIN && plat_cpu2_on() && plat_cpu2_run(syn_job2, 1u)) {
            syn_job(0);
            work = plat_ticks24() - t1;
            {
                CS_MARK(tw);
                const int gone = plat_cpu2_wait() != 0;
                const uint32_t wait = plat_ticks24() - tw;
                if (gone) {               /* it was given up (cpu2_fallback): its half here */
                    CS_MARK(t2);
                    syn_job(1);
                    work += plat_ticks24() - t2;
                } else {
                    work += syn_t2;
                    if (!split_fixed) {   /* (the deal for the next synth block: syn_prepare) */
                        if (wait > 2u * PLAT_TICKS_PER_US)
                            split_q8 += 4;
                        else if (work - syn_t2 > syn_t2)
                            split_q8 -= 4;
                        split_q8 = split_q8 < 32 ? 32 : split_q8 > 224 ? 224 : split_q8;
                        syn_split(split_q8);
                    }
                    split_wait_us += ((float)wait * (1.0f / (float)PLAT_TICKS_PER_US) - split_wait_us) * 0.01f;
                    split_share += ((float)split_q8 * (100.0f / 256.0f) - split_share) * 0.01f;
                }
            }
        } else {
            syn_job(0);
            syn_job(1);
            work = plat_ticks24() - t1;
        }
        t0 = plat_ticks24();
        syn_work += work;
        /* the voices' share of the synth's time: as halved between the two cores, whichever rendered them (while the
         * second core has the drums the first renders them all: the budget's cap must not fall for that) */
        sy_t[SY_VOICES] += cpu2 && plat_cpu2_on() ? work / 2u : work;
    }
    {
        const uint32_t there = cpu2 ? mix_split() : 0u;
        if (there && plat_cpu2_on() && plat_cpu2_run(mix_job, there)) {
            mix_job(14u & ~there);
            if (plat_cpu2_wait() != 0)    /* it was given up (cpu2_fallback): its tracks here */
                mix_job(there);
        } else {
            mix_job(14u);
        }
    }
    PLAT_FAR(syn_mix_end)(syn_l, syn_r, syn_rv, syn_dl);   /* (RAM code) */
    sy_t[SY_MIX] += plat_ticks24() - t0;
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

/* the stems of the block rendered, sent with the next block (the master pair comes a block late now: post_job), a
 * frame's tracks together */
static int16_t stem_hold[APP_BLOCK * 2u * NTRK];   /* (not in the pool: the project there grew by the clips' LFOs) */
static void stems_send(uint32_t n)        /* the block before's stems, into this block's channels; the hold cleared */
{
    uint32_t off = 0, k;
    const int16_t *h = stem_hold;
    while (off < n) {
        uint32_t run;
        int16_t *p = plat_usb_ch(off, &run);
        if (!p || !run)
            break;
        if (run > n - off)
            run = n - off;
        for (k = 0; k < run; k++, p += PLAT_USB_CH_STRIDE, h += 2u * NTRK)
            for (int c = 0; c < 2 * NTRK; c++)
                p[c] = h[c];
        off += run;
    }
    for (k = 0; k < APP_BLOCK * 2u * NTRK; k++)
        stem_hold[k] = 0;
}

/* ---- the screen's scope (0.5): one capture at a time of what the page on screen shows: a track's sound (its mono sum,
 * every dec'th sample: where the stems are put) or the master's (after the master bus; left and right in pairs). The
 * page asks (scope_take); the audio fills it; full, it waits until the page has drawn it and asks again (scope_again),
 * so the page never draws one half filled. 256 int16: 512 B, shared with a drum's hit (trial_capture) */
enum { SC_MASTER = 8, SC_NONE = 9 };
#define SC_N 256
static struct {
    volatile uint8_t src, state;          /* what's asked (a track 0..3, SC_MASTER); 0 idle, 1 filling, 2 full */
    uint8_t dec, pairs, k;
    uint16_t n;
    float acc, inv;                       /* the master's mono sum: the dec samples' mean (1 / dec), not one of them */
    int16_t buf[SC_N];
} sc = {SC_NONE, 0, 1, 0, 0, 0, 0.0f, 1.0f, {0}};

static void scope_feed(int src, const float *l, const float *r, uint32_t n, float scale)
{
    uint32_t i;
    int mean = src == SC_MASTER && !sc.pairs;   /* (the spectrum's: every 4th sample alone folded the highs into its lows) */
    if (sc.state != 1 || sc.src != (uint8_t)src)
        return;
    for (i = 0; i < n; i++) {
        float a;
        if (mean)
            sc.acc += (l[i] + r[i]) * 0.5f;
        if (++sc.k < sc.dec)
            continue;
        sc.k = 0;
        a = (sc.pairs ? l[i] : mean ? sc.acc * sc.inv : (l[i] + r[i]) * 0.5f) * scale;
        sc.acc = 0.0f;
        sc.buf[sc.n++] = (int16_t)(a >= 1.0f ? 32767 : a <= -1.0f ? -32767 : (int)(a * 32767.0f));
        if (sc.pairs) {
            a = r[i] * scale;
            sc.buf[sc.n++] = (int16_t)(a >= 1.0f ? 32767 : a <= -1.0f ? -32767 : (int)(a * 32767.0f));
        }
        if (sc.n >= SC_N) {
            sc.state = 2;
            return;
        }
    }
}

/* n frames of track s (l, r: the mix's scale) into its stem, from the frame off into the block being rendered */
static void stem_put(int s, const float *l, const float *r, uint32_t off, uint32_t n)
{
    {
        uint32_t k;
        int16_t *p;
        if (off >= APP_BLOCK)
            return;
        if (n > APP_BLOCK - off)
            n = APP_BLOCK - off;
        p = stem_hold + off * 2u * NTRK + 2 * s;
        for (k = 0; k < n; k++, p += 2u * NTRK) {
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
    }
}

/* the synth into the mix (and each of its tracks into its stem), from the frame off into the block being rendered;
 * set: written, not added (the hold, while the second core has the drums) */
static void synth_add(float *l, float *r, float *rv, float *dl, uint32_t n, uint32_t off, int set)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t k, m;
        if (syn_at >= SYN_CTL)
            syn_block();
        m = SYN_CTL - syn_at;
        if (m > n - i)
            m = n - i;
        if (set) {
            for (k = 0; k < m; k++) {
                l[i + k] = syn_l[syn_at + k];
                r[i + k] = syn_r[syn_at + k];
                rv[i + k] = syn_rv[syn_at + k];
                dl[i + k] = syn_dl[syn_at + k];
            }
        } else {
            for (k = 0; k < m; k++) {
                l[i + k] += syn_l[syn_at + k];
                r[i + k] += syn_r[syn_at + k];
                rv[i + k] += syn_rv[syn_at + k];
                dl[i + k] += syn_dl[syn_at + k];
            }
        }
        if (STEMS && stems_on) {
            int t;
            for (t = 1; t < NTRK; t++) {
                const float *a = syn_track(t, 0), *b = syn_track(t, 1);
                if (a && b)
                    stem_put(t, a + syn_at, b + syn_at, off + i, m);
            }
        }
        if (sc.state == 1 && sc.src >= 1 && sc.src < NTRK) {   /* the screen's scope: a track */
            const float *a = syn_track(sc.src, 0), *b = syn_track(sc.src, 1);
            if (a && b)
                scope_feed(sc.src, a + syn_at, b + syn_at, (uint32_t)m, STEM_GAIN);
        }
        syn_at += m;
        i += m;
    }
}

/* the delay's line, lent to STUTTER and TAPE STOP while they play: int16, 0.67 s of stereo (63000, 0.71 s, until
 * Bluetooth audio's stack needed 7 KB of the pool; 66160 until format 11's
 * locks needed 6 KB of the pool: the sampler's recording buffer's size, which went in 0.5. STUTTER's longest slice,
 * 1/8 of a bar at 60 BPM, is 0.5 s; TAPE STOP winds down what's in it) */
#define DLY_LEN 59400
static int16_t shared_buf[DLY_LEN] PLAT_POOL;

/* the render's buses: the dry mix and the reverb and delay sends of a block. Two sets (0.5, the second CPU pass): the
 * first core renders a block into one while the second finishes the block before in the other, in place (post_job:
 * the reverb and delay, the master bus, the perform effects, the metronome); app_render outputs that one. The pointers
 * are the set being rendered now */
typedef struct {
    float l[APP_BLOCK], r[APP_BLOCK], rev[APP_BLOCK], dly[APP_BLOCK];
} bus_t;
static bus_t bus_a;
static bus_t bus_b;                       /* (out of the pool since BongVerb's lines went in) */
static bus_t *const BUS[2] = {&bus_a, &bus_b};
static float *buf_l = bus_a.l, *buf_r = bus_a.r, *bus_rev = bus_a.rev, *bus_dly = bus_a.dly;
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

static void pf_output(float *l, float *r, uint32_t n, uint8_t on)   /* STUTTER, TAPE STOP, CRUSH on the output (under
                                                                      * MASTER); on: the perform keys the block had */
{
    int16_t *b = shared_buf;
    int32_t cap = DLY_LEN / 2;
    uint32_t i;
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

/* GhettoMachine 0.5, the second CPU pass: a block's tail on the second core while the first renders the next block.
 * The first core renders block N (the sequencer, the drums, the synth: its voices shared with the second core once
 * that is free, and drum Color) into one set of buses, and notes here what the tail needs. At the start of block
 * N + 1 the second core takes it (post_job): the reverb and the delay, a sample being recorded, the master bus,
 * STUTTER, TAPE STOP and CRUSH, the metronome, in place in that set; the first core waits for it at the end of block
 * N + 1 and outputs it. Everything is heard a block (5.8 ms) later; nothing else changes. Without the second core (off,
 * given up, the simulator, the browser) the first core does the tail itself, the same way, at the same place */
typedef struct {
    uint32_t n;
    float gain, bpm, beat_len, ratio;     /* MASTER; the tempo; a beat in samples; the click's beat stretched by (the
                                           * tempo moved: tempo_stretch) */
    float kick_vel;                       /* PUMP: the block's loudest kick (0: none), and its kit */
    int kick_kit, bar_at;                 /* (bar_at: where the bar starts in the block, -1: not in it) */
    uint8_t want, playing, counting, click_on;   /* the perform keys; playing; counting in; the click on */
    uint32_t t_fx, t_master;              /* its stages' ticks (the CPU view) */
    volatile uint8_t ready, done;         /* noted for the tail; the tail done */
} post_t;
static post_t post[2];
static uint8_t cur_set;                   /* the set the first core renders now */
static uint8_t pf_was;                    /* the perform keys the last tail had (the tail's own) */

static void pump_key(int kit, float vel)  /* audio interrupt: a kick for PUMP, given at this block's tail (fx_key) */
{
    post_t *j = &post[cur_set];
    if (vel > j->kick_vel) {
        j->kick_vel = vel;
        j->kick_kit = kit;
    }
}

static void click_stretch(float r) { post[cur_set].ratio *= r; }

static void post_job(uint32_t which)      /* a block's tail (the second core's job, or the first's) */
{
    post_t *j = &post[which];
    bus_t *b = BUS[which];
    float *l = b->l, *r = b->r;
    uint32_t n = j->n, i;
    {   /* STUTTER, TAPE STOP: their slices set going as their keys go down; let go, the live sound fades back */
        uint8_t w = j->want, up = (uint8_t)(w & ~pf_was), down = (uint8_t)(pf_was & ~w);
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
        pf_was = w;
    }
    fx_bpm(j->bpm);
    if (j->kick_vel > 0.0f)
        fx_key(j->kick_kit, j->kick_vel);
    {
        CS_MARK(t0);
        fx_process(l, r, b->rev, b->dly, l, r, (int)n);   /* the reverb and the delay (in place) */
        j->t_fx = plat_ticks24() - t0;
    }
    {
        CS_MARK(t0);
        PLAT_FAR(fx_master)(l, r, (int)n, j->gain);   /* compressor, filter, MASTER (gliding), limiter (RAM) */
        j->t_master = plat_ticks24() - t0;
    }
    scope_feed(SC_MASTER, l, r, n, 1.0f); /* (the screen's scope: the master) */
    if ((j->want & (1u << PF_STUT | 1u << PF_TAPE | 1u << PF_CRUSH)) || perf.fade < 1.0f)
        pf_output(l, r, n, j->want);
    {
        const float bl = j->beat_len;
        const int on = j->click_on;
        ck.to_next *= j->ratio;
        for (i = 0; i < n; i++) {         /* the metronome's beats: the bar's start, then the counter */
            if (!j->playing && !j->counting) {
                ck.beat = 0;
            } else if ((int)i == j->bar_at) {
                if (on)
                    click_hit(1);
                ck.beat = 1;
                ck.to_next = bl;
            } else if ((ck.to_next -= 1.0f) <= 0.0f) {
                ck.to_next += bl;
                if (on && (ck.beat || !j->playing))   /* (playing, beat 1 is the bar's) */
                    click_hit(ck.beat == 0);
                ck.beat = (ck.beat + 1) & 3;
            }
            if (ck.env > 0.0001f) {       /* after the master bus, under MASTER: not in a sample or the meters (in the
                                           * USB master pair, and under MASTER on the speaker: plat_usb_audio_full) */
                float y = ck.c * ck.y1 - ck.y2, v = ck.y1 * ck.env * j->gain;
                ck.y2 = ck.y1;
                ck.y1 = y;
                ck.env *= 0.9975f;
                l[i] += v;
                r[i] += v;
            }
        }
    }
    j->done = 1;
}

/* the budgets kept (the audio interrupt, at a block's end, the second core idle): the block's times in ticks */
#define BUD_US(pct) ((float)(pct) * (1.0e4f * (float)APP_BLOCK / 44100.0f))   /* a share of a block, us */
#define GOV_MIN_VOICES 8                  /* the cap's floor: however dear a voice is measured, the budget allows 8 (3
                                           * until the 0.5 CPU pass; 8 heavy voices checked on the FM-1) */
#define GUARD_MIN_VOICES 3                /* the guard's own floor: a block over BUD_GUARD % is an emergency */
static float gov_us(int stage, uint32_t ticks)
{
#ifdef PLAT_HOST
    (void)ticks;
    return gov_fake[stage];
#else
    (void)stage;
    return (float)ticks * (1.0f / (float)PLAT_TICKS_PER_US);
#endif
}

static void pool_apply(void)              /* the voices allowed: the CPU's shedding and the synth's budget, the fewer */
{
    syn_pool(pool_now < gov.cap ? pool_now : gov.cap);
}

/* Same selection for both pressure signals. Pad 0 is BD and pad 1 is SD in each kit.
 * An unsigned age keeps a hit's protection intact across the block counter wrapping. */
static void govern_drum_tail(void)
{
    enum { CLOSED_HAT = 7, OPEN_HAT = 8, ATTACK_BLOCKS = 4 };
    int oldest[2] = {-1, -1};             /* other percussion, hats */
    uint32_t max_age[2] = {0, 0};
    for (int pad = 2; pad < NPADS; pad++) {
        int hat = pad == CLOSED_HAT || pad == OPEN_HAT;
        uint32_t age = gov.clock - gov.pad_at[pad];
        if (age >= ATTACK_BLOCKS && age > max_age[hat] && drums_governor_tail_active(pad)) {
            oldest[hat] = pad;
            max_age[hat] = age;
        }
    }
    int pad = oldest[1] >= 0 ? oldest[1] : oldest[0];
    if (pad >= 0 && drums_governor_release_tail(pad)) {
        gov.cuts++;
        gov.act[0] = gov.clock;
    }
}

static void govern(uint32_t t_drums, uint32_t t_synth, uint32_t t_fxin, uint32_t t_fx, uint32_t t_total)
{
    const float d = gov_us(CS_DRUMS, t_drums), sy = gov_us(CS_SYNTH, t_synth);
    const float fx = gov_us(CS_FXIN, t_fxin) + gov_us(CS_FX, t_fx);
    const int guard = gov_us(CS_TOTAL, t_total) > BUD_US(BUD_GUARD);
    gov.clock++;
    {   /* the synth: its time is syn_base + per_voice a voice, both measured as it plays (three voices or more: a
         * running mean, a rise followed in about 4 blocks, a fall in about 20, so one slow block sheds little and a
         * real overload is met in 25 ms); the cap is what the budget buys */
        int v = syn_voices(-1), cap = pool_max;
        if (v == 0) {
            gov.syn_base += (sy - gov.syn_base) * 0.05f;
        } else if (v >= 3) {
            float per = (sy - gov.syn_base) / (float)v;
            if (per > 0.0f)
                gov.per_voice = gov.per_voice <= 0.0f ? per
                                : gov.per_voice + (per - gov.per_voice) * (per > gov.per_voice ? 0.25f : 0.05f);
        }
        if (gov.per_voice > 0.0f) {
            float room = BUD_US(BUD_SYNTH) - gov.syn_base;
            cap = room > 0.0f ? (int)(room / gov.per_voice) : 0;
            cap = cap < GOV_MIN_VOICES ? GOV_MIN_VOICES : cap > pool_max ? pool_max : cap;
        }
        if (cap != gov.cap) {
            if (cap < gov.cap && v > cap) {
                gov.caps += (uint32_t)(v - cap);
                gov.act[1] = gov.clock;
            }
            gov.cap = (uint8_t)cap;
            pool_apply();
        }
    }
    {   /* One release per block even when both the drum stage and the total run over. */
        float m = 0.5f * (d + gov.d_prev);
        gov.d_prev = d;
        if (!V3ON && (m > BUD_US(BUD_DRUMS) || guard))
            govern_drum_tail();
    }
    {   /* Keep the shared returns running. Drum Color can restart from silence when the combined cost falls. */
        gov.fx_mean += (fx - gov.fx_mean) * 0.25f;
        if (gov.fx_wait) {
            gov.fx_wait--;
        } else if (0) {                    /* (drum Color was the effect to shed: none in the tracks now) */
            gov.fx_shed = 1;
            gov.fx_wait = 8;
            gov.sheds++;
            gov.act[2] = gov.clock;
        } else if (gov.fx_shed && gov.fx_mean < 0.6f * BUD_US(BUD_FX)) {
            DRUM_COLOR_RESET();
            gov.fx_shed = 0;
            gov.fx_wait = 688;
        }
    }
    if (guard) {
        int v = syn_voices(-1);
        if (v > GUARD_MIN_VOICES) {
            int to = v - 2 < GUARD_MIN_VOICES ? GUARD_MIN_VOICES : v - 2;
            syn_pool(to);                 /* (the two go; the pool as it was for the notes to come) */
            pool_apply();
            gov.caps += (uint32_t)(v - to);
        }
        gov.guards++;
        gov.act[1] = gov.clock;
    }
}

/* GhettoMachine 0.5 (CPU): the drums on the second core. A chunk's drums (the kits, their Color, their level and sends,
 * their stem and meter) are one piece of work that nothing else touches until the synth is added to them, and the
 * synth's own work never touches the drums: the two can run at once. Usually the first core renders the drums while
 * the second finishes the block before (its tail: the reverb, the delay, the master), then the two share the synth's
 * voices. With both the drums and the synth heavy that leaves the first core the long side (the drums, then half the
 * voices, the control ticks, the mix and the hand-offs: over half a block on the FM-1) while the second has done by a
 * third. Then (drums2_mode, a block at a time, from what each part took lately) the first core starts on the synth at
 * once, all its voices, into a hold, and the drums go to the second core as soon as its tail is done (drum_offer: one
 * job at a time there); the first core waits for them after the synth and adds the hold. The same samples either way:
 * the drums and the synth are worked out as before, and their sum is the same one addition a sample.
 * Only between the hits: the sequencer (the first core) has joined the job before it fires anything, and the drums'
 * LFO is read at a hit (lfo_rt, by then ticked as many times either way) */
static float hold_l[APP_BLOCK], hold_r[APP_BLOCK], hold_rv[APP_BLOCK], hold_dl[APP_BLOCK];
static uint32_t dr_t[2];                  /* the drums' ticks this block, on each core */
static uint32_t syn_here;                 /* the first core's time in the synth this block (the CPU view's REST) */
static struct {
    uint32_t done, chunk;                 /* the chunk the job renders */
    volatile uint8_t fin;                 /* the job has rendered it */
    uint8_t due;                          /* a chunk's drums wait for the second core */
    uint8_t mode;                         /* this block: the drums are the second core's */
    uint8_t ran2;                         /* this block: it rendered them */
    uint8_t force;                        /* 0: there whenever it runs (the simulator: never); 1 always; 2 never;
                                           * 3 as drums2_pick chooses (the tests, CC 109) */
    float est_d, est_v, est_c;            /* running means, us a block: the drums and their Color; the voices'
                                           * rendering on both cores; the synth's control ticks and mix */
    float share;                          /* blocks the second core rendered the drums, % (a running mean) */
    uint32_t blocks;                      /* ... and how many so far */
} d2;

static uint32_t v3_voice_ticks, v3_bus_ticks;   /* this block's DrumEngine voices and bus (the board's telemetry) */

static void drum_chunk(uint32_t done, uint32_t chunk)   /* either core: a chunk's drums into the mix's buses */
{
    uint32_t i;
    const unsigned core = plat_core() & 1u;
    const uint32_t dc0 = plat_ticks24(), own0 = dr_t[core] + fxin_t[core];
    if (V3ON) {                        /* the DrumEngine: no drum Color (its bus has SAT, COMP, TRANS, LIMIT) */
        unsigned vt = 0, bt = 0;
        trial_render(buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, chunk, v3_mode, &vt, &bt);
        v3_voice_ticks += vt;
        v3_bus_ticks += bt;
        dr_t[plat_core() & 1u] += vt;
        fxin_t[plat_core() & 1u] += bt;
        if (track_is_muted(TR_DRUM))
            for (i = done; i < done + chunk; ++i)
                buf_l[i] = buf_r[i] = bus_rev[i] = bus_dly[i] = 0.f;
    } else {
    CS_MARK(cs_d0);                       /* the stock 808 / 909 (STOCK): no Color (the unified engine: none in a track) */
    drums_render(proj.kit, buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, (int)chunk);
    dr_t[plat_core() & 1u] += plat_ticks24() - cs_d0;
    }
    drum_mix(buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, chunk);
    if (STEMS && stems_on)                /* (the mix holds the drums alone until the synth comes in) */
        stem_put(TR_DRUM, buf_l + done, buf_r + done, done, chunk);
    scope_feed(TR_DRUM, buf_l + done, buf_r + done, chunk, STEM_GAIN);   /* (the screen's scope: the drums) */
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
    if (!core) {                          /* (OTHER's DRUM: the chunk's time but the drums' own and their bus's) */
        const uint32_t all = plat_ticks24() - dc0, own = dr_t[0] + fxin_t[0] - own0;
        ot_t[OT_DRUM] += all > own ? all - own : 0u;
    }
}

static void drum_job(uint32_t arg)        /* the second core's */
{
    (void)arg;
    drum_chunk(d2.done, d2.chunk);
    d2.fin = 1;
    plat_cpu2_sync();
}

static void drums2_force(unsigned v) { d2.force = (uint8_t)(v <= 3u ? v : 0u); }   /* (CC 109 on channel 16) */

static void drum_offer(void)              /* a chunk's drums to the second core, if it is free now */
{
    if (d2.due && plat_cpu2_on() && !plat_cpu2_busy() && plat_cpu2_run(drum_job, 0u))
        d2.due = 0;
}

/* Which way is shorter for the first core, in us a block: d the drums and their Color, p the block's tail (the second
 * core's), v the voices' rendering on both cores, c the synth's control ticks and mix; now: the drums are the second
 * core's already (it takes 300 us to change to that and 100 to stay: no flapping).
 *   the usual way: the drums, then the synth with the voices halved, and its hand-offs (8 synth blocks' voices and mix,
 *                  about 18 us each)
 *   the drums there: the longer of the whole synth here and the tail and the drums there */
static int drums2_pick(float d, float p, float v, float c, int now)
{
    const float hand = v > 1.0f ? 16.0f * 18.0f : 0.0f;
    const float usual = d + c + 0.5f * v + hand;
    const float there = c + v > p + d ? c + v : p + d;
    return there + (now ? 100.0f : 300.0f) < usual;
}

/* The drums go to the second core in every block it is running (0.5's CPU pass, measured on the FM-1 against the
 * pick above, CC 109): with 8 of the heaviest voices over the dense drums the pick's worst block was 5.7 ms dry and
 * 6.2 ms (one overrun) with the DrumEngine's bus, always here 5.7 and 6.1 (16 overruns, 104 voices shed), always there
 * 4.5 and 4.7 ms with nothing shed; with 10 light voices or the four-track score there was also the best worst block
 * (3.8 to 4.3 ms against 4.2 to 5.0). The pick's estimates lag the load and its switches cost a spike. With the drums
 * alone the first core waits for them a little longer than it would take them itself: no risk there (under 4 ms) */
static int drums2_mode(void)              /* this block: the drums to the second core */
{
    if (d2.force)
        return d2.force == 1 ? 1 : d2.force == 2 ? 0
               : drums2_pick(d2.est_d, cs_us[CS_FX] + cs_us[CS_MASTER], d2.est_v, d2.est_c, d2.mode);
#ifdef PLAT_HOST
    return 0;                             /* (the simulator's times are the Mac's: the tests force it) */
#else
    return cpu2 && plat_cpu2_on();
#endif
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t done = 0, i;
    const uint32_t prev = cur_set;        /* the block before's set: its tail runs now (post_job) */
    int bar_at = -1, post_there = 0, post_here = 0;
    float gain = (float)master_q12 / 4096.0f, pk = peak, pkl = peak_l, pkr = peak_r;
    uint32_t out0;
    CS_MARK(cs_start);
    for (i = 0; i < CS_NUM; i++)
        cs_t[i] = 0;
    for (i = 0; i < OT_NUM; i++)
        ot_t[i] = 0;
    dr_t[0] = dr_t[1] = 0;
    v3_voice_ticks = v3_bus_ticks = 0;
    syn_here = 0;
    syn_work = 0;
    d2.due = 0;
    d2.ran2 = 0;
    d2.mode = (uint8_t)drums2_mode();
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    cur_set ^= 1u;                        /* this block into the other set */
    {
        post_t *j = &post[cur_set];
        bus_t *b = BUS[cur_set];
        buf_l = b->l;
        buf_r = b->r;
        bus_rev = b->rev;
        bus_dly = b->dly;
        j->ready = j->done = 0;
        j->kick_vel = 0.0f;
        j->ratio = 1.0f;
        j->t_fx = j->t_master = 0;
    }
    if (post[prev].ready)                 /* the block before's tail: on the second core while this one renders */
        post_there = cpu2 && plat_cpu2_on() && plat_cpu2_run(post_job, prev);
    if (STEMS) {                          /* a computer records the USB stems this block: the block before's go */
        uint32_t run;
        stems_on = plat_usb_ch(0, &run) != 0;
        if (stems_on)
            stems_send(n);
    }
    midi_in();
    pfx_follow();                         /* MIX PER PATTERN: the focused pattern's FX */
    mo_tick((int32_t)n);                  /* SEQ OUT: the note-offs due */
    render_t += n;
    tempo_stretch();
    {   /* the perform keys: what changed since the last block (ROLL here; the output's effects: post_job); and the
         * QUICK MIXER's track moves, ROLL and STUTTER on the one track */
        uint8_t w = perf.want, qr = perf.qroll, qs = perf.qstut, rmask, fresh;
        int qt = perf.qtrk, t;
        if (qt < 1 || qt >= NTRK)
            qr = qs = 0;
        if (qs && (perf.qs_n != qs || perf.qs_trk != qt)) {   /* STUTTER: the slice of qs steps (on their grid) the
                                                               * track is in; another length: from where it is */
            int at = perf.qs_n && perf.qs_trk == qt ? perf.qs_start + perf.qs_k : sq.t[qt].step < 0 ? 0 : sq.t[qt].step;
            perf.qs_start = (int16_t)(at / qs * qs);
            perf.qs_k = (int16_t)(at - perf.qs_start);
            perf.qs_ps = (int16_t)at;
        }
        perf.qs_n = qs;
        perf.qs_trk = (uint8_t)qt;
        rmask = (w >> PF_ROLL & 1u) ? (uint8_t)((1u << NTRK) - 1u) : qr ? (uint8_t)(1u << qt) : 0u;
        fresh = (uint8_t)(rmask & ~perf.rmask);
        if (fresh) {                      /* tracks starting to roll: their steps as heard now */
            if (!perf.rmask) {
                roll_inhibit_tracks = proj.tmute;
                roll_inhibit_pads = dcur()->mute;
                perf.roll_to = 0.0f;
            }
            for (t = 0; t < NTRK; t++)
                if (fresh >> t & 1u)
                    perf.roll_step[t] = (int8_t)(perf.qs_n && perf.qs_trk == t ? perf.qs_ps
                                                 : sq.t[t].step < 0 ? 0 : sq.t[t].step);
        }
        if (rmask)                        /* its rate: the perform key's (its K1), else the QUICK MIXER key's */
            perf.roll_len = bar_samples() * (1.0f / 16.0f) * ROLL_F[((w >> PF_ROLL & 1u) ? perf.roll_rate : qr - 1u) % 5];
        perf.rmask = rmask;
        perf.on = w;
        fill_pos_follow();                /* FILL: the drum clip's own place kept underneath, taken up again after */
    }
    if (sq.playing && (proj.recq >> 2 & 3u) == 1u) {   /* MIDI CLOCK sent: 24 a beat (to the block) */
        float tick = bar_samples() / 96.0f;
        while (sq.mclk_to < (float)n) {
            plat_midi_send(0x0Fu | 0xF8u << 8, MIDI_TO);
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
    ot_t[OT_IN] = plat_ticks24() - cs_start;
    while (done < n) {                     /* rendered in pieces that end where a step (or the bar) starts */
        uint32_t chunk = n - done;
        int t;
        uint32_t sq0 = plat_ticks24();
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
            if (perf.rmask) {             /* ROLL: its own beat */
                if (perf.roll_to < 0.5f) {
                    roll_fire();
                    perf.roll_to += perf.roll_len;
                }
            }
            m = sq.bar_left;
            if (sub_on) {                 /* substeps: theirs */
                int i;
                for (i = 0; i < NPADS + NINST; i++) {
                    if (!(sub_on >> i & 1u))
                        continue;
                    if (sub[i].to < 0.5f) {
                        sub_fire(i);
                        sub[i].to += sub[i].len;
                        if (--sub[i].left == 0) {
                            sub_on &= ~(1u << i);
                            continue;
                        }
                    }
                    if (sub[i].to < m)
                        m = sub[i].to;
                }
            }
            for (t = 0; t < NTRK; t++)
                if (sq.t[t].to_next < m)
                    m = sq.t[t].to_next;
            if (perf.rmask && perf.roll_to < m)
                m = perf.roll_to;
            if (m < (float)chunk)
                chunk = m < 1.0f ? 1u : (uint32_t)m;
        }
        sync_drum_mutes();
        ot_t[OT_SEQ] += plat_ticks24() - sq0;
        if (d2.mode) {
            /* the drums for the second core this block (drums2_mode): the synth first, here, into the hold; the
             * drums handed over as soon as the second core has done the block before's tail (drum_offer: now, and
             * at each synth block), joined after the synth, and the synth added to them: the sums of the usual way */
            CS_MARK(cs_s0);
            d2.done = done;
            d2.chunk = chunk;
            d2.fin = 0;
            d2.due = 1;
            drum_offer();
            synth_add(hold_l + done, hold_r + done, hold_rv + done, hold_dl + done, chunk, done, 1);
            syn_here += plat_ticks24() - cs_s0;
            if (d2.due) {                 /* the second core never came free: here, after all */
                d2.due = 0;
                drum_chunk(done, chunk);
            } else {
                const uint32_t w0 = plat_ticks24();
                int gone = plat_cpu2_wait() != 0;
                plat_cpu2_sync();
                ot_t[OT_JOIN] += plat_ticks24() - w0;
                if (gone || !d2.fin)      /* it was given up with the job (cpu2_fallback): here */
                    drum_chunk(done, chunk);
                else
                    d2.ran2 = 1;
            }
            sq0 = plat_ticks24();
            for (i = done; i < done + chunk; i++) {
                buf_l[i] += hold_l[i];
                buf_r[i] += hold_r[i];
                bus_rev[i] += hold_rv[i];
                bus_dly[i] += hold_dl[i];
            }
            ot_t[OT_JOIN] += plat_ticks24() - sq0;
        } else {
            drum_chunk(done, chunk);
            {
                CS_MARK(cs_s0);
                synth_add(buf_l + done, buf_r + done, bus_rev + done, bus_dly + done, chunk, done, 0);
                syn_here += plat_ticks24() - cs_s0;
            }
        }
        sq0 = plat_ticks24();
        if (sq.playing) {
            sq.bar_left -= (float)chunk;
            perf.roll_to -= (float)chunk;
            if (sub_on) {
                int i;
                for (i = 0; i < NPADS + NINST; i++)
                    sub[i].to -= (float)chunk;
            }
            for (t = 0; t < NTRK; t++) {
                sq.t[t].to_next -= (float)chunk;
                sq.t[t].frac = sq.t[t].dur > 0.0f ? 1.0f - sq.t[t].to_next / sq.t[t].dur : 0.0f;
            }
            fill_pos_run(chunk);
            sq.clock += chunk;
        }
        done += chunk;
        ot_t[OT_SEQ] += plat_ticks24() - sq0;
    }
    if (perf.on & (1u << PF_THROW | 1u << PF_WASH))   /* THROW, WASH: the whole mix into the delay, the reverb */
        for (i = 0; i < n; i++) {
            float d = 0.4f * (buf_l[i] + buf_r[i]);
            if (perf.on >> PF_THROW & 1u)
                bus_dly[i] += d;
            if (perf.on >> PF_WASH & 1u)
                bus_rev[i] += d;
        }
    {   /* this block's tail, noted for the next block (post_job) */
        post_t *j = &post[cur_set];
        const int mode = click_mode();
        j->n = n;
        j->gain = gain;
        j->bpm = (float)proj.bpm10 * 0.1f;
        j->beat_len = bar_samples() * 0.25f;
        j->want = perf.on;
        j->playing = sq.playing;
        j->counting = ck.count > 0;
        j->bar_at = bar_at;
        j->click_on = (uint8_t)((sq.playing && (mode == CK_ON || (mode == CK_REC && ck.rec))) || ck.count > 0);
        j->ready = 1;
    }
    {   /* the block before's tail: the second core's, waited for (its synth jobs after it were, already); else here */
        post_t *j = &post[prev];
        if (j->ready) {
            if (post_there) {
                CS_MARK(w0);
                plat_cpu2_wait();
                ot_t[OT_WAIT] = plat_ticks24() - w0;
            }
            if (!j->done) {
                post_job(prev);
                post_here = 1;
            }
        } else {
            for (i = 0; i < n; i++)
                BUS[prev]->l[i] = BUS[prev]->r[i] = 0.0f;
        }
        j->ready = 0;
    }
    out0 = plat_ticks24();
    for (i = 0; i < n; i++) {
        float l = BUS[prev]->l[i], r = BUS[prev]->r[i];
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
    ot_t[OT_OUT] = plat_ticks24() - out0;
    {   /* the stages' running means (about the last 50 blocks: 0.3 s). REVERB+DELAY and MASTER: the block before's
         * tail, on the second core (or this one's, then counted out of REST); REST: the rest of this core's time,
         * waiting for the second included */
        uint32_t tot = plat_ticks24() - cs_start, sum = dr_t[0] + syn_here + fxin_t[0];   /* (this core's own work) */
        cs_t[CS_DRUMS] = dr_t[0] + dr_t[1];      /* (the drums' work, whichever core's: the row, their budget) */
        /* the synth's row and budget: its control ticks, its mix and its voices as halved between the cores
         * (syn_block), the same whichever core rendered them */
        cs_t[CS_SYNTH] = sy_t[SY_PREP] + sy_t[SY_VOICES] + sy_t[SY_MIX];
        cs_t[CS_FXIN] = fxin_t[0] + fxin_t[1];   /* (both cores' work: the row, the effects' budget) */
        {   /* what each part took, for the drums' place next block (drums2_mode) */
            const float us = 1.0f / (float)PLAT_TICKS_PER_US;
            d2.est_d += ((float)(cs_t[CS_DRUMS] + cs_t[CS_FXIN]) * us - d2.est_d) * 0.1f;
            d2.est_v += ((float)syn_work * us - d2.est_v) * 0.1f;
            d2.est_c += ((float)(sy_t[SY_PREP] + sy_t[SY_MIX]) * us - d2.est_c) * 0.1f;
            d2.share += ((d2.ran2 ? 100.0f : 0.0f) - d2.share) * 0.02f;
            d2.blocks += d2.ran2;
        }
        fxin_t[0] = fxin_t[1] = 0;
        cs_t[CS_FX] = post[prev].t_fx;
        cs_t[CS_MASTER] = post[prev].t_master;
        if (post_here)
            sum += cs_t[CS_FX] + cs_t[CS_MASTER];
        cs_t[CS_REST] = tot > sum ? tot - sum : 0u;
        cs_t[CS_TOTAL] = tot;
        {   /* OTHER's parts: what is left of it */
            uint32_t parts = 0;
            for (i = 0; i < OT_LEFT; i++)
                parts += ot_t[i];
            ot_t[OT_LEFT] = cs_t[CS_REST] > parts ? cs_t[CS_REST] - parts : 0u;
        }
        for (i = 0; i < CS_NUM; i++) {
            const float u = (float)cs_t[i] * (1.0f / (float)PLAT_TICKS_PER_US);
            cs_us[i] += (u - cs_us[i]) * 0.02f;
            cs_pk[i] = u > cs_pk[i] ? u : cs_pk[i] * 0.997f;
            if (u > cs_mx[cs_mxi][i])
                cs_mx[cs_mxi][i] = u;
        }
        for (i = 0; i < OT_NUM; i++) {
            const float u = (float)ot_t[i] * (1.0f / (float)PLAT_TICKS_PER_US);
            ot_us[i] += (u - ot_us[i]) * 0.02f;
            if (u > ot_mx[cs_mxi][i])
                ot_mx[cs_mxi][i] = u;
        }
        if (++cs_mxn >= 344u) {           /* 2 s: the next bucket, started again */
            cs_mxn = 0;
            cs_mxi = (uint8_t)((cs_mxi + 1u) % CS_MXB);
            for (i = 0; i < CS_NUM; i++)
                cs_mx[cs_mxi][i] = 0.0f;
            for (i = 0; i < OT_NUM; i++)
                ot_mx[cs_mxi][i] = 0.0f;
        }
        for (i = 0; i < SY_NUM; i++) {
            sy_us[i] += ((float)sy_t[i] * (1.0f / (float)PLAT_TICKS_PER_US) - sy_us[i]) * 0.02f;
            sy_t[i] = 0;
        }
        govern(cs_t[CS_DRUMS], cs_t[CS_SYNTH], cs_t[CS_FXIN], cs_t[CS_FX], cs_t[CS_TOTAL]);   /* the budgets, for the
                                                                                             * next block */
        trial_metrics_block(V3ON ? v3_voice_ticks : cs_t[CS_DRUMS],
                            V3ON ? v3_bus_ticks : cs_t[CS_FXIN],
                            plat_ticks24() - cs_start, (unsigned)syn_voices(-1), v3_mode);
    }
}

void app_panic(void)
{
    sq.playing = 0;
    syn_kill();
}

void app_init(void)
{
#if FM1B_USB_HOST && defined(PLAT_HOST)
    /* Simulator reboot keeps its data segment; hardware clears BSS on boot. */
    usb_host_selected = 0;
    mh_usb_start = 0;
    mh_panel_keys = 0;
    memset(mh_notes, 0, sizeof mh_notes);
    memset(mh_route, 0, sizeof mh_route);
    memset(mh_usb_arp, 0, sizeof mh_usb_arp);
#endif
    trial_init();
    trial_metrics_reset();
#ifdef PLAT_HOST
    {   /* GM_DRUMS2=1 or 2 in the environment: the drums' place for the whole run (every suite again that way) */
        const char *e = getenv("GM_DRUMS2");
        if (e && (e[0] == '1' || e[0] == '2'))
            d2.force = (uint8_t)(e[0] - '0');
        e = getenv("GM_SYNSPLIT");        /* GM_SYNSPLIT=N: the voices' share held at N / 256 (the same sound: tests) */
        if (e && atoi(e) > 0) {
            split_q8 = atoi(e);
            split_fixed = 1;
            syn_split(split_q8);
        }
    }
#endif
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
    gov.cap = pool_max;
    syn_pool(pool_max);
    user_load();
    project_load();
    DRUM_COLOR_RESET();
    drum_color_quiet = 0;
    seq_reset_tracks();
    sq.queued = -1;
}

/* ---------------------------------------------------------------------- undo --- */
/* a snapshot is a whole pattern (all four tracks) */
#define NUNDO 6                           /* (8 until BongVerb's lines took 26 KB of the pool: a step 3.6 KB) */
typedef struct { pattern_t p; gen_t g[NINST]; dgen_t dg; uint8_t k[NINST][NSTEPS / 8]; uint8_t idx; } snap_t;   /* (the parts' generators with them) */
/* The total can never exceed NUNDO: a new edit drops redo, and undo/redo
 * each move one item. Grow the stacks from opposite ends of one pool. */
static snap_t history[NUNDO] PLAT_POOL;
static uint8_t nundo = NUNDO;             /* the steps kept now: 1 while Bluetooth audio has the others' memory (gbta_set) */
static int undo_n, redo_n;
static uint32_t undo_t, undo_kind;

static void undo_capture(snap_t *s, uint8_t idx)
{
    s->p = proj.p[idx];
    for (int i = 0; i < NINST; i++)
        s->g[i] = proj.gen[idx][i];
    s->dg = proj.dgen[idx];
    memcpy(s->k, proj.gkeep[idx], sizeof s->k);
    s->idx = idx;
}

static void undo_push(uint8_t idx)
{
    redo_n = 0;                          /* edits invalidate the future */
    if (undo_n >= nundo) {
        for (int i = 1; i < nundo; i++)
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
    from = &history[redo ? nundo - redo_n : undo_n - 1];
    to = &history[redo ? undo_n : nundo - redo_n - 1];
    idx = from->idx;
    if (from != to)
        undo_capture(to, idx);
    f = plat_lock();
    if (from == to) {
        undo_swap(&proj.p[idx], &from->p, sizeof from->p);
        undo_swap(proj.gen[idx], from->g, sizeof from->g);
        undo_swap(&proj.dgen[idx], &from->dg, sizeof from->dg);
        undo_swap(proj.gkeep[idx], from->k, sizeof from->k);
    } else {
        proj.p[idx] = from->p;
        for (int i = 0; i < NINST; i++)
            proj.gen[idx][i] = from->g[i];
        proj.dgen[idx] = from->dg;
        memcpy(proj.gkeep[idx], from->k, sizeof from->k);
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
       PG_GEN, PG_PICK, PG_COLOR = 17, PG_V3_BUS, PG_QMIX };
static const char *const PAGE_NAME[] = {"STEPS", "EDIT", "MUTE", "PATTERNS", "KIT", "KEYS", "ENV", "LFO", "ARP", "MIXER",
                                        "FX", "SONG", "MASTER", "SETUP", "ACID", "PICK NOTES", "", "COLOR", "DrumEngine BUS",
                                        "QUICK MIXER"};
static const char *const V3_BUS_LABEL[] = {"SATURATOR", "COMPRESSOR", "TRANSIENT", "LIMITER"};
static const char *const V3_MODE_LABEL[] = {"STOCK", "DRY", "BUS"};
#define NGENPAGE 4                        /* the 303 generator's (SEQ after STEPS, T2..T4): ACID, NOTES, LINE, KEEP (SELECT) */
#define NDGENPAGE 3                       /* the drums' beat generator's (SEQ after STEPS): BEAT, BEAT 2, LINE */
static const char *const TRK_NAME[NTRK] = {"DRUMS", "T2", "T3", "T4"};
static const char *const TYPE_NAME[NTYPES] = {"MONO", "POLY"};
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
#define NENVPAGE 3
static const char *const ENVPAGE_NAME[NENVPAGE] = {"AMP ENV", "FILTER ENV", "MOD ENV"};
static const int8_t ENVPAGE_P[NENVPAGE][4] = {
    {SP_AA, SP_AD, SP_AS, SP_AR}, {SP_FA, SP_FD, SP_FS, SP_FR}, {SP_MA, SP_MD, SP_MS, SP_MR}};
static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
#define C_RED RGB(232, 76, 60)
#define C_GREEN RGB(90, 210, 130)
#define C_MUTEBG RGB(70, 18, 14)
/* the Acid Dub Deck look (0.5): every section draws in acid green (what's set, what's selected); yellow is what is
 * happening now (the screen's colours: below, at "screen") */
#define C_ACID RGB(195, 255, 49)          /* notes, set values, the selected (black type on it) */
#define C_NOW RGB(255, 227, 73)           /* now: the playhead, a held move, a live reading */
static const uint16_t KIT_RGB[KIT_NUM] = {C_ACID, C_ACID, C_ACID, C_ACID};
static const uint16_t TRK_RGB[NTRK] = {0, C_ACID, C_ACID, C_ACID};

static struct {
    uint8_t page;
    uint8_t trk;                          /* the track picked */
    uint8_t pad;                          /* the drum pad being edited */
    uint8_t color_page;
    uint8_t setup_view;                   /* SETUP: 0 the clock and the sounds, 1 the CPU by stage (SELECT) */
    uint8_t spage;                        /* the step page, 0..7 */
    uint8_t stool;                        /* an instrument track's STEPS: its tools' page (ST_*, SELECT) */
    uint8_t st_vel, st_gate;              /* a new step's velocity and gate there (0: 100, GATE_DEF) */
    uint8_t dtool;                        /* the drums' STEPS: 0 PART GROOVE's knobs, 1 the HIT's (SELECT) */
    uint8_t hnote;                        /* the held step's note being edited: 0 all of them, 1.. from its lowest */
    uint8_t seq_prev, seq_used;           /* a resize chord leaves the previous SEQ view alone */
    uint8_t epage;                        /* the EDIT page (four knobs each) */
    uint8_t envpage;                      /* an instrument track's ENV page: 0 the amp's, 1 the filter's, 2 MOD ENV */
    uint8_t lfopage, arppage;             /* the LFO's pages (LFO 1, its MORE; an instrument's LFO 2, MATRIX); ARP's 0, 1 */
    uint8_t mslot;                        /* MATRIX: the slot on the knobs, 0..SYN_SLOTS - 1 */
    uint8_t genpage;                      /* the 303 generator's page: 0 ACID (the chances), 1 NOTES, 2 LINE, 3 KEEP
                                           * (the drums' BEAT pages: 0..2 of it) */
    uint8_t gcur;                         /* KEEP: the step its knobs are on */
    int8_t batt;                          /* the battery as the band last drew it (plat_battery) */
    uint8_t mixpage, fxpage;              /* the mixer's page (LEVEL, PAN, REVERB, DELAY); the FX's */
    uint8_t mstpage;                      /* the master bus's: CHAIN, EQ, EQ 2, COMP, COMP 2, TAPE, STEREO, LIMIT */
    int8_t knob_k;                        /* the knob turned last (its card stands out a moment), -1 none */
    uint32_t knob_t, live_t;              /* when; when the meters were last drawn */
    uint8_t scur;                         /* the SONG page's entry (song_n: the end, where a new one goes) */
    int8_t save_slot;                     /* SAVE held + PRESETS: the user slot the release saves to, -1 none */
    uint32_t shed_t;                      /* when the voice pool last moved */
    uint8_t rec;                          /* recording armed */
    uint8_t rec_shown;                    /* REC held long enough (or used): its page shows */
    uint8_t help;                         /* SEL held by itself half a second: HELP, the page's held combinations */
    uint32_t sel_t;                       /* when SEL went down */
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
    int count = e > DRUM_MODEL_NUM ? DRUM_MODEL_NUM : e < -DRUM_MODEL_NUM ? DRUM_MODEL_NUM : e * dir;
    for (; count > 0; count--) {
        int next = which;
        for (int tries = 0; tries < DRUM_MODEL_NUM; tries++) {
            next = (next + dir + DRUM_MODEL_NUM) % DRUM_MODEL_NUM;
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
        if (pad >= 2)
            drums_cut_mapped(proj.kit, proj.model, pad);
        roll_inhibit_pads |= (uint16_t)(1u << pad);
        pad_hit[pad] = 0.0f;
        proj.model[pad] = which == proj.kit ? 0 : (uint8_t)(which + 1);
        if (proj.lfo[TR_DRUM].pad == pad && proj.lfo[TR_DRUM].dest != LFO_OFF && !proj.lfo[TR_DRUM].rsv[0])
            proj.lfo[TR_DRUM].rsv[0] = (uint8_t)(old + 1);
        plat_unlock(f);
        dirty = 1;
        ui.epage = (uint8_t)(drum_epages() - 1);
    }
    app_say(trial_pad_name(which, pad), drums_model_name(which));
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
    if (!drum_hits_off(pad)) {
        locks_apply(dcur(), -1, pad);     /* no step's locks; the drums' LFO, if it moves this pad */
        pad_fire(pad, v);
        if (pad == 0)
            fx_key(drum_model(0), v);
    }
    plat_unlock(f);
    plat_midi_send(0x09u | 0x99u << 8 | (uint32_t)PAD_GM[pad] << 16 | (vel ? vel : 1u) << 24, MIDI_TO);
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

static void held_sub(int32_t e)           /* a step held + ALGORITHM: its substeps, 1 (no lock) to 4 */
{
    int tr = ui.trk, s = ui.held_step, i, n, r = 0;
    dpart_t *d = dcur();
    ipart_t *p = tr == TR_DRUM ? 0 : ipart(tr);
    char t[4];
    uint32_t f;
    if (p ? !p->st[s].note[0] : !bit_get(d->hit[ui.pad], s))
        return;
    i = p ? ilock_find(p, s, IL_SUB) : lock_find(d, s, ui.pad, LK_SUB);
    n = clamp((i >= 0 ? (p ? p->lk[i].val : d->lk[i].val) : 1) + e, 1, 4);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (n < 2) {
        if (i >= 0 && p)
            p->lk[i] = p->lk[--p->nlk];
        else if (i >= 0)
            lock_del(d, i);
    } else {
        r = p ? ilock_put(p, s, IL_SUB, n) : lock_put(d, s, ui.pad, LK_SUB, n);
    }
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    int_text(n, t);
    app_say("SUBSTEPS", t);
    ui.redraw = 1;
}

/* ---- copy and paste (0.5). SAVE copies, REC pastes, with what is held saying what: a step (its key held on STEPS:
 * an instrument's notes and locks, the drums' whole column), a drum lane (its black key held on the drums' steps),
 * or the track's part (SEQ held). The clipboard is a note of where, not a copy: paste takes what is there then */
enum { CB_NONE, CB_STEP, CB_LANE, CB_PART };
static struct { uint8_t kind, pat, trk, pad, step; } cb;
static uint8_t cb_rec;                    /* this press of REC was a paste */

static int cb_lane_key(uint32_t keys)     /* the drums' steps: the pad whose black key is held, -1 none */
{
    int k;
    if (ui.trk != TR_DRUM || ui.page != PG_SEQ)
        return -1;
    for (k = 0; k < NPADS; k++)
        if (keys >> BK[k] & 1u)
            return k;
    return -1;
}

static void cb_copy(int kind, int pad)
{
    cb.kind = (uint8_t)kind;
    cb.pat = (uint8_t)clip_of(ui.trk);
    cb.trk = ui.trk;
    cb.pad = (uint8_t)pad;
    cb.step = (uint8_t)(ui.held_step >= 0 ? ui.held_step : 0);
    ui.held_used = 1;
    app_say("COPIED", kind == CB_STEP ? "STEP" : kind == CB_LANE ? "LANE" : "PART");
}

static void cb_paste(int kind, int pad)
{
    const int tr = ui.trk, inst = tr != TR_DRUM, to = ui.held_step;
    uint32_t f;
    int i, n, s, full = 0;
    if (cb.kind != kind) {
        app_say("PASTE", cb.kind == CB_NONE ? "NOTHING COPIED" : kind == CB_STEP ? "COPY A STEP FIRST"
                         : kind == CB_LANE ? "COPY A LANE FIRST" : "COPY A PART FIRST");
        return;
    }
    if ((cb.trk != TR_DRUM) != inst) {
        app_say("PASTE", inst ? "THAT IS A DRUM'S" : "THAT IS A SYNTH'S");
        return;
    }
    undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (kind == CB_PART && inst) {
        ipart_t *d = ipart(tr);
        const ipart_t *o = &proj.p[cb.pat].t[cb.trk - 1];
        if (d != o)
            *d = *o;
    } else if (kind == CB_PART) {
        dpart_t *d = dcur();
        const dpart_t *o = &proj.p[cb.pat].d;
        if (d != o)
            *d = *o;
    } else if (kind == CB_STEP && inst) {
        ipart_t *d = ipart(tr);
        const ipart_t *o = &proj.p[cb.pat].t[cb.trk - 1];
        ilock_t keep[8];
        for (i = n = 0; i < o->nlk && n < 8; i++)   /* (the source's locks first: it may be this very part) */
            if (o->lk[i].step == cb.step)
                keep[n++] = o->lk[i];
        d->st[to] = o->st[cb.step];
        for (i = d->nlk - 1; i >= 0; i--)
            if (d->lk[i].step == to)
                d->lk[i] = d->lk[--d->nlk];
        for (i = 0; i < n; i++)
            full |= ilock_put(d, to, keep[i].param, keep[i].val);
    } else if (kind == CB_STEP) {
        dpart_t *d = dcur();
        const dpart_t *o = &proj.p[cb.pat].d;
        lock_t keep[12];
        uint16_t hits = 0;
        const int acc = bit_get(o->acc, cb.step);
        for (i = 0; i < NPADS; i++)
            hits |= (uint16_t)(bit_get(o->hit[i], cb.step) << i);
        for (i = n = 0; i < o->nlk && n < 12; i++)
            if (o->lk[i].step == cb.step)
                keep[n++] = o->lk[i];
        for (i = 0; i < NPADS; i++) {
            lock_clear_step(d, to, i);
            if (hits >> i & 1u) bit_set(d->hit[i], to); else bit_clear(d->hit[i], to);
        }
        if (acc) bit_set(d->acc, to); else bit_clear(d->acc, to);
        for (i = 0; i < n; i++)
            full |= lock_put(d, to, keep[i].pad, keep[i].param, keep[i].val);
    } else {                              /* a lane: its hits, and its locks where there is room */
        dpart_t *d = dcur();
        const dpart_t *o = &proj.p[cb.pat].d;
        if (d != o || pad != cb.pad) {
            uint32_t h[STEP_WORDS];
            for (i = 0; i < STEP_WORDS; i++)
                h[i] = o->hit[cb.pad][i];
            for (s = 0; s < NSTEPS; s++)
                lock_clear_step(d, s, pad);
            for (i = 0; i < STEP_WORDS; i++)
                d->hit[pad][i] = h[i];
            for (i = 0, n = o->nlk; i < n && i < o->nlk; i++)
                if (o->lk[i].pad == cb.pad && (d != o || 1))
                    full |= lock_put(d, o->lk[i].step, pad, o->lk[i].param, o->lk[i].val);
        }
    }
    plat_unlock(f);
    dirty = 1;
    ui.redraw = 1;
    app_say("PASTED", full ? "SOME LOCKS DID NOT FIT" : kind == CB_STEP ? "STEP" : kind == CB_LANE ? "LANE" : "PART");
}

static int cb_button(int save, uint32_t keys)   /* SAVE (copy) or REC (paste) pressed: 1 taken as a copy / paste */
{
    const int lane = cb_lane_key(keys);
    int kind;
    if (ui.held_step >= 0 && ui.page == PG_SEQ)
        kind = CB_STEP;
    else if (lane >= 0)
        kind = CB_LANE;
    else if ((ui.btn >> B_SEQ & 1u) && (ui.page == PG_SEQ || ui.seq_prev == PG_SEQ))
        kind = CB_PART;
    else
        return 0;
    if (kind == CB_PART) {
        ui.seq_used = 1;
        ui.page = ui.seq_prev;
    }
    if (save)
        cb_copy(kind, lane);
    else
        cb_paste(kind, lane);
    return 1;
}

static void held_nudge(int32_t e)         /* a step held + PRESETS: how late it plays, sixteenths of its length */
{
    int tr = ui.trk, s = ui.held_step, i, n, r = 0;
    dpart_t *d = dcur();
    ipart_t *p = tr == TR_DRUM ? 0 : ipart(tr);
    char t[12];
    uint32_t f;
    if (p ? !p->st[s].note[0] : !bit_get(d->hit[ui.pad], s))
        return;
    i = p ? ilock_find(p, s, IL_NUDGE) : lock_find(d, s, ui.pad, LK_NUDGE);
    n = clamp((i >= 0 ? (p ? p->lk[i].val : d->lk[i].val) : 0) + e, 0, 15);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (!n) {
        if (i >= 0 && p)
            p->lk[i] = p->lk[--p->nlk];
        else if (i >= 0)
            lock_del(d, i);
    } else {
        r = p ? ilock_put(p, s, IL_NUDGE, n) : lock_put(d, s, ui.pad, LK_NUDGE, n);
    }
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    t[0] = 0;
    if (n) {
        int_text(n * 100 / 16, t);
        text_cat(t, "% LATE");
    } else {
        text_cat(t, "ON TIME");
    }
    app_say("NUDGE", t);
    ui.redraw = 1;
}

/* STEP LOCKS (0.5, an instrument's fifth step tool): a held step's own LFO 1 RATE and DEPTH and arpeggiator RATE and
 * GATE, each a lock of the part's; turned to the track's own value, the lock goes */
static int xl_base(int tr, int i)
{
    const lfo_t *l = &proj.lfo[tr];
    return i == XL_LRATE ? (l->sync ? l->div : l->rate) : i == XL_LDEPTH ? l->depth : i == XL_ARATE ? tset(tr)->arp_rate
           : tset(tr)->arp_gate;
}
static int xl_value(int tr, int step, int i, char *t)   /* the step's value (its lock, else the track's): 1 locked */
{
    int j = step >= 0 ? ilock_find(ipart(tr), step, (int)IL_X0 + i) : -1, v = j >= 0 ? ipart(tr)->lk[j].val : xl_base(tr, i);
    t[0] = 0;
    if (i == XL_LRATE && proj.lfo[tr].sync)
        text_cat(t, DIV_NAME[v < NDIV ? v : NDIV - 1]);
    else if (i == XL_ARATE)
        text_cat(t, ARP_RATE_NAME[v % NARPRATES]);
    else
        int_text(v, t);
    if (i == XL_AGATE)
        text_cat(t, "%");
    return j >= 0;
}
static void xl_knob(int i, int32_t e)
{
    static const char *const NM[XL_NUM] = {"LFO RATE", "LFO DEPTH", "ARP RATE", "ARP GATE"};
    int tr = ui.trk, s = ui.held_step, j, v, lo, hi, r = 0;
    ipart_t *p = ipart(tr);
    char t[16];
    uint32_t f;
    if (s < 0 || !p->st[s].note[0]) {
        app_say("STEP LOCKS", "HOLD A STEP");
        return;
    }
    lo = i == XL_LDEPTH ? -64 : i == XL_AGATE ? 5 : 0;
    hi = i == XL_LRATE ? (proj.lfo[tr].sync ? NDIV - 1 : 127) : i == XL_LDEPTH ? 63 : i == XL_ARATE ? NARPRATES - 1 : 100;
    j = ilock_find(p, s, (int)IL_X0 + i);
    v = clamp((j >= 0 ? p->lk[j].val : xl_base(tr, i)) + e * (i == XL_AGATE ? 5 : 1), lo, hi);
    if (!ui.held_used)
        undo_mark(0);
    ui.held_used = 1;
    f = plat_lock();
    if (v == xl_base(tr, i)) {
        if (j >= 0)
            p->lk[j] = p->lk[--p->nlk];
    } else {
        r = ilock_put(p, s, (int)IL_X0 + i, v);
    }
    plat_unlock(f);
    if (r) {
        app_say("NO LOCK LEFT", "");
        return;
    }
    if (!xl_value(tr, s, i, t))
        text_cat(t, " (TRACK'S)");
    app_say(NM[i], t);
    ui.redraw = 1;
}

static void dhit_knob(int k, int32_t e)   /* the drums' STEPS, a hit held (or their HIT page): K1 VELOCITY K2 CHANCE
                                           * K3 SUBSTEPS K4 ACCENT (the step's, every pad's) */
{
    dpart_t *p = dcur();
    int s = ui.held_step, i, v;
    char t[8];
    uint32_t f;
    if (s < 0 || !bit_get(p->hit[ui.pad], s)) {
        app_say("THE HIT", "HOLD A STEP");
        return;
    }
    if (k == 1) {
        held_chance(e);
    } else if (k == 2) {
        held_sub(e);
    } else {
        if (!ui.held_used)
            undo_mark(0);
        ui.held_used = 1;
        i = lock_find(p, s, ui.pad, LK_VEL);
        v = clamp((i >= 0 ? p->lk[i].val : bit_get(p->acc, s) ? 127 : (int)(DRUMS_VEL_NORMAL * 127.0f + 0.5f)) + e, 1, 127);
        f = plat_lock();
        if (k == 0)
            i = lock_put(p, s, ui.pad, LK_VEL, v);
        else if (e > 0)
            bit_set(p->acc, s);
        else
            bit_clear(p->acc, s);
        plat_unlock(f);
        if (k == 0 && i) {
            app_say("NO LOCK LEFT", "");
            return;
        }
        int_text(v, t);
        app_say(k ? "ACCENT" : "VELOCITY", k ? (e > 0 ? "ON" : "OFF") : t);
    }
    ui.redraw = 1;
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
    app_say(trial_pad_name(drum_model(pad), pad), muted ? "MUTED" : "ON");
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
        nstep_add(s, note[i], ui.st_vel ? ui.st_vel : 100);
    if (ui.st_gate && s->note[0])
        s->gate = ui.st_gate;
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
static nstep_t gen_buf[2][NSTEPS];       /* (the generators' scratch, defined below: the main loop's, one user at a time) */
#define q_old gen_buf[1]

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
    int cn[4], nc, i;
    note = KEY_NOTE0 + key + 12 * ts->oct;
    vel = touch_vel((uint32_t)key);
    if (!vel)
        vel = 100;
    ui.key_chord[key] = (uint8_t)(ts->chord | ts->chord_inv << 4);
    nc = chord_of(ui.key_chord[key], note, cn);
    f = plat_lock();
    if (!track_is_muted(tr)) {
#if FM1B_USB_HOST
        mh_panel_keys |= 1u << key;
        for (i = 0; i < nc; i++)
            mh_note(tr, (unsigned)cn[i], (unsigned)vel, MH_PANEL, 1);
#else
        for (i = 0; i < nc; i++) {
            if (ts->arp_mode)
                arp_key(tr, cn[i], vel, 1);
            else
                syn_note_on(tr, cn[i], (float)vel / 127.0f, -1, ts->type == TY_MONO);
        }
        if (!ts->arp_mode)
            lfo_note(tr);
#endif
    }
    plat_unlock(f);
    for (i = 0; i < nc; i++)
        plat_midi_send(0x09u | (0x90u | (uint32_t)tr) << 8 | (uint32_t)cn[i] << 16 | (uint32_t)vel << 24, MIDI_TO);
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
#if FM1B_USB_HOST
    if (mh_panel_keys & (1u << key)) {
        mh_panel_keys &= ~(1u << key);
        for (i = 0; i < nc; i++)
            mh_note(tr, (unsigned)cn[i], 0, MH_PANEL, 0);
    }
#else
    for (i = 0; i < nc; i++) {
        if (tset(tr)->arp_mode)
            arp_key(tr, cn[i], 0, 0);
        else
            syn_note_off(tr, cn[i]);
    }
#endif
    plat_unlock(f);
    for (i = 0; i < nc; i++)
        plat_midi_send(0x08u | (0x80u | (uint32_t)tr) << 8 | (uint32_t)cn[i] << 16, MIDI_TO);
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
    ui.hnote = 0;
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

static int hnote_at(const nstep_t *s)     /* the held step's note picked (ui.hnote): where in the step; -1: all */
{
    int n = nstep_count(s), i, j, r;
    if (!ui.hnote || ui.hnote > n)
        return -1;
    for (i = 0; i < n; i++) {             /* (its rank from the lowest) */
        for (r = 1, j = 0; j < n; j++)
            r += s->note[j] < s->note[i];
        if (r == ui.hnote)
            return i;
    }
    return -1;
}

static void hnote_move(nstep_t *s, int at, int e)   /* that one note e semitones (past the step's others), still picked */
{
    int n = nstep_count(s), v = s->note[at], d = e > 0 ? 1 : -1, j, r;
    while (e) {
        int w = v + d;
        for (j = 0; j < n; j++)
            if (j != at && s->note[j] == w)
                w += d, j = -1;
        if (w < 1 || w > 127)
            break;
        v = w;
        e -= d;
    }
    s->note[at] = (uint8_t)v;
    for (r = 1, j = 0; j < n; j++)
        r += s->note[j] < v;
    ui.hnote = (uint8_t)r;
}

static void hnote_pick(int32_t e)         /* a step held + SELECT: which of its notes the knobs move (ALL, or one) */
{
    const nstep_t *s = &ipart(ui.trk)->st[ui.held_step];
    char t[8];
    int at;
    if (!s->note[0])
        return;
    ui.hnote = (uint8_t)clamp(ui.hnote + e, 0, nstep_count(s));
    ui.held_used = 1;                     /* (looked at: not cleared at the release) */
    at = hnote_at(s);
    if (at >= 0)
        note_text(s->note[at], t);
    app_say("EDIT NOTE", at < 0 ? "ALL" : t);
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
    if (k == 0 && hnote_at(s) >= 0) {     /* the one note picked */
        hnote_move(s, hnote_at(s), e);
    } else if (k == 0) {
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
        i = hnote_at(s);
        ui.last_note[tr] = s->note[0];
        note_text(s->note[i < 0 ? 0 : i], t);
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

static int inst_epages(int tr) { (void)tr; return NEPAGE; }
static int epage_of(int tr) { return ui.epage < inst_epages(tr) ? ui.epage : 0; }
static const int8_t *epage_p(int tr) { return EPAGE_P[epage_of(tr)]; }
static const char *epage_name(int tr) { return EPAGE_NAME[epage_of(tr)]; }

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
static int dpreset_ok(int display)       /* in PRESETS' list now: the genre kits with the DrumEngine, else the stock
                                           * kits and the user kits saved */
{
    int i = dpreset_id(display);
    if (V3ON)
        return dpreset_genre(i) >= 0;
    return dpreset_factory(i) >= 0 || (dpreset_user(i) && user.kit[i - 2].used && drums_kit_present(user.kit[i - 2].kit));
}
static void dpreset_name(int i, char *t)
{
    char u[4];
    t[0] = 0;
    if (dpreset_genre(i) >= 0) {
        const char *n = kit_genre_name(dpreset_genre(i));
        int k;
        for (k = 0; n[k] && k < 15; k++)
            t[k] = (char)(n[k] >= 'a' && n[k] <= 'z' ? n[k] - 32 : n[k]);
        t[k] = 0;
        return;
    }
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

static void preset_load(int tr, int i, const char *bank)  /* one selected sound, not a search over intermediate presets */
{
    int k, mono;
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
    syn_sound(tr, ts->snd);
    plat_unlock(f);
    dirty = 1;
    preset_name(i, t);
    if (bank)
        app_say(bank, t);
    else
        app_say(t, TYPE_NAME[ts->type]);
}

static void preset_turn(int32_t e)
{
    int tr = ui.trk, old = tset(tr)->preset, i = preset_step(old, e, npresets(), preset_ok);
    /* Turning beyond the end leaves the current patch in place. */
    if (i == old && i >= PRESET_EXTRA_FIRST)
        return;
    preset_load(tr, i, 0);
}

static void preset_bank_turn(int32_t e)
{
    static const uint16_t START[] = {0, PRESET_USER_FIRST, PRESET_NEW_FIRST, PRESET_EXTRA_FIRST,
        PRESET_EXTRA_FIRST + SYN_BANK_SIZE, PRESET_EXTRA_FIRST + 2 * SYN_BANK_SIZE,
        PRESET_EXTRA_FIRST + 3 * SYN_BANK_SIZE, PRESET_LIMIT};
    static const char *const NAME[] = {"ORIGINAL", "USER SOUNDS", "MOTOR CITY", "AFTER HOURS",
        "MACHINE FUNK", "WAREHOUSE", "DUB / BROKEN"};
    int tr = ui.trk, p = tset(tr)->preset, n = (int)(sizeof NAME / sizeof NAME[0]);
    int bank = 0, start, direction = e < 0 ? -1 : 1;
    unsigned jumps = e < 0 ? 0u - (uint32_t)e : (uint32_t)e;
    while (bank < n - 1 && p >= START[bank + 1])
        bank++;
    start = bank;
    while (jumps--) {
        int next = bank + direction;
        if (next == 1) {              /* no empty User bank on the panel */
            int i = PRESET_USER_FIRST;
            while (i < PRESET_NEW_FIRST && !preset_ok(i)) i++;
            if (i == PRESET_NEW_FIRST) next += direction;
        }
        if (next < 0 || next >= n) break;
        bank = next;
    }
    if (bank != start) {
        int i = START[bank];
        while (i < START[bank + 1] && !preset_ok(i)) i++;
        preset_load(tr, i, NAME[bank]);
    } else {
        char t[16];
        preset_name(p, t);
        app_say(NAME[bank], t);
    }
}

static void kit_turn(int32_t e)
{
    const int from = V3ON ? DPRESET_GENRE + dpreset_genre_of(proj.dpreset)   /* (a stock id: its genre's place) */
                   : dpreset_genre(proj.dpreset) >= 0 ? GENRE_STOCK[dpreset_genre(proj.dpreset)]   /* (its stock kit's) */
                   : dpreset_display(proj.dpreset);
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
    if (!inst && V3ON) {               /* a user kit keeps the stock engine's knobs: nothing of the DrumEngine's yet */
        app_say("DRUM KITS", "NOT SAVED YET");
        return;
    }
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
            const uint8_t *pot = kit_pots(drum_model(r))[r];
            for (c = 0; c < NPOTS; c++)
                u->pots[r][c] = pot[c];
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
            plat_unlock(f);
            dirty = 1;
        }
        app_say("TYPE", TYPE_NAME[ts->type]);
    } else {                              /* K_LEVEL */
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

/* the drums' LFO: with the DrumEngine it moves one of the pad's own controls (its rsv[0] LFO_V3), with the stock
 * 808 / 909 one of the kit's knobs (the model + 1); one set up for the other engine does nothing until it's back */
static int lfo_drum_tag(int pad) { return V3ON ? LFO_V3 : drum_model(pad) + 1; }
static int lfo_drum_live(const lfo_t *l)
{
    if (l->dest == LFO_OFF || l->pad >= NPADS)
        return 0;
    if (l->rsv[0] == LFO_V3)
        return V3ON && kit_param_spec(l->pad + 1, l->dest) && !kit_param_option(l->pad + 1, l->dest, 0);
    return !V3ON && (!l->rsv[0] || l->rsv[0] == drum_model(l->pad) + 1) && drums_param(drum_model(l->pad), l->pad, l->dest);
}
static int lfo_drum_knobs(int pad, int *idx)   /* the knobs the drums' LFO can move on a pad (not switches or sends) */
{
    int i, n = 0, np = drums_nparams(drum_model(pad), pad);
    if (V3ON) {                        /* the DrumEngine: the drum's controls, its choices (MODEL ...) left out */
        np = kit_param_count(pad + 1);
        for (i = 0; i < np && i < NPOTS; i++)
            if (!kit_param_option(pad + 1, i, 0))
                idx[n++] = i;
        return n;
    }
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
        } else if (!lfo_drum_live(l)) {
            text_cat(t, "OFF");
        } else {
            text_cat(t, trial_pad_name(drum_model(l->pad), l->pad));
            text_cat(t, " ");
            text_cat(t, l->rsv[0] == LFO_V3 ? kit_param_spec(l->pad + 1, l->dest)->label
                                            : drums_param(drum_model(l->pad), l->pad, l->dest)->name);
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
        default: *max = tr == TR_DRUM ? 1 : SM_NUM - 1; return tr == TR_DRUM ? lfo_drum_live(l) : l->dest;
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
                if (lfo_drum_live(l) && l->pad == ui.pad)
                    for (i = 0; i < n; i++)
                        if (idx[i] == l->dest)
                            pos = i;
                pos = clamp(pos + e, -1, n - 1);
                l->pad = ui.pad;
                l->rsv[0] = (uint8_t)lfo_drum_tag(ui.pad);
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
#define NMIXPAGE 5                        /* the mixer's pages: the tracks' LEVEL, PAN, REVERB, DELAY; the RETURNS */
static const char *const MIX_NAME[NMIXPAGE] = {"LEVEL", "PAN", "REVERB", "DELAY", "RETURNS"};
/* RETURNS (0.5, for dub): the shared FX's own knobs, the same values as the FX pages', in pairs: K1 BongVerb's return
 * and K2 its decay; K3 BongDelay's return and K4 its feedback (the returns to +15.5 dB over the preset's: dsp/fx.c
 * LVL_MAX) */
#define MIX_RET 4
static const int8_t RET_P[4] = {FXP_RV_LEVEL, FXP_RV_DECAY, FXP_DL_LEVEL, FXP_DL_FDBK};
static const char *const RET_LABEL[4] = {"VERB RTN", "VERB DECAY", "DELAY RTN", "FEEDBACK"};
#define NFXPAGE 5                         /* (0.5) each effect two pages of its own (FX MODE's two knobs: on them) */
#define FXPG_VERB 0
#define FXPG_DELAY 2
#define FXPG_FILTER 4
static const char *const FX_PAGE_NAME[NFXPAGE] = {"BongVerb", "BongVerb 2", "BongDelay", "BongDelay 2", "DJ FILTER"};
static const int8_t FX_PAGE_P[NFXPAGE][4] = {
    {FXP_RV_DECAY, FXP_RV_DAMP, FXP_RV_PREDELAY, FXP_RV_LEVEL},
    {FXP_RV_SIZE, FXP_RV_MOTION, FXP_RV_LOWCUT, FXP_RV_MODE},
    {FXP_DL_TIME, FXP_DL_FDBK, FXP_DL_TONE, FXP_DL_LEVEL},
    {FXP_DL_WIDTH, FXP_DL_WOBBLE, FXP_DL_LOWCUT, FXP_DL_DRIVE},
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
static const char *const FX_LABEL[FXP_ALL] = {"DECAY", "DAMP", "PRE-DELAY", "LEVEL", "TIME", "REGEN", "TONE", "LEVEL",
                                              "VERB MODE", "DRIVE", "-", "-", "THRESHOLD", "RATIO", "ATTACK",
                                              "RELEASE", "MAKEUP", "MIX", "PUMP", "FILTER", "RESO", "LIMIT",
                                              "LOW", "MID", "HIGH", "LOW CUT", "LOW FREQ", "MID FREQ", "MID Q",
                                              "HIGH FREQ", "DRIVE", "WARMTH", "TONE", "MIX", "WIDTH", "BASS MONO",
                                              "BALANCE", "GAIN", "CEILING", "RELEASE", "PRESET", "BYPASS",
                                              "WIDTH", "WOBBLE", "LOW CUT", "SIZE", "MOTION", "LOW CUT"};
_Static_assert(sizeof FX_LABEL / sizeof FX_LABEL[0] == FXP_ALL, "a label for each FX knob");

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

static void mix_turn(int t, int page, int32_t e)   /* track t's mixer value page (0 LEVEL 1 PAN 2 REVERB 3 DELAY) */
{
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

static void fx_knob_i(int i, int32_t e);

static void mix_knob(int k, int32_t e)    /* the mixer: K1..K4 are the drums, T2, T3 and T4 (RETURNS: the FX's) */
{
    if (ui.mixpage == MIX_RET)
        fx_knob_i(RET_P[k], e);
    else
        mix_turn(k, ui.mixpage, e);
}

/* ---- the QUICK MIXER (HOME, then HOME again): the track picked (a track key: one press), its K1 LEVEL, K2 REVERB,
 * K3 DELAY, K4 PAN (the mixer's own values: mix_turn); the perform keys (black 5-11) as on PATTERNS. Its white keys:
 * on the drums the lanes' mutes (their audio: drum_hits_off), lit while the lane plays; on T2-T4 all sixteen are moves
 * held on that track: 1-4 its filter and sends (syn_perform), 5-9 ROLL at a rate, 10-12 STUTTER (a slice of its own
 * steps round and round: seq_inst), 13-16 its octave (syn_perform). Never saved, no notes of their own, no automation;
 * in a group (the filter, ROLL, STUTTER, OCT) the last held wins and letting it go falls back to the one held before;
 * let go on leaving the page, another track or a stop (qm_clear), so nothing sticks or moves to another track */
enum { QM_CLOSE, QM_OPEN, QM_SPLASH, QM_THROW, QM_ROLL, QM_STUT = QM_ROLL + 5, QM_OCT = QM_STUT + 3,
       QM_MOVES = QM_OCT + 4 };
static const char *const QM_NAME[QM_MOVES] = {"FILTER CLOSE", "FILTER OPEN", "VERB SPLASH", "DELAY THROW",
    "ROLL 1/8", "ROLL 1/16", "ROLL 1/32", "ROLL 1/16T", "ROLL 1/32T", "STUTTER 1/8", "STUTTER 1/4", "STUTTER 1/2",
    "OCT -2", "OCT -1", "OCT +1", "OCT +2"};
static const char *const QM_SHORT[QM_MOVES] = {"CLOSE", "OPEN", "SPLASH", "THROW", "R 1/8", "R 1/16", "R 1/32",
    "R 1/16T", "R 1/32T", "ST 1/8", "ST 1/4", "ST 1/2", "OCT -2", "OCT -1", "OCT +1", "OCT +2"};
static const uint8_t QM_STUT_N[3] = {2, 4, 8};    /* STUTTER's slices, in steps (1/8, 1/4, 1/2 at 1/16 a step) */
static const int8_t QM_OCT_N[4] = {-2, -1, 1, 2};
static const uint8_t QM_PAGE[4] = {0, 2, 3, 1};   /* K1..K4: the mixer's LEVEL, REVERB, DELAY, PAN */
static const char *const QM_KNOB[4] = {"LEVEL", "REVERB", "DELAY", "PAN"};
#define QM_CUT 48                         /* FILTER CLOSE / OPEN: cutoff steps down / up */
static struct {
    uint16_t held;                        /* the moves' keys down (a bit each) */
    uint16_t clock, order[QM_MOVES];      /* when each was pressed: in a group the latest held wins */
    int8_t trk;                           /* the track they're on (-1: none) */
} qm = {0, 0, {0}, -1};

static int qm_pick(int lo, int n)         /* a group's move in force: the latest pressed of those held; -1 none */
{
    int k, best = -1;
    for (k = lo; k < lo + n; k++)
        if ((qm.held >> k & 1u) && (best < 0 || (uint16_t)(qm.order[k] - qm.order[best]) < 0x8000u))
            best = k;
    return best;
}

static void qm_apply(void)                /* the held moves -> the synth (ramped there) and the sequencer */
{
    int f, r, st, o;
    uint32_t fl;
    if (qm.trk < 1)
        return;
    f = qm_pick(QM_CLOSE, 2);
    r = qm_pick(QM_ROLL, 5);
    st = qm_pick(QM_STUT, 3);
    o = qm_pick(QM_OCT, 4);
    fl = plat_lock();
    syn_perform(qm.trk, f < 0 ? 0 : f == QM_CLOSE ? -QM_CUT : QM_CUT, (qm.held >> QM_SPLASH & 1u) ? 1.0f : 0.0f,
                (qm.held >> QM_THROW & 1u) ? 1.0f : 0.0f, o < 0 ? 0 : QM_OCT_N[o - QM_OCT]);
    perf.qtrk = qm.trk;
    perf.qroll = (uint8_t)(r < 0 ? 0 : r - QM_ROLL + 1);
    perf.qstut = (uint8_t)(st < 0 ? 0 : QM_STUT_N[st - QM_STUT]);
    plat_unlock(fl);
    ui.redraw = 1;
}

static void qm_clear(void)                /* every held move let go (they ramp back to the track's own) */
{
    if (qm.held && qm.trk >= 1) {
        qm.held = 0;
        qm_apply();
    }
    qm.held = 0;
    qm.trk = -1;
}

static void qm_move(int m, int down)      /* a white key on the QUICK MIXER, T2-T4: its move held or let go */
{
    if (down) {
        if (ui.trk == TR_DRUM)
            return;
        if (qm.trk != ui.trk)
            qm_clear();
        qm.trk = (int8_t)ui.trk;
        qm.held |= (uint16_t)(1u << m);
        qm.order[m] = ++qm.clock;
        app_say(QM_NAME[m], "");
    } else {
        if (!(qm.held >> m & 1u))         /* (let go already: the page left, another track, a stop) */
            return;
        qm.held &= (uint16_t)~(1u << m);  /* (another of its group still down: that one again) */
    }
    qm_apply();
}

static void qm_knob(int k, int32_t e)
{
    mix_turn(ui.trk, QM_PAGE[k], e);
}

static void fx_text(int i, char *s)
{
    const x0x_param_t *p = fx_param(i);
    int v = *fxv(i);
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
    *fxv(i) = (uint8_t)clamp(*fxv(i) + e, 0, p->max);
    f = plat_lock();
    fx_set(i, *fxv(i));
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
static int page_fits(int pg);
static int npages(void);
#ifdef PLAT_HOST
static uint8_t old_pick;                  /* the simulator's older suites (oldpick1): a track change as it was before
                                           * 0.5, between the drums and an instrument to the track's own page */
static uint8_t old_seq;                   /* (oldseq1): SEQ on the drums' steps stays there, as before BEAT */
#else
#define old_pick 0
#define old_seq 0
#endif
static void track_pick(int tr)
{
    tr = clamp(tr, 0, NTRK - 1);
    if (tr != ui.trk) {
        const int cross = (tr == TR_DRUM) != (ui.trk == TR_DRUM);
        qm_clear();                       /* (the QUICK MIXER's held moves: not carried to another track) */
        if (ui.page == PG_PICK)
            pick_leave();
        keys_let_go();
        ui.trk = (uint8_t)tr;
        sel_trk = (uint8_t)tr;
        /* (0.5) the page stays wherever the track picked has it too (STEPS, EDIT, LFO, the mixers ...), with its step
         * bank and, between instrument tracks, its editor's page: the same thing on the next track. Only a page the
         * track hasn't (KEYS on the drums, MUTE on a synth ...) goes to the track's own */
        if (!page_fits(ui.page) || (old_pick && cross && ui.page != PG_EDIT && ui.page != PG_HOME &&
                                    ui.page != PG_QMIX && ui.page != PG_LFO && ui.page != PG_MIX && ui.page != PG_FX &&
                                    ui.page != PG_SONG && ui.page != PG_MST && ui.page != PG_SETUP))
            ui.page = (uint8_t)home_page(tr);
        ui.held_step = -1;
        if (ui.spage >= npages() || old_pick)
            ui.spage = 0;
        if (cross || old_pick)
            ui.epage = 0;
    }
    app_say(TRK_NAME[tr], tr == TR_DRUM ? drum_kit_title() : TYPE_NAME[tset(tr)->type]);
}

static int edit_pat(void) { return clip_of(ui.trk); }

static void pf_press(int i)               /* a perform key down (HOME): its effect while held */
{
    uint32_t f;
    f = plat_lock();
    if (i == PF_STUT || i == PF_TAPE)     /* the delay's memory, lent */
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

/* ---- Bluetooth audio (0.5; SETUP's third view): the FM-1's sound to Bluetooth headphones or a speaker (A2DP, on
 * fm1-base's own stack). Its buffers (17.7 KB) are lent from the undo history while it is on: undo keeps one step then,
 * all six again once it is off and the radio has let the memory go. 150-300 ms late at the ear: for listening */
static struct {
    uint8_t on, giving, cur, seen;        /* on; off, the memory not back yet; the list's cursor; what the page drew */
} gbta;
#define BTA_NAME 18
static int gbta_entry(int i, char *name, int *known)   /* the list: the remembered devices, then the ones found */
{
    int n = 0, j;
    for (j = 0; j < 4 && plat_bta_known(j, name); j++, n++)
        if (n == i) {
            *known = j;
            return 1;
        }
    for (j = 0; j < 8 && plat_bta_found(j, name); j++, n++)
        if (n == i) {
            *known = -1 - j;
            return 1;
        }
    return 0;
}
static int gbta_count(void)
{
    char nm[BTA_NAME];
    int n = 0, k;
    while (n < 12 && gbta_entry(n, nm, &k))
        n++;
    return n;
}

static void gbta_set(int on)
{
    char nm[BTA_NAME];
    if (!FM1B_BT_AUDIO || on == gbta.on)
        return;
    if (on) {
        const uint32_t need = plat_bta_mem_bytes();
        if (gbta.giving || need > (NUNDO - 1) * sizeof(snap_t)) {
            app_say("BT AUDIO", gbta.giving ? "WAIT A MOMENT" : "NO MEMORY");
            return;
        }
        if (need) {                       /* the undo history's other steps: lent */
            undo_n = redo_n = 0;
            undo_kind = 0;
            nundo = 1;
            plat_bta_mem(&history[1]);
        }
        plat_bta_on(1);
        gbta.on = 1;
        if (plat_bta_known(0, nm))        /* the last device, if there is one; else a look around */
            plat_bta_reconnect(0);
        else
            plat_bta_scan(1);
    } else {
        plat_bta_hangup();
        plat_bta_on(0);
        gbta.on = 0;
        gbta.giving = 1;                   /* (the main loop takes the memory back once the radio has let go) */
    }
    app_say("BT AUDIO", on ? "ON" : "OFF");
}

static void gbta_poll(void)                /* the main loop */
{
    if (!FM1B_BT_AUDIO)
        return;
    if (gbta.giving && plat_bta_mem(0) == 0) {
        gbta.giving = 0;
        undo_n = redo_n = 0;
        nundo = NUNDO;
    }
    if (ui.page == PG_SETUP && ui.setup_view == 2) {
        const uint8_t now = (uint8_t)(plat_bta_state() * 16 + gbta_count());
        if (now != gbta.seen)
            ui.redraw = 1;
    }
}

static void gbta_knob(int k, int32_t e)    /* K1 AUDIO, K2 DEVICE, K3 CONNECT (down: hang up), K4 SCAN (down: forget) */
{
    char nm[BTA_NAME];
    int kn = 0, n = gbta_count();
    if (!FM1B_BT_AUDIO || !e)
        return;
    if (k == 0) {
        gbta_set(e > 0);
        return;
    }
    if (!gbta.on) {
        app_say("BT AUDIO", "TURN IT ON: K1");
        return;
    }
    if (k == 1) {
        gbta.cur = (uint8_t)clamp(gbta.cur + e, 0, n ? n - 1 : 0);
    } else if (k == 2) {
        if (e < 0) {
            plat_bta_hangup();
            app_say("BT AUDIO", "HUNG UP");
        } else if (gbta_entry(gbta.cur, nm, &kn)) {
            if (kn >= 0)
                plat_bta_reconnect(kn);
            else
                plat_bta_pick(-1 - kn);
            app_say("CONNECT", nm);
        }
    } else if (e > 0) {
        plat_bta_scan(1);
        gbta.cur = 0;
        app_say("BT AUDIO", "LOOKING");
    } else if (gbta_entry(gbta.cur, nm, &kn) && kn >= 0) {
        plat_bta_forget(kn);
        gbta.cur = 0;
        app_say("FORGOT", nm);
    }
}

static int ble_wanted(void) { return (int)(proj.mflags >> 1 & 1u); }
int app_ble_wanted(void) { return ble_wanted(); }   /* (app.h APP_BLE_AT_BOOT: Bluetooth at power-on, as the project says) */

static void setup_knob(int k, int32_t e)  /* SETUP: K1 MIDI CLOCK, K2 SOUNDS, K3 USB PORT, K4 MIX */
{
    static const char *const CL[3] = {"OFF", "SEND", "FOLLOW"};
    uint32_t f;
    int v;
    if (ui.setup_view == 1) {             /* the MIDI view: K1 SEQ OUT, K2 BLUETOOTH */
        if (k == 0) {
            v = clamp(seq_out() + e, 0, 1);
            f = plat_lock();
            if (!v)
                mo_flush();
            proj.mflags = (uint8_t)((proj.mflags & ~1u) | (uint32_t)v);
            plat_unlock(f);
            app_say("SEQ OUT", v ? "ON" : "OFF");
        } else if (k == 1 && FM1B_BLE) {
            v = clamp(ble_wanted() + e, 0, 1);
            if (v == ble_wanted())
                return;
            proj.mflags = (uint8_t)((proj.mflags & ~2u) | (uint32_t)v << 1);
            plat_ble_enable(v);
            app_say("BLUETOOTH", v ? "ON" : "OFF");
#if FM1B_BLE_CENTRAL
        } else if (k == 2 && e) {         /* K3 DEVICE: which of the keyboards found */
            char nm[24];
            if (!ble_wanted()) {
                app_say("BLUETOOTH", "TURN IT ON: K2");
                return;
            }
            btdev_choose(btdev_pick() + e);
            app_say("DEVICE", btdev_found(btdev_pick(), nm, 0) ? nm : "NONE FOUND");
            return;
        } else if (k == 3 && e) {         /* K4 CONNECT: up connects the one picked, down lets it go */
            if (!ble_wanted() || (e > 0) == (btdev_busy() != 0))
                return;
            v = btdev_save();
            app_say("DEVICE", v == BTDEV_CONNECTING ? btdev_name() : btdev_word(v));
            return;
#endif
        } else {
            return;
        }
        dirty = 1;
        return;
    }
    if (ui.setup_view == 2) {
        gbta_knob(k, e);
        return;
    }
    if (ui.setup_view)
        return;
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
#if FM1B_USB_HOST
    } else if (k == 2) {
        if (!e || sq.playing || ui.rec)
            return;                     /* no role switch during transport or a take */
        usb_host_selected = (uint8_t)(e > 0);
        plat_usb_host(usb_host_selected);
        app_say("USB PORT", usb_host_selected ? "MIDI HOST" : "COMPUTER");
        return;                         /* role is session-only, not a project setting */
#endif
    } else if (k == 3) {
        v = clamp(mix_pp() + e, 0, 1);
        f = plat_lock();
        if (v && !mix_pp()) {             /* on: each track's strip now is its clip's, the FX the pattern's */
            proj.recq |= 0x80u;
            fx_pat = proj.pat;
            pmix_store_all();
        } else if (!v) {
            proj.recq &= 0x7Fu;
        }
        plat_unlock(f);
        app_say("MIX", v ? "PER PATTERN" : "SHARED");
    } else {
        return;
    }
    dirty = 1;
}

static void reload_sound(void)            /* SAVE + OCT-: the track's sound (the drums: their knobs) as last saved */
{
    int tr = ui.trk, fmt = proj_file_format();
    uint32_t f;
    if (!fmt) {
        app_say("RELOAD", "NOTHING SAVED");
        return;
    }
    if (tr == TR_DRUM) {
        static uint8_t pots[2][NPADS + 1][NPOTS];
        uint8_t saved_map[NPADS] = {0}, saved_kit, saved_preset;
        const uint32_t offmap = PROJ_PREFIX + (uint32_t)offsetof(project_t, model) - PROJ_TAIL;
        if (plat_file_read(&PROJ_FILE, offsetof(project_t, kit), &saved_kit, 1) ||
            plat_file_read(&PROJ_FILE, offsetof(project_t, dpreset), &saved_preset, 1) || saved_kit >= KIT_NUM)
            return;
        saved_kit = (uint8_t)kit_now(saved_kit);   /* (saved on a cut kit: its heir) */
        if (fmt >= 7 && plat_file_read(&PROJ_FILE, offmap, saved_map, sizeof saved_map))
            return;
        if (plat_file_read(&PROJ_FILE, PROJ_PREFIX, pots, sizeof pots) != 0)
            return;
        f = plat_lock();
        locks_release();
        {
            int k, r, c;
            for (k = 0; k < 2; k++)
                for (r = 0; r <= NPADS; r++)
                    for (c = 0; c < NPOTS; c++)
                        kit_pots(k)[r][c] = pots[k][r][c];
        }
        if (fmt >= 7) {
            drum_map_apply(saved_kit, saved_map);
            proj.dpreset = !dpreset_valid(saved_preset) ? (uint8_t)kit_factory(saved_kit) : saved_preset;
            trial_kit(dpreset_genre_of(proj.dpreset));
            if (fmt >= 10) {              /* and the drums' edits saved on it */
                static struct { KitPatch p; uint16_t size; uint8_t kit, rsv; } sv;
                if (!plat_file_read(&PROJ_FILE, PROJ_PREFIX + (uint32_t)offsetof(project_t, dpatch) - PROJ_TAIL, &sv,
                                    sizeof sv) && sv.size == sizeof(KitPatch) && sv.kit == proj.dpreset)
                    trial_restore(&sv.p);
            }
        }
        plat_unlock(f);
        pots_to_engines();                /* (a pad at a time under the lock) */
    } else {
        static track_t t;
        static psound_t q;
        track_t *ts = tset(tr);
        int k;
        if (plat_file_read(&PROJ_FILE, (uint32_t)((uint8_t *)&proj.tr[tr - 1] - (uint8_t *)&proj), &t, sizeof t) != 0)
            return;
        q.used = 0;
        if (sounds_pp())
            plat_file_read(&PROJ_FILE, PROJ_PREFIX + (uint32_t)((uint8_t *)&proj.psnd[clip_of(tr)][tr - 1] - (uint8_t *)proj.pots), &q,
                           sizeof q);
        f = plat_lock();
        for (k = 0; k < SP_NUM; k++)
            ts->snd[k] = q.used ? q.snd[k] : t.snd[k];
        ts->type = q.used ? q.type : t.type;
        ts->level = q.used ? q.level : t.level;
        ts->preset = q.used ? q.preset : t.preset;
        ts->edited = q.used ? q.edited : t.edited;
        syn_sound(tr, ts->snd);
        syn_level(tr, (float)ts->level / 127.0f);
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
        DRUM_COLOR_RESET();
    }
    for (t = 1; t < NTRK; t++)
        if (cut >> t & 1u) {
            arp_clear(t);
            sq.slid[t] = 0;
            syn_cut_track(t);
        }
    if ((cut & 0x0Eu) && syn_at < SYN_CTL)
        PLAT_FAR(syn_mix_repack)(syn_l, syn_r, syn_rv, syn_dl);
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
    proj.dgen[n] = proj.dgen[proj.pat];
    memcpy(proj.gkeep[n], proj.gkeep[proj.pat], sizeof proj.gkeep[n]);
    for (i = 0; i < NTRK; i++) {          /* its clips' LFOs with it (a track on the pattern now: the one it plays) */
        proj.plfo[n][i] = clip_of(i) == proj.pat ? proj.lfo[i] : proj.plfo[proj.pat][i];
        if (clip_of(i) == n)
            proj.lfo[i] = proj.plfo[n][i];
    }
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

/* The editors' buttons (EDIT, ENV, LFO, ARP, FX, GLO; 0.5): a button opens its editor on the page it was left on and
 * keeps the screen it was opened from; pressed again, back to that screen. SELECT turns an editor's pages. (SEQ and
 * HOME go round their own screens as before.) */
enum { ED_EDIT, ED_ENV, ED_LFO, ED_ARP, ED_FX, ED_GLO, NED };
static uint8_t ed_from[NED] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t glo_last = PG_MIX;         /* GLO's page left last: the mixer or the master bus */

static int page_fits(int pg)              /* a page the track picked can show (and come back to) */
{
    const int inst = ui.trk != TR_DRUM;
    switch (pg) {
    case PG_KEYS: case PG_ENV: case PG_ARP: case PG_COLOR: return inst;
    case PG_MUTE: case PG_KIT: case PG_V3_BUS: return !inst;
    case PG_PICK: return 0;               /* (a chord being picked: not from a button) */
    default: return pg <= PG_QMIX;
    }
}

static void ed_open(int ed, int pg)       /* an editor's button: its page (as it was left), from the screen on now */
{
    if (ui.page != pg)
        ed_from[ed] = ui.page;
    ui.page = (uint8_t)pg;
    ui.redraw = 1;
}

static void ed_back(int ed)               /* pressed again: back to the screen it was opened from */
{
    const int pg = ed_from[ed];
    ui.page = (uint8_t)(pg != ui.page && page_fits(pg) ? pg : home_page(ui.trk));
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

/* KEEP (0.5, the ACID pages' fourth): a step marked kept is left as it is by every new line, MUTATE and knob */
static uint8_t *gkeep_of(int tr) { return proj.gkeep[clip_of(tr)][tr - 1]; }
static int gkept(int tr, int s) { return gkeep_of(tr)[s >> 3] >> (s & 7) & 1; }

static void gen_write(int tr)             /* the part's steps from its settings (its steps past its length untouched,
                                           * and the ones kept) */
{
    ipart_t *p = ipart(tr);
    int s, len = p->len;
    uint32_t f;
    gen_make(gen_of(tr), len, gen_buf[0]);
    f = plat_lock();
    for (s = 0; s < len; s++)
        if (!gkept(tr, s))
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

/* OCT+'s dice (0.5): a seed drawn at random from all 65535, never the one playing. The moment of the press, in samples
 * rendered (render_t: a human's timing lands anywhere in 44100 a second), stirred into a state kept from press to press;
 * the simulator's scripts run the same each time, so its tests stay repeatable. The line is still its seed's: shown,
 * saved, undone, and the SEED knob steps on from it in order */
static uint32_t gen_rng = 0x2545F491u;

static uint16_t gen_random_seed(uint16_t now)
{
    uint32_t s;
    gen_rng = gen_hash(gen_rng ^ render_t, plat_ms());
    s = gen_rng % 65535u + 1u;
    if (s == now)
        s = s % 65535u + 1u;
    return (uint16_t)s;
}

static void gen_new(void)                 /* OCT+: a random seed's line */
{
    int tr = ui.trk;
    gen_t *g;
    char t[16];
    undo_mark(0);
    g = gen_begin(tr);
    g->seed = gen_random_seed(g->seed);
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
static const char *const GEN_LABEL[16] = {"DENSITY", "WHOLE", "SLIDE", "ACCENT", "ROOT", "SCALE", "NOTES", "OCTAVE",
                                          "SEED", "LENGTH", "RATE", "SWING", "STEP", "KEEP", "KEEP ALL", "NOTE"};
static int gcur_of(int tr) { int n = ipart(tr)->len; return ui.gcur < n ? ui.gcur : n - 1; }
static int gkeep_count(int tr)
{
    int s, n = 0;
    for (s = 0; s < ipart(tr)->len; s++)
        n += gkept(tr, s);
    return n;
}
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
    case 8:                               /* SEED: every one of OCT+'s, a step at a time */
        *lo = 1;
        *hi = 65535;
        int_text(g->seed, t);
        return g->seed;
    case 9:
        *lo = 1;
        *hi = NSTEPS;
        int_text(*part_len(tr), t);
        return *part_len(tr);
    case 12:                              /* KEEP: the step its knobs are on */
        *lo = 1;
        *hi = *part_len(tr);
        int_text(gcur_of(tr) + 1, t);
        return gcur_of(tr) + 1;
    case 13:
        *hi = 1;
        text_cat(t, gkept(tr, gcur_of(tr)) ? "KEPT" : "FREE");
        return gkept(tr, gcur_of(tr));
    case 14:
        *hi = *part_len(tr);
        int_text(gkeep_count(tr), t);
        text_cat(t, " KEPT");
        return gkeep_count(tr);
    case 15:
        *hi = 108;
        v = ipart(tr)->st[gcur_of(tr)].note[0];
        if (v)
            note_text(v, t);
        else
            text_cat(t, "REST");
        return v;
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
    if (i >= 12) {                        /* KEEP: K1 STEP, K2 KEEP, K3 KEEP ALL (up: all, down: none), K4 its NOTE */
        const int c = gcur_of(tr), len = ipart(tr)->len;
        uint8_t *kp = gkeep_of(tr);
        nstep_t *st = &ipart(tr)->st[c];
        uint32_t f;
        int s;
        if (i == 12) {
            ui.gcur = (uint8_t)clamp(c + e, 0, len - 1);
            ui.spage = (uint8_t)(ui.gcur / 16);
            ui.spage_t = plat_ms();
        } else if (i == 13 && e) {
            undo_mark(30);
            kp[c >> 3] = (uint8_t)(e > 0 ? kp[c >> 3] | 1u << (c & 7) : kp[c >> 3] & ~(1u << (c & 7)));
        } else if (i == 14 && e) {
            undo_mark(31);
            for (s = 0; s < NSTEPS; s++)
                kp[s >> 3] = (uint8_t)(e > 0 && s < len ? kp[s >> 3] | 1u << (s & 7) : kp[s >> 3] & ~(1u << (s & 7)));
        } else if (i == 15 && e) {        /* its note by hand (a semitone a click; under C1: a rest): kept at once */
            int n = st->note[0] ? st->note[0] + e : e > 0 ? gen_view(tr).root : 0;
            undo_mark(32);
            f = plat_lock();
            if (n < 24) {
                nstep_clear(st);
            } else {
                if (!st->note[0]) {
                    nstep_clear(st);
                    st->vel = GEN_VEL;
                    st->prob = 100;
                    st->gate = GATE_HALF;
                }
                st->note[0] = (uint8_t)(n > 108 ? 108 : n);
            }
            plat_unlock(f);
            kp[c >> 3] |= (uint8_t)(1u << (c & 7));
        }
        dirty = 1;
        gen_value(tr, gen_of(tr), i, &lo, &hi, t);
        app_say(GEN_LABEL[i], t);
        return;
    }
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
        g->seed = (uint16_t)clamp(g->seed + e, 1, 65535);
        g->mut = 0;
        break;
    }
    gen_write(tr);
    gen_value(tr, g, i, &lo, &hi, t);
    app_say(GEN_LABEL[i], t);
}

/* ---- the drums' beat generator (0.5: the BEAT pages, SEQ again on the drums) ----
 * A style is five rows of 16 weights (0..10: how sure a sixteenth is): bass drum, snare, clap, closed hat, open hat.
 * A knob at 50 plays the style as it comes (10: always; 5: half the time); higher fills in the unlikely steps, lower
 * thins even the sure ones. PERC scatters rim, toms and the second cymbal off the beat; FILL crowds the part's last
 * four steps with snare and toms and may put a crash on its 1; ACCENT marks steps, the beats most. All from the seed,
 * as the 303's lines are: the same seed and knobs, the same beat; MUTATE re-rolls an eighth of the steps */
static const char *const DSTYLE_NAME[NDSTYLES] = {"HOUSE", "TECHNO", "ELECTRO", "BOOM BAP", "TRAP", "BREAKS"};
static const uint8_t DSTYLE[NDSTYLES][5][16] = {
    {{10,0,0,0, 10,0,0,1, 10,0,0,0, 10,0,1,0}, {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,2}, {0,0,0,0, 10,0,0,0, 0,0,0,0, 10,0,0,0},
     {3,2,10,2, 3,2,10,2, 3,2,10,2, 3,2,10,3}, {0,0,5,0, 0,0,5,0, 0,0,5,0, 0,0,6,0}},
    {{10,0,0,0, 10,0,0,0, 10,0,0,0, 10,0,0,2}, {0,0,0,0, 2,0,0,1, 0,0,1,0, 2,0,1,1}, {0,0,0,0, 6,0,0,0, 0,0,0,0, 6,0,0,0},
     {6,5,10,5, 6,5,10,5, 6,5,10,5, 6,5,10,6}, {0,0,3,0, 0,0,3,0, 0,0,3,0, 0,0,4,0}},
    {{10,0,0,3, 0,0,6,0, 2,0,7,0, 0,2,0,0}, {0,0,0,0, 10,0,0,0, 0,0,0,0, 10,0,0,2}, {0,0,0,0, 4,0,0,0, 0,0,0,0, 4,0,0,0},
     {10,5,8,5, 10,5,8,5, 10,5,8,5, 10,5,8,6}, {0,0,1,0, 0,0,2,0, 0,0,1,0, 0,0,3,0}},
    {{10,0,0,1, 0,0,2,4, 7,0,5,0, 0,2,0,1}, {0,0,0,0, 10,0,0,1, 0,1,0,0, 10,0,0,2}, {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
     {10,1,9,2, 10,1,9,2, 10,1,9,2, 10,1,9,3}, {0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,2,0}},
    {{10,0,0,0, 0,0,3,2, 0,0,7,3, 0,2,0,2}, {0,0,0,0, 0,0,0,0, 10,0,0,0, 0,0,0,1}, {0,0,0,0, 0,0,0,0, 8,0,0,0, 0,0,0,0},
     {10,6,10,6, 10,6,10,6, 10,6,10,6, 10,7,10,8}, {0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,2,0}},
    {{10,0,2,0, 0,0,0,2, 0,3,8,0, 0,1,0,0}, {0,0,0,0, 10,0,0,3, 0,3,0,0, 10,0,0,3}, {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0},
     {10,3,10,3, 10,3,10,3, 10,3,10,3, 10,3,10,4}, {0,0,0,0, 0,0,2,0, 0,0,0,0, 0,0,3,0}},
};
static const uint8_t DPERC_W[16] = {0,1,0,1, 0,1,1,2, 0,1,0,2, 1,2,2,3};
static const uint8_t DPERC_PAD[5] = {5, 2, 3, 4, 10};   /* rim, the toms, the second cymbal */
static dgen_t dgen_last;                  /* the settings a new part's first turn starts from */

static dgen_t *dgen_of(void) { return &proj.dgen[clip_of(TR_DRUM)]; }

static dgen_t dgen_view(void)             /* what the BEAT page shows: the part's, else what its first turn starts from */
{
    dgen_t g;
    if (dgen_of()->seed)
        return *dgen_of();
    if (dgen_last.seed)
        return dgen_last;
    memset(&g, 0, sizeof g);
    g.seed = 1;
    g.kick = g.snare = g.hats = 50;
    g.perc = 15;
    g.fill = 25;
    g.accent = 30;
    return g;
}

#define DG_ACC 0x8000u                    /* (a step's hits: a bit a pad, and its accent) */
static void dgen_make(const dgen_t *g, int len, uint16_t *out)
{
    const uint8_t (*w)[16] = DSTYLE[g->style % NDSTYLES];
    int s, m, j, k = len / 8 > 1 ? len / 8 : 1, fill0 = len >= 8 ? len - 4 : len;
    uint8_t ep[NSTEPS];
    for (s = 0; s < len; s++)
        ep[s] = 0;
    for (m = 1; m <= g->mut; m++)
        for (j = 0; j < k; j++)
            ep[gen_hash(g->seed * 7919u + (uint32_t)m, (uint32_t)j) % (uint32_t)len] = (uint8_t)m;
    for (s = 0; s < len; s++) {
        uint32_t base = (uint32_t)g->seed * 65537u + (uint32_t)ep[s] * 1009u + 77u, h[8];
        int i, q = s & 15;
        uint16_t b = 0;
        for (i = 0; i < 8; i++)
            h[i] = gen_hash(base, (uint32_t)(s * 8 + i));
#define DG_ON(i, wt, kn) (h[i] % 1000u < (uint32_t)(wt) * (uint32_t)(kn) * 2u)
        if (DG_ON(0, w[0][q], g->kick)) b |= 1u << 0;
        if (DG_ON(1, w[1][q], g->snare)) b |= 1u << 1;
        if (DG_ON(2, w[2][q], g->snare)) b |= 1u << 6;
        if (DG_ON(4, w[4][q], g->hats)) b |= 1u << 8;          /* the open hat in the closed one's place */
        else if (DG_ON(3, w[3][q], g->hats)) b |= 1u << 7;
        if (DG_ON(5, DPERC_W[q], g->perc * 2u)) b |= (uint16_t)(1u << DPERC_PAD[h[5] / 1000u % 5u]);
        if (s >= fill0 && h[6] % 1000u < g->fill * 6u)       /* the fill: snare, or a tom falling */
            b |= (uint16_t)(1u << ((h[6] / 1000u & 1u) ? 1 : 4 - (s - fill0) * 3 / 4));
        if (s == 0 && len >= 16 && h[6] % 1000u < g->fill * 4u)
            b |= 1u << 9;                 /* a crash on the 1 */
#undef DG_ON
        if (b && h[7] % 1000u < g->accent * ((q & 3) ? 3u : 10u))
            b |= DG_ACC;
        out[s] = b;
    }
}

static void dgen_write(void)              /* the part's hits from its settings (its steps past its length untouched) */
{
    uint16_t *buf = (uint16_t *)(void *)gen_buf[0];   /* (the 303 generator's scratch: never both at once) */
    dpart_t *p = dcur();
    int s, pad, len = p->len;
    uint32_t f;
    dgen_make(dgen_of(), len, buf);
    f = plat_lock();
    for (s = 0; s < len; s++) {
        for (pad = 0; pad < NPADS; pad++) {
            if (buf[s] >> pad & 1u)
                bit_set(p->hit[pad], s);
            else
                bit_clear(p->hit[pad], s);
        }
        if (buf[s] & DG_ACC)
            bit_set(p->acc, s);
        else
            bit_clear(p->acc, s);
    }
    plat_unlock(f);
    dgen_last = *dgen_of();
    dirty = 1;
}

static dgen_t *dgen_begin(void)
{
    dgen_t *g = dgen_of();
    if (!g->seed)
        *g = dgen_view();
    return g;
}

static void dgen_roll(int fresh)          /* OCT+: a random seed's beat; OCT-: a few steps re-rolled */
{
    uint16_t *was = (uint16_t *)(void *)gen_buf[1], *now = was + NSTEPS;
    dgen_t *g;
    int len = dcur()->len, n, same = 1;
    char t[16], u[8];
    undo_mark(0);
    g = dgen_begin();
    if (fresh) {
        g->seed = gen_random_seed(g->seed);
        g->mut = 0;
    } else {
        dgen_make(g, len, was);
        for (n = 0; n < 8 && same && g->mut < 255; n++) {
            g->mut++;
            dgen_make(g, len, now);
            same = !memcmp(was, now, (size_t)len * sizeof was[0]);
        }
    }
    dgen_write();
    t[0] = 0;
    text_cat(t, "SEED ");
    int_text(g->seed, u);
    text_cat(t, u);
    app_say(fresh ? "NEW BEAT" : "MUTATE", t);
}

/* the pages: BEAT (STYLE KICK SNARE HATS), MORE (PERC FILL ACCENT), LINE (SEED, and the part's LENGTH, RATE, SWING) */
static const char *const DGEN_LABEL[12] = {"STYLE", "KICK", "SNARE", "HATS", "PERC", "FILL", "ACCENT", 0,
                                           "SEED", "LENGTH", "RATE", "SWING"};
static int dgen_value(const dgen_t *g, int i, int *lo, int *hi, char *t)
{
    int v;
    t[0] = 0;
    *lo = 0;
    *hi = 100;
    switch (i) {
    case 0:
        *hi = NDSTYLES - 1;
        text_cat(t, DSTYLE_NAME[g->style % NDSTYLES]);
        return g->style;
    case 1: v = g->kick; break;
    case 2: v = g->snare; break;
    case 3: v = g->hats; break;
    case 4: v = g->perc; break;
    case 5: v = g->fill; break;
    case 6: v = g->accent; break;
    case 8:
        *lo = 1;
        *hi = 65535;
        int_text(g->seed, t);
        return g->seed;
    case 9:
        *lo = 1;
        *hi = NSTEPS;
        int_text(*part_len(TR_DRUM), t);
        return *part_len(TR_DRUM);
    case 10:
        *hi = NRATES - 1;
        text_cat(t, RATE_NAME[*part_rate(TR_DRUM) % NRATES]);
        return *part_rate(TR_DRUM);
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

static void seq_knob(int k, int32_t e);
static void dgen_knob(int k, int32_t e)   /* the BEAT pages' knobs: the beat made again at once */
{
    int i = (ui.genpage < NDGENPAGE ? ui.genpage : NDGENPAGE - 1) * 4 + k, lo, hi;
    dgen_t *g;
    char t[16];
    if (i == 7)
        return;
    if (i >= 10) {                        /* RATE, SWING: as on STEPS, the beat as it is */
        seq_knob(i - 9, e);
        return;
    }
    if (i == 9) {                         /* LENGTH: as on STEPS, then a beat made for it (if there is one) */
        seq_knob(0, e);
        if (dgen_of()->seed)
            dgen_write();
        return;
    }
    undo_mark(40u + (uint32_t)i);
    g = dgen_begin();
    switch (i) {
    case 0: g->style = (uint8_t)clamp(g->style + e, 0, NDSTYLES - 1); break;
    case 1: g->kick = (uint8_t)clamp(g->kick + e, 0, 100); break;
    case 2: g->snare = (uint8_t)clamp(g->snare + e, 0, 100); break;
    case 3: g->hats = (uint8_t)clamp(g->hats + e, 0, 100); break;
    case 4: g->perc = (uint8_t)clamp(g->perc + e, 0, 100); break;
    case 5: g->fill = (uint8_t)clamp(g->fill + e, 0, 100); break;
    case 6: g->accent = (uint8_t)clamp(g->accent + e, 0, 100); break;
    default:
        g->seed = (uint16_t)clamp(g->seed + e, 1, 65535);
        g->mut = 0;
        break;
    }
    dgen_write();
    dgen_value(g, i, &lo, &hi, t);
    app_say(DGEN_LABEL[i], t);
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

/* ---- an instrument track's STEPS: four pages of tools (0.5; SELECT turns them), for lines and for harmony.
 *   STEP NOTES    K1 NOTE  K2 VELOCITY  K3 GATE  K4 CHANCE: the step held; none held: what a new step gets (the stamp)
 *   STEP HARMONY  K1 CHORD  K2 INVERT  K3 OCTAVE  K4 SUBSTEPS: the step held (its chord found from its notes, rebuilt
 *                 on its root); none held: the stamp's chord, inversion and octave
 *   PART TOOLS    K1 TRANSPOSE  K2 SHIFT  K3 VELOCITY  K4 GATE: every step of the part
 *   PART GROOVE   K1 LENGTH  K2 RATE  K3 SWING  K4 TEMPO (as the drums' steps) */
enum { ST_STEP, ST_HARM, ST_PART, ST_GROOVE, ST_MOD, NSTOOL };
static const char *const STOOL_NAME[NSTOOL] = {"STEP NOTES", "STEP HARMONY", "PART TOOLS", "PART GROOVE", "STEP LOCKS"};

static void sort4(int *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && a[j] < a[j - 1]; j--) {
            int x = a[j];
            a[j] = a[j - 1];
            a[j - 1] = x;
        }
}

static int step_chord(const nstep_t *s, int *root, int *inv)   /* the chord a step's notes make (0: none of them), its
                                                                * root (else its lowest note) and inversion */
{
    int n = nstep_count(s), c, v, i, a[NCHORD], b[NCHORD];
    for (i = 0; i < n; i++)
        a[i] = s->note[i];
    sort4(a, n);
    *root = n ? a[0] : 60;
    *inv = 0;
    for (c = 1; c < NCHORDS && n > 1; c++)
        for (v = 0; v < n; v++) {
            if (chord_of(c | v << 4, 60, b) != n)
                break;
            sort4(b, n);
            for (i = 0; i < n && a[i] - a[0] == b[i] - b[0]; i++)
                ;
            if (i == n) {
                *root = a[0] - (b[0] - 60);
                *inv = v;
                return c;
            }
        }
    return 0;
}

static void istep_knob(int k, int32_t e)
{
    int tr = ui.trk, held = ui.held_step >= 0, tool = ui.stool, i, n, lo = 127, hi = 1;
    ipart_t *p = ipart(tr);
    track_t *ts = tset(tr);
    nstep_t *s = held ? &p->st[ui.held_step] : 0;
    char t[16];
    uint32_t f;
    if (tool == ST_GROOVE && !held) {
        seq_knob(k, e);
        return;
    }
    if (tool == ST_MOD) {
        xl_knob(k, e);
        return;
    }
    if (held && tool != ST_HARM) {        /* the step's own */
        if (k < 3)
            iheld_knob(k, e);
        else
            iheld_chance(e);
        return;
    }
    t[0] = 0;
    if (!held && tool != ST_PART) {       /* the stamp: what a new step gets */
        if (k == 3) {
            app_say(tool == ST_STEP ? "CHANCE" : "SUBSTEPS", "HOLD A STEP");
        } else if (tool == ST_STEP && k == 1) {
            ui.st_vel = (uint8_t)clamp((ui.st_vel ? ui.st_vel : 100) + e, 1, 127);
            int_text(ui.st_vel, t);
            app_say("NEW VELOCITY", t);
        } else if (tool == ST_STEP && k == 2) {
            ui.st_gate = (uint8_t)clamp((ui.st_gate ? ui.st_gate : GATE_DEF) + e, 1, 255);
            gate_text(ui.st_gate, t);
            app_say("NEW GATE", t);
        } else if (tool == ST_HARM && k < 2) {
            if (k == 0)
                ts->chord = (uint8_t)clamp(ts->chord + e, 0, NCHORDS - 1);
            else
                ts->chord_inv = (uint8_t)clamp(ts->chord_inv + e, 0, 3);
            dirty = 1;
            app_say(k ? "INVERSION" : "CHORD", k ? INV_NAME[ts->chord_inv & 3u] : CHORD_NAME[ts->chord]);
        } else {                          /* NOTE; OCTAVE */
            ui.last_note[tr] = (uint8_t)clamp(track_note(tr) + (tool == ST_HARM ? 12 * e : e), 12, 115);
            note_text(ui.last_note[tr], t);
            app_say("NEW NOTE", t);
        }
        return;
    }
    if (held) {                           /* HARMONY on the step held */
        int root, inv, c, b[NCHORD], m;
        if (!s->note[0])
            return;
        if (k == 3) {
            held_sub(e);
            return;
        }
        n = nstep_count(s);
        c = step_chord(s, &root, &inv);
        if (k == 1 && !c) {
            app_say("INVERSION", "PICK A CHORD: K1");
            return;
        }
        if (k == 2 && hnote_at(s) >= 0) { /* OCTAVE on the one note picked: a voicing */
            int at = hnote_at(s), w = s->note[at] + 12 * e;
            for (i = 0; i < n; i++)
                if (w < 1 || w > 127 || s->note[i] == w)
                    return;
            if (!ui.held_used)
                undo_mark(0);
            ui.held_used = 1;
            f = plat_lock();
            s->note[at] = (uint8_t)w;
            plat_unlock(f);
            hnote_move(s, at, 0);
            note_text(w, t);
            app_say("OCTAVE", t);
            preview(tr, s);
            return;
        }
        if (k != 2)
            ui.hnote = 0;                 /* (a chord rebuilt: its notes are others) */
        if (k == 2) {
            for (i = 0; i < n; i++) {
                if (s->note[i] < lo) lo = s->note[i];
                if (s->note[i] > hi) hi = s->note[i];
            }
            if (lo + 12 * e < 1 || hi + 12 * e > 127)
                return;
        }
        if (!ui.held_used)
            undo_mark(0);
        ui.held_used = 1;
        if (k == 0)
            c = clamp(c + e, 0, NCHORDS - 1);
        else if (k == 1)
            inv = clamp(inv + e, 0, 3);
        m = chord_of(c | inv << 4, root, b);
        f = plat_lock();
        for (i = 0; i < NCHORD; i++)
            s->note[i] = (uint8_t)(k == 2 ? (i < n ? s->note[i] + 12 * e : 0) : i < m ? b[i] : 0);
        plat_unlock(f);
        if (k == 2)
            note_text(s->note[0], t);
        app_say(k == 0 ? "CHORD" : k == 1 ? "INVERSION" : "OCTAVE", k == 0 ? CHORD_NAME[c] : k == 1 ? INV_NAME[inv] : t);
        preview(tr, s);
        return;
    }
    /* PART TOOLS: every step */
    for (i = 0; i < NSTEPS; i++)
        for (n = 0; n < NCHORD && p->st[i].note[n]; n++) {
            if (p->st[i].note[n] < lo) lo = p->st[i].note[n];
            if (p->st[i].note[n] > hi) hi = p->st[i].note[n];
        }
    if (hi < lo)
        return;                           /* (an empty part) */
    if (k == 0)
        e = clamp(e, 1 - lo, 127 - hi);
    if (!e)
        return;
    undo_mark(5u + (uint32_t)k);
    f = plat_lock();
    if (k == 1) {                         /* SHIFT: the part's steps round, later for > 0 (their locks with them) */
        int len = p->len ? p->len : 1, d = ((e % len) + len) % len, r;
        for (r = 0; r < d; r++) {
            nstep_t last = p->st[len - 1];
            for (i = len - 1; i > 0; i--)
                p->st[i] = p->st[i - 1];
            p->st[0] = last;
        }
        for (i = 0; i < p->nlk; i++)
            if (p->lk[i].step < len)
                p->lk[i].step = (uint8_t)((p->lk[i].step + d) % len);
    } else {
        for (i = 0; i < NSTEPS; i++) {
            nstep_t *q = &p->st[i];
            if (!q->note[0])
                continue;
            if (k == 0)
                for (n = 0; n < NCHORD && q->note[n]; n++)
                    q->note[n] = (uint8_t)(q->note[n] + e);
            else if (k == 2)
                q->vel = (uint8_t)((q->vel & NS_ACC) | (uint32_t)clamp(ns_vel(q) + 2 * e, 1, 127));
            else
                q->gate = (uint16_t)clamp(q->gate + e, 1, GATE_MAX);
        }
    }
    plat_unlock(f);
    {
        static const char *const N[4] = {"TRANSPOSE", "SHIFT", "ALL VELOCITY", "ALL GATE"};
        if (k == 0)
            note_text(lo + e, t);
        app_say(N[k], k == 0 ? t : k == 1 ? (e > 0 ? "LATER" : "EARLIER") : e > 0 ? "UP" : "DOWN");
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

static const char *const COLOR_LABEL[COL_NUM] = {"AMOUNT", "WEAR", "WOW", "DIGITAL", "DROPOUT", "COLOR"};
static int color_index(int k)
{
    return ui.color_page ? COL_WEAR + k : k == 0 ? COL_AMOUNT : k == 3 ? COL_ON : -1;
}
static void color_knob(int k, int32_t e)
{
    int id = color_index(k), v, was;
    color_params_t *p = &proj.color[TR_DRUM];
    color_state_t *rt = &drum_color_rt;
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
    plat_unlock(f);
    dirty = 1;
    int_text(v*100/127,t);
    text_cat(t,"%");
    app_say(COLOR_LABEL[id], id == COL_ON ? (v ? "ON" : "BYPASS") : t);
}

static void v3_bus_percent(int k, int raw, char *text)
{
    if (k == 2) {
        int d = raw - 64;
        if (d > 0) *text++ = '+';
        int_text(d >= 0 ? (d * 100 + 31) / 63 : -((-d * 100 + 32) / 64), text);
    } else {
        int_text((raw * 100 + 63) / 127, text);
    }
    text_cat(text, "%");
}

static void v3_bus_knob(int k, int32_t delta)
{
    char text[12];
    uint32_t f = plat_lock();
    int raw = clamp((int)trial_macro_value(k) + delta, 0, 127);
    trial_macro(k, raw);
    plat_unlock(f);
    v3_bus_percent(k, raw, text);
    app_say(V3_BUS_LABEL[k], text);
}

static void v3_bus_mode(int delta, int toggle)
{
    uint32_t f = plat_lock();
    unsigned mode = toggle ? (v3_mode == 2 ? 1 : 2) : (unsigned)clamp((int)v3_mode + delta, V3_MODE_MIN, 2);
    v3_set_mode(mode);
    plat_unlock(f);
    app_say("DrumEngine", V3_MODE_LABEL[mode]);
}

static int v6_voice_page(void)
{
    return V3ON && ui.trk == TR_DRUM && ui.page == PG_EDIT && ui.pad < NPADS;
}

static const char *v6_voice_name(void)
{
    static const char *const names[] = {"KICK","SNARE","CLAP","LOW TOM","MID TOM","HIGH TOM",
                                        "CLOSED HAT","OPEN HAT","CYMBAL","PERC 1","PERC 2"};
    return names[ui.pad];
}

static const char *v6_recipe_name(int pad, int recipe)
{
    return recipe < 0 ? "CUSTOM" : kit_preset_name(pad, recipe);
}

static int v6_pages(void) { return (kit_param_count(ui.pad+1)+3)/4; }

/* The knobs' order on a voice's pages (slot 0 = page 1's K1): its sound's controls first, then its choices (MODEL,
 * PROFILE, MODE), so K1 always turns a sound and never steps through what the voice's presets (SELECT) choose between
 * (the cymbal's MODE is CRASH, RIDE, RISER, NOISE DROP: its presets' own names). -1: no such slot */
static int v6_param(int pad, int slot)
{
    int total = kit_param_count(pad), n = 0;
    for (int choice = 0; choice < 2; ++choice)
        for (int i = 0; i < total; ++i)
            if ((kit_param_option(pad, i, 0) != 0) == choice && n++ == slot)
                return i;
    return -1;
}

static void v6_page_text(char *text)
{
    char digit[] = "PAGE 1/1";
    digit[5] += (char)ui.epage;
    digit[7] += (char)(v6_pages()-1);
    text[0] = 0;
    text_cat(text, digit);
}

/* Use the same settled frequency as the voice, regardless of its recipe name. */
static void v6_append_tom_pitch(char *text, const char *separator, int pad, const KitPatch *patch)
{
    if (pad < 4 || pad > 6) return;
    float hz = kit_tom_pitch_hz(pad, kit_tom_controls(patch,pad));
    int tenths = (int)(hz*10.f + .5f);
    char digits[12], suffix[] = ".0 Hz";
    int_text(tenths/10,digits);
    suffix[1] += (char)(tenths%10);
    text_cat(text,separator);
    text_cat(text,digits);
    text_cat(text,suffix);
}

static void v6_value_text(int pad, int param, int tick, char *text)
{
    const KitParamSpec *spec = kit_param_spec(pad, param);
    const char *option = kit_param_option(pad, param, tick);
    text[0] = 0;
    if (option) {
        text_cat(text, option);
        return;
    }
    if (spec->decimals == 1) {
        if (tick < 0) { *text++ = '-'; tick = -tick; }
        int_text(tick/10, text);
        char fraction[] = ".0";
        fraction[1] += (char)(tick%10);
        text_cat(text, fraction);
    } else int_text(tick, text);
    if (spec->unit[0] && spec->unit[0] != '%') text_cat(text, " ");
    text_cat(text, spec->unit);
}

static void v6_voice_turn(int knob_index, int delta)
{
    int pad = ui.pad+1;
    dirty = 1;                            /* (format 10: SAVE keeps the drums' edits) */
    int fine = (ui.btn >> B_SEL) & 1u;
    if (fine) ui.sel_used = 1;
    if (knob_index < 0) {
        trial_recipe_turn(pad, delta);
        KitPatch current;
        char name[32] = "";
        trial_snapshot(pad, &current);
        text_cat(name,v6_recipe_name(pad, kit_preset_match(&current,pad)));
        v6_append_tom_pitch(name," / ",pad,&current);
        app_say(v6_voice_name(),name);
        return;
    }
    int param = v6_param(pad, 4*ui.epage + knob_index);
    const KitParamSpec *spec = kit_param_spec(pad, param);
    if (!spec) return;
    if (ui.held_step >= 0) {
        app_say("HIT EDIT", "RELEASE STEP");
        return;
    }
    trial_parameter_turn(pad, param, delta, fine);
    KitPatch current;
    char text[24];
    trial_snapshot(pad, &current);
    if (kit_param_editable(&current, pad, param))
        v6_value_text(pad, param, kit_param_get(&current, pad, param), text);
    else { text[0] = 0; text_cat(text,"FIXED BY MODEL"); }
    if (param <= 1) v6_append_tom_pitch(text," / ",pad,&current);
    app_say(spec->label, text);
}

static void knob(int k, int32_t e)        /* KNOB 1..4 (k 0..3) on the page shown */
{
    int inst = ui.trk != TR_DRUM;
    ui.knob_k = (int8_t)k;
    ui.knob_t = plat_ms();
    if (ui.btn >> B_REC & 1u) {
        ui.rec_used = ui.rec_shown = 1;
        rec_knob(k, e);
        ui.redraw = 1;
        return;
    }
    if (ui.page == PG_QMIX) {             /* the QUICK MIXER: the track picked's level, sends and pan */
        qm_knob(k, e);
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
    if (v6_voice_page()) {
        v6_voice_turn(k,e);
    } else if (!inst && ui.page == PG_EDIT && drum_model_page()) {
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
    } else if (ui.page == PG_V3_BUS) {
        v3_bus_knob(k, e);
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
    } else if (ui.page == PG_GEN) {
        if (inst)
            gen_knob(k, e);
        else
            dgen_knob(k, e);
    } else if (inst && ui.page == PG_SEQ) {
        istep_knob(k, e);
    } else if (!inst && ui.page == PG_SEQ && (ui.dtool || ui.held_step >= 0)) {
        dhit_knob(k, e);
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
    {   /* the keys' pressure (0.5): each instrument track's hardest-held key, to its matrix (PRESSURE) and out as
         * channel pressure */
        static uint8_t was[NTRK];
        uint8_t pr[NTRK] = {0};
        int t;
        for (k = 0; k < NKEYS; k++)
            if (ui.key_note[k] && (keys >> k & 1u) && ui.key_trk[k] < NTRK) {
                const uint8_t v = touch_pressure(k);
                if (v > pr[ui.key_trk[k]])
                    pr[ui.key_trk[k]] = v;
            }
        for (t = 1; t < NTRK; t++)
            if (pr[t] != was[t] && (!pr[t] || pr[t] > was[t] + 1 || pr[t] + 1 < was[t])) {
                was[t] = pr[t];
                syn_pressure(t, (float)pr[t] / 127.0f);
                plat_midi_send(0x0Du | (0xD0u | (uint32_t)t) << 8 | (uint32_t)pr[t] << 16, MIDI_TO);
            }
    }
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
        ui.save_used = (uint8_t)cb_button(1, keys);   /* (a step, a lane's key or SEQ held: COPY, not the project) */
        ui.save_slot = -1;
    }
    if (pressed >> B_SEL & 1u) {          /* SEL: held + pad keys 1-4: tracks */
        ui.sel_used = 0;
        ui.sel_t = plat_ms();
        if (btn >> B_REC & 1u)            /* REC held + SEL (sampling until 0.5): nothing, neither RECORD on or off
                                           * as REC is let go nor SEL's help (found on the FM-1) */
            ui.sel_used = ui.rec_used = 1;
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
        } else if (btn >> B_REC & 1u) {   /* REC held: OCT- QUANTIZE, OCT+ LEGATO */
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
        } else if (ui.page == PG_GEN) {   /* the generators: OCT+ a new line (beat), OCT- a few steps changed */
            if (!inst)
                dgen_roll(up);
            else if (up)
                gen_new();
            else
                gen_mutate();
        } else if (inst && (ui.page == PG_KEYS || ui.page == PG_EDIT || ui.page == PG_ENV || ui.page == PG_LFO ||
                            ui.page == PG_ARP || ui.page == PG_MIX || ui.page == PG_FX || ui.page == PG_MST)) {
            octave(up);
        } else if (ui.page == PG_SEQ || ui.page == PG_EDIT) {
            ui.spage = (uint8_t)clamp(ui.spage + (up ? 1 : -1), 0, npages() - 1);
            ui.spage_t = plat_ms();
        }
    }
    if ((released >> B_EDIT & 1u) && !ui.edit_used) {   /* EDIT tapped: the editor, on the page it was left on (SELECT
                                                         * turns them); again: back to the screen it was opened from */
        if (ui.page != PG_EDIT) {
            if (ui.epage >= (inst ? inst_epages(ui.trk) : V3ON && ui.pad < NPADS ? v6_pages() : drum_epages()))
                ui.epage = 0;
            ed_open(ED_EDIT, PG_EDIT);
        } else {
            ed_back(ED_EDIT);
        }
    }
    if ((pressed >> B_SEQ & 1u) && !ui.seq_used) {      /* the steps; on an instrument track, again: the 303 generator (the page of it
                                           * shown last; SELECT its other), then back to the keys */
        if (ui.page == PG_PICK)
            pick_leave();
        else if ((ui.page != PG_SEQ && ui.page != PG_GEN) || (!inst && old_seq))
            ui.page = PG_SEQ;
        else                              /* (the drums: their steps and BEAT, turn about) */
            ui.page = (uint8_t)(ui.page == PG_SEQ ? PG_GEN : inst ? PG_KEYS : PG_SEQ);
        ui.held_step = -1;
    }
    if (pressed >> B_ENV & 1u) {          /* the drums' MUTE page; an instrument's envelopes; again: back */
        const int pg = inst ? PG_ENV : PG_MUTE;
        if (ui.page != pg)
            ed_open(ED_ENV, pg);
        else
            ed_back(ED_ENV);
    }
    if (pressed >> B_HOME & 1u)           /* HOME: the patterns, the quick mixer, the song, SETUP, then back */
        ui.page = (uint8_t)(ui.page == PG_HOME ? PG_QMIX : ui.page == PG_QMIX ? PG_SONG : ui.page == PG_SONG ? PG_SETUP
                            : ui.page == PG_SETUP ? home_page(ui.trk) : PG_HOME);
    if (pressed >> B_GLO & 1u) {          /* the mixer: LEVEL, PAN, REVERB, DELAY; the master bus's pages; then back */
        if (btn >> B_REC & 1u) {          /* REC + GLO: the track's part erased, to record again */
            ui.rec_used = ui.rec_shown = 1;
            part_erase();
            ui.redraw = 1;
        } else if (btn >> B_SEL & 1u) {   /* SEL + GLO: the master bus, at once */
            ui.sel_used = 1;
            if (ui.page != PG_MIX && ui.page != PG_MST)
                ed_from[ED_GLO] = ui.page;
            ui.page = PG_MST;
            ui.mstpage = 0;
        } else if (ui.page == PG_MIX || ui.page == PG_MST) {   /* again: back */
            ed_back(ED_GLO);
        } else {                          /* the mixer or the master bus, whichever was left last, as it was */
            ed_open(ED_GLO, glo_last);
        }
    }
    if (pressed >> B_PLAY & 1u) {
        if (sq.playing) {
            seq_stop();
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
        ui.rec_used = cb_rec = (uint8_t)cb_button(0, keys);   /* (the same held: PASTE, not RECORD or its page) */
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
    if ((released >> B_SEL & 1u) && ui.help) {   /* (HELP shown: let go, the page back) */
        ui.help = 0;
        ui.redraw = 1;
    } else if ((released >> B_SEL & 1u) && !ui.sel_used)
        app_say("SEL+PADS: TRACK", "HOLD SEL: HELP");
    if ((released >> B_SAVE & 1u) && !ui.save_used)
        do_save();
    else if ((released >> B_SAVE & 1u) && ui.save_slot >= 0)   /* SAVE held + PRESETS, let go: a user preset */
        save_user();
    if (pressed >> B_LFO & 1u) {          /* every track's LFO (an instrument's: LFO 2, MATRIX too); again: back */
        if (ui.page != PG_LFO) {
            if (ui.lfopage >= lfo_pages(ui.trk))
                ui.lfopage = 0;
            ed_open(ED_LFO, PG_LFO);
        } else {
            ed_back(ED_LFO);
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
            ed_open(ED_ARP, PG_ARP);
        } else {
            ed_back(ED_ARP);
        }
    }
    if (pressed >> B_FX & 1u) {           /* the FX: REVERB, DELAY, DELAY 2, FILTER, then back */
        if ((btn >> B_SEL & 1u) && inst)
            ui.sel_used = 1;
        if (!inst && (btn >> B_SEL & 1u)) {   /* SEL + FX on the drums: their bus; again: back */
            ui.sel_used = 1;
            if (ui.page != PG_V3_BUS)
                ed_open(ED_FX, PG_V3_BUS);
            else
                ed_back(ED_FX);
        } else if (ui.page == PG_V3_BUS) {
            v3_bus_mode(0, 1);
        } else if (ui.page == PG_COLOR) {
            ui.page = (uint8_t)home_page(ui.trk);
        } else if (ui.page != PG_FX) {
            ed_open(ED_FX, PG_FX);
        } else {
            ed_back(ED_FX);
        }
    }

    /* keys */
    kp = keys & ~ui.keys;
    kr = ~keys & ui.keys;
    for (k = 0; k < 16u; k++) {           /* white keys */
        uint32_t key = WK[k], bit = 1u << key;
        if ((kr & bit) && k < QM_MOVES && (qm.held >> k & 1u)) {   /* a QUICK MIXER move let go */
            qm_move((int)k, 0);
            continue;
        }
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
        } else if (ui.page == PG_QMIX) {   /* the drums: a lane's mute; T2-T4: keys 1-4 the moves, the rest nothing */
            if (!inst) {
                if ((int)k < NPADS)
                    mute_toggle((int)k);
            } else if (k < QM_MOVES) {
                qm_move((int)k, 1);
            }
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
        } else if (ui.page == PG_QMIX) {  /* a track key: that track picked (one press); 5-11: the perform keys */
            if (k < NTRK) {
                track_pick((int)k);
                inst = ui.trk != TR_DRUM;
                ui.redraw = 1;
            } else if (k - NTRK < NPF) {
                pf_press((int)k - NTRK);
            }
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
    if ((e = plat_enc(EN_PRESET)) != 0) { /* PRESETS: the presets wherever you are: the track's sound; on the drums the
                                           * kit, or in a drum's editor that drum's preset; SAVE held: where to save it */
        if (ui.btn >> B_SAVE & 1u) {
            ui.save_used = 1;
            save_target(e);
        } else if (ui.held_step >= 0 && ui.page == PG_SEQ) {   /* a step held: its NUDGE (late) */
            held_nudge(e);
        } else if (inst && (ui.btn >> B_SEL & 1u)) {
            ui.sel_used = 1;
            preset_bank_turn(e);
        } else if (inst) {
            preset_turn(e);
        } else if (v6_voice_page() && ui.held_step < 0) {
            v6_voice_turn(-1, e);
        } else {
            kit_turn(e);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {   /* ALGORITHM: the track; a step held: its substeps */
        if (ui.held_step >= 0)
            held_sub(e);
        else
            track_pick(ui.trk + e);
    }
    inst = ui.trk != TR_DRUM;
    if ((e = plat_enc(EN_SELECT)) != 0 && ui.page != PG_PICK) { /* SELECT: the editor's pages (every editor's), a held
                                           * step's chance, the cursor */
        if (ui.page == PG_V3_BUS) {
            v3_bus_mode(e, 0);
        } else if (v6_voice_page()) {     /* a drum's editor: its pages (its preset: PRESETS) */
            if (ui.held_step >= 0) {
                if (ui.btn >> B_SEL & 1u) ui.sel_used = 1;
                held_chance(e);
            } else ui.epage = (uint8_t)clamp(ui.epage + e, 0, v6_pages() - 1);
        } else if (ui.page == PG_COLOR) {
            ui.color_page = (uint8_t)clamp(ui.color_page+e,0,1);
        } else if (ui.page == PG_SETUP) {   /* SETUP: SELECT turns to the CPU by stage and back */
            char t[16];
            ui.setup_view = (uint8_t)clamp(ui.setup_view + e, 0, 3);
            t[0] = 0;
            if (ui.setup_view == 3) {
                text_cat(t, "CPU ");
                int_text((int32_t)plat_cpu_pct(), t + 4);
                text_cat(t, "%");
            }
            app_say("SETUP", ui.setup_view == 3 ? t : ui.setup_view == 2 ? "BLUETOOTH AUDIO" : ui.setup_view ? "MIDI, BLUETOOTH"
                             : "CLOCK, SOUNDS");
        } else if (ui.held_step >= 0 && !(inst && (ui.page == PG_EDIT || ui.page == PG_ENV || ui.page == PG_LFO))) {
            /* (there: pages to lock on) */
            if (inst && ui.page == PG_SEQ)   /* its steps: which note (its chance: STEP NOTES' K4) */
                hnote_pick(e);
            else if (inst)
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
            int g = clamp((ui.page == PG_MIX ? ui.mixpage : NMIXPAGE + ui.mstpage) + e, 0, NMIXPAGE + NMSTPAGE - 1);
            ui.page = g < NMIXPAGE ? PG_MIX : PG_MST;
            if (g < NMIXPAGE)
                ui.mixpage = (uint8_t)g;
            else
                ui.mstpage = (uint8_t)(g - NMIXPAGE);
        } else if (ui.page == PG_FX) {
            ui.fxpage = (uint8_t)clamp(ui.fxpage + e, 0, NFXPAGE - 1);
        } else if (ui.page == PG_LFO) {
            ui.lfopage = (uint8_t)clamp(ui.lfopage + e, 0, lfo_pages(ui.trk) - 1);
        } else if (inst && ui.page == PG_ARP) {
            ui.arppage = (uint8_t)clamp(ui.arppage + e, 0, 1);
        } else if (ui.page == PG_GEN) {
            ui.genpage = (uint8_t)clamp(ui.genpage + e, 0, (inst ? NGENPAGE : NDGENPAGE) - 1);
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
        } else if (!inst && ui.page == PG_SEQ) {  /* the drums' steps: PART GROOVE's knobs or the HIT's (the banks: OCT) */
            ui.dtool = (uint8_t)clamp(ui.dtool + e, 0, 1);
            app_say("TOOLS", ui.dtool ? "DRUM HIT" : "PART GROOVE");
        } else if (inst && ui.page == PG_SEQ) {   /* an instrument's steps: its tools' pages (the banks: the black keys) */
            ui.stool = (uint8_t)clamp(ui.stool + e, 0, NSTOOL - 1);
            app_say("TOOLS", STOOL_NAME[ui.stool]);
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
    } else if (ui.page == PG_QMIX) {      /* the QUICK MIXER: the track picked lit, the others dim; the perform keys dim,
                                           * lit while held; the drums' lanes lit while they play, a muted one dark;
                                           * T2-T4's moves dim, lit while held */
        for (k = 0; k < NTRK; k++)
            if ((int)k == tr)
                bk |= 1u << BK[k];
            else
                dimk |= 1u << BK[k];
        for (k = 0; k < NPF; k++)
            if (perf.want >> k & 1u)
                bk |= 1u << BK[NTRK + k];
            else
                dimk |= 1u << BK[NTRK + k];
        if (tr == TR_DRUM) {
            const uint16_t mute = dcur()->mute;
            for (k = 0; k < NPADS; k++)
                if (!(mute >> k & 1u))
                    wk |= 1u << WK[k];
        } else {
            for (k = 0; k < QM_MOVES; k++)
                if (qm.trk == tr && (qm.held >> k & 1u))
                    wk |= 1u << WK[k];
                else
                    dimk |= 1u << WK[k];
        }
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
                    : ui.page == PG_LFO ? B_LFO : ui.page == PG_ARP ? B_ARP : ui.page == PG_FX || ui.page == PG_COLOR || ui.page == PG_V3_BUS ? B_FX
                    : ui.page == PG_HOME || ui.page == PG_QMIX || ui.page == PG_SONG || ui.page == PG_SETUP ? B_HOME
                    : B_GLO);
    if (sq.playing)
        b |= LED_PLAY_GREEN;
    if (ui.rec)
        b |= 1u << B_REC;
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
/* The Acid Dub Deck look (0.5, the user's page atlas): three fixed bands. The header (y 0-43) and the knob strip
 * (y 176-239) deep purple, ruled off in acid green: the header "GM / T2" and the pattern and tempo, then the page's
 * title large in a monospace face; the strip four cells, K1..K4: the knob, its name, its value large. Between them the
 * work area on black: grids in purple, what's set or selected in acid green (black type on it), what is happening now
 * in yellow (the playhead, a held move, the knob being turned), so colour is never the only cue: a held move says
 * HELD, the playhead has its flag. (Earlier looks: git, attic/screen_dark.c.) */
#define C_CANVAS RGB(9, 8, 10)                /* the work area */
#define C_FRAME RGB(40, 18, 79)               /* the header, the knob strip */
#define C_FIELD RGB(51, 32, 77)               /* a box, a chip, an empty cell: secondary purple */
#define C_BONE RGB(255, 255, 255)             /* text, values */
#define C_MUTED RGB(250, 247, 255)            /* labels, hints: near-white */
#define C_RULE RGB(96, 66, 146)               /* a divider on the purple */
#define C_DUSK RGB(51, 32, 77)                /* the grid, empty steps */
#define C_VIOLET C_ACID                       /* selected (black type on it) */
#define C_FOCUS C_ACID                        /* a lock */
#define C_METER C_NOW                         /* live readings: now */
#define C_INK C_CANVAS                        /* black type: on acid green, yellow or white */
#define C_CREAM C_ACID                        /* HOME, SONG, SETUP and the mixer */
#define C_BANDBG C_FRAME
#define C_TILE C_FIELD
#define C_TILET C_BONE
#define C_TILET2 C_MUTED
#define C_WELL C_DUSK
#define C_WHITE2 C_BONE
#define C_RECCOL C_NOW                        /* hold REC: recording's page (now) */
#define C_FXCOL C_ACID
#define C_MSTCOL C_ACID
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

static void grad_init(void);
static void theme(uint16_t col)          /* the page: the canvas, drawn on in the section's colour, written in bone */
{
    grad_init();
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
    return ui.rec_shown && (ui.btn >> B_REC & 1u);
}

static uint16_t sec_col(void)             /* the section's colour */
{
    if (rec_page())
        return C_RECCOL;
    switch (ui.page) {
    case PG_HOME: case PG_QMIX: case PG_SONG: case PG_MIX: case PG_SETUP: return C_CREAM;
    case PG_FX: case PG_V3_BUS: return C_FXCOL;
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

static void grid_bg(void)                 /* a curve's ground: the work area's black, a purple grid, 8 by 5 */
{
    int i;
    for (i = 0; i <= 8; i++)
        cv_rect(12 + i * 27, 6, 1, 101, C_DUSK);
    for (i = 0; i <= 5; i++)
        cv_rect(12, 6 + i * 20, 217, 1, C_DUSK);
}

static void foot(const char *left, const char *right, int now)   /* the work area's last line: a hint at the left;
                                                                  * a state at the right, yellow if it's happening now */
{
    if (left && left[0])
        cv_text(8, BODY_H - 17, &FONT_XS, left, C_MUTED);
    if (right && right[0])
        text_r(232, BODY_H - 17, &FONT_XS, right, now ? C_NOW : C_MUTED);
}

#define PLAYHEAD mix565(C_CANVAS, C_NOW, 72)   /* the step playing: a dim yellow column (its flag: playflag) */

/* ---- gradients (0.5): three ramps, built once, 48 steps each.
 *   HEAT, for meters and faders, by where along them: aqua, acid green, yellow, orange, hot pink at the top: a level's
 *     colour says how hot it is;
 *   GLOW, under a scope's trace, by the distance from it: acid at the trace through teal and violet into the black;
 *   TRACE, a trace's own colour by its height: cyan low, acid in the middle, yellow high. */
#define NGRAD 48
enum { GR_HEAT, GR_GLOW, GR_TRACE, NGR };
static uint16_t grad[NGR][NGRAD];

static void grad_init(void)
{
    static const uint8_t STOP[NGR][5][3] = {
        {{18, 200, 180}, {195, 255, 49}, {255, 227, 73}, {255, 138, 31}, {255, 45, 111}},
        {{160, 236, 44}, {25, 184, 154}, {91, 43, 184}, {40, 16, 70}, {16, 8, 26}},
        {{43, 217, 255}, {120, 240, 160}, {195, 255, 49}, {225, 245, 60}, {255, 227, 73}}};
    static const uint8_t AT[NGR][5] = {{0, 102, 179, 219, 255}, {0, 64, 140, 210, 255}, {0, 80, 128, 190, 255}};
    static uint8_t done;
    int g, i, k;
    if (done)
        return;
    done = 1;
    for (g = 0; g < NGR; g++)
        for (i = 0; i < NGRAD; i++) {
            int t = i * 255 / (NGRAD - 1);
            for (k = 0; k < 3 && t > AT[g][k + 1]; k++)
                ;
            {
                const uint8_t *a = STOP[g][k], *b = STOP[g][k + 1];
                int f = (t - AT[g][k]) * 256 / (AT[g][k + 1] - AT[g][k] ? AT[g][k + 1] - AT[g][k] : 1);
                grad[g][i] = mix565(RGB(a[0], a[1], a[2]), RGB(b[0], b[1], b[2]), f);
            }
        }
}

static inline uint16_t gradc(int g, int num, int den)   /* ramp g at num / den, 0..1 */
{
    int i = den > 0 ? num * (NGRAD - 1) / den : 0;
    return grad[g][i < 0 ? 0 : i >= NGRAD ? NGRAD - 1 : i];
}

static void vheat(int x, int y, int w, int h, int fill)   /* a meter or fader up: fill rows from the bottom, by height */
{
    int r;
    for (r = 0; r < fill && r < h; r++)
        cv_rect(x, y + h - 1 - r, w, 1, gradc(GR_HEAT, r, h - 1));
}

static void hheat(int x, int y, int w, int h, int fill)   /* a meter across: fill columns from the left, by place */
{
    int c;
    for (c = 0; c < fill && c < w; c++)
        cv_rect(x + c, y, 1, h, gradc(GR_HEAT, c, w - 1));
}

static void glow_col(int x, int yc, int yb)   /* under (or over) a trace at yc toward yb: GLOW by the distance */
{
    int d = yb > yc ? 1 : -1, n = yb > yc ? yb - yc : yc - yb, k, span = n > 40 ? n : 40;
    for (k = 1; k <= n; k++)
        cv_pset(x, yc + d * k, gradc(GR_GLOW, k, span));
}

static void gline(int x0, int y0, int x1, int y1, int ytop, int ybot)   /* a 2-pixel trace, TRACE by its height */
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy, guard = 2000;
    while (guard--) {
        int32_t e2 = 2 * err;
        uint16_t c = gradc(GR_TRACE, ybot - y0, ybot - ytop);
        cv_pset(x0, y0, c);
        cv_pset(x0, y0 + 1, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- live pictures (0.5): the scope's capture asked and taken; a history of one reading; a yellow dot (now) */
static void scope_ask(int src, int dec, int pairs)   /* a new capture of src from here (the one filling: dropped) */
{
    sc.state = 0;
    sc.src = (uint8_t)src;
    sc.dec = (uint8_t)dec;
    sc.pairs = (uint8_t)pairs;
    sc.n = sc.k = 0;
    sc.acc = 0.0f;
    sc.inv = dec == 4 ? 0.25f : dec == 2 ? 0.5f : 1.0f;
    sc.state = 1;
}

static int scope_take(int src, int dec, int pairs)   /* 1: a full capture of src is in sc.buf (draw it, then again) */
{
    if (sc.src != src || sc.dec != dec || sc.pairs != pairs) {
        scope_ask(src, dec, pairs);
        return 0;
    }
    if (sc.state == 2)
        return 1;
    if (sc.state == 0)
        scope_ask(src, dec, pairs);
    return 0;
}

static void scope_again(void)
{
    scope_ask(sc.src, sc.dec, sc.pairs);
}

/* the history: one reading (0..255) of the page on screen, every `every` ms, the newest at the right; another page's
 * reading starts it again. 216 B, one for all the pages that draw one */
#define HN 216
static struct {
    uint8_t v[HN];
    uint16_t n;
    uint8_t src;
    uint32_t t;
} hist;

static void hist_feed(int src, int v, uint32_t every)
{
    uint32_t now = plat_ms();
    int i;
    if (hist.src != src) {
        hist.src = (uint8_t)src;
        hist.n = 0;
        hist.t = now - every;
    }
    if (now - hist.t < every)
        return;
    hist.t = now;
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    if (hist.n < HN) {
        hist.v[hist.n++] = (uint8_t)v;
        return;
    }
    for (i = 1; i < HN; i++)
        hist.v[i - 1] = hist.v[i];
    hist.v[HN - 1] = (uint8_t)v;
}

static void hist_draw(int x, int y, int w, int h)   /* an area HEAT by height, its edge in TRACE */
{
    int i, n = hist.n < w ? hist.n : w, x0 = x + w - n;
    for (i = 0; i < n; i++) {
        int v = hist.v[hist.n - n + i], hh = v * h / 255;
        vheat(x0 + i, y, 1, h, hh);
        if (hh > 0)
            cv_pset(x0 + i, y + h - hh, gradc(GR_TRACE, hh, h));
    }
    cv_rect(x, y + h, w, 1, C_GRID);
}

static void now_dot(int x, int y)         /* a voice or a reading where it is now: a yellow ring */
{
    rrect(x - 4, y - 4, 9, 9, 4, C_NOW);
    rrect(x - 2, y - 2, 5, 5, 2, C_INK);
}

/* the track's own sound now, triggered on a rising zero, 170 samples of every 5th (19 ms) across: 0 if it's quiet (the
 * page draws its picture of the settings instead) */
static int track_scope(int tr, const char *what)
{
    static uint8_t live;
    int i, trig = 0, pk = 0, py = 0, mid = 62, amp = 46;
    char t[24];
    if (!scope_take(tr, 5, 0) && !live)
        return 0;
    for (i = 0; i < SC_N; i++) {
        int a = sc.buf[i] < 0 ? -sc.buf[i] : sc.buf[i];
        pk = a > pk ? a : pk;
    }
    if (pk < 300) {                       /* quiet: the picture */
        live = 0;
        scope_again();
        return 0;
    }
    live = 1;
    for (i = 1; i < SC_N / 3; i++)
        if (sc.buf[i - 1] < 0 && sc.buf[i] >= 0) {
            trig = i;
            break;
        }
    grid_bg();
    cv_rect(10, mid, 220, 1, C_GRID);
    for (i = 0; i < 220; i++) {
        int y = mid - sc.buf[trig + i * 169 / 219] * amp / pk;
        if (i) {
            glow_col(10 + i, y, mid);
            gline(10 + i - 1, py, 10 + i, y, mid - amp, mid + amp);
        }
        py = y;
    }
    t[0] = 0;
    text_cat(t, "LIVE ");
    text_cat(t, what);
    cv_text(12, 8, &FONT_XS, t, C_NOW);
    scope_again();
    return 1;
}
static void playflag(int x, int w)        /* the playhead's flag on top of its column: a small yellow triangle */
{
    int j, c = x + w / 2;
    for (j = 0; j < 4; j++)
        cv_rect(c - 3 + j, j, 7 - 2 * j, 1, C_NOW);
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

/* ---- the band: the header (the Acid Dub Deck look). Line 1: "GM / " the family (the track, or ALL), the pattern
 * (the next one beside it, blinking yellow) and the tempo; a change just made in yellow in their place a moment.
 * Line 2: the page's title, large; its context small to the right where it fits. (bnd.npg, bnd.pg: no dots now) */
static struct {
    uint16_t col;                         /* the section's colour */
    char id[12], title[24], ctx[32];
    uint8_t npg, pg;
} bnd;

static void band_set(uint16_t col, const char *id, const char *title, const char *ctx, int npg, int pg)
{
    int i;
    bnd.col = col;
    for (i = 0; id[i] && i < (int)sizeof bnd.id - 1; i++)
        bnd.id[i] = id[i];
    bnd.id[i] = 0;
    for (i = 0; title[i] && i < (int)sizeof bnd.title - 1; i++)
        bnd.title[i] = title[i];
    bnd.title[i] = 0;
    upper(bnd.title);
    for (i = 0; ctx[i] && i < (int)sizeof bnd.ctx - 1; i++)
        bnd.ctx[i] = ctx[i];
    bnd.ctx[i] = 0;
    upper(bnd.ctx);
    bnd.npg = (uint8_t)npg;
    bnd.pg = (uint8_t)pg;
}

static void band(void)
{
    uint32_t now = plat_ms();
    char t[48], u[32];
    int32_t xl, xr;
    const felucca_font_t *f;
    cv_begin(240, 44, C_FRAME);
    cv_rect(0, 43, 240, 1, C_ACID);       /* the acid rule under the header */
    t[0] = 0;
    text_cat(t, "GM / ");
    text_cat(t, bnd.id);
    xl = cv_text(8, 2, &FONT_MS, t, C_BONE);
    if (now < ui.msg_until) {             /* a change just made: what, and its value, in yellow */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        upper(t);
        fit(&FONT_MS, t, 232 - xl - 10, u);
        text_r(232, 2, &FONT_MS, u, C_NOW);
    } else {                              /* the tempo; the pattern (the song's entry), the next blinking; play, rec */
        int home = ui.page == PG_HOME || ui.page == PG_SONG, bp = home ? proj.pat : clip_of(ui.trk);
        int q = sq.queued >= 0 ? sq.queued : (!home && qclip[ui.trk] >= 0) ? qclip[ui.trk] : -1;
        int_text(proj.bpm10 / 10, u);
        text_cat(u, " BPM");
        xr = 232 - text_w(&FONT_MS, u);
        cv_text(xr, 2, &FONT_MS, u, C_BONE);
        if (q >= 0 && (now / 250u & 1u)) {
            u[0] = '>';
            two_digits(q + 1, u + 1);
            xr -= 8 + text_w(&FONT_MS, u);
            cv_text(xr, 2, &FONT_MS, u, C_NOW);
        }
        if (srt.pos >= 0 && sq.playing) {
            u[0] = 'S';
            two_digits(srt.pos + 1, u + 1);
        } else if (!home && clip[ui.trk] < 0) {
            u[0] = u[1] = '-';
            u[2] = 0;
        } else {
            u[0] = 'P';
            two_digits(bp + 1, u + 1);
        }
        xr -= 8 + text_w(&FONT_MS, u);
        cv_text(xr, 2, &FONT_MS, u, C_BONE);
        if (sq.playing) {                 /* a triangle, yellow: playing now */
            int j;
            for (j = 0; j < 9; j++)
                cv_rect(xr - 12, 5 + j, j < 5 ? j + 1 : 9 - j, 1, C_NOW);
            xr -= 12;
        }
        if (ui.rec) {                     /* recording: a red dot */
            cv_rect(xr - 12, 6, 8, 8, C_RED);
            xr -= 12;
        }
        if (dirty) {                      /* (0.5) changed since the last save: a hollow acid box (SAVE keeps it) */
            frame(xr - 12, 6, 8, 8, C_ACID);
            xr -= 12;
        }
        ui.batt = (int8_t)plat_battery();
        if (ui.batt >= 0 && ui.batt <= 3 && xr - 20 > xl + 4) {   /* on the battery: its bars (red when empty) */
            int j;
            frame(xr - 18, 6, 13, 8, ui.batt ? C_BONE : C_RED);
            cv_rect(xr - 5, 8, 2, 4, ui.batt ? C_BONE : C_RED);
            for (j = 0; j < ui.batt; j++)
                cv_rect(xr - 16 + j * 3, 8, 2, 4, C_BONE);
        }
    }
    f = text_w(&FONT_MT, bnd.title) <= 224 ? &FONT_MT : text_w(&FONT_MB, bnd.title) <= 224 ? &FONT_MB : &FONT_B;
    fit(f, bnd.title, 224, t);
    xl = cv_text(8, f == &FONT_MT ? 17 : 21, f, t, C_BONE);
    if (bnd.ctx[0] && 232 - xl - 10 >= 40) {   /* the context, small, where it fits */
        fit(&FONT_XS, bnd.ctx, 232 - xl - 10, u);
        text_r(232, 24, &FONT_XS, u, C_MUTED);
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
    c->acc = bnd.col;
}

static void strip(void)                   /* four cells, K1..K4 as the knobs sit: the knob, its name, its value */
{
    int k;
    uint32_t now = plat_ms();
    cv_begin(240, 64, C_FRAME);
    cv_rect(0, 0, 240, 1, C_ACID);
    for (k = 1; k < 4; k++)
        cv_rect(k * 60 - 1, 6, 1, 52, C_RULE);
    for (k = 0; k < 4; k++) {
        const card_t *c = &cards[k];
        int x = k * 60, w = 59, hot = c->on && ui.knob_k == k && now - ui.knob_t < 1300u && !c->ro;
        char t[32], num[3];
        const felucca_font_t *f;
        num[0] = 'K';
        num[1] = (char)('1' + k);
        num[2] = 0;
        /* the knob's number (dim: none here, or a reading); its name; a lock's L; its value large, acid while it
         * turns (a yellow bar under it: now) or locked */
        cv_text(x + 6, 3, &FONT_XS, num, c->on && !c->ro ? C_BONE : C_RULE);
        if (!c->on) {
            cv_rect(x + 6, 24, 9, 2, C_RULE);
            continue;
        }
        f = text_w(&FONT_XS, c->label) <= w - (c->lock ? 20 : 9) ? &FONT_XS : &FONT_T;
        fit(f, c->label, w - (c->lock ? 20 : 9), t);
        cv_text(x + 6, f == &FONT_T ? 19 : 17, f, t, c->ro ? C_RULE : C_MUTED);
        if (c->lock) {
            cv_rect(x + w - 15, 4, 10, 12, C_ACID);
            cv_text(x + w - 13, 2, &FONT_XS, "L", C_INK);
        }
        f = text_w(&FONT_MB, c->text) <= w - 8   ? &FONT_MB
            : text_w(&FONT_B, c->text) <= w - 8  ? &FONT_B
            : text_w(&FONT_S, c->text) <= w - 8  ? &FONT_S
            : text_w(&FONT_XS, c->text) <= w - 8 ? &FONT_XS
                                                 : &FONT_T;
        fit(f, c->text, w - 8, t);
        cv_text(x + 6, f == &FONT_MB ? 35 : f == &FONT_T ? 39 : 36, f, t, hot || c->lock ? C_ACID : c->ro ? C_MUTED : C_BONE);
        if (hot)
            cv_rect(x + 6, 59, w - 14, 3, C_NOW);
    }
    cv_blit(0, STRIP_Y);
}

/* ---- each kind of page's four knobs */
enum { KD_SEQ, KD_HELD, KD_STOOL, KD_DHIT, KD_DRUM, KD_KIT, KD_MODEL, KD_INST, KD_ENV, KD_LFO, KD_ARP, KD_MIX, KD_FX, KD_MST, KD_SONG, KD_REC, KD_PF,
       KD_SETUP, KD_FOLLOW, KD_GEN, KD_COLOR, KD_QMIX };

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
        v = proj.color[TR_DRUM].v[id];
        int_text(v * 100 / 127,t);
        text_cat(t, "%");
        card_put(c, COLOR_LABEL[id], id == COL_ON ? (v ? "ON" : "BYPASS") : t,
                 v, 0, id == COL_ON ? 1 : 127, -1);
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
    case KD_DHIT: {                       /* the drums' hit held: its velocity, chance, substeps; the step's accent */
        static const char *const N[4] = {"VELOCITY", "CHANCE", "SUBSTEPS", "ACCENT"};
        const dpart_t *d = dcur();
        int s = ui.held_step, j;
        if (s < 0 || !bit_get(d->hit[ui.pad], s)) {
            text_cat(t, "HOLD STEP");
        } else if (k == 3) {
            text_cat(t, bit_get(d->acc, s) ? "ON" : "OFF");
        } else {
            j = lock_find(d, s, ui.pad, k == 0 ? LK_VEL : k == 1 ? LK_PROB : LK_SUB);
            int_text(j >= 0 ? d->lk[j].val : k == 0 ? (bit_get(d->acc, s) ? 127 : (int)(DRUMS_VEL_NORMAL * 127.0f + 0.5f))
                                             : k == 1 ? 100 : 1, t);
            if (k == 1)
                text_cat(t, "%");
        }
        card_put(c, N[k], t, 0, 0, 1, -1);
        return 1;
    }
    case KD_STOOL: {                      /* an instrument's step tools: STEP (the stamp), HARMONY, PART */
        static const char *const N[3][4] = {{"NOTE", "VELOCITY", "GATE", "CHANCE"},
                                            {"CHORD", "INVERT", "OCTAVE", "SUBSTEPS"},
                                            {"TRANSPOSE", "SHIFT", "VELOCITY", "GATE"}};
        const track_t *ts = tset(tr);
        int held = ui.held_step >= 0, root = track_note(tr), inv = ts->chord_inv & 3, ch = ts->chord % NCHORDS, sub = 1;
        const nstep_t *s = held ? &ipart(tr)->st[ui.held_step] : 0;
        if (page == ST_MOD) {             /* STEP LOCKS: the held step's (else the track's own, to hold a step for) */
            static const char *const XN[XL_NUM] = {"LFO RATE", "LFO DEPTH", "ARP RATE", "ARP GATE"};
            const int lk = xl_value(tr, held ? ui.held_step : -1, k, t);
            card_put(c, XN[k], t, lk, 0, 1, -1);
            return 1;
        }
        if (s && s->note[0]) {
            int j = ilock_find(ipart(tr), ui.held_step, IL_SUB);
            ch = step_chord(s, &root, &inv);
            if (!ch && nstep_count(s) > 1)
                ch = -1;
            sub = j >= 0 ? ipart(tr)->lk[j].val : 1;
        }
        if (page == ST_PART) {
            text_cat(t, "ALL");
        } else if (k == 3) {
            if (held && page == ST_HARM)
                int_text(sub, t);
            else
                text_cat(t, "HOLD STEP");
        } else if (page == ST_STEP && k == 1) {
            int_text(ui.st_vel ? ui.st_vel : 100, t);
        } else if (page == ST_STEP && k == 2) {
            gate_text(ui.st_gate ? ui.st_gate : GATE_DEF, t);
        } else if (page == ST_HARM && k == 0) {
            text_cat(t, ch < 0 ? "CUSTOM" : CHORD_NAME[ch]);
        } else if (page == ST_HARM && k == 1) {
            text_cat(t, INV_NAME[inv]);
        } else {
            note_text(root, t);
        }
        card_put(c, N[page % 3][k], t, 0, 0, 1, -1);
        return 1;
    }
    case KD_HELD: {
        const nstep_t *s = &ipart(tr)->st[ui.held_step];
        static const char *const N[4] = {"NOTE", "VELOCITY", "GATE", "CHANCE"};
        if (k == 0) {
            int at = hnote_at(s);
            note_text(s->note[0] ? s->note[at < 0 ? 0 : at] : 60, t);
            if (nstep_count(s) > 1 && at < 0)
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
        card_put(c, "MODEL", !drums_has_pad(v, ui.pad) ? "--" : drums_kit_name(v), v, 0, KIT_808, -1);
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
                                : EPAGE_P[page % NEPAGE][k];
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
    case KD_QMIX:                         /* the QUICK MIXER: the track picked's LEVEL, REVERB, DELAY, PAN */
        mix_text(ui.trk, QM_PAGE[k], t);
        card_put(c, QM_KNOB[k], t, mix_value(ui.trk, QM_PAGE[k]), 0, 127, QM_PAGE[k] == 1 ? 64 : -1);
        c->acc = trk_rgb(ui.trk);
        return 1;
    case KD_MIX:
        if (page == MIX_RET) {            /* RETURNS: the FX's knobs */
            const x0x_param_t *p = fx_param(RET_P[k]);
            fx_text(RET_P[k], t);
            card_put(c, RET_LABEL[k], t, proj.fx[RET_P[k]], 0, p->max, -1);
            return 1;
        }
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
        card_put(c, FX_LABEL[i], t, *fxv(i), 0, p->max, fx_mid(i));
        return 1;
    }
    case KD_SETUP: {
        static const char *const CL[3] = {"OFF", "SEND", "FOLLOW"};
        if (page == 1) {                  /* the MIDI view */
            if (k == 0)
                card_put(c, "SEQ OUT", seq_out() ? "ON" : "OFF", seq_out(), 0, 1, -1);
            else if (k == 1 && FM1B_BLE)
                card_put(c, "BLUETOOTH", ble_wanted() ? "ON" : "OFF", ble_wanted(), 0, 1, -1);
#if FM1B_BLE_CENTRAL
            else if (k == 2) {
                int_text(btdev_count() ? btdev_pick() + 1 : 0, t);
                card_put(c, "DEVICE", t, btdev_pick(), 0, 7, -1);
            } else if (k == 3)
                card_put(c, "CONNECT", btdev_busy() ? "DOWN: LET GO" : "UP", btdev_busy(), 0, 1, -1);
#endif
            else
                return 0;
            return 1;
        }
        if (page == 2) {                  /* Bluetooth audio */
            static const char *const NM[4] = {"AUDIO", "DEVICE", "CONNECT", "SCAN"};
            if (!FM1B_BT_AUDIO)
                return 0;
            if (k == 0)
                card_put(c, NM[0], gbta.on ? "ON" : "OFF", gbta.on, 0, 1, -1);
            else if (k == 1) {
                int_text(gbta.cur + 1, t);
                card_put(c, NM[1], t, gbta.cur, 0, 11, -1);
            } else
                card_put(c, NM[k], k == 2 ? "UP / DOWN" : "UP: LOOK", 0, 0, 1, -1);
            return 1;
        }
        if (page)
            return 0;
        if (k == 0) {
            card_put(c, "MIDI CLOCK", CL[proj.recq >> 2 & 3u], (int)(proj.recq >> 2 & 3u), 0, 2, -1);
            return 1;
        }
        if (k == 1) {
            card_put(c, "SOUNDS", sounds_pp() ? "OWN" : "SHARED", sounds_pp(), 0, 1, -1);
            return 1;
        }
        if (k == 3) {
            card_put(c, "MIX", mix_pp() ? "OWN" : "SHARED", mix_pp(), 0, 1, -1);
            return 1;
        }
#if FM1B_USB_HOST
        if (k == 2) {
            int host = plat_usb_host_state();
            card_put(c, "USB PORT", host == USBH_CONNECTED ? "MIDI READY" : usb_host_selected ? "WAITING" : "COMPUTER",
                     usb_host_selected, 0, 1, -1);
            return 1;
        }
#endif
        return 0;
    }
    case KD_GEN: {                        /* the 303 generator: page 0 ACID, 1 NOTES, 2 LINE */
        gen_t g;
        int lo, hi, i = (page % NGENPAGE) * 4 + k;
        if (tr == TR_DRUM) {              /* the drums' BEAT pages */
            const dgen_t dg = dgen_view();
            if (i >= 12 || !DGEN_LABEL[i])
                return 0;
            v = dgen_value(&dg, i, &lo, &hi, t);
            card_put(c, DGEN_LABEL[i], t, v, lo, hi, -1);
            return 1;
        }
        g = gen_view(tr);
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
static void sound_name(int tr, char *t)   /* the track's sound: the preset it started from (* if changed), or the sample */
{
    const track_t *ts = tset(tr);
    t[0] = 0;
    preset_name(ts->preset, t);
    if (ts->edited)
        text_cat(t, " *");
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
            glow_col(x0 + i, y, y0 + h);
        if (i)
            gline(x0 + i - 1, py, x0 + i, y, y0, y0 + h);
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
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
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
    {   /* (0.5) the newest voice's cutoff now, as the envelope, the LFOs and the matrix move it: yellow, its trail
         * fading behind */
        static uint8_t trail[24], tn;
        int k;
        if (syn_voices(tr) > 0) {
            int st = syn_probe(tr, 1) / 256, xl = x_of_hz(cut_hz(st < 0 ? 0 : st > 127 ? 127 : st), 10, 220);
            for (k = 23; k > 0; k--)
                trail[k] = trail[k - 1];
            trail[0] = (uint8_t)xl;
            tn = tn < 24 ? tn + 1 : 24;
        } else {
            tn = 0;
        }
        for (k = tn - 1; k >= 0; k--)
            cv_rect(trail[k], 10, k ? 1 : 2, 112, k ? mix565(C_BODY, C_NOW, 200 - k * 8) : C_NOW);
    }
}

static void pic_env(int tr, int pg)       /* the envelope's shape: attack, decay, sustain, release (ENV page pg) */
{
    const int8_t *ids = ENVPAGE_P[pg % NENVPAGE];
    int a = snd_value(tr, ids[0]), d = snd_value(tr, ids[1]), s = snd_value(tr, ids[2]), r = snd_value(tr, ids[3]);
    int wa = 6 + a * 56 / 127, wd = 6 + d * 56 / 127, ws = 36, wr = 6 + r * 56 / 127, tot = wa + wd + ws + wr;
    int x0 = 10, w = 220, top = 14, bot = 112, ys = bot - s * (bot - top) / 127, x1, x2, x3, x4, x;
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    x1 = x0 + wa * w / tot;
    x2 = x1 + wd * w / tot;
    x3 = x2 + ws * w / tot;
    x4 = x0 + w;
    for (x = x0; x <= x4; x++) {          /* filled under it */
        int y = x <= x1 ? bot - (x - x0) * (bot - top) / (x1 - x0 ? x1 - x0 : 1)
                : x <= x2 ? top + (x - x1) * (ys - top) / (x2 - x1 ? x2 - x1 : 1)
                : x <= x3 ? ys : ys + (x - x3) * (bot - ys) / (x4 - x3 ? x4 - x3 : 1);
        glow_col(x, y, bot);
    }
    gline(x0, bot, x1, top, top, bot);
    gline(x1, top, x2, ys, top, bot);
    gline(x2, ys, x3, ys, top, bot);
    gline(x3, ys, x4, bot, top, bot);
    cv_rect(x0, bot, w, 1, C_GRID);
    {
        static const char *const N[4] = {"A", "D", "S", "R"};
        int xs[5] = {x0, x1, x2, x3, x4}, i;
        for (i = 0; i < 4; i++) {
            cv_rect(xs[i + 1], top - 4, 1, bot - top + 8, C_GRID);
            text_c((xs[i] + xs[i + 1]) / 2, bot + 1, &FONT_XS, N[i], C_TXT2);
        }
    }
    {   /* (0.5) the voices sounding: each a yellow dot where it is (attack, decay, held, release) */
        uint8_t st[8];
        float lv[8], b[8], sus = (float)s / 127.0f;
        int n = syn_voice_view(tr, pg, st, lv, b, 8), k;
        for (k = 0; k < n; k++) {
            float l = lv[k];
            int xx = st[k] == 1 ? x0 + (int)(l * (float)(x1 - x0))
                     : st[k] == 2 ? (l > sus + 0.01f ? x1 + (int)((1.0f - l) / (1.0f - sus + 1e-6f) * (float)(x2 - x1))
                                                      : (x2 + x3) / 2)
                                  : x3 + (int)((sus > 0.01f ? 1.0f - l / sus : 1.0f - l) * (float)(x4 - x3));
            xx = xx < x0 ? x0 : xx > x4 ? x4 : xx;
            now_dot(xx, bot - (int)(l * (float)(bot - top)));
        }
    }
}

static void pic_osc(int tr)               /* the oscillator's wave, two cycles; the second, by MIX */
{
    int wave = snd_value(tr, SP_WAVE), mixv = snd_value(tr, SP_MIX), i, py = 0, py2 = 0;
    uint16_t col = TH_FG;
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
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
            if (i) {
                glow_col(10 + i, y, 66);
                gline(10 + i - 1, py, 10 + i, y, 16, 116);
            }
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
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
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
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
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
    /* (the atlas: on black) */
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
    /* (the atlas: on black) */
    for (k = 0; k < SYN_SLOTS; k++) {
        int y = 8 + k * 19, me = k == ui.mslot, on = tset(tr)->snd[SP_XM + 3 * k] && tset(tr)->snd[SP_XM + 3 * k + 2];
        char t[32], u[32], n[4] = {(char)('1' + k), 0, 0, 0};
        if (me)                           /* the slot on the knobs: selected */
            rrect(8, y, 224, 18, 4, C_VIOLET);
        cv_text(14, y + 3, &FONT_XS, n, me ? C_INK : C_TXT2);
        slot_text(tr, k, t);
        fit(&FONT_XS, t, 158, u);
        cv_text(30, y + 3, &FONT_XS, u, me ? C_INK : on ? TH_TXT : C_TXT2);
        {   /* (0.5) its move now, on the newest voice: a bar either way from the middle, yellow */
            int bw = (int)(syn_slot_now(tr, k) * 30.0f);
            bw = bw > 30 ? 30 : bw < -30 ? -30 : bw;
            cv_rect(200, y + 3, 1, 12, me ? C_INK : C_GRID);
            if (bw > 0)
                cv_rect(201, y + 6, bw, 6, me ? C_INK : C_NOW);
            else if (bw < 0)
                cv_rect(200 + bw, y + 6, -bw, 6, me ? C_INK : C_NOW);
        }
    }
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
    band_set(KIT_RGB[proj.kit], "DR", title, ctx && ctx[0] ? ctx : drum_kit_title(), npg, pg);
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
    text_cat(hint, t + 6);                /* (STEPS 1-16: the steps, then the pad) */
    text_cat(hint, " ");
    text_cat(hint, trial_pad_name(drum_model(ui.pad), ui.pad));
    if (drum_lane_muted(ui.pad))
        text_cat(hint, " MUTED");
    kit_band(ui.dtool || ui.held_step >= 0 ? "DRUM HIT" : "DRUM STEPS", hint, npages(), ui.spage);
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
        cv_text(3, y - 2, &FONT_XS, row < NPADS ? trial_pad_name(drum_model(row), row) : "AC",
                muted ? TH_RED : sel || (row < NPADS && hit[row] > 0.4f) ? C_INK : C_TXT2);
        if (row < NPADS)                  /* (0.5) its level now: a thin meter, HEAT */
            vheat(23, y + 1, 2, 9, (int)(hit[row] * 9.0f + 0.5f));
        for (k = 0; k < 16; k++) {
            int s = ui.spage * 16 + k, x = 27 + k * 13;
            if (s >= p->len)
                continue;
            if (!(k & 3))                 /* the beats (steps 1, 5, 9, 13 of the page): a column of their own */
                cv_rect(x - 1, y, 13, 11, mix565(sel ? C_PANEL : C_BODY, TH_INK, 22));
            if (sq.playing && s == st)
                cv_rect(x - 1, y, 13, 11, PLAYHEAD);
            if (bit_get(bits, s)) {       /* (an accented hit: orange) */
                rrect(x, y + 1, 11, 9, 2, row < NPADS && !muted && bit_get(p->acc, s) ? gradc(GR_HEAT, 40, 47) : on);
                if (row < NPADS && step_locked(p, s, row)) {
                    int j = lock_find(p, s, row, LK_SUB), q;
                    if (j < 0)
                        cv_rect(x + 7, y + 1, 4, 3, C_WHITE2);
                    for (q = 1; j >= 0 && q < p->lk[j].val; q++)   /* substeps: the hit cut in as many */
                        cv_rect(x + q * 11 / p->lk[j].val, y + 1, 1, 9, C_CANVAS);
                }
                if (row == ui.pad && s == ui.held_step)
                    cv_rect(x + 3, y + 3, 5, 5, C_WHITE2);
            } else {
                cv_rect(x + 4, y + 4, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            }
        }
    }
    cards_of(ui.dtool || ui.held_step >= 0 ? KD_DHIT : KD_SEQ, 0);
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
    {
        char cx[44];
        cx[0] = 0;
        text_cat(cx, t + 6);              /* (STEPS 1-16: the steps, then what a key does) */
        text_cat(cx, " ");
        text_cat(cx, u);
        band_set(col, TRK_NAME[tr], STOOL_NAME[ui.stool % NSTOOL], cx, npages(), ui.spage);
    }
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
        if (sq.playing && s == st) {
            cv_rect(x - 1, 0, 13, H, PLAYHEAD);
            playflag(x - 1, 13);
        }
        if (!ns->note[0]) {
            cv_rect(x + 4, H + 1, 3, 3, (k & 3) ? C_GRID : C_TXT2);
            continue;
        }
        {   /* substeps: as many yellow dots under it */
            int j = ilock_find(p, s, IL_SUB), q;
            for (q = 0; j >= 0 && q < p->lk[j].val; q++)
                cv_rect(x + q * 3, H + 1, 2, 3, C_NOW);
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
                  s != ui.held_step ? mix565(C_BODY, TH_FG, 110 + ns_vel(ns))
                  : ui.hnote && hnote_at(ns) != i ? TH_FG : hnote_at(ns) == i ? C_NOW : TH_HI);
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
    if (ui.stool == ST_HARM || ui.stool == ST_MOD || (ui.held_step < 0 && ui.stool != ST_GROOVE))
        cards_of(KD_STOOL, ui.stool);
    else if (ui.held_step >= 0)
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
    band_set(col, TRK_NAME[tr], ui.genpage == 0 ? "ACID" : ui.genpage == 1 ? "ACID: NOTES" : ui.genpage == 2 ? "ACID: LINE"
             : "KEEP", t, NGENPAGE, ui.genpage);
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
        if (s < p->len && ui.genpage == 3 && s == gcur_of(tr))   /* KEEP: the step its knobs are on */
            cv_rect(28 + k * 13 - 1, 0, 13, 114, mix565(C_BODY, TH_INK, 45));
        if (s < p->len && sq.playing && s == st) {
            cv_rect(28 + k * 13 - 1, 0, 13, 110, PLAYHEAD);
            playflag(28 + k * 13 - 1, 13);
        }
        if (s < p->len && gkept(tr, s))   /* a kept step: a bar under its column */
            cv_rect(28 + k * 13, 111, 11, 3, C_WHITE2);
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
    if (ui.genpage == 3) {
        cv_text(6, 116, &FONT_XS, "KEPT STEPS STAY: NEW, MUTATE, KNOBS", TH_TXT);
    } else {
        cv_text(6, 116, &FONT_XS, "OCT+ NEW LINE", TH_TXT);
        cv_text(100, 116, &FONT_XS, "OCT- MUTATE", TH_TXT);
    }
    if (p->len > 16 && ui.genpage != 3) {
        steps_text(t);
        text_r(236, 116, &FONT_XS, t, C_TXT2);
    }
    cards_of(KD_GEN, ui.genpage);
}

static void page_dgen(void)               /* the drums' beat generator: the beat (16 steps of the page), a row a pad */
{
    static const char *const NM[NDGENPAGE] = {"BEAT", "BEAT: MORE", "BEAT: LINE"};
    const dpart_t *p = dcur();
    const dgen_t g = dgen_view();
    int st = sq.t[TR_DRUM].step, k, s, pad, gp = ui.genpage < NDGENPAGE ? ui.genpage : NDGENPAGE - 1;
    char t[32], u[12];
    t[0] = 0;
    if (dgen_of()->seed) {
        text_cat(t, "SEED ");
        int_text(g.seed, u);
        text_cat(t, u);
        if (g.mut) {
            text_cat(t, " M");
            int_text(g.mut, u);
            text_cat(t, u);
        }
        text_cat(t, "  ");
        text_cat(t, DSTYLE_NAME[g.style % NDSTYLES]);
    } else {
        text_cat(t, "OCT+ OR A KNOB: MAKE A BEAT");
    }
    band_set(C_ACID, "DR", NM[gp], t, NDGENPAGE, gp);
    cv_text(3, 0, &FONT_T, "CY", C_TXT2);
    cv_text(3, 15, &FONT_T, "HH", C_TXT2);
    cv_text(3, 36, &FONT_T, "PC", C_TXT2);
    cv_text(3, 61, &FONT_T, "SD", C_TXT2);
    cv_text(3, 72, &FONT_T, "BD", C_TXT2);
    cv_text(3, 86, &FONT_T, "AC", C_TXT2);
    for (k = 0; k < 16; k++) {
        int x = 28 + k * 13;
        s = ui.spage * 16 + k;
        if (s >= p->len)
            continue;
        if (sq.playing && s == st) {
            cv_rect(x - 1, 0, 13, 100, PLAYHEAD);
            playflag(x - 1, 13);
        }
        for (pad = 0; pad < NPADS; pad++) {   /* the bass drum at the bottom, the cymbals at the top */
            int y = 2 + (NPADS - 1 - pad) * 7;
            if (bit_get(p->hit[pad], s))
                rrect(x, y, 11, 5, 1, bit_get(p->acc, s) ? C_WHITE2 : TH_FG);
            else
                cv_rect(x + 4, y + 2, 3, 1, (k & 3) ? C_GRID : C_TXT2);
        }
        if (bit_get(p->acc, s))
            rrect(x + 1, 87, 9, 9, 2, C_WHITE2);
        else
            cv_rect(x + 4, 90, 3, 3, C_GRID);
    }
    cv_text(6, 116, &FONT_XS, "OCT+ NEW BEAT", TH_TXT);
    cv_text(100, 116, &FONT_XS, "OCT- MUTATE", TH_TXT);
    if (p->len > 16) {
        steps_text(t);
        text_r(236, 116, &FONT_XS, t, C_TXT2);
    }
    cards_of(KD_GEN, gp);
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
        if (ui.rec && sq.playing) {
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
            text_cat(ttl, "  ");
            text_cat(ttl, ctx);
            band_set(col, TRK_NAME[tr], "KEYS", ttl, 0, 0);
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

/* The hit editor owns the entire 240px screen. Each scanout is at most 104
 * rows and reuses the stock canvas. A resting editor requires no LCD blits. */
/* A DrumEngine voice's page (the Acid Dub Deck look, 0.5): the header "GM / DR", KICK / BODY and its sound; the work
 * area the lane's voice drawn (an illustration, not the DSP: a struck tone, noise, or a clap's bursts, its decay and
 * pitch from the lane's own DECAY and TUNE); the knob strip its four knobs. (SELECT its pages, PRESETS its sounds, SEL
 * held: fine steps.) */
static const char *const V6_PGNAME[NPADS][2] = {{"BODY", "TONE"}, {"BODY", "TONE"}, {"BODY", "MODEL"}, {"BODY", "MODEL"},
    {"BODY", "MODEL"}, {"BODY", "MODEL"}, {"TONE", "MODEL"}, {"TONE", "MODEL"}, {"TONE", "MODE"}, {"SHAPE", "MODEL"},
    {"SHAPE", "MODEL"}};

static int has_word(const char *s, const char *w)   /* w in s (no libc) */
{
    int i, j;
    for (i = 0; s[i]; i++) {
        for (j = 0; w[j] && s[i + j] == w[j]; j++)
            ;
        if (!w[j])
            return 1;
    }
    return 0;
}

static float sin_q(float x)               /* a sine, near enough to draw (Bhaskara): x in turns */
{
    float p;
    x -= (float)(int)x;
    if (x < 0.0f)
        x += 1.0f;
    p = x < 0.5f ? x * 2.0f : (x - 0.5f) * 2.0f;
    p = 16.0f * p * (1.0f - p) / (5.0f - 4.0f * p * (1.0f - p));
    return x < 0.5f ? p : -p;
}

static void v6_voice_art(int pad, const KitPatch *patch)
{
    int j, x, total = kit_param_count(pad), py = 56, kind;
    float tune = 0.5f, decay = 0.5f, e = 1.0f, r, ph = 0.0f, cyc;
    uint32_t n = 0x9E3779B9u;
    for (j = 0; j < total; j++) {         /* the lane's TUNE and DECAY, 0..1 */
        const KitParamSpec *sp = kit_param_spec(pad, j);
        int t = kit_param_get(patch, pad, j), span = sp->max_tick - sp->min_tick;
        float v = span > 0 ? (float)(t - sp->min_tick) / (float)span : 0.5f;
        if (has_word(sp->label, "Tune") || has_word(sp->label, "TUNE"))
            tune = v;
        else if (has_word(sp->label, "Decay") || has_word(sp->label, "DECAY"))
            decay = v;
    }
    kind = pad == 2 ? 1 : pad == 3 ? 2 : pad >= 7 && pad <= 9 ? 3 : 0;   /* a tone; the snare; the clap; hats, cymbal */
    cyc = (pad == 1 ? 3.0f : pad >= 4 && pad <= 6 ? 6.0f : 8.0f) + tune * 10.0f;
    r = 1.0f - (2.0f + (1.0f - decay) * 14.0f) / 216.0f;   /* the decay a pixel */
    grid_bg();
    for (x = 0; x < 216; x++) {
        float t = (float)x / 216.0f, a, nz;
        int y;
        n = n * 1664525u + 1013904223u;
        nz = (float)(int32_t)n * (1.0f / 2147483648.0f);
        ph += cyc / 216.0f * (pad == 1 ? 1.0f + 2.0f * e : 1.0f);   /* (the kick: its pitch falling) */
        a = kind == 0 ? sin_q(ph) : kind == 1 ? 0.45f * sin_q(ph) + 0.65f * nz : nz;
        if (kind == 2)                    /* the clap: three bursts, then its tail */
            a *= t < 0.04f || (t > 0.07f && t < 0.11f) || (t > 0.14f && t < 0.18f) ? 1.3f : t < 0.18f ? 0.2f : 0.8f;
        e *= r;
        y = 56 - (int)(a * e * 46.0f);
        y = y < 8 ? 8 : y > 104 ? 104 : y;
        if (x)
            cv_line(12 + x - 1, py, 12 + x, y, gradc(GR_HEAT, 215 - x, 215));
        py = y;
    }
}

static void v6_label(const char *in, char *out)   /* a knob's name for its cell: the long ones shortened */
{
    static const char *const LONG[][2] = {{"PITCH DROP AMOUNT", "DROP AMT"}, {"PITCH DROP TIME", "DROP TIME"},
        {"CLICK LEVEL", "CLICK LVL"}, {"BRIGHTNESS", "BRIGHT"}};
    unsigned i;
    out[0] = 0;
    text_cat(out, in);
    upper(out);
    for (i = 0; i < sizeof LONG / sizeof LONG[0]; i++)
        if (has_word(out, LONG[i][0])) {
            out[0] = 0;
            text_cat(out, LONG[i][1]);
            return;
        }
}

static void cap_set(int pad, uint8_t *env, int len, int step)   /* the drums' capture (trial_capture), when it changes */
{
    static int lp = -1;
    static uint8_t *le;
    uint32_t f;
    if (pad == lp && env == le)
        return;
    lp = pad;
    le = env;
    if (env) {                            /* (the scope's buffer lent to it) */
        sc.state = 0;
        sc.src = SC_NONE;
    }
    f = plat_lock();
    trial_capture(pad, env, len, step);
    plat_unlock(f);
}

static void page_v6(void)
{
    int pad = ui.pad + 1, total = kit_param_count(pad), first, k, recipe, fine = (int)(ui.btn >> B_SEL & 1u);
    KitPatch current;
    char ttl[24], ctx[32];
    if (ui.epage >= v6_pages())
        ui.epage = 0;
    first = 4 * ui.epage;
    trial_snapshot(pad, &current);
    recipe = kit_preset_match(&current, pad);
    ttl[0] = 0;
    text_cat(ttl, v6_voice_name());
    text_cat(ttl, " / ");
    text_cat(ttl, ui.epage < 2 ? V6_PGNAME[ui.pad][ui.epage] : "MORE");
    ctx[0] = 0;
    text_cat(ctx, v6_recipe_name(pad, recipe));
    v6_append_tom_pitch(ctx, " ", pad, &current);
    band_set(KIT_RGB[proj.kit], "DR", ttl, ctx, v6_pages(), ui.epage);
    for (k = 0; k < 4; k++) {             /* the four knobs: the page's */
        int param = first + k < total ? v6_param(pad, first + k) : -1, tick, i, o;
        const KitParamSpec *spec;
        char label[24], value[24];
        cards[k].on = 0;
        if (param < 0)
            continue;
        spec = kit_param_spec(pad, param);
        tick = kit_param_get(&current, pad, param);
        v6_label(spec->label, label);
        if (!kit_param_editable(&current, pad, param)) {
            card_put(&cards[k], label, "FIXED", 0, 0, 0, -1);
            cards[k].ro = 1;
            continue;
        }
        v6_value_text(pad, param, tick, value);
        for (i = o = 0; value[i]; i++)    /* (52.0 Hz: 52.0Hz, to fit the cell) */
            if (value[i] != ' ')
                value[o++] = value[i];
        value[o] = 0;
        if (kit_param_option(pad, param, tick))
            upper(value);
        card_put(&cards[k], label, value, tick, spec->min_tick, spec->max_tick, -1);
    }
    cap_set(pad, (uint8_t *)sc.buf, 216, 160);   /* (0.5) its next hit captured: 216 peaks, 0.78 s */
    {
        int n = trial_capture_pos(), i;
        const uint8_t *env = (const uint8_t *)sc.buf;
        if (n > 0) {                      /* the last hit as it sounded: -48..0 dB, mirrored, HEAT by time */
            grid_bg();
            for (i = 0; i < n && i < 216; i++) {
                int h = db_h((float)env[i] * (1.0f / 255.0f), 46, 48.0f);
                cv_rect(12 + i, 56 - h, 1, 2 * h + 1, gradc(GR_HEAT, 215 - i, 215));
            }
            foot("LAST HIT", fine ? "FINE ON" : "SEL: FINE", fine);
        } else {
            v6_voice_art(pad, &current);
            foot("LANE VOICE: STRIKE IT", fine ? "FINE ON" : "SEL: FINE", fine);
        }
    }
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
        const int model = drum_model(ui.pad);
        cv_text(7, 75, &FONT_B, model >= KIT_NUM ? drums_model_name(model) : trial_pad_name(model, ui.pad), TH_FG);
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
    text_cat(t, trial_pad_name(drum_model(ui.pad), ui.pad));
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
        text_c(37, 6, &FONT_L, trial_pad_name(drum_model(ui.pad), ui.pad), h > 0.5f ? C_INK : kc);
        text_c(37, 28, &FONT_XS, drums_kit_name(drum_model(ui.pad)), h > 0.5f ? C_INK : kc);
    }
    for (k = 0; k < 16; k++) {            /* its steps on this page */
        int s = ui.spage * 16 + k, x = 76 + k * 10;
        if (s >= pat->len)
            continue;
        if (sq.playing && s == st)
            cv_rect(x - 1, 8, 10, 28, PLAYHEAD);
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
    int tr = ui.trk, page = env ? ui.envpage % NENVPAGE : ui.epage;
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
    band_set(col, TRK_NAME[tr], env ? ENVPAGE_NAME[page] : EPAGE_NAME[page], t,
             env ? NENVPAGE : inst_epages(tr), page);
    if (env) {
        pic_env(tr, page);
    } else if (page == 0) {               /* VOICE: the type, the voices, the level */
        float lv;
        char u[16];
        levels_take();
        lv = lvl[tr];
        /* (the atlas: on black) */
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
    } else if (page == 1) {               /* (sounding: the track's own sound, live; quiet: the settings drawn) */
        if (!track_scope(tr, "OSC"))
            pic_osc(tr);
    } else if (page == 3) {
        pic_filter(tr);
    } else if (page == EP_OSC2) {
        if (!track_scope(tr, "OSC 1 + 2"))
            pic_osc2(tr);
    } else if (page == EP_SHAPER) {
        if (!track_scope(tr, "SHAPED"))
            pic_shaper(tr);
    } else if (page == EP_MACRO) {
        pic_macro(tr);
    } else {
        page_map(KD_INST, inst_epages(tr), page, EPAGE_NAME, 8, 112);
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
            grid_bg();                     /* (the atlas: curves on a grid, on black) */
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
            {                             /* (0.5) each voice's LFO 2 now: a yellow dot in the first cycle */
                uint8_t st2[8];
                float ph2[8], val2[8];
                int nv = syn_voice_view(tr, 3, st2, ph2, val2, 8), k2;
                for (k2 = 0; k2 < nv; k2++)
                    now_dot(10 + (int)(ph2[k2] * 110.0f), 64 - (int)(val2[k2] * 50.0f));
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
    band_set(col, tr == TR_DRUM ? "DR" : TRK_NAME[tr],
             tr == TR_DRUM ? (pg ? "DRUM LFO MORE" : "DRUM LFO") : LFO_PAGE_NAME[pg], t, lfo_pages(tr), pg);
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    cv_rect(10, 64, 220, 1, C_GRID);
    cv_rect(120, 6, 1, 101, PLAYHEAD);    /* now: the middle, where the dot stays */
    if (amp < 0.25f)
        amp = 0.25f;
    {   /* a scope (0.5): the wave scrolls past the dot, a cycle 110 pixels wide, now in the middle, what's to come at
         * the right. S&H and SMOOTH draw this cycle's value and the next's as the LFO has them (the one before as it
         * was; further off, made up) */
        static float last_a[NTRK], prev_a[NTRK];
        const float ph = lfo_rt[tr].ph, A = lfo_rt[tr].a, B = lfo_rt[tr].b;
        if (A != last_a[tr]) {
            prev_a[tr] = last_a[tr];
            last_a[tr] = A;
        }
        for (c = 0; c < 220; c++) {
            float pos = ph + (float)(c - 110) / 110.0f, f, v, va, vb;
            int k = (int)(pos + 8.0f) - 8, y;   /* the cycle, from this one (0) */
            f = pos - (float)k;
            va = k == 0 ? A : k == 1 ? B : k == -1 ? prev_a[tr] : RND[(k + 12) % 6];
            vb = k == -1 ? A : k == 0 ? B : RND[(k + 13) % 6];
            v = (l->shape == LS_SH || l->shape == LS_SMOOTH) ? lfo_wave(l->shape, f, va, vb) : lfo_wave(l->shape, f, 0.0f, 0.0f);
            if (l->depth < 0)
                v = -v;
            y = 64 - (int)(v * amp * 52.0f);
            if (c && l->depth) {
                glow_col(10 + c, y, 64);
                gline(10 + c - 1, py, 10 + c, y, 12, 116);
            } else if (c) {
                thick(10 + c - 1, py, 10 + c, y, wc);
            }
            py = y;
        }
    }
    {   /* where it is now: the dot, fixed in the middle, yellow; up and down with the LFO's value */
        float v = lfo_rt[tr].out * (l->depth < 0 ? -1.0f : 1.0f);
        int y = 64 - (int)(v * amp * 52.0f);
        rrect(115, y - 5, 11, 11, 5, C_NOW);
        rrect(117, y - 3, 7, 7, 3, C_INK);
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
        /* (the atlas: on black) */
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
    /* (the atlas: on black) */
    t[0] = 0;
    text_cat(t, ARP_MODE_NAME[mode]);
    cv_text(12, 8, &FONT_L, t, mode ? TH_TXT : C_GRID);
    for (i = 0; i < 12; i++) {            /* its order, drawn: rising, falling, both, scattered, as played */
        static const uint8_t RN[12] = {3, 7, 1, 5, 2, 6, 4, 0, 6, 2, 5, 1}, PL[12] = {0, 4, 2, 6, 0, 4, 2, 6, 0, 4, 2, 6};
        int v = mode == AM_UP ? i % 4 * 2 : mode == AM_DOWN ? 6 - i % 4 * 2 : mode == AM_UPDOWN ? (i % 6 < 4 ? i % 6 : 6 - i % 6) * 2
              : mode == AM_RANDOM ? RN[i] : PL[i];
        int x = 14 + i * 18, h = 12 + v * 7;
        if (!mode)
            rrect(x, 120 - h, 14, h, 3, C_GRID);
        else if (i == (int)(arp[tr].idx % 12u))   /* the step now: yellow */
            rrect(x, 120 - h, 14, h, 3, C_NOW);
        else
            vheat(x, 66, 14, 54, h);
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
    kit_band("LANE MUTE", track_is_muted(TR_DRUM) ? "DRUM TRACK MUTED" : "", 0, 0);
    hits_take();
    for (k = 0; k < NPADS; k++) {
        int x = 3 + (k % 4) * 59, y = 4 + (k / 4) * 36, muted = (int)(p->mute >> k & 1u);
        uint16_t bg = muted ? C_PANEL : hit[k] > 0.05f ? mix565(C_ACID, C_NOW, (int)(hit[k] * 256.0f)) : C_ACID;
        rrect(x, y, 56, 33, 6, bg);
        cv_text(x + 7, y + 1, &FONT_B, trial_pad_name(drum_model(k), k), muted ? C_TXT2 : C_INK);
        cv_text(x + 7, y + 17, &FONT_XS, muted ? "MUTE" : "ON", muted ? C_MUTED : C_INK);
        if (k == ui.pad)                  /* the pad picked: a mark */
            rrect(x + 40, y + 6, 10, 10, 6, muted ? kc : C_INK);
    }
    foot("BLACK: MUTE LANE", "OCT+: ALL ON", 0);
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
    } else if (sq.playing && sq.queued >= 0) {   /* the pattern to come, apart from the one playing */
        text_cat(c, "NEXT ");
        two_digits(sq.queued + 1, u);
        text_cat(c, u);
    } else {
        text_cat(c, "CPU ");
        int_text((int32_t)plat_cpu_pct(), u);
        text_cat(c, u);
        text_cat(c, "%  V");
        int_text(syn_voices(-1), u);
        text_cat(c, u);
    }
    {
        char pl[16];
        pl[0] = 0;
        text_cat(pl, sq.playing ? "PLAYING " : "PATTERN ");
        two_digits(proj.pat + 1, u);
        text_cat(pl, u);
        band_set(C_CREAM, "ALL", "PATTERNS", c[0] ? c : pl, 0, 0);
    }
    levels_take();
    rrect(32 + proj.pat * 13, 0, 14, 132, 4, C_PANEL);   /* the focused scene's column */
    for (k = 0; k < NPAT; k++) {          /* the scene numbers */
        two_digits(k + 1, u);
        text_c(39 + k * 13, 0, &FONT_T, u[0] == '0' ? u + 1 : u,
               k == proj.pat ? TH_TXT : sq.playing && k == sq.queued ? (blink ? C_NOW : TH_TXT) : C_RULE);
    }
    for (t = 0; t < NTRK; t++) {
        int y = 13 + t * 24, muted = (int)(proj.tmute >> t & 1u), h;
        uint16_t tc = trk_rgb(t);
        /* the track: its name, muted or stopped (purple), its level now (black on acid) */
        rrect(1, y, 29, 22, 4, muted || clip[t] < 0 ? C_PANEL : tc);
        text_c(15, y, &FONT_S, t == TR_DRUM ? drum_kit_short() : TRK_NAME[t], muted ? C_RULE : clip[t] < 0 ? C_TXT2 : C_INK);
        h = db_h(lvl[t], 23, 48.0f);
        cv_rect(4, y + 17, 23, 3, muted || clip[t] < 0 ? C_GRID : mix565(tc, C_INK, 90));
        if (h > 0)
            cv_rect(4, y + 17, h, 3, muted || clip[t] < 0 ? C_NOW : C_INK);
        if (tk == t)                      /* its key held: selected */
            rrect(1, y, 3, 22, 1, C_NOW);
        for (k = 0; k < NPAT; k++) {      /* its clips: empty purple, with a part acid (dim), playing acid, coming yellow */
            int x = 33 + k * 13, has = t == TR_DRUM ? !dpart_empty(&proj.p[k].d) : !ipart_empty(&proj.p[k].t[t - 1]);
            int on = clip[t] == k, q = qclip[t] == k || (sq.queued == k && qclip[t] == -1);
            int stopping = on && qclip[t] == -2;
            uint16_t fill = has ? mix565(C_BODY, tc, 150) : k == proj.pat ? mix565(C_DUSK, C_TXT2, 40) : C_DUSK;
            if (on)
                fill = (stopping && blink) ? mix565(C_BODY, tc, 150) : tc;
            else if (q)
                fill = blink ? C_NOW : mix565(C_BODY, C_NOW, 120);
            rrect(x, y, 12, 22, 3, fill);
            if (on && sq.playing && !muted) {   /* where it is: a bar up the clip */
                int len = t == TR_DRUM ? proj.p[k].d.len : proj.p[k].t[t - 1].len, st = sq.t[t].step;
                int ph = len > 0 && st >= 0 ? (st + 1) * 18 / len : 0;
                if (ph > 0)
                    cv_rect(x + 4, y + 20 - ph, 4, ph, C_INK);
            }
        }
    }
    {
        char f[24], u2[4];
        f[0] = 'P';
        two_digits(proj.pat + 1, f + 1);
        text_cat(f, sq.playing ? " PLAYING" : " STOPPED");
        if (sq.playing && sq.queued >= 0) {
            text_cat(f, ", NEXT ");
            two_digits(sq.queued + 1, u2);
            text_cat(f, u2);
        }
        foot(tk >= 0 ? "WHITE: CLIP" : "WHITE: SCENE", f, sq.playing);
    }
    if (ui.scene_key >= 0) {              /* a scene key held: its follow action */
        char t2[24];
        t2[0] = 'P';
        two_digits(ui.scene_key + 1, t2 + 1);
        text_cat(t2, " FOLLOW");
        band_set(C_CREAM, "ALL", "SCENE FOLLOW", t2, 0, 0);
        cards_of(KD_FOLLOW, ui.scene_key);
    } else if (ui.pf_key >= 0 && (perf.want >> ui.pf_key & 1u)) {   /* a perform key held: its knob */
        cards_of(KD_PF, ui.pf_key);
    } else {
        cards_of(KD_SEQ, 0);
    }
}

/* SETUP turned with SELECT (0.5, CPU): the render's time by stage (app_render's cs_us), each as a share of the time a
 * block has (APP_BLOCK frames at 44.1 kHz), a bar and a number; a tick on the bar at its worst block lately (cs_pk);
 * the total, the total's worst block (PEAK), and the blocks that came late since the start. The reverb and delay and
 * the master bus run on the second core (CORE2: post_job), beside the first's; the rest, and TOTAL, are the first
 * core's, which waits for the second at the end of a block (OTHER) and for its half of the synth's voices (SYNTH) */
/* the last late block (0.5): seen by the main loop a moment after (the count went up): the stage with the worst
 * block lately and the page on screen then. The CPU page names them */
static struct {
    uint32_t seen, n;
    uint8_t stage, page;
} late;

static void late_watch(void)
{
    uint32_t x = plat_xruns();
    int i, w = 0;
    if (x == late.seen)
        return;
    if (x > late.seen) {
        for (i = 1; i < CS_TOTAL; i++)
            if (cs_pk[i] > cs_pk[w])
                w = i;
        late.stage = (uint8_t)w;
        late.page = ui.page;
        late.n++;
    }
    late.seen = x;
}

static void page_cpu(void)
{
    static const char *const NM[CS_NUM] = {"DRUMS", "SYNTH", "DRUM BUS", "VERB+DLY CORE2", "MASTER CORE2", "OTHER",
                                           "TOTAL"};
    const float per = 100.0f * 44100.0f / (1.0e6f * (float)APP_BLOCK);   /* us a block -> % of its time */
    char t[24], u[8];
    int i;
    t[0] = 0;
    text_cat(t, "PEAK ");
    int_text((int32_t)(cs_pk[CS_TOTAL] * per + 0.5f), u);
    text_cat(t, u);
    text_cat(t, "% LATE ");
    int_text((int32_t)plat_xruns(), u);
    text_cat(t, u);
    band_set(C_CREAM, "ALL", "CPU BY STAGE", t, 0, 0);
    rrect(3, 2, 234, BODY_H - 4, 6, C_PANEL);
    for (i = 0; i < CS_NUM; i++) {
        int y = 4 + i * 13, pct = (int)(cs_us[i] * per + 0.5f), w = pct > 100 ? 100 : pct;
        cv_text(8, y - 2, &FONT_XS, NM[i], i == CS_TOTAL ? TH_TXT : C_TXT2);
        cv_rect(104, y + 3, 100, 8, C_GRID);
        if (w > 0)
            hheat(104, y + 3, 100, 8, w);
        {   /* the worst block lately: a tick (at the end when past 100 %) */
            int pk = (int)(cs_pk[i] * per + 0.5f);
            pk = pk > 99 ? 99 : pk;
            if (pk > w)
                cv_rect(104 + pk, y + 2, 2, 10, pk >= 99 ? TH_RED : TH_TXT);
        }
        {   /* its budget: a notch under the bar (the effects': the two rows together); the number red for a second
             * after the budget gave something up (a drum sound cut, a voice shed, an effect out), and while one is out */
            const int sec = i == CS_DRUMS ? 0 : i == CS_SYNTH ? 1 : i == CS_FXIN || i == CS_FX ? 2 : -1;
            const int bud = sec == 0 ? BUD_DRUMS : sec == 1 ? BUD_SYNTH : BUD_FX;
            int hot = 0;
            if (sec >= 0) {
                if (sec < 2)
                    cv_rect(104 + bud, y + 11, 1, 3, TH_TXT);
                hot = (gov.act[sec] && gov.clock - gov.act[sec] < 172u) || (sec == 2 && gov.fx_shed);
            }
            int_text(pct, u);
            text_cat(u, "%");
            text_r(232, y - 2, &FONT_XS, u, hot ? TH_RED : TH_TXT);
        }
    }
    {   /* (0.5) the total over the last 22 s; under its name the last late block: its longest stage and the page */
        hist_feed(4, (int)(cs_us[CS_TOTAL] * per * 2.55f), 100);
        cv_text(8, 96, &FONT_XS, late.n ? "LAST LATE:" : "TOTAL, 22 S", late.n ? TH_RED : C_TXT2);
        if (late.n) {
            cv_text(8, 107, &FONT_XS, NM[late.stage % CS_NUM], TH_TXT);
            cv_text(8, 118, &FONT_XS, PAGE_NAME[late.page], C_TXT2);
        }
        hist_draw(104, 96, 128, 30);
    }
    cards_of(KD_SETUP, 0);
}

static void page_gbta(void)                /* SETUP's third view: Bluetooth audio's switch, state and devices */
{
    static const char *const ST[12] = {"OFF", "LOOKING FOR DEVICES", "FOUND: K2 PICK, K3 CONNECT", "NONE FOUND: K4 LOOK AGAIN",
                                       "CONNECTING", "LINKED", "SETTING UP", "PLAYING THROUGH IT", "IT REFUSED",
                                       "NO DEVICE YET: K4 LOOK", "FAILED", "READY: K3 CONNECT, K4 LOOK"};
    char nm[BTA_NAME];
    int i, kn, n = FM1B_BT_AUDIO ? gbta_count() : 0, st = FM1B_BT_AUDIO ? plat_bta_state() : 0, top;
    band_set(C_CREAM, "ALL", "BT AUDIO", "SELECT: CPU", 0, 0);
    gbta.seen = (uint8_t)(st * 16 + n);
    if (gbta.cur >= n)
        gbta.cur = (uint8_t)(n ? n - 1 : 0);
    rrect(3, 2, 234, 36, 6, C_PANEL);
    cv_text(10, 4, &FONT_XS, "SOUND TO BLUETOOTH HEADPHONES / SPEAKER", C_TXT2);
    cv_text(10, 19, &FONT_S, !FM1B_BT_AUDIO ? "NOT IN THIS BUILD" : !gbta.on ? (gbta.giving ? "TURNING OFF" : "OFF")
                             : ST[st < 12 ? st : 0], TH_TXT);
    top = gbta.cur > 3 ? gbta.cur - 3 : 0;
    for (i = 0; i < 4 && gbta.on && gbta_entry(top + i, nm, &kn); i++) {
        const int y = 42 + i * 17, sel = top + i == gbta.cur;
        if (sel)
            rrect(3, y, 234, 16, 4, TH_FG);
        cv_text(10, y, &FONT_S, nm[0] ? nm : "(NO NAME)", sel ? C_BODY : TH_TXT);
        text_r(230, y + 1, &FONT_XS, kn >= 0 ? "SAVED" : "FOUND", sel ? C_BODY : C_TXT2);
    }
    cv_text(6, 116, &FONT_XS, gbta.on ? "LATE AT THE EAR: FOR LISTENING. UNDO: 1 STEP" : "K1 ON: UNDO KEEPS 1 STEP WHILE IT IS", TH_TXT);
    cards_of(KD_SETUP, 2);
}

static int ble_word_now;                  /* what the MIDI view last showed of Bluetooth (the main loop redraws on a change) */
static int ble_word(void)                 /* 0 not built, 1 off, 2 starting, 3 waiting, 4 connected, 5 failed */
{
#if FM1B_BLE
    const int st = plat_ble_state();
    return !ble_wanted() ? 1 : plat_ble_connected() ? 4 : st == BLE_READY ? 3 : st == BLE_FAILED ? 5 : 2;
#else
    return 0;
#endif
}

static void page_midi(void)               /* SETUP's second view: SEQ OUT and Bluetooth MIDI */
{
    static const char *const BW[6] = {"NOT IN THIS BUILD", "OFF", "STARTING", "ON: PAIR FROM A COMPUTER OR PHONE",
                                      "CONNECTED", "THE RADIO DID NOT START"};
    band_set(C_CREAM, "ALL", "MIDI", "SELECT: BT AUDIO", 0, 0);
    rrect(3, 2, 234, 40, 6, C_PANEL);
    cv_text(10, 4, &FONT_XS, "SEQUENCER TO MIDI (SEQ OUT)", C_TXT2);
    cv_text(10, 20, &FONT_S, seq_out() ? "SENT: DRUMS CH 10, T2-T4 CH 2-4" : "OFF: ONLY THE KEYS YOU PLAY", TH_TXT);
    rrect(3, 46, 234, 40, 6, C_PANEL);
    cv_text(10, 48, &FONT_XS, "BLUETOOTH MIDI", C_TXT2);
    ble_word_now = ble_word();
    cv_text(10, 64, &FONT_S, BW[ble_word_now], TH_TXT);
    rrect(3, 90, 234, 40, 6, C_PANEL);
#if FM1B_BLE_CENTRAL
    {   /* a Bluetooth keyboard or controller the FM-1 connects out to (lib/btdev): the scan runs while this is up */
        char t[32], nm[24];
        btdev_title(t);
        upper(t);
        cv_text(10, 92, &FONT_XS, ble_wanted() ? t : "A BLUETOOTH KEYBOARD: BLUETOOTH ON FIRST", C_TXT2);
        if (ble_wanted() && !btdev_busy() && btdev_found(btdev_pick(), nm, 0)) {
            upper(nm);
            cv_text(10, 108, &FONT_S, nm, TH_TXT);
            text_r(230, 110, &FONT_XS, "K4 UP: CONNECT", C_TXT2);
        } else if (ble_wanted() && btdev_busy()) {
            cv_text(10, 108, &FONT_XS, "K4 DOWN: LET IT GO. NO COMPUTER MEANWHILE", C_TXT2);
        } else {
            cv_text(10, 108, &FONT_XS, "IT PLAYS CH 10 PADS, 2-4 TRACKS, 1 THE PICKED", C_TXT2);
        }
    }
#else
    cv_text(10, 92, &FONT_XS, "IN: CH 10 PADS, CH 2-4 TRACKS", C_TXT2);
    cv_text(10, 108, &FONT_XS, "CH 1 PLAYS THE TRACK PICKED", C_TXT2);
#endif
    cards_of(KD_SETUP, 1);
}

static void page_setup(void)              /* SETUP: MIDI CLOCK; SOUNDS shared or per pattern, and which have their own */
{
    static const char *const CL[3] = {"OFF: THE FM-1'S OWN TEMPO", "SENT: 24 A BEAT, START, STOP", "FOLLOWED: TEMPO, START, STOP"};
    int t, k, pp = sounds_pp();
    if (ui.setup_view == 3) {
        page_cpu();
        return;
    }
    if (ui.setup_view == 2) {
        page_gbta();
        return;
    }
    if (ui.setup_view == 1) {
        page_midi();
        return;
    }
#if FM1B_USB_HOST
    band_set(C_CREAM, "ALL", "SETUP", "K3: USB PORT", 0, 0);
#else
    band_set(C_CREAM, "ALL", "SETUP", "SELECT: CPU", 0, 0);
#endif
    rrect(3, 2, 234, 40, 6, C_PANEL);
    cv_text(10, 4, &FONT_XS, FM1B_BLE ? "MIDI CLOCK (USB, BLUETOOTH)" : "MIDI CLOCK (USB)", C_TXT2);
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
    text_cat(t, proj.song_on ? ", ON" : ", OFF");
    band_set(C_CREAM, "ALL", "SONG", t, 0, 0);
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

static void page_qmix(void)               /* the QUICK MIXER: the tracks (the picked one lit), its white keys, its knobs */
{
    int t, k, tr = ui.trk;
    band_set(C_CREAM, tr == TR_DRUM ? "DR" : TRK_NAME[tr], "QUICK MIXER", tr == TR_DRUM ? drum_kit_title() : "", 0, 0);
    levels_take();
    for (t = 0; t < NTRK; t++) {          /* the four tracks (the track keys): the picked one lit, its level beside */
        int x = 3 + t * 59, h, muted = (int)(proj.tmute >> t & 1u);
        uint16_t col = trk_rgb(t);
        rrect(x, 2, 56, 24, 5, t == tr ? col : C_PANEL);
        text_c(x + 28, 5, &FONT_S, t == TR_DRUM ? drum_kit_short() : TRK_NAME[t],
               t == tr ? C_INK : muted ? C_RULE : C_TXT2);
        h = db_h(lvl[t], 52, 48.0f);      /* its level now: yellow (on the picked one, black) */
        cv_rect(x + 2, 22, 52, 2, t == tr ? mix565(col, C_INK, 90) : C_GRID);
        if (h > 0 && t == tr)
            cv_rect(x + 2, 22, h, 2, C_INK);
        else
            hheat(x + 2, 22, 52, 2, h);
    }
    if (tr == TR_DRUM) {                  /* the lanes: a white key each, acid while on (yellow as it sounds), muted dark */
        const uint16_t mute = dcur()->mute;
        hits_take();
        for (k = 0; k < NPADS; k++) {
            int x = 3 + (k % 6) * 39, y = 30 + (k / 6) * 42, muted = (int)(mute >> k & 1u);
            rrect(x, y, 37, 38, 5, muted ? C_PANEL : hit[k] > 0.05f ? mix565(C_ACID, C_NOW, (int)(hit[k] * 256.0f)) : C_ACID);
            text_c(x + 18, y + 2, &FONT_B, trial_pad_name(drum_model(k), k), muted ? C_TXT2 : C_INK);
            text_c(x + 18, y + 21, &FONT_XS, muted ? "MUTE" : "ON", muted ? C_MUTED : C_INK);
        }
        foot("WHITE: MUTE LANE", V3ON ? "AUDIO ONLY" : "", 0);
    } else {                              /* the moves: a white key each, four a row; held: yellow (now), HELD below */
        char h[40], u[4];
        h[0] = 0;
        for (k = 0; k < QM_MOVES; k++) {
            int x = 3 + (k % 4) * 59, y = 30 + (k / 4) * 21, on = qm.trk == tr && (qm.held >> k & 1u);
            rrect(x, y, 56, 19, 4, on ? C_NOW : C_PANEL);
            text_c(x + 28, y + 2, &FONT_S, QM_SHORT[k], on ? C_INK : TH_TXT);
            if (on && text_w(&FONT_XS, h) < 150) {
                text_cat(h, "W");
                int_text(k + 1, u);
                text_cat(h, u);
                text_cat(h, " ");
            }
        }
        if (h[0])
            text_cat(h, "HELD");
        foot("WHITE: HOLD", h[0] ? h : "BLACK 1-4: TRACK", h[0] != 0);
    }
    cards_of(KD_QMIX, 0);
}

static void page_mix(void)                /* the mixer: a strip a track, its meter beside the page's value */
{
    int t;
    {
        char ttl[24];
        ttl[0] = 0;
        text_cat(ttl, "MIX / ");
        text_cat(ttl, MIX_NAME[ui.mixpage]);
        band_set(C_CREAM, "ALL", ttl, ui.mixpage == MIX_RET ? "SHARED FX" : "K1-K4: TRACKS", NMIXPAGE + NMSTPAGE,
                 ui.mixpage);
    }
    if (ui.mixpage == MIX_RET) {          /* RETURNS: each knob's control drawn over its cell: the returns as faders (past
                                           * 0 dB, the preset's, a mark; the boost past the old top yellow), the decay
                                           * and the feedback as bars beside them */
        static const char *const NM[4] = {"VERB", "DECAY", "DELAY", "FDBK"};
        int k;
        for (k = 0; k < 4; k++) {
            int i = RET_P[k], v = proj.fx[i], mx = fx_param(i)->max, ret = !(k & 1), w = ret ? 40 : 16;
            int x = 30 + k * 60 - w / 2, h = v * 92 / mx;
            char u[16];
            text_c(30 + k * 60, 4, &FONT_B, k == 3 && v >= 115 ? "RUNAWAY" : NM[k],
                   k == 3 && v >= 100 ? gradc(GR_HEAT, v >= 115 ? 47 : 40, 47) : TH_TXT);   /* (feedback near self-oscillation) */
            fx_text(i, u);
            text_c(30 + k * 60, 17, &FONT_XS, u, ret && v > 127 ? C_NOW : C_TXT2);
            rrect(x, 30, w, 92, 4, C_GRID);
            vheat(x, 30, w, 92, h);       /* HEAT up its range: a return's boost goes orange, then pink */
            if (ret) {                    /* (0.5) what it puts out now, beside it */
                static float rm[2];
                float o = fx_return_now(k >> 1) * STEM_GAIN * 2.0f;   /* (a sample: x2 toward its peak) */
                rm[k >> 1] = o > rm[k >> 1] ? o : rm[k >> 1] * 0.85f;
                rrect(x + w + 2, 30, 4, 92, 1, C_GRID);
                vheat(x + w + 2, 30, 4, 92, db_h(rm[k >> 1], 92, 60.0f));   /* (-60..0 dBFS: returns sit low) */
            }
            if (ret) {
                cv_rect(x - 4, 122 - 85 * 92 / mx, w + 8, 1, TH_TXT);    /* 0 dB: the preset's return */
                cv_rect(x - 2, 122 - 127 * 92 / mx, w + 4, 1, C_RULE);   /* the old top: past it, the boost */
            }
        }
        cards_of(KD_MIX, ui.mixpage);
        return;
    }
    levels_take();
    for (t = 0; t < NTRK; t++) {
        int x = 3 + t * 59, v = mix_value(t, ui.mixpage), muted = (int)(proj.tmute >> t & 1u), h;
        uint16_t col = trk_rgb(t);
        rrect(x, 2, 56, 128, 6, t == ui.trk ? C_PANEL : mix565(C_BODY, TH_INK, 12));
        if (t == ui.trk)                  /* the track picked: selected */
            frame(x, 2, 56, 128, C_VIOLET);
        rrect(x + 4, 6, 48, 18, 4, muted ? C_GRID : col);
        text_c(x + 28, 7, &FONT_S, t == TR_DRUM ? drum_kit_short() : TRK_NAME[t],
               muted ? TH_RED : C_INK);
        h = db_h(lvl[t], 92, 48.0f);      /* the meter, -48..0 dBFS */
        rrect(x + 6, 30, 8, 92, 3, C_GRID);
        if (h > 0)
            vheat(x + 6, 30, 8, 92, h);
        if (ui.mixpage == 1) {            /* PAN: a slider across */
            cv_rect(x + 20, 76, 30, 2, C_GRID);
            cv_rect(x + 35, 70, 1, 14, C_TXT2);
            rrect(x + 18 + v * 30 / 127, 68, 6, 18, 3, col);
        } else {                          /* a bar up */
            int b = v * 92 / 127;
            rrect(x + 20, 30, 30, 92, 4, C_GRID);
            vheat(x + 20, 30, 30, 92, b);
        }
    }
    cards_of(KD_MIX, ui.mixpage);
}

static void page_fx(void)                 /* the sends' FX: what they do, drawn */
{
    int pg = ui.fxpage, i;
    char t[32];
    t[0] = 0;
    const int dl = pg == FXPG_DELAY || pg == FXPG_DELAY + 1;
    if (dl && !fx_delay_on()) {
        text_cat(t, "PAUSED: SAMPLING");
    } else if (dl) {                      /* the time, in notes and in ms */
        char u[12];
        fx_text(FXP_DL_TIME, t);
        text_cat(t, "  ");
        int_text((int32_t)(fx_delay_ms() + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " MS");
    } else {
        text_cat(t, pg == FXPG_FILTER ? "ON THE MASTER BUS" : "FROM THE SENDS");
    }
    band_set(C_FXCOL, "ALL", FX_PAGE_NAME[pg], t, NFXPAGE, pg);
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    if (pg <= FXPG_VERB + 1) {            /* the reverb: its tail, long by DECAY, bright by TONE, loud by LEVEL */
        float decay = 0.2f + (float)proj.fx[FXP_RV_DECAY] / 127.0f * 0.78f, lvlv = (float)proj.fx[FXP_RV_LEVEL] / 127.0f, a = 1.0f;
        cv_rect(10, 118, 220, 1, C_GRID);
        for (i = 0; i < 54; i++) {
            int h = (int)(a * (0.3f + 0.7f * lvlv) * 100.0f);
            if (h > 0)
                vheat(12 + i * 4, 18, 3, 100, h);
            a *= (decay * 0.12f + 0.86f) * (0.93f + 0.07f * (float)proj.fx2[FXP_RV_SIZE - FXP_NUM] / 127.0f);   /* (SIZE: a smaller room's shorter) */
        }
    } else if (dl) {                      /* the delay: the echoes over 2 s, each lower by FEEDBACK; DELAY 2 to the sides */
        float ms = fx_delay_ms(), fb = fx_delay_fb(), a = (float)proj.fx[FXP_DL_LEVEL] / 127.0f * 0.8f + 0.2f;
        int wide = proj.fx2[FXP_DL_WIDTH - FXP_NUM] > 0, n;   /* (WIDTH: the echoes side to side) */
        uint16_t c = fx_delay_on() ? TH_FG : C_GRID;
        cv_rect(10, 66, 220, 1, C_GRID);
        rrect(10, 18, 4, 96, 2, TH_HI);  /* the sound */
        for (n = 1; n < 24 && ms > 1.0f; n++) {
            int x = 12 + (int)((float)n * ms * 216.0f / 2000.0f), h = (int)(a * 46.0f);
            if (x > 228 || h < 1)
                break;
            {                             /* each echo HEAT out from the middle by its height (dim with the delay off) */
                int r;
                for (r = 0; r < h; r++) {
                    uint16_t cc = c == C_GRID ? C_GRID : gradc(GR_HEAT, r, 45);
                    if (!wide || (n & 1))
                        cv_rect(x, 65 - r, 4, 1, cc);
                    if (!wide || !(n & 1))
                        cv_rect(x, 67 + r, 4, 1, cc);
                }
            }
            a *= fb;
        }
        if (wide) {
            cv_text(220, 10, &FONT_XS, "L", C_TXT2);
            cv_text(220, 106, &FONT_XS, "R", C_TXT2);
        }
        if (proj.fx[FXP_DL_FDBK] >= 100)  /* (0.5) feedback near self-oscillation */
            text_r(214, 8, &FONT_XS, proj.fx[FXP_DL_FDBK] >= 115 ? "FEEDBACK: RUNS AWAY" : "FEEDBACK: HOT",
                   gradc(GR_HEAT, proj.fx[FXP_DL_FDBK] >= 115 ? 47 : 40, 47));
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
        hheat(x + 30, y, w, h, v);
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
    for (int c = 0; c < v; c++)           /* a reduction from the right: hotter as it goes deeper */
        cv_rect(x + 30 + w - 1 - c, y, 1, h, gradc(GR_HEAT, w / 2 + c / 2, w));
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
        cv_text(44, 100, &FONT_MB, "--", C_GRID);
    } else {
        db1(lu, s);
        cv_text(44, 100, &FONT_MB, s, TH_TXT);
        {   /* (0.5) the momentary loudness over the last 22 s (-48..0 LUFS), HEAT; now in numbers */
            hist_feed(3, (int)((lm + 48.0f) * 255.0f / 48.0f), 100);
            hist_draw(122, 101, 112, 26);
            db1(lm, s);
            text_r(234, 99, &FONT_XS, s, TH_TXT);
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
        grid_bg();                     /* (the atlas: curves on a grid, on black) */
        cv_text(12, 8, &FONT_XS, "REDUCTION NOW", C_TXT2);
        u[0] = 0;
        text_cat(u, "-");
        db1(gr, t);
        text_cat(u, t);
        text_cat(u, " DB");
        cv_text(12, 24, &FONT_L, u, gr >= 0.05f ? TH_RED : C_GRID);
        hist_feed(1, (int)(gr * 20.0f), 30);   /* (0.5) the last 6.5 s of it, 0..12.75 dB down */
        hist_draw(150, 14, 80, 54);
        rrect(10, 76, 220, 16, 4, C_GRID);
        if (w > 220)
            w = 220;
        for (i = 0; i < w; i++)           /* the reduction from the right, hotter as it goes deeper */
            cv_rect(229 - i, 76, 1, 16, gradc(GR_HEAT, 110 + i / 2, 220));
        for (i = 0; i <= 12; i += 3)
            cv_rect(230 - i * 220 / 12, 94, 1, 5, C_TXT2);
        cv_text(10, 104, &FONT_XS, proj.fx[FXP_PUMP] ? "PUMP: DUCKS AT EACH KICK" : "PUMP OFF", C_TXT2);
        return;
    }
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    cv_line(x0, y0 + s, x0 + s, y0, C_GRID);   /* 1:1 */
    for (i = 0; i <= s; i++) {
        float in = -48.0f + 48.0f * (float)i / (float)s, over = in - thr, g;
        int y;
        g = over <= -3.0f ? 0.0f : over >= 3.0f ? slope * over : slope * (over + 3.0f) * (over + 3.0f) / 12.0f;
        y = y0 + s - (int)((in - g + mk + 48.0f) * (float)s / 48.0f);
        if (y < y0)
            y = y0;
        if (i) {
            glow_col(x0 + i, y, y0 + s);
            gline(x0 + i - 1, py, x0 + i, y, y0, y0 + s);
        }
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
        for (i = 0; i < w; i++)
            cv_rect(229 - i, 42, 1, 8, gradc(GR_HEAT, 46 + i / 2, 92));
        cv_text(138, 60, &FONT_XS, proj.fx[FXP_C_RATIO] ? "THE MIX," : "OFF:", C_TXT2);
        cv_text(138, 74, &FONT_XS, proj.fx[FXP_C_RATIO] ? "COMPRESSED" : "RATIO 1:1", C_TXT2);
    }
}

static void mst_tape(void)                /* TAPE: its curve, in to out, -1..1 */
{
    float k = um_exp2((float)proj.fx[FXP_TP_DRIVE] * (12.0f / 127.0f) * 0.166096f), b = (float)proj.fx[FXP_TP_WARM] * (0.35f / 127.0f);
    float tb = um_tanh(b), nrm = 1.0f / (k * (1.0f - tb * tb));
    int x0 = 58, y0 = 6, s = 120, i, py = 0, on = proj.fx[FXP_TP_DRIVE] > 0;
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    cv_line(x0, y0 + s, x0 + s, y0, C_GRID);
    cv_rect(x0, y0 + s / 2, s, 1, C_GRID);
    cv_rect(x0 + s / 2, y0, 1, s, C_GRID);
    for (i = 0; i <= s; i++) {
        float x = -1.0f + 2.0f * (float)i / (float)s, y = on ? (um_tanh(k * x + b) - tb) * nrm : x;
        int py2;
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        py2 = y0 + s / 2 - (int)(y * (float)(s / 2));
        if (i && on)
            gline(x0 + i - 1, py, x0 + i, py2, y0, y0 + s);
        else if (i)
            thick(x0 + i - 1, py, x0 + i, py2, mix565(C_BODY, TH_FG, 110));
        py = py2;
    }
}

static void mst_stereo(void)              /* STEREO: the image's width as a fan, the lows in the middle */
{
    int w = proj.fx[FXP_ST_WIDTH] * 5, i;
    float spread = (float)w / 100.0f * 0.7854f;   /* 100 %: 45 degrees each way */
    grid_bg();                     /* (the atlas: curves on a grid, on black) */
    cv_text(10, 8, &FONT_XS, "L", C_TXT2);
    cv_text(224, 8, &FONT_XS, "R", C_TXT2);
    for (i = -20; i <= 20; i++) {
        float a = spread * (float)i / 20.0f;
        int x = 120 + (int)(um_sin(a) * 100.0f), y = 120 - (int)(um_sin(a + 1.5708f) * 100.0f);
        if (a > 1.5f || a < -1.5f)
            continue;
        cv_line(120, 120, x, y, mix565(C_BODY, gradc(GR_HEAT, i < 0 ? -i : i, 20), 80));   /* (dim: the scope over it) */
    }
    if (proj.fx[FXP_ST_BASS])             /* BASS MONO: the lows centred */
        rrect(114, 70, 13, 50, 4, TH_FG);
    cv_rect(120, 20, 1, 100, C_GRID);
    if (scope_take(SC_MASTER, 2, 1)) {    /* (0.5) the master now: a vectorscope, the mid up, the sides across */
        int pk = 0, k;
        for (k = 0; k < SC_N; k++) {
            int a = sc.buf[k] < 0 ? -sc.buf[k] : sc.buf[k];
            pk = a > pk ? a : pk;
        }
        if (pk > 300)
            for (k = 0; k < SC_N; k += 2) {
                int m = (sc.buf[k] + sc.buf[k + 1]) / 2, sd = (sc.buf[k] - sc.buf[k + 1]) / 2, x, y, r;
                if (m < 0) {
                    m = -m;
                    sd = -sd;
                }
                x = 120 + sd * 96 / pk;
                y = 120 - m * 96 / pk;
                r = (sd < 0 ? -sd : sd) + m;
                cv_rect(x - 1, y - 1, 3, 3, gradc(GR_TRACE, r, pk));
            }
        scope_again();
    }
}

static void mst_limit(void)               /* LIMIT: the reduction and the output against the ceiling */
{
    float out = fx_out_peak(0), ce = -0.1f * (float)proj.fx[FXP_L_CEIL];
    int xc;
    mtr_out = out > mtr_out ? out : mtr_out * 0.85f;
    /* (the atlas: on black) */
    gr_bar(10, 20, 150, 12, "GR", fx_lgr());
    hmeter(10, 52, 150, 12, "OUT", mtr_out, 24.0f);
    xc = 40 + 150 + (int)(ce * 150.0f / 24.0f);
    cv_rect(xc, 46, 2, 24, TH_FG);
    cv_text(xc - 30, 74, &FONT_XS, "CEILING", TH_TXT);
    if (proj.fx[FXP_LIMIT]) {             /* (0.5) its reduction over the last 6.5 s */
        hist_feed(2, (int)(fx_lgr() * 20.0f), 30);
        cv_text(10, 88, &FONT_XS, "REDUCTION, LAST 6 S", C_TXT2);
        hist_draw(10, 102, 220, 24);
    } else {
        cv_text(10, 96, &FONT_XS, "OFF: IT CLIPS", TH_RED);
    }
}

/* (0.5) The master's spectrum behind the EQ's curve: 24 bands, 40 Hz to 16 kHz in even octaves, by Goertzel on the
 * scope's capture (Welch window); in turn the highs (every sample) and the lows (4 samples' mean: 43 Hz apart), each
 * asked for as the other is drawn (a half 15 times a second); a bar up at once, down 3 dB a frame drawn to its
 * last reading (a steady tone: a steady bar); -66..0 dB, HEAT */
static float spec_db[24], spec_to[24];
#ifdef PLAT_HOST
static int spec_n;
#endif
static void mst_spectrum(int x0, int y0, int w, int h)
{
    static uint8_t phase, ready;
    static uint32_t fall_t;
    int lo = phase & 1, b, i;
    uint32_t now = plat_ms();
    if (!ready) {                         /* (from silence) */
        for (b = 0; b < 24; b++)
            spec_db[b] = spec_to[b] = -120.0f;
        ready = 1;
    }
    if (now - fall_t >= 30u) {            /* (a frame drawn, not a redraw for a knob between two) */
        fall_t = now;
        for (b = 0; b < 24; b++)
            spec_db[b] = spec_db[b] - 3.0f > spec_to[b] ? spec_db[b] - 3.0f : spec_to[b];
    }
    if (scope_take(SC_MASTER, lo ? 4 : 1, 0)) {
        const float fs = lo ? 11025.0f : 44100.0f;
        for (b = 0; b < 24; b++) {
            float hz = 40.0f * um_exp2((float)b * (8.64f / 23.0f)), c, s1 = 0.0f, s2 = 0.0f, pw, db;
            if ((hz < 1000.0f) != lo)
                continue;
            c = 2.0f * um_sin(6.2831853f * hz / fs + 1.5707963f);
            for (i = 0; i < SC_N; i++) {
                float u = (float)(2 * i - SC_N) / (float)SC_N, s0 = (float)sc.buf[i] * (1.0f - u * u) + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            pw = s1 * s1 + s2 * s2 - c * s1 * s2;
            db = 0.5f * um_db(pw > 1.0f ? pw : 1.0f) - 132.3f;   /* (a full-scale sine: 0 dB) */
            spec_to[b] = db;
            if (db > spec_db[b])
                spec_db[b] = db;
        }
        phase++;
        scope_ask(SC_MASTER, lo ? 1 : 4, 0);   /* the other half's, from now (not this one's again, then dropped) */
#ifdef PLAT_HOST
        spec_n++;
#endif
    }
    for (b = 0; b < 24; b++) {
        int bh = (int)((spec_db[b] + 66.0f) * (float)h / 66.0f), x = x0 + b * w / 24;
        vheat(x + 1, y0, w / 24 - 2, h, bh < 0 ? 0 : bh > h ? h : bh);
    }
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
    {
        char ttl[24];
        ttl[0] = 0;
        text_cat(ttl, "MASTER ");
        text_cat(ttl, MST_PAGE_NAME[pg]);
        band_set(C_MSTCOL, "ALL", ttl, t, NMIXPAGE + NMSTPAGE, NMIXPAGE + pg);
    }
    switch (pg) {
    case 0: mst_chain(); break;
    case 1:
    case 2: {
        float db[112];
        int i, x0 = 10, y0 = 8, w = 220, h = 112, mid = y0 + h / 2, py = mid;
        grid_bg();                     /* (the atlas: curves on a grid, on black) */
        grid_hz(x0, y0, w, h);
        for (i = -12; i <= 12; i += 6)
            cv_rect(x0, mid - i * h / 30, w, 1, i ? mix565(C_PANEL, TH_INK, 30) : C_GRID);
        mst_spectrum(x0, y0, w, h);       /* (0.5) the master's spectrum behind it */
        fx_eq_curve(db, 112);
        for (i = 0; i < 112; i++) {
            float d = db[i] < -15.0f ? -15.0f : db[i] > 15.0f ? 15.0f : db[i];
            int x = x0 + i * 2, y = mid - (int)(d * (float)h / 30.0f + (d < 0.0f ? -0.5f : 0.5f));
            glow_col(x, y, mid);
            glow_col(x + 1, y, mid);
            if (i && proj.fx[FXP_M_BYPASS])
                thick(x - 2, py, x, y, C_TXT2);
            else if (i)
                gline(x - 2, py, x, y, y0, y0 + h);
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
    band_set(C_RECCOL, ui.trk == TR_DRUM ? "DR" : TRK_NAME[ui.trk], "RECORDING", ui.rec ? "ON. GLO: ERASE PART" : "OFF. GLO: ERASE PART", 0, 0);   /* (REC + GLO) */
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

static void page_v3_bus(void)
{
    static const char *const short_label[] = {"SAT", "COMP", "TRANS", "LIMIT"};
    int raw[4], mode;
    uint32_t f = plat_lock();
    mode = v3_mode;
    for (int k = 0; k < 4; ++k) raw[k] = (int)trial_macro_value(k);
    plat_unlock(f);

    band_set(C_FXCOL, "DR", "DRUM BUS", "DRUMENGINE", 0, 0);
    cv_text(7, 8, &FONT_XS, "SELECT", C_MUTED);
    for (int i = 0; i < 3; ++i) {
        int x = 51 + i * 62;
        cv_rect(x, 4, 58, 24, i == mode ? C_FXCOL : C_FIELD);
        text_c(x + 29, 7, &FONT_S, V3_MODE_LABEL[i], i == mode ? C_INK : C_MUTED);
    }
    for (int k = 0; k < 4; ++k) {
        char text[12], num[] = "K1";
        int x = 4 + (k % 2) * 117, y = 36 + (k / 2) * 31;
        num[1] += (char)k;
        (void)x;
        (void)y;
        (void)num;
        v3_bus_percent(k, raw[k], text);
        card_put(&cards[k], short_label[k], text, raw[k], 0, 127, k == 2 ? 64 : -1);
    }
    {   /* (0.5) SAT's curve (in across, out up, 1:1 the diagonal); the compressor's and the limiter's reduction now */
        float d = 1.0f + (float)raw[0] * (8.0f / 127.0f), nt = um_tanh(d);
        int i, py = 0;
        frame(8, 34, 100, 66, C_GRID);
        cv_line(8, 99, 107, 34, C_GRID);
        for (i = 0; i < 100; i++) {
            int yy = 67 - (int)(um_tanh(d * ((float)i / 49.5f - 1.0f)) / nt * 32.0f);
            if (i)
                gline(8 + i - 1, py, 8 + i, yy, 35, 99);
            py = yy;
        }
        cv_text(12, 36, &FONT_XS, "SAT", C_TXT2);
        gr_bar(118, 50, 54, 10, "COMP", trial_bus_gr(0));
        gr_bar(118, 82, 54, 10, "LIMIT", trial_bus_gr(1));
    }
    cv_text(7, 106, &FONT_XS, "FX: DRY/BUS", C_BONE);
    text_r(233, 106, &FONT_XS, "SEL + FX: EXIT", C_BONE);
}

static void page_color(void)
{
    int i;
    char title[24], amount[12];
    const color_params_t *p = &proj.color[TR_DRUM];
    band_set(trk_rgb(ui.trk), TRK_NAME[ui.trk], ui.color_page ? "COLOR: MODULES" : "COLOR: AMOUNT", "PRE-SEND + USB STEM", 2, ui.color_page);
    /* (the atlas: on black) */
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

/* the page now: its band's text and its knobs set, its picture drawn (the canvas begun and blitted by the caller) */
/* HELP (0.5): SEL held by itself: what the page on screen does with held keys and second presses, the things no knob
 * card shows. A line each (38 characters), the page's own first, then the ones that work everywhere */
static const char *help_text(void)
{
    const int inst = ui.trk != TR_DRUM;
    switch (ui.page) {
    case PG_SEQ:
        return inst ? "SELECT: NOTES / HARMONY / PART / GROOVE\nHOLD STEP + K1-K4: EDIT THAT STEP\n"
                      "HOLD STEP + SELECT: ONE OF ITS NOTES\nHOLD STEP + ALGO: SUBSTEPS\n"
                      "HOLD STEP + ARP: PICK ITS NOTES\nHOLD STEP + OCT+: HALF, WHOLE, SLIDE\n"
                      "HOLD STEP + EDIT / ENV / LFO: LOCKS\nBLACK 1-8: STEP BANK   SEQ: ACID"
                    : "WHITE: STEPS   BLACK: PADS\nSELECT: GROOVE KNOBS / HIT KNOBS\n"
                      "HOLD STEP + K1-K4: VEL CHANCE SUB ACC\nHOLD STEP + ALGO: SUBSTEPS\n"
                      "HOLD SEQ + STEP: ACCENT\nSEQ + OCT+: DOUBLE   SEQ + OCT-: HALVE\n"
                      "OCT- / OCT+: STEP BANK\nENV: LANE MUTES   EDIT: THE DRUM";
    case PG_EDIT: case PG_ENV: case PG_LFO: case PG_ARP:
        return inst ? "SELECT: PAGES   SAME BUTTON: BACK\nPRESETS: SOUNDS\nSAVE + PRESETS: KEEP AS A USER SOUND\n"
                      "SAVE + OCT-: THE SOUND AS SAVED\nSTEP HELD (STEPS), THEN A KNOB: LOCK\n"
                      "REC ON, PLAYING, A KNOB: RECORDED\nARP TWICE: CHORD MODE"
                    : "SELECT: PAGES   SAME BUTTON: BACK\nPRESETS: THIS DRUM'S PRESETS\nSEL + KNOB: FINE STEPS\n"
                      "STEP HELD (STEPS), THEN A KNOB: LOCK\nLFO: MOVES ONE KNOB AT EACH HIT\n"
                      "ITS EDITS ARE SAVED WITH THE PROJECT";
    case PG_HOME:
        return "WHITE: SCENE   PAD 1-4: MUTE TRACK\nPAD + WHITE: THAT TRACK'S CLIP ONLY\n"
               "PAD + OCT-: STOP IT   PAD + OCT+: SOLO\nWHITE + K1 / K2: FOLLOW ACTION\n"
               "BLACK 5-11 HELD: PERFORM KEYS (K1)\nOCT-: TAP TEMPO   OCT+ HELD: TAPE STOP\n"
               "SEL + SELECT: NUDGE\nHOME: QUICK MIXER, SONG, SETUP";
    case PG_QMIX:
        return "PAD 1-4: THE TRACK (NO MUTE HERE)\nK1-K4: LEVEL REVERB DELAY PAN\n"
               "DRUMS: WHITE 1-11 MUTE LANES\nT2-T4: WHITE HELD: FILTER, SENDS,\n   ROLL, STUTTER, OCTAVE\n"
               "BLACK 5-11 HELD: PERFORM KEYS";
    case PG_MIX: case PG_FX: case PG_MST: case PG_V3_BUS:
        return "SELECT: PAGES (MIXER ON TO MASTER)\nSAME BUTTON AGAIN: BACK\nSEL + GLO: THE MASTER BUS\n"
               "SEL + FX: THE TRACK'S INSERT (T2-T4)\nRETURNS: PAST THE MIXER'S DELAY PAGE";
    case PG_KEYS:
        return "ALL KEYS PLAY   OCT- / OCT+: OCTAVE\nREC, THEN PLAY: RECORD AS YOU PLAY\n"
               "REC, STOPPED: STEP RECORD (SELECT:\n   THE CURSOR, TO LEAVE A REST)\n"
               "ARP TWICE: CHORD MODE   SEQ: STEPS";
    case PG_GEN:
        return inst ? "OCT+: A NEW LINE   OCT-: MUTATE\nSELECT: ACID, NOTES, LINE, KEEP\n"
                      "KEEP: K1 STEP  K2 KEEP IT  K4 ITS NOTE\n   KEPT STEPS STAY THROUGH EVERY\n   NEW LINE, MUTATE AND KNOB\n"
                      "SEQ: THE KEYS"
                    : "OCT+: A NEW BEAT   OCT-: MUTATE\nK1 STYLE   K2-K4: HOW BUSY\n"
                      "   (50: THE STYLE AS IT COMES)\nSELECT: MORE (PERC FILL ACCENT), LINE\n"
                      "SEQ: BACK TO THE STEPS";
    case PG_SETUP:
        return "SELECT: SETUP, MIDI, BT AUDIO, CPU\nK1 CLOCK   K2 SOUNDS PER PATTERN\nK3 USB PORT   K4 MIX PER PATTERN\n"
               "MIDI: K1 SEQ OUT (THE SEQUENCER'S\n   NOTES SENT)   K2 BLUETOOTH";
    case PG_SONG:
        return "WHITE: SET THE ENTRY, MOVE ON\nSELECT: CURSOR   K1 PATTERN  K2 BARS\n"
               "K3 LAUNCH   K4 SONG ON / OFF\nOCT+: COPY ENTRY   OCT-: DELETE IT";
    default:
        return "SELECT: PAGES OR THE CURSOR";
    }
}

static void page_help(void)
{
    static const char *const ALWAYS = "ALGO: TRACK, THE PAGE STAYS\nEDIT + OCT- / OCT+: UNDO / REDO\n"
                                      "SAVE: PROJECT   SAVE + KEY: COPY PATTERN\nHOLD REC: CLICK, QUANTIZE, ERASE";
    const char *src[2] = {help_text(), ALWAYS};
    char line[44];
    int y = 0, k, n;
    band_set(C_CREAM, "ALL", "HELP", PAGE_NAME[ui.page], 0, 0);
    for (k = 0; k < 2; k++) {
        const char *q = src[k];
        while (*q && y < BODY_H - 10) {
            for (n = 0; *q && *q != '\n' && n < 43; n++)
                line[n] = *q++;
            line[n] = 0;
            if (*q == '\n')
                q++;
            cv_text(6, y, &FONT_XS, line, k ? C_TXT2 : TH_TXT);
            y += 11;
        }
        if (!k) {
            cv_rect(6, y + 3, 228, 1, C_GRID);
            y += 6;
        }
    }
    for (k = 0; k < 4; k++)
        cards[k].on = 0;
}

static void page(void)
{
    int inst = ui.trk != TR_DRUM;
    if (!(ui.page == PG_EDIT && v6_voice_page()))
        cap_set(0, 0, 0, 0);              /* (the scope's buffer back) */
    if (rec_page()) {
        page_rec();
        return;
    }
    if (ui.help) {
        page_help();
        return;
    }
    switch (ui.page) {
    case PG_EDIT: inst ? page_iedit(0) : v6_voice_page() ? page_v6() : page_dedit(); break;
    case PG_ENV: page_iedit(1); break;
    case PG_LFO: page_lfo(); break;
    case PG_MIX: page_mix(); break;
    case PG_FX: page_fx(); break;
    case PG_V3_BUS: page_v3_bus(); break;
    case PG_COLOR: page_color(); break;
    case PG_MST: page_mst(); break;
    case PG_ARP: page_arp(); break;
    case PG_MUTE: page_mute(); break;
    case PG_HOME: page_home(); break;
    case PG_QMIX: page_qmix(); break;
    case PG_SONG: page_song(); break;
    case PG_SETUP: page_setup(); break;
    case PG_KIT:
        kit_band("KIT", "THE KIT'S OWN KNOBS", 0, 0);
        page_map(KD_KIT, 1, 0, 0, 8, 28);
        cards_of(KD_KIT, 0);
        break;
    case PG_KEYS: page_keys(); break;
    case PG_GEN:
        if (ui.trk == TR_DRUM)
            page_dgen();
        else
            page_gen();
        break;
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
    if (rec_page())
        return 0;
    switch (ui.page) {
    case PG_HOME: case PG_QMIX: case PG_MIX: case PG_MUTE: case PG_LFO: case PG_KEYS: case PG_ARP: return 1;
    case PG_SEQ: return !inst;
    case PG_EDIT:                         /* (a drum's page while its hit is captured; the synth's live pictures) */
        if (v6_voice_page())
            return trial_capture_pos() >= 0 && trial_capture_pos() < 216;
        return !inst || ui.epage == 0 || ui.epage == 1 || ui.epage == 3 || ui.epage == EP_OSC2 || ui.epage == EP_SHAPER;
    case PG_ENV: return inst;
    case PG_V3_BUS: return 1;
    case PG_SONG: return sq.playing;
    case PG_SETUP: return ui.setup_view;
    case PG_MST: return ui.mstpage != 5;    /* (all but TAPE: live now, the EQ's spectrum too) */
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

/* The processor's last lines (speak): 1 the blocks late, the voices, the budgets and the second core; 2 the synth's
 * parts; 3 OTHER's. Said one at a time SPEAK_GAP ms apart after the rest: the device's SysEx ring holds 384 bytes, and
 * six lines at once (about 620) lost the last ones on the FM-1 (the simulator has no such limit) */
#define SPEAK_GAP 25u
static uint8_t speak_more;                /* the line due next (0: none) */
static uint32_t speak_more_t;
static void speak_line(int which, char *t)
{
    char u[12];
    int i, b;
    t[0] = 0;
    if (which == 1) {
        text_cat(t, "LATE ");
        int_text((int32_t)plat_xruns(), u);
        text_cat(t, u);
        text_cat(t, ". VOICES ");
        int_text(syn_voices(-1), u);
        text_cat(t, u);
        text_cat(t, " CAP ");
        int_text(gov.cap, u);
        text_cat(t, u);
        text_cat(t, " POOL ");
        int_text(pool_now, u);
        text_cat(t, u);
        text_cat(t, ". CUT ");
        int_text((int32_t)gov.cuts, u);
        text_cat(t, u);
        text_cat(t, " SHED ");
        int_text((int32_t)gov.caps, u);
        text_cat(t, u);
        text_cat(t, " FX OUT ");
        int_text((int32_t)gov.sheds, u);
        text_cat(t, u);
        text_cat(t, " NOW ");
        int_text(gov.fx_shed, u);
        text_cat(t, u);
        text_cat(t, " GUARD ");
        int_text((int32_t)gov.guards, u);
        text_cat(t, u);
        text_cat(t, ". CORE2 ");
        int_text(cpu2 && plat_cpu2_on(), u);
        text_cat(t, u);
        text_cat(t, " DRUMS THERE ");     /* the blocks it rendered the drums, %; its stack's depth, bytes */
        int_text((int32_t)(d2.share + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " STACK ");
        int_text((int32_t)plat_cpu2_stack(), u);
        text_cat(t, u);
        text_cat(t, " KIT ");
        text_cat(t, drum_kit_title());
        ann_say(t, 0, 0);
    } else if (which == 2) {             /* the synth's parts, us a block; what a voice is measured to cost */
        text_cat(t, "SYNTH US PREP ");
        int_text((int32_t)(sy_us[SY_PREP] + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " VOICES ");
        int_text((int32_t)(sy_us[SY_VOICES] + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " MIX ");
        int_text((int32_t)(sy_us[SY_MIX] + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " A VOICE ");
        int_text((int32_t)(gov.per_voice + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " BASE ");
        int_text((int32_t)(gov.syn_base + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " SPLIT ");              /* the first core's share of the voices, %; its wait a synth block, us */
        int_text((int32_t)(split_share + 0.5f), u);
        text_cat(t, u);
        text_cat(t, " WAIT ");
        int_text((int32_t)(split_wait_us + 0.5f), u);
        text_cat(t, u);
        ann_say(t, 0, 0);
    } else {
        /* OTHER's parts, us a block: the mean and the worst of the last 10 s */
        static const char *const ON[OT_NUM] = {"IN", "SEQ", "DRUM", "JOIN", "WAIT", "OUT", "LEFT"};
        text_cat(t, "OTHER US");
        for (i = 0; i < OT_NUM; i++) {
            float m = 0.0f;
            for (b = 0; b < CS_MXB; b++)
                if (ot_mx[b][i] > m)
                    m = ot_mx[b][i];
            text_cat(t, i ? ". " : " ");
            text_cat(t, ON[i]);
            text_cat(t, " ");
            int_text((int32_t)(ot_us[i] + 0.5f), u);
            text_cat(t, u);
            text_cat(t, " MAX ");
            int_text((int32_t)(m + 0.5f), u);
            text_cat(t, u);
        }
        ann_say(t, 0, 0);
    }
}

static void speak(void)                   /* a screen reader that has just started listening hears the screen */
{
    char t[200];
    if (speak_more && !ann_on())
        speak_more = 0;
    if (speak_more && plat_ms() - speak_more_t >= SPEAK_GAP) {
        speak_line(speak_more, t);
        speak_more = (uint8_t)(speak_more < 3 ? speak_more + 1 : 0);
        speak_more_t = plat_ms();
    }
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, TRK_NAME[ui.trk]);
        text_cat(t, ". ");
        text_cat(t, PAGE_NAME[ui.page]);
        if (ui.trk == TR_DRUM) {
            text_cat(t, ". PAD ");
            text_cat(t, trial_pad_name(drum_model(ui.pad), ui.pad));
        }
        if (v6_voice_page()) {
            char page[16];
            int pad = ui.pad+1;
            KitPatch current;
            trial_snapshot(pad, &current);
            v6_page_text(page);
            text_cat(t,". V6 VOICE ");
            text_cat(t,v6_voice_name());
            text_cat(t," ");
            text_cat(t,v6_recipe_name(pad,kit_preset_match(&current,pad)));
            text_cat(t," ");
            text_cat(t,page);
            v6_append_tom_pitch(t," PITCH ",pad,&current);
            for (int k=0; k<4; ++k) {
                int param = v6_param(pad, 4*ui.epage+k);
                if (!kit_param_spec(pad,param)) break;
                char key[] = " K1 ", text[24];
                key[2] += (char)k;
                text_cat(t,key);
                if (kit_param_editable(&current,pad,param)) {
                    v6_value_text(pad,param,kit_param_get(&current,pad,param),text);
                    text_cat(t,text);
                } else text_cat(t,"FIXED");
            }
        }
        if (ui.page == PG_QMIX) {         /* the QUICK MIXER: its track's values; the drums' mutes; the moves held */
            char u[12];
            int k;
            for (k = 0; k < 4; k++) {
                text_cat(t, k ? " " : ". ");
                text_cat(t, QM_KNOB[k]);
                text_cat(t, " ");
                mix_text(ui.trk, QM_PAGE[k], u);
                text_cat(t, u);
            }
            if (ui.trk == TR_DRUM) {
                text_cat(t, dcur()->mute ? ". MUTED" : ". NO LANES MUTED");
                for (k = 0; k < NPADS; k++)
                    if (dcur()->mute >> k & 1u) {
                        text_cat(t, " ");
                        text_cat(t, trial_pad_name(drum_model(k), k));
                    }
            } else if (qm.held && qm.trk == ui.trk) {
                text_cat(t, ". HELD");
                for (k = 0; k < QM_MOVES; k++)
                    if (qm.held >> k & 1u) {
                        text_cat(t, " ");
                        text_cat(t, QM_NAME[k]);
                    }
            }
        } else if (ui.page == PG_HOME) {  /* PATTERNS: the one playing and the one to come */
            char u[4];
            text_cat(t, sq.playing ? ". PLAYING " : ". PATTERN ");
            two_digits(proj.pat + 1, u);
            text_cat(t, u);
            if (sq.playing && sq.queued >= 0) {
                text_cat(t, " NEXT ");
                two_digits(sq.queued + 1, u);
                text_cat(t, u);
            }
        }
        ann_say(t, 0, ANN_NOW);
        trial_metrics_speak(V3ON);
        {   /* and the processor (0.5: read remotely while someone plays, tools/fm1_remote.py screen): each stage's
             * share of a block, its mean and its worst block of the last 10 s; the blocks late; the voices
             * sounding, the budget's cap and the pool; what the budgets gave up (drum sounds cut, voices shed,
             * effects taken out, and which are out now); the second core; the synth's parts */
            static const char *const NM[CS_NUM] = {"DRUMS", "SYNTH", "DRUM BUS", "RETURNS", "MASTER", "OTHER", "CPU"};
            static const uint8_t SEQ[CS_NUM] = {CS_TOTAL, CS_DRUMS, CS_SYNTH, CS_FXIN, CS_FX, CS_MASTER, CS_REST};
            const float per = 100.0f * 44100.0f / (1.0e6f * (float)APP_BLOCK);
            char u[12];
            float mx[CS_NUM];
            int i, b;
            for (i = 0; i < CS_NUM; i++) {
                mx[i] = 0.0f;
                for (b = 0; b < CS_MXB; b++)
                    if (cs_mx[b][i] > mx[i])
                        mx[i] = cs_mx[b][i];
            }
            t[0] = 0;
            for (i = 0; i < CS_NUM; i++) {
                int k = SEQ[i];
                text_cat(t, NM[k]);
                text_cat(t, " ");
                int_text((int32_t)(cs_us[k] * per + 0.5f), u);
                text_cat(t, u);
                text_cat(t, " MAX ");
                int_text((int32_t)(mx[k] * per + 0.5f), u);
                text_cat(t, u);
                text_cat(t, ". ");
            }
            ann_say(t, 0, 0);
            speak_more = 1;               /* the rest a moment later, a line at a time (speak_line) */
            speak_more_t = plat_ms();
        }
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
    pool_apply();
    plat_unlock(f);
}

void app_frame(void)
{
    uint32_t now = plat_ms(), t = sq.ticks;
    input();
    if (ui.page == PG_MIX || ui.page == PG_MST)   /* (GLO opens again on whichever of them was left last) */
        glo_last = ui.page;
    if (qm.held && (ui.page != PG_QMIX || ui.trk != qm.trk))   /* the QUICK MIXER left: its held moves let go */
        qm_clear();
#if FM1B_USB_HOST
    if (plat_usb_host_msg() != USBH_MSG_NONE && ui.page == PG_SETUP)
        ui.redraw = 1;
#endif
    ck.rec = ui.rec;
    if (sq.ext_stop) {                    /* MIDI CLOCK followed: a Stop */
        sq.ext_stop = 0;
        seq_stop();
    }
    if ((ui.btn >> B_SEL & 1u) && !ui.sel_used && !ui.help && now - ui.sel_t >= 500u) {
        ui.help = 1;                      /* SEL held by itself: HELP for the page on screen */
        ui.redraw = 1;
        app_say("HELP", PAGE_NAME[ui.page]);
    } else if (ui.help && (ui.sel_used || !(ui.btn >> B_SEL & 1u))) {   /* (used as a modifier after all) */
        ui.help = 0;
        ui.redraw = 1;
    }
    if ((ui.btn >> B_REC & 1u) && !ui.rec_shown && !cb_rec && now - ui.rec_t >= 300u) {
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
    late_watch();
    gbta_poll();
#if FM1B_BLE_CENTRAL
    {   /* Bluetooth keyboards: the scan while the MIDI view is up; what happened, said */
        static uint8_t seen;
        const int up = ui.page == PG_SETUP && ui.setup_view == 1 && ble_wanted();
        int ev;
        btdev_page(up);
        while ((ev = btdev_event()) != BTDEV_NONE)
            if (ev != BTDEV_COMPUTER && ev != BTDEV_COMPUTER_GONE)
                app_say("DEVICE", btdev_word(ev));
        ev = btdev_count() * 4 + btdev_busy() * 2 + btdev_ready();
        if (up && ev != seen)
            ui.redraw = 1;
        seen = (uint8_t)ev;
    }
#endif
    {   /* the headphone jack (0.5): said as it changes */
        static int8_t hp = -1;
        const int now_hp = plat_headphones();
        if (now_hp >= 0 && now_hp != hp) {
            if (hp >= 0)
                app_say("OUTPUT", now_hp ? "HEADPHONES" : "SPEAKER");
            hp = (int8_t)now_hp;
        }
    }
    if (plat_battery() != ui.batt)
        ui.redraw = 1;                    /* the band's battery */
    if (ui.page == PG_SETUP && ui.setup_view == 1 && ble_word() != ble_word_now)
        ui.redraw = 1;                    /* the MIDI view: Bluetooth's state as it changes */
    {   /* the delay's line back once STUTTER and TAPE STOP are done with it: the keys up, and the last block
         * that had them through its tail (perf.on: the block rendering; pf_was: the tail's, a block later). Zeroed
         * here, outside the lock (it took 1.5 ms with the audio held off, a quarter of a block: a glitch as the live
         * sound came back) */
        int free = !((perf.want | perf.on | pf_was) & (1u << PF_STUT | 1u << PF_TAPE));
        if (free != fx_delay_on()) {
            uint32_t f;
            if (free)
                fx_delay_zero();
            f = plat_lock();
            fx_delay_line(free);
            plat_unlock(f);
        }
    }
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
/* the companion's files: none (the samples went with the sampler, 0.5; the hooks keep plat_file_*, the project's) */
int app_file(uint32_t i, plat_file_info_t *f) { (void)i; (void)f; return -1; }
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n) { (void)i; (void)off; (void)dst; (void)n; return -1; }
int app_file_begin(uint32_t i, uint32_t size) { (void)i; (void)size; return -1; }
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n) { (void)i; (void)off; (void)src; (void)n; return -1; }
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc) { (void)i; (void)size; (void)crc; return -1; }
int app_file_delete(uint32_t i) { (void)i; return -1; }
int app_file_busy(void) { return sq.playing; }

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
    else if (!strncmp(what, "lanegate", 8))   /* lanegateN: drum pad N's (1..11) audio gate now, thousandths */
        *got = trial_lane_gate((unsigned)atoi(what + 8));
    else if (!strncmp(what, "lanepk", 6))     /* lanepkN: drum pad N's lane peak out since last asked, dB (-200 silent) */
        *got = trial_lane_peak((unsigned)atoi(what + 6));
    else if (!strncmp(what, "settool", 7))   /* settoolN: an instrument's step tools turned to page N (older suites:
                                              * 3, the GROOVE knobs they were written with); stool: the page */
        *got = ui.stool = (uint8_t)(atoi(what + 7) % NSTOOL);
    else if (!strncmp(what, "oldseq", 6))    /* oldseqN: 1 the drums' SEQ as before BEAT (the older suites) */
        *got = old_seq = (uint8_t)(what[6] == '1');
    else if (!strncmp(what, "oldpick", 7))   /* oldpickN: 1 a track change as before 0.5 (the older suites) */
        *got = old_pick = (uint8_t)(what[7] == '1');
    else if (!strncmp(what, "dparam", 6)) {   /* dparamPAD_I: the DrumEngine's parameter I of pad PAD (1..11), its tick */
        const char *u = strchr(what, '_');
        KitPatch cur;
        trial_snapshot(1, &cur);
        *got = u ? kit_param_get(&cur, atoi(what + 6), atoi(u + 1)) : -1;
    }
    else if (!strcmp(what, "dtool"))      /* the drums' STEPS: 0 PART GROOVE's knobs, 1 the HIT's */
        *got = ui.dtool;
    else if (!strncmp(what, "dvel", 4)) { /* dvelPAD_STEP: the hit's velocity lock (0: none) */
        const char *u = strchr(what, '_');
        int j = u ? lock_find(p, (atoi(u + 1) - 1) & (NSTEPS - 1), atoi(what + 4) % NPADS, LK_VEL) : -1;
        *got = j >= 0 ? p->lk[j].val : 0;
    }
    else if (!strcmp(what, "hnote"))      /* the held step's note picked: 0 all, 1.. from its lowest */
        *got = ui.hnote;
    else if (!strcmp(what, "stool"))
        *got = ui.stool;
    else if (!strcmp(what, "subfires"))   /* substeps played since last asked */
    {
        *got = sub_fires;
        sub_fires = 0;
    }
    else if (!strcmp(what, "specn"))      /* the EQ page's spectrum: its updates (a half each) since last asked */
    {
        *got = spec_n;
        spec_n = 0;
    }
    else if (!strcmp(what, "qmheld"))     /* the QUICK MIXER's moves held (a bit each), and the track they're on */
        *got = qm.held;
    else if (!strcmp(what, "qmtrk"))
        *got = qm.trk;
    else if (!strncmp(what, "fxret", 5))  /* fxretN: return N's output now (0 the verb, 1 the delay), millionths */
        *got = (int)(fx_return_now(atoi(what + 5)) * 1e6f);
    else if (!strcmp(what, "rmask"))      /* the tracks ROLL replays now, a bit each */
        *got = perf.rmask;
    else if (!strcmp(what, "stut"))       /* STUTTER: its slice in steps (0: none) */
        *got = perf.qs_n;
    else if (!strcmp(what, "stutps"))     /* the step it played last (1-based) */
        *got = perf.qs_ps + 1;
    else if (!strncmp(what, "tstep", 5)) { /* tstepN: track N's own step now (1-based; 0: not started) */
        int n = atoi(what + 5) - 1;
        *got = n >= 0 && n < NTRK ? sq.t[n].step + 1 : -1;
    }
    else if (!strncmp(what, "poct", 4))   /* poctN: a held OCT move on track N, octaves */
        *got = syn_perform_now(atoi(what + 4) - 1, 3);
    else if (!strncmp(what, "pcut", 4) || !strncmp(what, "prb", 3) || !strncmp(what, "pdb", 3))   /* pcutN prbN pdbN */
        *got = syn_perform_now(atoi(what + (what[1] == 'c' ? 4 : 3)) - 1, what[1] == 'c' ? 0 : what[1] == 'r' ? 1 : 2);
    else if (!strcmp(what, "drumlfo"))    /* the last hit's move by the drums' LFO, thousandths of the control's range */
        *got = drum_lfo_moved;
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
    } else if (!strcmp(what, "bdevs")) {  /* Bluetooth keyboards found; bdevok: one connected */
        *got = FM1B_BLE_CENTRAL ? btdev_count() : -1;
    } else if (!strcmp(what, "bdevok")) {
        *got = FM1B_BLE_CENTRAL ? btdev_ready() : -1;
    } else if (!strcmp(what, "btaon")) {  /* Bluetooth audio: on; btast: plat_bta_state; nundo: undo's steps now */
        *got = gbta.on;
    } else if (!strcmp(what, "btast")) {
        *got = FM1B_BT_AUDIO ? plat_bta_state() : -1;
    } else if (!strcmp(what, "nundo")) {
        *got = nundo;
    } else if (!strcmp(what, "seqout")) { /* SEQ OUT; ble: BLUETOOTH wanted */
        *got = seq_out();
    } else if (!strcmp(what, "blewant")) {
        *got = ble_wanted();
    } else if (!strcmp(what, "mixpp")) {  /* MIX PER PATTERN */
        *got = mix_pp();
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
    } else if (!strncmp(what, "synsplit_", 9)) {   /* synsplit_N: the first core's share of the voices held at N / 256
                                                  * (0: let it move again) */
        int v = atoi(what + 9);
        split_fixed = v > 0;
        if (v > 0)
            split_q8 = v;
        syn_split(split_q8);
        *got = 1;
    } else if (!strcmp(what, "synsplit")) {   /* the share now, of 256 */
        *got = split_q8;
    } else if (!strcmp(what, "fxasleep")) {   /* the returns asleep: bit 0 BongVerb, bit 1 BongDelay */
        *got = fx_returns_asleep();
    } else if (!strncmp(what, "fxsleep", 7) && what[7] >= '1' && what[7] < '1' + NTRK && !what[8]) {
        /* Only drums have a local effect chain. */
        *got = what[7] != '1' || drum_color_quiet >= FXS_AFTER;
    } else if (!strcmp(what, "setupview")) {   /* SETUP: 0 the clock and sounds, 1 the CPU by stage */
        *got = ui.setup_view;
    } else if (!strncmp(what, "cpu", 3) && what[3] >= '0' && what[3] < '0' + CS_NUM && !what[4]) {
        /* cpuN: stage N's running mean, us x 10 per block (0 DRUMS 1 SYNTH 2 FX IN 3 FX 4 MASTER 5 REST 6 TOTAL) */
        *got = (int)(cs_us[what[3] - '0'] * 10.0f + 0.5f);
    } else if (!strncmp(what, "cpufake", 7) && what[7] >= '0' && what[7] < '0' + CS_NUM && what[8] == '_') {
        /* cpufakeN_US: the budgets see stage N take US microseconds a block from now (1); the simulator only */
        int v = 0;
        const char *q = what + 9;
        while (*q >= '0' && *q <= '9')
            v = v * 10 + (*q++ - '0');
        gov_fake[what[7] - '0'] = (float)v;
        *got = 1;
    } else if (!strncmp(what, "drums2set_", 10) && what[10] >= '0' && what[10] <= '3' && !what[11]) {
        /* drums2set_N: the drums' place: 0 as built (the simulator: the first core), 1 the second core, 2 the first,
         * 3 drums2_pick's choice */
        d2.force = (uint8_t)(what[10] - '0');
        *got = 1;
    } else if (!strcmp(what, "drums2")) { /* the blocks the second core rendered the drums, so far */
        *got = (int)d2.blocks;
    } else if (!strncmp(what, "drums2pick_", 11)) {
        /* drums2pick_D_P_V_C_N: drums2_pick's choice for those times (us) and the present place N */
        int v[5] = {0, 0, 0, 0, 0}, k = 0;
        const char *q = what + 11;
        for (; *q && k < 5; q++) {
            if (*q == '_')
                k++;
            else if (*q >= '0' && *q <= '9')
                v[k] = v[k] * 10 + (*q - '0');
        }
        *got = drums2_pick((float)v[0], (float)v[1], (float)v[2], (float)v[3], v[4]);
    } else if (!strncmp(what, "demode_", 7) && what[7] >= '0' && what[7] <= '2' && !what[8]) {
        /* demode_N: the drums' engine: 0 the stock 808 / 909 (STOCK), 1 the DrumEngine dry, 2 through its bus */
        uint32_t f = plat_lock();
        v3_set_mode((unsigned)(what[7] - '0'));
        plat_unlock(f);
        *got = 1;
    } else if (!strcmp(what, "demode")) { /* the drums' engine now (demode_N) */
        *got = v3_mode;
    } else if (!strcmp(what, "govcap")) { /* the budgets: the synth's voices allowed */
        *got = gov.cap;
    } else if (!strcmp(what, "govcuts")) {   /* drum sounds cut so far */
        *got = (int)gov.cuts;
    } else if (!strcmp(what, "govshed")) {   /* voices shed so far */
        *got = (int)gov.caps;
    } else if (!strcmp(what, "govguard")) {   /* the guard's blocks so far */
        *got = (int)gov.guards;
    } else if (!strcmp(what, "govfx")) {  /* drum Color out of the chain now */
        *got = gov.fx_shed;
    } else if (!strncmp(what, "cpupk", 5) && what[5] >= '0' && what[5] < '0' + CS_NUM && !what[6]) {
        /* cpupkN: stage N's worst block lately, us x 10 */
        *got = (int)(cs_pk[what[5] - '0'] * 10.0f + 0.5f);
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
    } else if (!strncmp(what, "fxlive", 6)) {   /* fxliveI: what the FX knob I is set to in the engine now (THROW and
                                                 * WASH move it while held; the project keeps the knob) */
        int i = atoi(what + 6);
        if (i < 0 || i >= FXP_NUM)
            return 0;
        *got = fx_get(i);
    } else if (!strncmp(what, "fx", 2)) {    /* fxI: the FX knob I (dsp/fx.h FXP_*) */
        int i = atoi(what + 2);
        if (i < 0 || i >= FXP_ALL)
            return 0;
        *got = *fxv(i);
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
    } else if (!strncmp(what, "nudge", 5)) {   /* nudgeT_S: track T's step S late, sixteenths (the drums: the pad picked) */
        const char *u = strchr(what, '_');
        int t = atoi(what + 5), st = u ? atoi(u + 1) - 1 : -1, j;
        if (t < 1 || t > NTRK || st < 0 || st >= NSTEPS) return 0;
        j = t == 1 ? lock_find(dcur(), st, ui.pad, LK_NUDGE) : ilock_find(ipart(t - 1), st, IL_NUDGE);
        *got = j < 0 ? 0 : t == 1 ? dcur()->lk[j].val : ipart(t - 1)->lk[j].val;
    } else if (!strncmp(what, "xlk", 3)) {     /* xlkT_I: track T's lock I running now (XL_*), -999 none */
        const char *u = strchr(what, '_');
        int t = atoi(what + 3) - 1, i = u ? atoi(u + 1) : -1;
        if (t < 1 || t >= NTRK || i < 0 || i >= XL_NUM) return 0;
        *got = (xlk_on[t] >> i & 1u) ? xlk[t][i] : -999;
    } else if (!strncmp(what, "gkeep", 5)) {   /* gkeepT_S: track T's step S (1-based) kept; gcur: KEEP's step */
        const char *u = strchr(what, '_');
        int t = atoi(what + 5), st = u ? atoi(u + 1) - 1 : -1;
        if (t < 2 || t > NTRK || st < 0 || st >= NSTEPS) return 0;
        *got = gkept(t - 1, st);
    } else if (!strcmp(what, "gcur")) {
        *got = ui.gcur + 1;
    } else if (!strncmp(what, "dgen", 4)) {   /* dgenI: the drum part's generator: 0 seed 1 mut 2 style 3 kick 4 snare
                                               * 5 hats 6 perc 7 fill 8 accent */
        const dgen_t *g = dgen_of();
        const int i = atoi(what + 4), v[9] = {g->seed, g->mut, g->style, g->kick, g->snare, g->hats, g->perc, g->fill, g->accent};
        if (i < 0 || i > 8) return 0;
        *got = v[i];
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
