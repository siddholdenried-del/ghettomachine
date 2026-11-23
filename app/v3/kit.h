/* Portable mono drum bus. Initialize off the audio thread. Patches are sampled
   on each strike. Where updates and rendering share a thread, no locks are needed. */
#ifndef FM1_ORIGINAL_KIT_H
#define FM1_ORIGINAL_KIT_H
#include "kit_patch.h"
#include "kit_math.h"
#include "shells.h"
#include "metal.h"
#include "perc.h"
enum { KIT_BLOCK = 256, KIT_PADS = 11 };
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
