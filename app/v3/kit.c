#include "freestanding.h"
#include "../dsp/gm_ram.h"
#include "kit.h"

KickPatch kit_kick_preset(int which)
{
    /* Tune Hz, amplitude T60 ms, pitch span in semitones, sweep ms, click,
       drive. These are presets for one renderer; recalling either copies all
       six values into the patch that the user edits. */
    static const KickPatch recipe[2] = {
        {52.0f, 1500.f, 11.f, 5.f, .30f, .68f},
        {53.0f, 880.f, 28.f, 14.f, .54f, .82f}
    };
    return recipe[which == 1];
}

void kit_make_pad_local(KitPatch *p, int pad)
{
    if (!p || pad < 4 || pad > 9) return;
    const unsigned bit = 1u << (pad-1);
    if (p->local_pads & bit) return;
    if (pad <= 6) p->tom[pad-4] = kit_tom_controls(p, pad);
    else if (pad <= 8) p->hat[pad-7] = kit_metal_controls(p, pad);
    else p->cymbal = kit_metal_controls(p, pad);
    p->local_pads |= bit;
}

KitPatch kit_default_patch(void)
{
    KitPatch p = {0};
    p.kick = kit_kick_preset(0);
    p.body_attack = 0.48f;
    p.body_tune = p.body_decay = 0.50f;
    p.snare_tune = 0.47f; p.snare_body = 0.55f;
    p.snare_snappy = 0.60f; p.snare_wire = 0.46f;
    p.clap_spread = 0.46f; p.clap_tone = 0.54f; p.clap_tail = 0.48f;
    /* (the Electro and Techno recipes, presets.c: model | filter << 4 | reso << 8) */
    p.perc[0] = (PercPatch){6 | 640 << 8, 0.44f, 0.40f, 0.90f, -0.82f, 0.80f};
    p.perc[1] = (PercPatch){4 | 1 << 4 | 560 << 8, 0.68f, 0.42f, 0.70f,  0.18f, 0.50f};
    return p;
}

void kit_init(Kit *kit)
{
    V3_CLEAR(kit, 0, sizeof(*kit));
    shells_init(&kit->shells);
    metal_init(&kit->metal);
    perc_init(&kit->perc[0]);
    perc_init(&kit->perc[1]);
    kit->perc[1].noise = 0x37e95ac1u;  /* independent channel noise stream */
}

void kit_hit(Kit *kit, int pad, float velocity, const KitPatch *patch)
{
    if (!patch || pad < 1 || pad > KIT_PADS || !(velocity > 0)) return;
    velocity = kit_unit(velocity);
    if (pad <= 6) shells_hit(&kit->shells, pad, velocity, patch);
    else if (pad <= 9) metal_hit(&kit->metal, pad, velocity, patch);
    else perc_hit(&kit->perc[pad-10], velocity, &patch->perc[pad-10]);
}

/* Keep the ordinary bus linear. When a player stacks simultaneous accents
   above the knee, a continuous curve keeps the output below full scale. */
float kit_finish(float mix)
{
    const float gain = 0.75f;
    const float knee = 0.82f;
    float s = mix * gain;
    const float absolute = s < 0 ? -s : s;
    if (absolute <= knee) return s;
    const float over = absolute - knee;
    const float limited = knee + over * (1.0f-knee) / (over + (1.0f-knee));
    return s < 0 ? -limited : limited;
}

void kit_render(Kit *kit, float *out, unsigned frames)
{
    while (frames) {
        unsigned n = frames > KIT_BLOCK ? KIT_BLOCK : frames;
        V3_CLEAR(out, 0, n * sizeof(*out));
        shells_render(&kit->shells, out, n);
        metal_render(&kit->metal, out, n);
        GM_FAR(perc_render)(&kit->perc[0], out, n);
        GM_FAR(perc_render)(&kit->perc[1], out, n);
        for (unsigned i = 0; i < n; ++i) out[i] = kit_finish(out[i]);
        frames -= n;
        out += n;
    }
}

static void render_lanes(Kit *kit, float *lanes[KIT_PADS], unsigned n)
{
    /* Idle voices ignore their output pointers. Always advance the shared
       metal frame counter so a future hit resumes at its free-running phase. */
    shells_render_routed(&kit->shells, lanes, n);
    metal_render_routed(&kit->metal, lanes + 6, n);
    GM_FAR(perc_render)(&kit->perc[0], lanes[9], n);
    GM_FAR(perc_render)(&kit->perc[1], lanes[10], n);
}

void kit_lanes(Kit *kit, float *lanes[KIT_PADS], unsigned n)
{
    for (unsigned pad = 0; pad < KIT_PADS; ++pad)
        V3_CLEAR(lanes[pad], 0, n * sizeof(float));
    render_lanes(kit, lanes, n);
}

void kit_render_active_lanes(Kit *kit, float storage[KIT_PADS][KIT_BLOCK],
                             float *lanes[KIT_PADS], unsigned n)
{
    unsigned active = shells_active_mask(&kit->shells);
    /* Either profile can still be ringing after a selector change. Low two
       source bits are the hats, every remaining articulation feeds pad 9. */
    unsigned metal = kit->metal.bank[0].active | kit->metal.bank[1].active;
    active |= (metal & 3u) << 6;
    if (metal >> 2) active |= 1u << 8;
    if (kit->perc[0].level != 0.f) active |= 1u << 9;
    if (kit->perc[1].level != 0.f) active |= 1u << 10;
    for (unsigned pad = 0; pad < KIT_PADS; ++pad) {
        if (active & (1u << pad)) {
            lanes[pad] = storage[pad];
            V3_CLEAR(lanes[pad], 0, n * sizeof(float));
        } else {
            lanes[pad] = NULL;
        }
    }
    render_lanes(kit, lanes, n);
}
