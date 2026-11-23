/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_V3_MACRO_H
#define FM1_V3_MACRO_H
#include "drum_bus.h"
enum { V3_SAT, V3_COMP, V3_TRANS, V3_LIMIT, V3_FX_COUNT };
enum { V3_SAT_DEFAULT=22, V3_COMP_DEFAULT=64, V3_TRANS_DEFAULT=86, V3_LIMIT_DEFAULT=64 };
#define V3_FX_DEFAULTS {V3_SAT_DEFAULT,V3_COMP_DEFAULT,V3_TRANS_DEFAULT,V3_LIMIT_DEFAULT}
/* pot is SAT/COMP/TRANS/LIMIT, one unsigned 7-bit reading each. */
void v3_macro_patch(BusPatch *patch, const unsigned char pot[4]);
/* Called once on the stereo sum after returns and level, before compression. */
void v3_saturate(float *left,float *right,unsigned n,unsigned knob);
#endif
