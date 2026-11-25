/* Controls are sampled at each strike. The caller owns and can copy this patch. */
#ifndef FM1_ORIGINAL_PATCH_H
#define FM1_ORIGINAL_PATCH_H
/* (GhettoMachine 0.5) mode packs three controls, so the patch keeps its size: bits 0-3 the model (A through I), 4-5
   the filter (0 low-pass, as before; 1 band-pass; 2 high-pass), 8-17 the resonance in thousandths (0: the filter as it
   was). A patch from before has only the model. */
#define PERC_MODEL(m) ((m) & 15)
#define PERC_FILTER(m) (((m) >> 4) & 3)
#define PERC_RESO(m) (((m) >> 8) & 1023)
typedef struct {
    int mode;                         /* PERC_MODEL, PERC_FILTER, PERC_RESO */
    float tune, decay, width;
    float sweep;                      /* -1 to +1, neutral at 0 */
    float speed;
} PercPatch;

/* Amp decay is time to -60 dB. Drop time is the e-folding time of the
   frequency excursion above Tune; click and drive are in [0,1]. */
typedef struct {
    float tune_hz, decay_ms, drop_st, drop_ms, click, drive;
} KickPatch;
KickPatch kit_kick_preset(int which); /* 0 = 808, 1 = 909 */

typedef struct {
    int model;
    float tune, decay, attack;
} TomPatch;
/* Settled membrane pitch for Low/Mid/High Tom (pads 4–6), in Hz.
   Returns zero for another pad. Includes the current model and tune. */
float kit_tom_pitch_hz(int pad, TomPatch controls);

typedef struct {
    int profile;
    float decay, brightness;
} MetalPatch;

typedef struct {
    KickPatch kick;
    int body_preset, snare_preset, clap_preset;
    float body_attack, body_tune, body_decay;
    float snare_tune, snare_body, snare_snappy, snare_wire;
    float clap_spread, clap_tone, clap_tail;
    int metal_mode, hat_profile, cymbal_profile;
    PercPatch perc[2];
    /* A zero bit inherits the original body/profile fields. Take a local
       snapshot before editing a tom, hat or cymbal for the first time. */
    unsigned local_pads;
    TomPatch tom[3];
    MetalPatch hat[2], cymbal;
    /* Per-drum output shaping (BongDrums 0.4), sampled at each strike like every control.
       Tone -1 (low-pass, darker) .. 0 (off) .. +1 (high-pass, thinner);
       Drive 0 (off) .. 1. Both at zero leave the voice untouched. */
    float tone[11], drive[11];
    /* The snare's overall decay (GhettoMachine 0.5): -1 (a quarter as long) .. 0 (as its Body and Wire set it) ..
       +1 (three times as long); its body, upper head and wires together. */
    float snare_decay;
} KitPatch;

static inline TomPatch kit_tom_controls(const KitPatch *p, int pad)
{
    if (pad >= 4 && pad <= 6 && (p->local_pads & (1u << (pad-1))))
        return p->tom[pad-4];
    return (TomPatch){p->body_preset, p->body_tune, p->body_decay, p->body_attack};
}

static inline MetalPatch kit_metal_controls(const KitPatch *p, int pad)
{
    if (pad >= 7 && pad <= 9 && (p->local_pads & (1u << (pad-1))))
        return pad == 9 ? p->cymbal : p->hat[pad-7];
    return (MetalPatch){pad == 9 ? p->cymbal_profile : p->hat_profile, .5f, .5f};
}

void kit_make_pad_local(KitPatch *p, int pad);
KitPatch kit_default_patch(void);
#endif
