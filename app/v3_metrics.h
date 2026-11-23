/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_V3_METRICS_H
#define FM1_V3_METRICS_H

/* Included once from the app, after the SDK declarations. The audio interrupt
 * owns the live counters. The UI masks it only long enough to copy a snapshot. */
static void text_cat(char *dst, const char *src);
static void int_text(int32_t value, char *buf);

enum { V3_HIST_BINS = 96, V3_HIST_SHIFT = 11 };
typedef struct {
    uint64_t voice_sum, bus_sum, total_sum;
    uint32_t blocks, mode, voice_max, bus_max, total_max, voices_max;
    uint32_t over, xruns_start;
    uint32_t total_hist[V3_HIST_BINS];
} v3_trial_metrics;

static v3_trial_metrics v3_metrics;

/* Audio owner only. No lock or announcement on this path. */
static void trial_metrics_reset(void)
{
    v3_metrics = (v3_trial_metrics){0};
    v3_metrics.xruns_start = plat_xruns();
}

static void trial_metrics_block(unsigned voice_ticks, unsigned bus_ticks,
                                unsigned total_ticks, unsigned voices,
                                unsigned active_mode)
{
    /* Floor the exact period in ticks. The first integer above it is late. */
    const uint32_t deadline = (uint32_t)(1000000ULL * APP_BLOCK * PLAT_TICKS_PER_US / 44100ULL);
    unsigned bin = total_ticks >> V3_HIST_SHIFT;
    if (v3_metrics.blocks == UINT32_MAX) return; /* >288 days at 44.1 kHz/256 */
    v3_metrics.mode = active_mode;
    ++v3_metrics.blocks;
    v3_metrics.voice_sum += voice_ticks;
    v3_metrics.bus_sum += bus_ticks;
    v3_metrics.total_sum += total_ticks;
    if (voice_ticks > v3_metrics.voice_max) v3_metrics.voice_max = voice_ticks;
    if (bus_ticks > v3_metrics.bus_max) v3_metrics.bus_max = bus_ticks;
    if (total_ticks > v3_metrics.total_max) v3_metrics.total_max = total_ticks;
    if (voices > v3_metrics.voices_max) v3_metrics.voices_max = voices;
    v3_metrics.over += total_ticks > deadline;
    if (bin >= V3_HIST_BINS) bin = V3_HIST_BINS - 1;
    ++v3_metrics.total_hist[bin];
}

/* Rounded integer mean over the exact 64-bit sum. At most 32 one-bit
 * divisions on a UI query, avoiding a 64-bit division runtime on pi32v2. */
static uint32_t v3_mean(uint64_t sum, uint32_t n)
{
    uint32_t q = (uint32_t)sum, r = (uint32_t)(sum >> 32);
    if (!n) return 0;
    if (r == 0) {
        r = q % n;
        q /= n;
    } else {
        for (int i = 0; i < 32; ++i) {
            uint32_t carry = r >> 31;
            r = (r << 1) | (q >> 31);
            q <<= 1;
            if (carry || r >= n) {
                r -= n;
                ++q;
            }
        }
    }
    return q + (r >= n - n / 2);
}

/* Nearest-rank p99 is at or below this value. The last bin is unbounded,
 * so its honest upper bound is the exact maximum. */
static uint32_t v3_p99_upper(const v3_trial_metrics *s)
{
    uint32_t rank = s->blocks - s->blocks / 100;
    for (unsigned i = 0; i + 1 < V3_HIST_BINS; ++i) {
        if (rank <= s->total_hist[i]) {
            uint32_t upper = ((i + 1u) << V3_HIST_SHIFT) - 1u;
            return upper < s->total_max ? upper : s->total_max;
        }
        rank -= s->total_hist[i];
    }
    return s->total_max;
}

static void v3_field(char *line, const char *label, uint32_t value)
{
    char digits[12];
    text_cat(line, label);
    if (value <= INT32_MAX) {
        int_text((int32_t)value, digits);
        text_cat(line, digits);
    } else {
        int_text((int32_t)(value / 10u), digits);
        text_cat(line, digits);
        digits[0] = (char)('0' + value % 10u);
        digits[1] = 0;
        text_cat(line, digits);
    }
}

/* One SAY within the SDK's ANN_MAX (even every field at UINT32_MAX).
 * All timing fields are raw timer ticks including both ends of each pair. */
static void trial_metrics_speak(unsigned mode)
{
    char line[216] = "V3 MODE";
    uint32_t irq = plat_lock();
    v3_trial_metrics s = v3_metrics;
    uint32_t xruns = plat_xruns() - s.xruns_start;
    plat_unlock(irq);
    v3_field(line, " ", s.blocks ? s.mode : mode);
    v3_field(line, " N ", s.blocks);
    v3_field(line, " V ", v3_mean(s.voice_sum, s.blocks));
    v3_field(line, " ", s.voice_max);
    v3_field(line, " B ", v3_mean(s.bus_sum, s.blocks));
    v3_field(line, " ", s.bus_max);
    v3_field(line, " T ", v3_mean(s.total_sum, s.blocks));
    v3_field(line, " ", s.total_max);
    v3_field(line, " P99LE ", v3_p99_upper(&s));
    v3_field(line, " O ", s.over);
    v3_field(line, " X ", xruns);
    v3_field(line, " S ", s.voices_max);
    ann_say(line, 0, ANN_NOW);
}
#endif
