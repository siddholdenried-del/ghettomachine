/* Dry factory-bank audition through the current instrument mixer. */
#include "synth.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RATE 44100
#define BLOCKS 3300
#define FRAMES (BLOCKS * SYN_CTL)
#define OLD (SYN_FACTORY_LEGACY_COUNT + SYN_MOTOR_CITY_COUNT)
#define USER_SLOTS 16
#define SELECTIONS 9
static void le(FILE *f, uint32_t v, int n)
{
    for (int i = 0; i < n; i++, v >>= 8) fputc(v & 255u, f);
}
static FILE *wav(const char *dir, const char *name, unsigned frames)
{
    char p[1024];
    if (snprintf(p, sizeof p, "%s/%s", dir, name) >= (int)sizeof p) return NULL;
    FILE *f = fopen(p, "wb");
    if (!f) return NULL;
    fwrite("RIFF", 4, 1, f); le(f, 36u + 4u * frames, 4);
    fwrite("WAVEfmt ", 8, 1, f); le(f, 16, 4);
    le(f, 1, 2); le(f, 2, 2); le(f, RATE, 4); le(f, RATE * 4, 4); le(f, 4, 2); le(f, 16, 2);
    fwrite("data", 4, 1, f); le(f, frames * 4, 4);
    return f;
}
static void pcm(FILE *f, float sample)
{
    if (f) {
        sample = !isfinite(sample) ? 0.f : sample < -1.f ? -1.f : sample > 1.f ? 1.f : sample;
        le(f, (uint16_t)(int16_t)lrintf(sample * 32767.f), 2);
    }
}

static void phrase(int b, int bank, int kind, int variant, int stress, const int8_t *v)
{
    static const int riffs[][11] = {
        {0,0,7,12,10,12,7,3,0,10,7},
        {0,12,0,7,3,10,13,12,7,0,5},
        {0,0,1,7,6,0,8,1,6,10,0},
        {0,0,7,10,12,0,5,3,10,5,0},
    };
    static const int metro[][11] = {
        {74,320,486,733,925,1151,1235,1658,1950,2165,2600},
        {74,245,502,674,817,1138,1431,1533,1999,2171,2540},
        {74,235,398,568,728,1020,1449,1640,1804,1960,2500},
        {74,332,509,772,1138,1334,1533,1700,2010,2363,2540},
    };
    static const int interval[][4] = {{0,3,10,14}, {0,7,10,15}, {0,5,10,13}, {0,3,7,14}};
    if (stress) {
        if (b == 70 || b == 1100 || b == 2150) {
            int note = b == 70 ? 35 : b == 1100 ? 49 : 67;
            for (int k = 0; k < (kind == 1 ? v[SP_VOICES] : 1); k++)
                syn_note_on_f(1, note + k * 4, 1.f, 640 * SYN_CTL, kind != 1, 0);
        }
        return;
    }
    if (kind == 1) {
        int pad = v[SP_AA] >= 42;
        static const int chord_at[] = {74, 386, 780, 1215, 1688, 1950, 2410};
        for (int i = 0; i < 7; i++) if (b == chord_at[i] && (!pad || i == 0 || i == 4)) {
            int note = 45 + bank + variant % 3 + (i >= 4 ? (bank & 1 ? 3 : -2) : 0);
            int len = pad ? 1050 : bank == 2 ? 95 : (i & 1 ? 102 : 168);
            for (int k = 0; k < 4; k++)
                syn_note_on(1, note + interval[(bank + (i >= 6)) & 3][k], .78f - (float)k * .036f,
                            len * SYN_CTL, 0);
        }
    } else {
        for (int i = 0; i < 11; i++) if (b == metro[bank][i]) {
            int note = (kind == 0 ? 34 + (variant >= 4 ? 12 : 0) : 36 + variant * 3) +
                       riffs[bank][(i + variant * 2) % 11] + ((bank + variant) % 3);
            int gate = kind == 2 ? 255 : i == 10 ? 250 : 83 + (i % 3 == 0 ? 75 : 19);
            int flags = kind == 0 && v[SP_LEGATO] == 2 && i % 4 == 2 ? SN_SLIDE : 0;
            if (kind == 0 && v[SP_LEGATO] == 2 && i % 4 == 1) gate = metro[bank][i + 1] - b + 32;
            if (kind == 0 && v[SP_LEGATO] == 2 && i % 4 == 0) flags |= SN_ACCENT;
            syn_note_on_f(1, note, i % 3 == 0 ? .86f : i % 2 == 0 ? .70f : .62f,
                          gate * SYN_CTL, 1, flags);
        }
    }
}
static int render(int p, int stress, FILE *f, float *peak)
{
    int8_t v[SP_NUM];
    int mono = syn_preset(p, v);
    int bank = (p - OLD) / SYN_BANK_SIZE, kind = (p - OLD) % SYN_BANK_SIZE / 7, variant = (p - OLD) % 7;
    int fail = 0;
    *peak = 0.f;
    syn_kill(); syn_sound(1, v); syn_level(1, 100.f/127.f); syn_mixer(1,64,0,0);
    for (int b = 0; b < BLOCKS; b++) {
        float l[SYN_CTL], r[SYN_CTL], rev[SYN_CTL], dly[SYN_CTL];
        phrase(b,bank,kind,variant,stress,v);
        syn_prepare(); syn_half(0); syn_half(1); syn_mix(l,r,rev,dly);
        for (int j = 0; j < SYN_CTL; j++) for (int ch = 0; ch < 2; ch++) {
            float y = ch ? r[j] : l[j];
            float a = fabsf(y);
            if (!isfinite(y) || a >= .75f || rev[j] != 0 || dly[j] != 0) fail = 1;
            if (a > *peak) *peak = a;
            float fade = b < BLOCKS - 32 ? 1.f : (float)((BLOCKS - b) * SYN_CTL - j) / (32 * SYN_CTL);
            pcm(f, y * fade);
        }
    }
    if ((!stress && *peak < .018f) || mono != (kind != 1)) fail = 1;
    return fail;
}
static float db(float x) { return 20.f * log10f(x > 0 ? x : 1e-9f); }

int main(int argc, char **argv)
{
    static const char *name[SYN_NEW_BANKS] = {
        "GhettoMachine-After-Hours.wav", "GhettoMachine-Machine-Funk.wav",
        "GhettoMachine-Warehouse.wav", "GhettoMachine-Dub-Broken.wav",
    };
    if (argc != 2 || syn_presets() != OLD + SYN_NEW_BANKS * SYN_BANK_SIZE) return 2;
    int fail = 0;
    syn_init();
    printf("id\tname\tarchived_recipe\treel_second\tpeak_dbfs\tstress_peak_dbfs\n");
    for (int bank = 0; bank < SYN_NEW_BANKS; bank++) {
        int archived_recipes = 0, written = 0;
        FILE *f = wav(argv[1], name[bank], FRAMES * SELECTIONS);
        if (!f) return 2;
        for (int i = 0; i < SYN_BANK_SIZE; i++) {
            int p = OLD + bank * SYN_BANK_SIZE + i, recipe = syn_preset_color(p), slot = i % 7;
            float peak, stress_peak;
            int selected = (slot == 0 || slot == 2 || slot == 4);
            int8_t v[SP_NUM];
            syn_preset(p,v);
            int bad = (strlen(syn_preset_name(p)) > 15 || recipe < SYN_PRESET_COLOR_CLEAN || recipe > SYN_PRESET_COLOR_DUST);
            for (int k = 0; k < SP_NUM; k++) {
                const syn_param_t *spec = syn_param(k);
                if (v[k] < spec->min || v[k] > spec->max) bad = 1;
            }
            for (int k = 0; k < p; k++) {
                int8_t other[SP_NUM]; syn_preset(k,other);
                if (!strcmp(syn_preset_name(p),syn_preset_name(k)) || !memcmp(v,other,sizeof v)) bad = 1;
            }
            bad |= render(p,0,selected ? f : NULL,&peak);
            bad |= render(p,1,NULL,&stress_peak);
            if (bad) fprintf(stderr,"FAIL signal, patch or headroom: %s\n",syn_preset_name(p));
            fail |= bad;
            printf("%d\t%s\t%d\t%.3f\t%.2f\t%.2f\n",p+USER_SLOTS,syn_preset_name(p),
                   recipe,selected ? written * (float)FRAMES/RATE : -1.f,db(peak),db(stress_peak));
            written += selected;
            archived_recipes += recipe != SYN_PRESET_COLOR_CLEAN;
        }
        if (archived_recipes < 5 || archived_recipes > 12 || written != SELECTIONS) fail = 1;
        if (fclose(f)) return 2;
    }
    return fail;
}
