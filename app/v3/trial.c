/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's DrumEngine: the V6 synthesized kit (app/v3: kit, shells, metal, perc, presets, drum bus and macros)
 * as the drum track renders it. The code keeps its trial_* names and the v3 directory (the board scripts, receipts and
 * saved IDs use them).
 *
 * The level (GhettoMachine 0.5): the kit was made in a Mac audition whose full scale is the FM-1's master bus input,
 * so on the drum track it came out about 14 dB under the 808 and the 909, and its balance was the Mac's: the cymbal as
 * loud as the kick, the snare and the clap low. Now
 *   - every factory sound is trimmed (DE_TRIM, tenths of a dB, by pad and recipe: measured, the loudest 100 ms of a
 *     hit at the sequencer's normal velocity, K-weighted) to its pad's place in the stock kits' balance (the 808's and
 *     the 909's own pads, averaged: kick -1.7 LUFS, snare -6.5, clap -12.2, toms -7.0, closed hat -20.9, open hat
 *     -15.8, cymbal -13.5, percussion -13.5, on the drum track at level 100). A pad edited off its recipe (CUSTOM)
 *     keeps the trim of the recipe it came from;
 *   - after the drum bus and its limiter, DE_STAGE (x5, +14 dB: 1 / FX_BUS_TRIM) puts the kit on the drum track's
 *     scale, so the bus's own thresholds and ceiling keep their meaning: its -5 dB ceiling is -5 dB at the master;
 *   - the pads have a mix of their own (drum_bus.c bus_default_mix: pans, reverb and delay sends), and the track's
 *     level, pan and sends then apply as to the 808 and 909 (app.c drum_mix).
 * Each pad's Tone and Drive (BongDrums 0.4) are taken when it strikes, like every control, and shape its lane before
 * the mix; at 0 they leave it untouched. */
#include "trial.h"
#include "freestanding.h"
#include "kit.h"
#include "presets.h"
#include "drum_bus.h"
#include "macro.h"

/* Same declaration as fm1-base/include/plat.h. Pulling the full platform
 * header into this DSP unit also imports its independent math environment. */
extern uint32_t plat_ticks24(void);

static DrumBus bus;
static Kit kit;
/* Pad controls in a single RAM patch are sampled on the next strike. The audio render never
 * walks the factory catalog. The physical editor retains its browsing cursor. */
static KitPatch patch;
static volatile unsigned patch_version;
static unsigned char cursors[KIT_PADS];
#ifdef PLAT_HOST
/* The firmware's offline panel and standalone host DSP probes run one thread. */
static unsigned edit_lock(void) { return 0; }
static void edit_unlock(unsigned flags) { (void)flags; }
#else
extern uint32_t plat_lock(void);
extern void plat_unlock(uint32_t flags);
#define edit_lock plat_lock
#define edit_unlock plat_unlock
#endif
static BusMix mix;
static unsigned char knobs[4];
static float scratch[KIT_PADS][KIT_BLOCK];
static struct {
    float last_l, last_r, fade_l, fade_r;
    unsigned left, rendered;
} transition;
enum { FADE_FRAMES = 64 };

/* ---- the level */
#define DE_STAGE 5.0f                     /* the kit's scale to the drum track's (1 / FX_BUS_TRIM) */
#define DE_RAMP 32u                       /* a pad's trim changing (another recipe struck): over 0.7 ms */
/* each factory recipe's trim, tenths of a dB (presets.c's order; DE_TRIM_RECIPES the most a pad has) */
enum { DE_TRIM_RECIPES = KIT_GENRES };
static const int16_t DE_TRIM[KIT_PADS][DE_TRIM_RECIPES] = {
#include "levels.inc"
};
static float trim_now[KIT_PADS], trim_goal[KIT_PADS];
/* a pad muted while it sounds (trial_cut): its lane fades out over DE_GATE samples and stays out until it strikes
 * again, as a muted 808 or 909 pad's sound is cut (a mute let go does not bring a cut tail back) */
#define DE_GATE 64u
static float gate[KIT_PADS];
static unsigned char gate_shut[KIT_PADS];
/* a lane muted (trial_mute): its audio, not its hits. Its voices go on striking and sounding underneath; the lane fades
 * out over DE_GATE samples and back in when let go, so a tail still ringing comes back */
static unsigned char lane_mute[KIT_PADS];

static float db10_gain(int db10)
{
    return fm_exp2f((float)db10 * (0.1f * 0.16609640474f));   /* 10^(dB/20) = 2^(dB / 6.0206) */
}

int trial_trim_db10(unsigned pad, unsigned recipe)
{
    return pad >= 1 && pad <= KIT_PADS && recipe < DE_TRIM_RECIPES ? DE_TRIM[pad - 1][recipe] : 0;
}

static float pad_trim(unsigned i)         /* the recipe the pad is on, or came from */
{
    unsigned r = cursors[i];
    return db10_gain(r < DE_TRIM_RECIPES ? DE_TRIM[i][r] : 0);
}

/* ---- each pad's Tone and Drive (BongDrums 0.4 station.c), worked out when it strikes */
static struct {
    float a, gain, out, z, x1;
    signed char mode;                     /* -1 low-pass, 0 none, +1 high-pass */
    unsigned char drive;
} shape[KIT_PADS];

static void capture_shaping(unsigned i, const KitPatch *p)
{
    const float tone = p->tone[i], drive = p->drive[i];
    if (tone < 0.f) {                     /* low-pass 18 kHz -> 250 Hz */
        const float t = tone < -1.f ? 1.f : -tone;
        const float hz = 18000.f * fm_exp2f(t * -6.16992500f);    /* (250 / 18000)^t */
        shape[i].mode = -1;
        shape[i].a = 1.f - fm_exp2f(-6.2831853f * hz / (float)KIT_RATE * 1.44269504f);
    } else if (tone > 0.f) {              /* high-pass 20 Hz -> 2.5 kHz */
        const float t = tone > 1.f ? 1.f : tone;
        const float hz = 20.f * fm_exp2f(t * 6.96578428f);        /* (2500 / 20)^t */
        shape[i].mode = 1;
        shape[i].a = fm_exp2f(-6.2831853f * hz / (float)KIT_RATE * 1.44269504f);
    } else {
        shape[i].mode = 0;
    }
    if (drive > 0.f) {
        const float d = drive > 1.f ? 1.f : drive;
        shape[i].drive = 1;
        shape[i].gain = 1.f + 9.f * d;
        shape[i].out = fm_exp2f(-0.6f * fm_log2f(shape[i].gain));  /* gain^-0.6 */
    } else {
        shape[i].drive = 0;
    }
}

#ifdef PLAT_HOST
static float lane_pk[KIT_PADS];           /* each lane's peak out since the tests last asked (lanepkN) */
#endif

static void render_lanes(float *lanes[KIT_PADS], unsigned span)
{
    for (unsigned p = 0; p < KIT_PADS; ++p) {
        float *x = lanes[p];
        if (!x) {
            shape[p].z = shape[p].x1 = 0.f;
            trim_now[p] = trim_goal[p];
            continue;
        }
        if ((gate_shut[p] || lane_mute[p]) && gate[p] <= 0.f) {   /* cut (silent until its next strike) or muted */
            for (unsigned n = 0; n < span; ++n) x[n] = 0.f;
            continue;
        }
        if (shape[p].mode < 0) {
            float z = shape[p].z;
            const float a = shape[p].a;
            for (unsigned n = 0; n < span; ++n) { z += a * (x[n] - z); x[n] = z; }
            shape[p].z = z;
        } else if (shape[p].mode > 0) {
            float y = shape[p].z, x1 = shape[p].x1;
            const float a = shape[p].a;
            for (unsigned n = 0; n < span; ++n) { const float in = x[n]; y = a * (y + in - x1); x1 = in; x[n] = y; }
            shape[p].z = y;
            shape[p].x1 = x1;
        }
        if (shape[p].drive) {
            const float g = shape[p].gain, o = shape[p].out;
            for (unsigned n = 0; n < span; ++n) x[n] = o * fm_tanhf(g * x[n]);
        }
        if (trim_now[p] != trim_goal[p]) {   /* the trim (the bus applies trim_goal: drum_bus_render): only a change of
                                              * recipe is ramped here, from the old to the new, as a ratio to it */
            const float goal = trim_goal[p], inv = 1.f / goal;
            float t = trim_now[p];
            const unsigned ramp = span < DE_RAMP ? span : DE_RAMP;
            const float step = (goal - t) / (float)ramp;
            for (unsigned n = 0; n < ramp; ++n) { t += step; x[n] *= t * inv; }
            trim_now[p] = goal;
        }
        if (gate_shut[p] || lane_mute[p]) {   /* fading out (trial_cut, trial_mute) */
            float g = gate[p];
            for (unsigned n = 0; n < span; ++n) {
                g = g > 1.f / (float)DE_GATE ? g - 1.f / (float)DE_GATE : 0.f;
                x[n] *= g;
            }
            gate[p] = g;
        } else if (gate[p] < 1.f) {       /* a mute let go: back in over DE_GATE samples, the tail as it is now */
            float g = gate[p];
            for (unsigned n = 0; n < span; ++n) {
                g = g < 1.f - 1.f / (float)DE_GATE ? g + 1.f / (float)DE_GATE : 1.f;
                x[n] *= g;
            }
            gate[p] = g;
        }
#ifdef PLAT_HOST
        for (unsigned n = 0; n < span; ++n) {
            const float a = x[n] < 0.f ? -x[n] : x[n];
            if (a > lane_pk[p]) lane_pk[p] = a;
        }
#endif
    }
}

#ifdef PLAT_HOST
int trial_lane_peak(unsigned pad)        /* a lane's peak out since last asked, dB (-200: silent); asking resets it */
{
    if (pad < 1 || pad > KIT_PADS) return -1;
    const float a = lane_pk[pad - 1];
    lane_pk[pad - 1] = 0.f;
    return a > 1e-10f ? (int)(fm_log2f(a) * 6.0206f - .5f) : -200;
}
#endif

int trial_lane_gate(unsigned pad)        /* a lane's gate now, thousandths (the tests) */
{
    return pad >= 1 && pad <= KIT_PADS ? (int)(gate[pad - 1] * 1000.f + .5f) : -1;
}

void trial_mute(unsigned pad, int on)
{
    if (pad >= 1 && pad <= KIT_PADS)
        lane_mute[pad - 1] = (unsigned char)(on != 0);
}

void trial_cut(unsigned pad)
{
    if (pad >= 1 && pad <= KIT_PADS)
        gate_shut[pad - 1] = 1;
}

void trial_init(void)
{
    kit_init(&kit);
    patch = kit_default_patch();
    patch_version = 0;
    for (unsigned i = 0; i < KIT_PADS; ++i)   /* (the factory patch is the ELECTRO kit: each pad's preset in it) */
        cursors[i] = (unsigned char)kit_genre_recipe(0, (int)i + 1);
    V3_CLEAR(shape, 0, sizeof shape);
    drum_bus_init(&bus);
    mix = bus_default_mix();
    mix.dry_only = 1;
    static const unsigned char defaults[4] = V3_FX_DEFAULTS;
    for (unsigned i = 0; i < 4; ++i) knobs[i] = defaults[i];
    v3_macro_patch(&mix.bus, knobs);
    bus.saturation = knobs[0];
    for (unsigned i = 0; i < KIT_PADS; ++i) {
        trim_now[i] = trim_goal[i] = pad_trim(i);
        gate[i] = 1.f;
        gate_shut[i] = 0;
    }
    transition.last_l = transition.last_r = 0.f;
    transition.fade_l = transition.fade_r = 0.f;
    transition.left = transition.rendered = 0;
}

void trial_kit(int genre)
{
    KitPatch p = kit_default_patch();
    if (!kit_genre_apply(&p, genre)) return;
    unsigned flags = edit_lock();
    patch = p;
    ++patch_version;
    for (unsigned i = 0; i < KIT_PADS; ++i) {
        cursors[i] = (unsigned char)kit_genre_recipe(genre, (int)i + 1);
        trim_goal[i] = pad_trim(i);       /* (the next strike's level: its preset's trim) */
    }
    edit_unlock(flags);
}

void trial_patch(int what, int value)
{
    switch (what) {
    case 1:
        if ((unsigned)value >= 2u) return;
        patch.kick = kit_kick_preset(value);
        break;
    case 2:
        if ((unsigned)value >= 4u) return;
        patch.metal_mode = value;
        break;
    case 3: case 4:
        if ((unsigned)value >= 6u) return;
        patch.perc[what-3].mode = value;
        break;
    default: return;
    }
    ++patch_version;
}

/* A MIDI CC may arrive in the audio interrupt. Interrupt masking covers only
 * one fixed-size copy and a version word; lookup, comparisons and UI formatting
 * run outside that window. The main loop retries if a CC changed this pad. */
static unsigned patch_snapshot(KitPatch *copy)
{
    unsigned flags = edit_lock();
    unsigned version = patch_version;
    *copy = patch;
    edit_unlock(flags);
    return version;
}

void trial_snapshot(unsigned pad, KitPatch *copy)
{
    if (pad && pad <= KIT_PADS && copy) patch_snapshot(copy);
}

int trial_recipe(unsigned pad)
{
    if (!pad || pad > KIT_PADS) return -1;
    KitPatch copy;
    trial_snapshot(pad, &copy);
    return kit_preset_match(&copy, (int)pad);
}

static void panel_turn(unsigned pad, int param, int delta, int fine)
{
    if (!pad || pad > KIT_PADS || !delta) return;
    unsigned i = pad - 1;
    KitPatch copy;
    int matched;
    for (;;) {
        unsigned version = patch_snapshot(&copy);
        if (param < 0) {
            int count = kit_preset_count((int)pad);
            if (!count) return;
            int current = kit_preset_match(&copy, (int)pad);
            if (current < 0) current = cursors[i];
            matched = (current + delta % count + count) % count;
            if (!kit_preset_apply(&copy, (int)pad, matched)) return;
        } else {
            const KitParamSpec *spec = kit_param_spec((int)pad, param);
            if (!spec || !kit_param_editable(&copy, (int)pad, param)) return;
            int range = spec->max_tick - spec->min_tick;
            int steps = delta > range ? range : delta < -range ? -range : delta;
            int change = fine ? spec->fine_step : spec->coarse_step;
            int tick = kit_param_get(&copy, (int)pad, param) + steps*change;
            if (!kit_param_set(&copy, (int)pad, param, tick)) return;
            matched = kit_preset_match(&copy, (int)pad);
        }
        unsigned flags = edit_lock();
        if (patch_version == version) {
            patch = copy;
            ++patch_version;
            if (matched >= 0) cursors[i] = (unsigned char)matched;   /* (the trim the next strike takes) */
            edit_unlock(flags);
            return;
        }
        edit_unlock(flags);
    }
}

void trial_recipe_turn(unsigned pad, int delta) { panel_turn(pad, -1, delta, 0); }

void trial_parameter_turn(unsigned pad, int param, int delta, int fine)
{
    if (param >= 0) panel_turn(pad, param, delta, fine);
}

static const struct { unsigned offset; int lower, upper, scale; } kick_unit[] = {
    {offsetof(KickPatch,tune_hz),300,800,10}, {offsetof(KickPatch,decay_ms),100,2400,1},
    {offsetof(KickPatch,drop_st),0,360,10}, {offsetof(KickPatch,drop_ms),20,1200,10},
    {offsetof(KickPatch,click),0,1000,1000}, {offsetof(KickPatch,drive),0,1000,1000}
};

int trial_kick_bound(unsigned control, unsigned upper)
{
    if (control<1 || control>6) return 0;
    return upper ? kick_unit[control-1].upper : kick_unit[control-1].lower;
}

static float *kick_field(KickPatch *p,unsigned i)
{
    return (float *)((unsigned char *)p+kick_unit[i].offset);
}

static int kick_raw(float value,unsigned i)
{
    return (int)(value * (float)kick_unit[i].scale + .5f);
}

static int kick_preset_index(void)
{
    for (int p=0;p<2;++p) {
        KickPatch factory=kit_kick_preset(p);
        unsigned i=0;
        while (i<6 && *kick_field(&patch.kick,i)==*kick_field(&factory,i)) ++i;
        if (i==6) return p;
    }
    return 2;
}

static int *voice_model(unsigned pad)
{
    if (pad == 2) return &patch.snare_preset;
    if (pad == 3) return &patch.clap_preset;
    if (pad == 9) return &patch.metal_mode;
    return 0;
}

static float *voice_knob(unsigned pad, unsigned control)
{
    if (pad == 2) {
        switch (control) {
        case 1: return &patch.snare_tune;
        case 2: return &patch.snare_body;
        case 3: return &patch.snare_snappy;
        case 4: return &patch.snare_wire;
        }
    } else if (pad == 3) {
        switch (control) {
        case 1: return &patch.clap_spread;
        case 2: return &patch.clap_tone;
        case 3: return &patch.clap_tail;
        }
    }
    return 0;
}

int trial_voice_get(unsigned pad, unsigned control)
{
    if (pad==1) {
        if (!control) return kick_preset_index();
        return control<=6 ? kick_raw(*kick_field(&patch.kick,control-1),control-1) : -1;
    }
    if (!control) {
        int *model=voice_model(pad);
        return model ? *model : -1;
    }
    float *value=voice_knob(pad, control);
    return value ? (int)(*value * 127.f + .5f) : -1;
}

void trial_voice_set(unsigned pad, unsigned control, int value)
{
    if (pad == 0 || pad > KIT_PADS) return;
    ++patch_version;
    if (pad==1) {
        if (!control) {
            if ((unsigned)value<2) patch.kick=kit_kick_preset(value);
        } else if (control<=6 && value>=trial_kick_bound(control,0) && value<=trial_kick_bound(control,1)) {
            unsigned i=control-1;
            float *field=kick_field(&patch.kick,i);
            /* Re-entering a displayed factory number must reproduce its exact
               float even on a target that optimizes division by a constant. */
            for (int p=0;p<2;++p) {
                KickPatch factory=kit_kick_preset(p);
                float exact=*kick_field(&factory,i);
                if (kick_raw(exact,i)==value) { *field=exact; return; }
            }
            *field=(float)value/(float)kick_unit[i].scale;
        }
        return;
    }
    if (!control) {
        int *model=voice_model(pad);
        if (model && (unsigned)value < (pad==9 ? 4u : 3u)) *model=value;
    } else {
        float *knob=voice_knob(pad,control);
        if (knob && (unsigned)value<=127u) *knob=(float)value*(1.f/127.f);
    }
}

void trial_hit(unsigned pad, float velocity)
{
    if (pad >= 1 && pad <= KIT_PADS) {
        const unsigned i = pad - 1;
        capture_shaping(i, &patch);
        trim_goal[i] = pad_trim(i);
        gate_shut[i] = 0;
        if (!lane_mute[i])
            gate[i] = 1.f;
        kit_hit(&kit, (int)pad, velocity, &patch);
    }
}

void trial_hit_moved(unsigned pad, float velocity, int param, float move)
{
    static KitPatch moved;                /* (the audio interrupt's: one hit at a time) */
    const KitParamSpec *spec = kit_param_spec((int)pad, param);
    if (pad < 1 || pad > KIT_PADS)
        return;
    moved = patch;
    if (spec && kit_param_editable(&moved, (int)pad, param)) {
        const float d = move * (float)(spec->max_tick - spec->min_tick);
        kit_param_set(&moved, (int)pad, param, kit_param_get(&moved, (int)pad, param) + (int)(d + (d < 0.f ? -.5f : .5f)));
    }
    {
        const unsigned i = pad - 1;
        capture_shaping(i, &moved);
        trim_goal[i] = pad_trim(i);
        gate_shut[i] = 0;
        if (!lane_mute[i])
            gate[i] = 1.f;
        kit_hit(&kit, (int)pad, velocity, &moved);
    }
}

static void begin_transition(void)
{
    if (!transition.rendered) return;
    transition.fade_l = transition.last_l;
    transition.fade_r = transition.last_r;
    transition.left = FADE_FRAMES;
}

/* Only explicit exit to the stock kits cuts voices */
void trial_quiet(void)
{
    kit_init(&kit);
    V3_CLEAR(shape, 0, sizeof shape);
    for (unsigned i = 0; i < KIT_PADS; ++i) {
        trim_now[i] = trim_goal[i];
        gate[i] = 1.f;
        gate_shut[i] = 0;
    }
    transition.rendered = transition.left = 0;
    bus.comp_env = bus.fast_env = bus.slow_env = bus.limiter_reduction = 0;
    bus.comp_gr_db = bus.limiter_gr_db = 0;
}

void trial_macro(unsigned which, unsigned value)
{
    if (which >= 4u) return;
    if (value > 127u) value = 127u;
    if (knobs[which] == value) return;
    if (!mix.dry_only) begin_transition();
    knobs[which] = (unsigned char)value;
    v3_macro_patch(&mix.bus, knobs);
    bus.saturation = knobs[0];
}

unsigned trial_macro_value(unsigned which)
{
    return which < 4u ? knobs[which] : 0u;
}

/* after the bus. DRY has its out folded into its one pass (drum_bus.c render_dry): only the mode change's de-click is
 * left, on the drum track's scale. With the bus on: the audition trim, the de-click, the final limiter (on the kit's
 * own scale, its ceiling's, and after the de-click as it always was: else it holds the bus's first transient down for
 * half a second), then the drum track's scale. The de-click's level is kept on the drum track's scale (last_l) */
static void fade_in(float *l, float *r, unsigned n, float scale)
{
    unsigned end = transition.left < n ? transition.left : n;
    const float fl = transition.fade_l * scale, fr = transition.fade_r * scale;
    for (unsigned i = 0; i < end; ++i) {
        float t = (float)(FADE_FRAMES - transition.left) / (float)FADE_FRAMES;
        l[i] = fl + t * (l[i] - fl);
        r[i] = fr + t * (r[i] - fr);
        --transition.left;
    }
}

static void finish_bus(float *l, float *r, float *rev, float *dly, unsigned n)
{
    if (!mix.dry_only) {
        for (unsigned i = 0; i < n; ++i) {
            l[i] *= .63095734f;
            r[i] *= .63095734f;
        }
        fade_in(l, r, n, 1.f / DE_STAGE);
        drum_bus_limit_final(&bus, &mix, l, r, n);
        for (unsigned i = 0; i < n; ++i) {
            l[i] *= DE_STAGE;
            r[i] *= DE_STAGE;
            rev[i] *= DE_STAGE * .63095734f;
            dly[i] *= DE_STAGE * .63095734f;
        }
    } else {
        bus.limiter_reduction = 0.f;
        bus.limiter_gr_db = 0.f;
        fade_in(l, r, n, 1.f);
    }
    transition.last_l = l[n - 1];
    transition.last_r = r[n - 1];
    transition.rendered = 1;
}

void trial_render(float *l, float *r, float *rev, float *dly, unsigned n, unsigned mode,
                  unsigned *voice_ticks, unsigned *bus_ticks)
{
    unsigned voices = 0, effects = 0;
    int dry = mode == 1;
    if (dry != mix.dry_only) {
        begin_transition();
        mix.dry_only = dry;
    }
    while (n) {
        unsigned span = n < KIT_BLOCK ? n : KIT_BLOCK;
        float *lanes[KIT_PADS];
        uint32_t start = plat_ticks24();
        kit_render_active_lanes(&kit, scratch, lanes, span);
        render_lanes(lanes, span);
        uint32_t split = plat_ticks24();
        voices += split - start;
        drum_bus_render(&bus, &mix, lanes, trim_goal, DE_STAGE * .63095734f, l, r, rev, dly, span);
        finish_bus(l, r, rev, dly, span);
        effects += plat_ticks24() - split;
        n -= span;
        l += span;
        r += span;
        rev += span;
        dly += span;
    }
    if (voice_ticks) *voice_ticks = voices;
    if (bus_ticks) *bus_ticks = effects;
}
