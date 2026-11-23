/* Portable mono drum bus. Initialize off the audio thread. Patches are sampled
   on each strike. Where updates and rendering share a thread, no locks are needed. */
#ifndef FM1_ORIGINAL_KIT_H
#define FM1_ORIGINAL_KIT_H
#include "kit_patch.h"
#include "kit_math.h"
#include "shells.h"
#include "metal.h"
#include "perc.h"
/* KIT_BLOCK: the most a render call takes, and the lanes' stride. GhettoMachine 0.5: 64 on the FM-1 (the adapter's
   lane storage 2.8 KB rather than 11 KB of its 96 KB of RAM); the voices give the same samples whatever the span
   (tests: 256, 128, 64 and 32 compared, every pad and recipe) */
enum { KIT_BLOCK = 64, KIT_PADS = 11 };
typedef struct {
    Shells shells;
    Metal metal;
    PercVoice perc[2];
} Kit;

void kit_init(Kit *kit);
void kit_hit(Kit *kit, int pad, float velocity, const KitPatch *patch);
/* Overwrite mono samples. Subdivides larger host buffers at the FM-1 block size. */
void kit_render(Kit *kit, float *out, unsigned frames);
/* Write separate pre-trim pad streams, at most KIT_BLOCK frames. */
void kit_lanes(Kit *kit, float *lanes[KIT_PADS], unsigned n);
/* Overwrite active pad streams and hand back their pointers in lanes. A null
 * output pointer identifies an inactive pad whose storage remains untouched.
 * The caller splits at note boundaries; 0 <= n <= KIT_BLOCK. */
void kit_render_active_lanes(Kit *kit, float storage[KIT_PADS][KIT_BLOCK],
                             float *lanes[KIT_PADS], unsigned n);
float kit_finish(float mix);
#endif
