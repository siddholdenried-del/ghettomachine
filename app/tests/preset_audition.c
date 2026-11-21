/* Standalone dry 44.1 kHz GhettoMachine synth audition/QA. Never uses the web or the device. */
#include "synth.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define BLOCKS 3300  /* 2.395 seconds per excerpt */
#define FACTORY SYN_FACTORY_LEGACY_COUNT
#define OLD_USER_COUNT 16
static void put_u(FILE *f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++, v >>= 8) fputc(v & 255, f);
}
static FILE *wav_open(const char *dir, const char *name, int frames) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite("RIFF", 4, 1, f); put_u(f, 36 + 4 * frames, 4);
    fwrite("WAVEfmt ", 8, 1, f); put_u(f, 16, 4); put_u(f, 1, 2);
    put_u(f, 2, 2); put_u(f, SR, 4); put_u(f, SR * 4, 4); put_u(f, 4, 2); put_u(f, 16, 2);
    fwrite("data", 4, 1, f); put_u(f, 4 * frames, 4);
    return f;
}
static void trigger(int group, int variant, int b, int mono) {
    static const int mono_at[] = {88, 259, 603, 775, 1119, 1377, 1635, 1807, 2151, 2408};
    static const int riff[][10] = {
        {36,36,43,36,46,43,36,48,43,34}, {37,37,44,49,47,37,35,42,49,44},
        {40,40,47,46,52,40,44,43,47,40}, {34,34,41,46,39,41,34,46,44,34},
        {48,55,58,55,46,50,48,62,55,53}, {36,36,43,46,34,36,48,43,46,34},
        {41,48,53,52,54,48,41,53,56,44}
    };
    static const int chords[][2][4] = {
        {{48,58,63,67},{46,56,60,65}}, {{43,53,58,62},{41,51,55,60}},
        {{51,55,58,62},{50,53,57,60}}, {{48,55,58,62},{45,52,55,60}},
        {{48,51,58,62},{46,53,56,60}}, {{53,60,63,67},{55,58,65,69}},
        {{46,53,56,60},{44,51,55,58}}
    };
    static const int hits_at[] = {88, 432, 689, 1119, 1291, 1550, 1980, 2237, 2495};
    static const int drums[][5] = {{36,45,40,52,43},{53,69,57,72,62},{46,50,43,55,47},
        {58,70,64,76,61},{52,54,50,59,52},{58,66,61,73,65},{33,36,31,41,33}};
    if (group == 0) {
        for (int i = 0; i < 10; i++) if (b == mono_at[i]) {
            const int slide = variant == 1 && (i == 3 || i == 9);
            const int tie = variant == 1 && (i == 2 || i == 8);
            const int length = tie ? 212 : i == 9 ? 370 : variant == 5 ? 148 : (i % 3 == 0 ? 132 : 84);
            syn_note_on_f(1, riff[variant][i], i % 3 == 0 ? 0.87f : 0.68f,
                          length * SYN_CTL, mono, (slide ? SN_SLIDE : 0) | (variant == 1 && i % 3 == 0 ? SN_ACCENT : 0));
        }
    } else if (group == 1) {
        const int pad = variant == 2 || variant == 6;
        static const int at[] = {88, 336, 775, 1205, 1721, 1980, 2495};
        for (int i = 0; i < 7; i++) if (b == at[i] && (!pad || i == 0 || i == 4)) {
            const int length = pad ? 1020 : variant == 0 && i % 2 == 0 ? 185 : i % 3 == 1 ? 70 : 118;
            for (int n = 0; n < 4; n++) syn_note_on(1, chords[variant][i >= 4][n],
                (i % 2 == 0 ? 0.79f : 0.70f) - 0.04f * n, length * SYN_CTL, 0);
        }
    } else {
        for (int i = 0; i < 9; i++) if (b == hits_at[i]) syn_note_on(1, drums[variant][i % 5],
            i % 3 == 0 ? 0.90f : i % 2 == 0 ? 0.72f : 0.58f, (variant == 0 ? 295 : 215) * SYN_CTL, mono);
    }
}

static int legacy(const char *dest) {
    FILE *f = fopen(dest, "wb");
    if (!f) return 1;
    syn_init();
    for (int p = 0; p < FACTORY; p++) {
        int8_t v[SP_NUM]; int mono = syn_preset(p, v);
        size_t name_bytes = strlen(syn_preset_name(p)) + 1;
        fwrite(syn_preset_name(p), name_bytes, 1, f);
        put_u(f, (uint32_t)mono, 1); fwrite(v, sizeof v, 1, f);
        syn_kill(); syn_sound(1, v); syn_level(1, 0.7874f); syn_mixer(1, 64, 0, 0);
        for (int b = 0; b < 650; b++) {
            if (b == 12 || b == 355) {
                syn_note_on(1, b == 12 ? 48 : 55, .8f, 256 * SYN_CTL, mono);
                if (!mono) syn_note_on(1, b == 12 ? 55 : 62, .72f, 256 * SYN_CTL, 0);
            }
            float l[SYN_CTL],r[SYN_CTL],rev[SYN_CTL],dly[SYN_CTL];
            syn_prepare(); syn_half(0); syn_half(1); syn_mix(l, r, rev, dly);
            fwrite(l, sizeof l, 1, f); fwrite(r, sizeof r, 1, f);
        }
    }
    return fclose(f) != 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--legacy")) return legacy(argv[2]);
    if (argc != 2 || syn_presets() < FACTORY + 21) return 2;
    static const char *files[] = {"GhettoMachine-Motor-City-Mono.wav", "GhettoMachine-Motor-City-Chords.wav", "GhettoMachine-Motor-City-Percussion.wav"};
    printf("id\tname\tsecond\tdry_peak_dbfs\tdry_active_rms_dbfs\tstress_peak_dbfs\n");
    int fail = 0;
    syn_init();
    for (int group = 0; group < 3; group++) {
        FILE *f = wav_open(argv[1], files[group], BLOCKS * SYN_CTL * 7);
        if (!f) return 2;
        for (int i = 0; i < 7; i++) {
            int p = FACTORY + group * 7 + i;
            int8_t v[SP_NUM];
            int mono = syn_preset(p, v);
            if (mono != (group != 1) || strlen(syn_preset_name(p)) > 15) fail = 1;
            for (int k = 0; k < p; k++)
                if (!strcmp(syn_preset_name(p), syn_preset_name(k))) fail = 1;
            for (int k = 0; k < SP_NUM; k++) {
                const syn_param_t *spec = syn_param(k);
                if (v[k] < spec->min || v[k] > spec->max) fail = 1;
            }
            float peak = 0, stress_peak = 0;
            double sum = 0;
            int n = 0;
            for (int mode = 0; mode < 2; mode++) {
                syn_kill(); syn_sound(1, v); syn_level(1, 100.0f / 127.0f); syn_mixer(1, 64, 0, 0);
                for (int b = 0; b < BLOCKS; b++) {
                    float l[SYN_CTL], r[SYN_CTL], rev[SYN_CTL], dly[SYN_CTL];
                    if (mode == 0) trigger(group, i, b, mono);
                    else if (b == 88 || b == 1100 || b == 2150) {
                        int root = b == 88 ? 38 : b == 1100 ? 56 : 69;
                        for (int j = 0; j < (mono ? 1 : v[SP_VOICES]); j++)
                            syn_note_on(1, root + j * 4, 1.0f, 620 * SYN_CTL, mono);
                    }
                    syn_prepare(); syn_half(0); syn_half(1); syn_mix(l, r, rev, dly);
                    for (int j = 0; j < SYN_CTL; j++) for (int side = 0; side <= 1; side++) {
                        float x = side ? r[j] : l[j], a = fabsf(x);
                        if (!isfinite(x) || a >= .75f || rev[j] != 0.0f || dly[j] != 0.0f) fail = 1;
                        if (mode != 0) { if (a > stress_peak) stress_peak = a; continue; }
                        if (a > peak) peak = a;
                        if (a > .0001f) { sum += x * x; n++; }
                        /* No gain, processing, or FX. Tiny fade to silence just at the section boundary. */
                        if (b >= BLOCKS - 32) x *= (float)((BLOCKS - b) * SYN_CTL - j) / (32 * SYN_CTL);
                        if (!isfinite(x)) x = 0;
                        if (x > 1) x = 1;
                        if (x < -1) x = -1;
                        put_u(f, (uint16_t)(int16_t)lrintf(x * 32767.0f), 2);
                    }
                }
            }
            if (peak < .02f || n < 1000) fail = 1;
            printf("%d\t%s\t%.3f\t%.1f\t%.1f\t%.1f\n", p+OLD_USER_COUNT, syn_preset_name(p),
                (double)i * BLOCKS * SYN_CTL / SR, 20 * log10(peak), 10 * log10(sum / n), 20 * log10(stress_peak));
        }
        if (fclose(f)) return 2;
    }
    return fail;
}
