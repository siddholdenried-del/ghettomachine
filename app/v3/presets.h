/* Off-audio-thread voice catalog. A pad is 1..11; recipe and parameter
 * indices start at zero. Every drum's presets are named after the genre they're for, one per genre in the kits'
 * order; the factory patch (kit_default_patch) is the ELECTRO kit: recipe zero on every pad but PERC 2's. */
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

/* the genre kits (KIT_GENRES): a name; the preset each pad takes (PERC 2 another genre's percussion); all of them
 * applied to a patch (1 = done) */
enum { KIT_GENRES = 12 };
int kit_genre_count(void);
const char *kit_genre_name(int genre);
int kit_genre_recipe(int genre, int pad);
int kit_genre_apply(KitPatch *patch, int genre);

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
