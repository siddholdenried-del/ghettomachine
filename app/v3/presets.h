/* Off-audio-thread voice catalog. A pad is 1..11; recipe and parameter
 * indices start at zero. Each factory patch matches recipe zero on every pad. */
#ifndef FM1_VOICE_PRESETS_H
#define FM1_VOICE_PRESETS_H
#include "kit_patch.h"

typedef struct {
    const char *label, *unit;
    int min_tick, max_tick, coarse_step, fine_step, decimals;
} KitParamSpec;

int kit_preset_count(int pad);
const char *kit_preset_name(int pad, int preset);
int kit_preset_apply(KitPatch *patch, int pad, int preset);
int kit_preset_match(const KitPatch *patch, int pad);  /* -1 = Custom */

int kit_param_count(int pad);
const KitParamSpec *kit_param_spec(int pad, int param);
/* Display tick / 10^decimals. Numeric fields have no option name. */
const char *kit_param_option(int pad, int param, int tick);
int kit_param_get(const KitPatch *patch, int pad, int param);
/* Clamp numeric or categorical values to the parameter range. Return 1 for
 * valid writes; invalid inputs and mode-locked controls leave patch as-is. */
int kit_param_set(KitPatch *patch, int pad, int param, int tick);
int kit_param_editable(const KitPatch *patch, int pad, int param);
#endif
