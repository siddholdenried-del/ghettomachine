/* SPDX-License-Identifier: GPL-3.0-only */
/* Link against either the original two-kit wrapper or the current wrapper;
 * compare the SHA-256 of its stdout to detect any DSP/sample/bus regression. */
#include <stdio.h>
#include "../dsp/drums.h"

int main(void)
{
    float left[128], right[128], rev[128], dly[128];
    for (int kit = KIT_909; kit <= KIT_808; kit++) {
        drums_init();
        for (int block = 0; block < 2100; block++) {
            if (block % 21 == 0)
                drums_trigger(kit, (block / 21) % DRUMS_NPADS,
                              block % 42 ? DRUMS_VEL_NORMAL : 1.0f);
            drums_render(kit, left, right, rev, dly, 128);
            fwrite(left, sizeof left, 1, stdout);
            fwrite(right, sizeof right, 1, stdout);
            fwrite(rev, sizeof rev, 1, stdout);
            fwrite(dly, sizeof dly, 1, stdout);
        }
    }
    return ferror(stdout) ? 1 : 0;
}
