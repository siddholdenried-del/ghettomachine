/* SPDX-License-Identifier: GPL-3.0-only */
/* Run against both the original 3D577AD6 synth and this branch. Stdout carries
 * the exact float master pair, sends, and all three stem pairs for every block.
 * Argument 0 selects all oscillators. 1..3 puts a packed loop on that track. */
#include "synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static float left[SYN_CTL], right[SYN_CTL], reverb[SYN_CTL], delay[SYN_CTL];
static unsigned char packed[3 * 1024];

static void write_block(void)
{
    static const float zero[SYN_CTL];
    const float *mix[] = {left, right, reverb, delay};
    for (int i = 0; i < 4; ++i)
        assert(fwrite(mix[i], sizeof zero, 1, stdout) == 1);
    for (int t = 1; t < SYN_TRACKS; ++t) {
        for (int side = 0; side < 2; ++side) {
            const float *stem = syn_track(t, side);
            assert(fwrite(stem ? stem : zero, sizeof zero, 1, stdout) == 1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    int sampler = atoi(argv[1]);
    if (sampler < 0 || sampler >= SYN_TRACKS) return 2;
    const int presets[] = {0, 0, 12, 4};
    unsigned rng = 0x1c04bc35u;
    for (unsigned k = 0; k < sizeof packed; ++k) {
        rng = rng * 1664525u + 1013904223u;
        packed[k] = rng >> 24;
    }
    syn_init();
    for (int t = 1; t < SYN_TRACKS; ++t) {
        int8_t patch[SP_NUM];
        syn_preset(presets[t], patch);
        patch[SP_VOICES] = t == 2 ? 8 : 2;
        patch[SP_SPREAD] = 94;
        patch[SP_GLIDE] = t == 2 ? 0 : 29;
        syn_sound(t, patch);
        syn_level(t, .50f + .1f * t);
        syn_mixer(t, 24 + t * 23, .13f * t, .23f * t);
        syn_source(t, t == sampler);
        if (t == sampler) {
            syn_sample(t, packed, 2048, 22050);
            syn_sample_shape(t, 10, 960, 1, 95, 60, 17);
            assert(syn_sample_frames(t) == 2048);
        }
    }
    unsigned audible[SYN_TRACKS] = {0};
    double reverb_energy = 0., delay_energy = 0.;
    for (int b = 0; b < 600; ++b) {
        if (!(b % 100)) {
            int root = 43 + b / 100;
            for (int t = 1; t < SYN_TRACKS; ++t) {
                syn_note_on_f(t, root + 5 * t, .87f, 64 * SYN_CTL, t != 2, b == 100 ? SN_ACCENT : 0);
                if (t == 2) {
                    syn_note_on(t, root + 14, .65f, 52 * SYN_CTL, 0);
                    syn_note_on(t, root + 17, .7f, 60 * SYN_CTL, 0);
                }
            }
        }
        for (int t = 1; t < SYN_TRACKS; ++t)
            syn_mod(t, SM_CUTOFF, (b % 43 - 21) / 21.f, .65f);
        syn_prepare();
        syn_half(b & 1);
        syn_half(!(b & 1));
        syn_mix(left, right, reverb, delay);
        for (int t = 1; t < SYN_TRACKS; ++t) {
            const float *stem = syn_track(t, 0);
            if (stem) {
                for (int i = 0; i < SYN_CTL; ++i)
                    audible[t] += stem[i] > .00001f || stem[i] < -.00001f;
            }
        }
        for (int i = 0; i < SYN_CTL; ++i) {
            reverb_energy += reverb[i] * reverb[i];
            delay_energy += delay[i] * delay[i];
        }
        write_block();
        if (b == 150 || b == 260 || b == 370) {
            int t = (b - 150) / 110 + 1;
            syn_cut_track(t);
            syn_mix_repack(left, right, reverb, delay);
            assert(!syn_track(t, 0));
            write_block();
        }
    }
    assert(audible[1] && audible[2] && audible[3]);
    assert(reverb_energy > 1e-6 && delay_energy > 1e-6);
}
