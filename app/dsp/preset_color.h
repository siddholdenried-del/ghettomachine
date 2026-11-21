/* Small factory Color recipes for expansion banks. Already stored per track by project format 6. */
#ifndef GM_PRESET_COLOR_H
#define GM_PRESET_COLOR_H
#include "color.h"
#include "synth.h"

/* Return whether anything changed. No recipe means leave the track entirely alone.
 * CLEAN bypasses, but leaves the track's existing knobs intact. */
static inline int preset_color_apply(color_params_t *track, int recipe)
{
    static const uint8_t wet[][5] = {
/*       amt wear wow digital dropout (all 0..127) */
        {26, 45,  0,  0,  0},    /* TAPE: restrained tape/body */
        {21, 13, 37,  0,  0},    /* DRIFT: slight wear and delayed modulation */
        {20, 17,  0, 43,  0},    /* EDGE: a little digital grit */
        {21, 22,  0,  0, 34},    /* DUST: limited wear/drop on textures */
    };
    int changed, i;
    if (recipe == SYN_PRESET_COLOR_KEEP)
        return 0;
    if (recipe == SYN_PRESET_COLOR_CLEAN) {
        changed = track->v[COL_ON] != 0;
        track->v[COL_ON] = 0;
        return changed;
    }
    if (recipe < SYN_PRESET_COLOR_TAPE || recipe > SYN_PRESET_COLOR_DUST)
        return 0;
    changed = track->v[COL_ON] == 0;
    track->v[COL_ON] = 1;
    for (i = 0; i < COL_ON; i++) {
        uint8_t v = wet[recipe - SYN_PRESET_COLOR_TAPE][i];
        changed |= track->v[i] != v;
        track->v[i] = v;
    }
    return changed;
}
#endif
