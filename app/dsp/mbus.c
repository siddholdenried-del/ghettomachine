/* SPDX-License-Identifier: GPL-3.0-only */
/* GhettoMachine's master bus stages (see mbus.h). */
#define FM_ALWAYS_INLINE                  /* (code in RAM here: fastmath.h, gm_ram.h) */
#include "mbus.h"
#include "fastmath.h"
#include "gm_ram.h"

#define FS 44100.0f

static const char *const CUT_N[] = {"OFF", "20Hz", "25Hz", "30Hz", "40Hz", "50Hz", "60Hz", "80Hz", "100Hz", "120Hz",
                                    "150Hz"};
static const float CUT_HZ[] = {0.0f, 20.0f, 25.0f, 30.0f, 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f};
static const char *const LF_N[] = {"40Hz", "60Hz", "80Hz", "100Hz", "120Hz", "150Hz", "200Hz", "300Hz"};
static const float LF_HZ[] = {40.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f, 200.0f, 300.0f};
static const char *const Q_N[] = {"0.4", "0.7", "1.0", "1.4", "2.0", "3.0"};
static const float Q_V[] = {0.4f, 0.7f, 1.0f, 1.4f, 2.0f, 3.0f};
static const char *const HF_N[] = {"2k", "3k", "4k", "5k", "6k", "8k", "10k", "12k", "16k"};
static const float HF_HZ[] = {2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f};
static const char *const BASS_N[] = {"OFF", "60Hz", "80Hz", "100Hz", "120Hz", "150Hz", "200Hz"};
static const float BASS_HZ[] = {0.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f, 200.0f};

static const x0x_param_t MB_P[MB_NPARAMS] = {
    {"LOW", 48, 24, 0},                   /* -12 .. +12 dB, 0.5 dB a step */
    {"MID", 48, 24, 0},
    {"HIGH", 48, 24, 0},
    {"LOW CUT", 10, 0, CUT_N},
    {"LOW FREQ", 7, 2, LF_N},             /* 80 Hz */
    {"MID FREQ", 127, 61, 0},             /* 150 Hz .. 8 kHz: 61 = 1 kHz */
    {"MID Q", 5, 1, Q_N},                 /* 0.7 */
    {"HIGH FREQ", 8, 5, HF_N},            /* 8 kHz */
    {"DRIVE", 127, 0, 0},                 /* 0 = off .. +12 dB into the curve */
    {"WARMTH", 127, 0, 0},
    {"TONE", 127, 127, 0},                /* 127 = open; under it a roll-off from 20 kHz down to 3 kHz */
    {"MIX", 127, 127, 0},
    {"WIDTH", 40, 20, 0},                 /* 5 % a step: 20 = 100 % */
    {"BASS MONO", 6, 0, BASS_N},
    {"BALANCE", 127, 64, 0},
};

int mbus_nparams(void) { return MB_NPARAMS; }
const x0x_param_t *mbus_param(int i) { return (i >= 0 && i < MB_NPARAMS) ? &MB_P[i] : 0; }
int mbus_get(const mbus_t *m, int i) { return (i >= 0 && i < MB_NPARAMS) ? m->pot[i] : 0; }

static float gain_db(int p) { return (float)(p - 24) * 0.5f; }
static float mid_hz(int p) { return 150.0f * fm_exp2f((float)p * (5.737f / 127.0f)); }
static float drive_db(int p) { return (float)p * (12.0f / 127.0f); }
static float tone_hz(int p) { return 3000.0f * fm_exp2f((float)p * (2.737f / 127.0f)); }

/* ---- biquads (RBJ's cookbook), transposed direct form II */
static void bq_put(mb_bq_t *q, float b0, float b1, float b2, float a0, float a1, float a2)
{
    float ia = 1.0f / a0;
    q->b0 = b0 * ia;
    q->b1 = b1 * ia;
    q->b2 = b2 * ia;
    q->a1 = a1 * ia;
    q->a2 = a2 * ia;
}

static void bq_clear(mb_bq_t *q) { q->z1[0] = q->z1[1] = q->z2[0] = q->z2[1] = 0.0f; }

static void bq_highpass(mb_bq_t *q, float hz, float qv)
{
    float w = FM_TWO_PI * hz / FS, cs = fm_cosf(w), al = fm_sinf(w) / (2.0f * qv);
    bq_put(q, 0.5f * (1.0f + cs), -(1.0f + cs), 0.5f * (1.0f + cs), 1.0f + al, -2.0f * cs, 1.0f - al);
}

static void bq_shelf(mb_bq_t *q, float hz, float db, int high)   /* slope S = 1 */
{
    float A = fm_db2lin(0.5f * db), w = FM_TWO_PI * hz / FS, cs = fm_cosf(w), sn = fm_sinf(w);
    float sa = 2.0f * fm_sqrtf(A) * sn * 0.70710678f;            /* 2 sqrt(A) alpha, alpha = sin w / sqrt 2 */
    float ap = A + 1.0f, am = A - 1.0f;
    if (!high)
        bq_put(q, A * (ap - am * cs + sa), 2.0f * A * (am - ap * cs), A * (ap - am * cs - sa),
               ap + am * cs + sa, -2.0f * (am + ap * cs), ap + am * cs - sa);
    else
        bq_put(q, A * (ap + am * cs + sa), -2.0f * A * (am + ap * cs), A * (ap + am * cs - sa),
               ap - am * cs + sa, 2.0f * (am - ap * cs), ap - am * cs - sa);
}

static void bq_bell(mb_bq_t *q, float hz, float db, float qv)
{
    float A = fm_db2lin(0.5f * db), w = FM_TWO_PI * hz / FS, cs = fm_cosf(w), al = fm_sinf(w) / (2.0f * qv);
    bq_put(q, 1.0f + al * A, -2.0f * cs, 1.0f - al * A, 1.0f + al / A, -2.0f * cs, 1.0f - al / A);
}

/* one biquad over a block, side c of its state */
GM_RAM static void bq_run(mb_bq_t *q, float *x, int n, int c)
{
    float b0 = q->b0, b1 = q->b1, b2 = q->b2, a1 = q->a1, a2 = q->a2, z1 = q->z1[c], z2 = q->z2[c];
    int i;
    for (i = 0; i < n; i++) {
        float in = x[i], y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        x[i] = y;
    }
    q->z1[c] = fm_flush(z1);
    q->z2[c] = fm_flush(z2);
}

/* one biquad over a block, both sides together: two independent recursions in one loop, so an in-order FPU works on
 * one while the other waits for its result */
GM_RAM static void bq_run2(mb_bq_t *q, float *l, float *r, int n)
{
    float b0 = q->b0, b1 = q->b1, b2 = q->b2, a1 = q->a1, a2 = q->a2;
    float zl1 = q->z1[0], zl2 = q->z2[0], zr1 = q->z1[1], zr2 = q->z2[1];
    int i;
    for (i = 0; i < n; i++) {
        float xl = l[i], xr = r[i], yl = b0 * xl + zl1, yr = b0 * xr + zr1;
        zl1 = b1 * xl - a1 * yl + zl2;
        zr1 = b1 * xr - a1 * yr + zr2;
        zl2 = b2 * xl - a2 * yl;
        zr2 = b2 * xr - a2 * yr;
        l[i] = yl;
        r[i] = yr;
    }
    q->z1[0] = fm_flush(zl1);
    q->z2[0] = fm_flush(zl2);
    q->z1[1] = fm_flush(zr1);
    q->z2[1] = fm_flush(zr2);
}

/* |H| in dB where phi = sin^2(w / 2): RBJ's form, which keeps its accuracy at low frequencies (the cos w form cancels
 * to nothing there in floats: a shelf at 80 Hz drew flat) */
static float bq_db(const mb_bq_t *q, float phi)
{
    float b0 = q->b0, b1 = q->b1, b2 = q->b2, a1 = q->a1, a2 = q->a2, sb = b0 + b1 + b2, sa = 1.0f + a1 + a2;
    float num = sb * sb - 4.0f * (b0 * b1 + 4.0f * b0 * b2 + b1 * b2) * phi + 16.0f * b0 * b2 * phi * phi;
    float den = sa * sa - 4.0f * (a1 + 4.0f * a2 + a1 * a2) * phi + 16.0f * a2 * phi * phi;
    return 3.0103f * fm_log2f(fm_maxf(num, 1e-20f) / fm_maxf(den, 1e-20f));
}

/* ---- the stages' coefficients from the pots */
static void eq_coefs(mbus_t *m)
{
    uint8_t was = m->eq_on, on = 0;
    int b;
    if (m->pot[MB_EQ_CUT]) {
        bq_highpass(&m->eq[0], CUT_HZ[m->pot[MB_EQ_CUT]], 0.70710678f);
        on |= 1u;
    }
    if (m->pot[MB_EQ_LOW] != 24) {
        bq_shelf(&m->eq[1], LF_HZ[m->pot[MB_EQ_LFREQ]], gain_db(m->pot[MB_EQ_LOW]), 0);
        on |= 2u;
    }
    if (m->pot[MB_EQ_MID] != 24) {
        bq_bell(&m->eq[2], mid_hz(m->pot[MB_EQ_MFREQ]), gain_db(m->pot[MB_EQ_MID]), Q_V[m->pot[MB_EQ_MQ]]);
        on |= 4u;
    }
    if (m->pot[MB_EQ_HIGH] != 24) {
        bq_shelf(&m->eq[3], HF_HZ[m->pot[MB_EQ_HFREQ]], gain_db(m->pot[MB_EQ_HIGH]), 1);
        on |= 8u;
    }
    for (b = 0; b < 4; b++)
        if ((on >> b & 1u) && !(was >> b & 1u))   /* a band coming in starts from silence, not from old state */
            bq_clear(&m->eq[b]);
    m->eq_on = on;
}

static void tape_coefs(mbus_t *m)
{
    int c;
    uint8_t was = m->tape_on;
    m->drive = m->pot[MB_TP_DRIVE] ? fm_db2lin(drive_db(m->pot[MB_TP_DRIVE])) : 0.0f;
    m->bias = (float)m->pot[MB_TP_WARM] * (0.35f / 127.0f);
    m->tb = fm_tanhf(m->bias);
    m->norm = m->drive > 0.0f ? 1.0f / (m->drive * (1.0f - m->tb * m->tb)) : 1.0f;   /* the slope at zero: 1 */
    m->tone_a = m->pot[MB_TP_TONE] >= 127 ? 0.0f : 1.0f - fm_expf(-FM_TWO_PI * tone_hz(m->pot[MB_TP_TONE]) / FS);
    m->mix = (float)m->pot[MB_TP_MIX] * (1.0f / 127.0f);
    m->tape_on = (m->drive > 0.0f || m->tone_a > 0.0f) && m->pot[MB_TP_MIX] > 0;
    if (m->tape_on && !was)
        for (c = 0; c < 2; c++)
            m->lp[c] = m->dc_x[c] = m->dc_y[c] = 0.0f;
}

static void stereo_coefs(mbus_t *m)
{
    uint8_t was = m->bass_on;
    m->width = (float)m->pot[MB_ST_WIDTH] * 0.05f;
    x0x_pan_gains(m->pot[MB_ST_BAL], &m->bal_l, &m->bal_r);
    m->bass_on = m->pot[MB_ST_BASS] != 0;
    if (m->bass_on)
        bq_highpass(&m->side_hp, BASS_HZ[m->pot[MB_ST_BASS]], 0.70710678f);
    if (m->bass_on && !was)
        bq_clear(&m->side_hp);
    m->st_on = m->pot[MB_ST_WIDTH] != 20 || m->bass_on || m->pot[MB_ST_BAL] != 64;
}

/* K-weighting at 44.1 kHz from BS.1770's analog prototypes (the published coefficients are 48 kHz's) */
static void kweight_coefs(mbus_t *m)
{
    float K = fm_tanf(FM_PI * 1681.974450955533f / FS), Q = 0.7071752369554196f;
    float Vh = fm_db2lin(3.999843853973347f), Vb = fm_powf(Vh, 0.4996667741545416f);
    float a0 = 1.0f + K / Q + K * K;
    bq_put(&m->kw[0], Vh + Vb * K / Q + K * K, 2.0f * (K * K - Vh), Vh - Vb * K / Q + K * K, a0, 2.0f * (K * K - 1.0f),
           1.0f - K / Q + K * K);
    K = fm_tanf(FM_PI * 38.13547087602444f / FS);
    Q = 0.5003270373238773f;
    a0 = 1.0f + K / Q + K * K;
    bq_put(&m->kw[1], a0, -2.0f * a0, a0, a0, 2.0f * (K * K - 1.0f), 1.0f - K / Q + K * K);   /* b: 1, -2, 1 */
}

void mbus_init(mbus_t *m)
{
    int i;
    uint8_t *z = (uint8_t *)m;
    for (i = 0; i < (int)sizeof *m; i++)
        z[i] = 0;
    for (i = 0; i < MB_NPARAMS; i++)
        m->pot[i] = MB_P[i].def;
    eq_coefs(m);
    tape_coefs(m);
    stereo_coefs(m);
    kweight_coefs(m);
}

void mbus_set(mbus_t *m, int i, int v)
{
    if (i < 0 || i >= MB_NPARAMS)
        return;
    m->pot[i] = (uint8_t)(v < 0 ? 0 : v > MB_P[i].max ? MB_P[i].max : v);
    if (i <= MB_EQ_HFREQ)
        eq_coefs(m);
    else if (i <= MB_TP_MIX)
        tape_coefs(m);
    else
        stereo_coefs(m);
}

/* ---- the audio (GhettoMachine 0.5, CPU: from RAM, gm_ram.h: fx_master calls it; on the second core, whose stack is
 * 2.8 KB: the meter's and STEREO's scratch in pieces of MB_PIECE samples, the same sums) */
#define MB_PIECE 64
GM_RAM void mbus_eq(mbus_t *m, float *l, float *r, int n)
{
    int b;
    for (b = 0; b < 4; b++)
        if (m->eq_on >> b & 1u)
            bq_run2(&m->eq[b], l, r, n);
}

GM_RAM static void tape_run(mbus_t *m, float *x, int n, int c)
{
    float k = m->drive, b = m->bias, tb = m->tb, nrm = m->norm, ta = m->tone_a, mix = m->mix;
    float lp = m->lp[c], dx = m->dc_x[c], dy = m->dc_y[c];
    int i;
    for (i = 0; i < n; i++) {
        float in = x[i], y = in;
        if (k > 0.0f) {
            y = (fm_tanhf(k * y + b) - tb) * nrm;
            if (b > 0.0f) {                       /* WARMTH's offset leaves DC: a 10 Hz trap */
                float h = y - dx + 0.998576f * dy;
                dx = y;
                dy = h;
                y = h;
            }
        }
        if (ta > 0.0f) {
            lp += ta * (y - lp);
            y = lp;
        }
        x[i] = in + (y - in) * mix;
    }
    m->lp[c] = fm_flush(lp);
    m->dc_x[c] = fm_flush(dx);
    m->dc_y[c] = fm_flush(dy);
}

GM_RAM void mbus_tape_stereo(mbus_t *m, float *l, float *r, int n)
{
    int i;
    if (m->tape_on) {
        tape_run(m, l, n, 0);
        tape_run(m, r, n, 1);
    }
    if (m->st_on) {
        float w = 0.5f * m->width, gl = m->bal_l, gr = m->bal_r, side[MB_PIECE];
        int done = 0;
        while (done < n) {                        /* (in pieces: the side's own buffer) */
            int k = n - done < MB_PIECE ? n - done : MB_PIECE;
            float *L = l + done, *R = r + done;
            for (i = 0; i < k; i++)
                side[i] = (L[i] - R[i]) * w;
            if (m->bass_on)
                bq_run(&m->side_hp, side, k, 0);
            for (i = 0; i < k; i++) {
                float mid = 0.5f * (L[i] + R[i]);
                L[i] = (mid + side[i]) * gl;
                R[i] = (mid - side[i]) * gr;
            }
            done += k;
        }
    }
}

void mbus_meter_on(mbus_t *m, int on)
{
    int i;
    if (on && !m->meter_on) {                    /* (the interrupt leaves them alone while off) */
        bq_clear(&m->kw[0]);
        bq_clear(&m->kw[1]);
        m->lu_acc = 0.0f;
        m->lu_n = 0;
        m->lu_pos = m->lu_have = 0;
        for (i = 0; i < MB_LU_BLOCKS; i++)
            m->lu_blk[i] = 0.0f;
        m->c_lr = m->c_ll = m->c_rr = 0.0f;
    }
    m->meter_on = (uint8_t)(on != 0);
}

GM_RAM void mbus_meter(mbus_t *m, const float *l, const float *r, int n)
{
    float kl[MB_PIECE], kr[MB_PIECE], acc = m->lu_acc, lr = m->c_lr, ll = m->c_ll, rr = m->c_rr;
    const float a = 7.558e-5f;                   /* the correlation's averages: 300 ms */
    int i, k, done = 0, cnt = m->lu_n;
    if (!m->meter_on)
        return;
    while (done < n) {
        k = n - done < MB_PIECE ? n - done : MB_PIECE;
        for (i = 0; i < k; i++) {
            kl[i] = l[done + i];
            kr[i] = r[done + i];
            lr += a * (kl[i] * kr[i] - lr);
            ll += a * (kl[i] * kl[i] - ll);
            rr += a * (kr[i] * kr[i] - rr);
        }
        bq_run2(&m->kw[0], kl, kr, k);
        bq_run2(&m->kw[1], kl, kr, k);
        for (i = 0; i < k; i++) {
            acc += kl[i] * kl[i] + kr[i] * kr[i];
            if (++cnt == 4410) {                 /* a 100 ms block */
                m->lu_blk[m->lu_pos] = acc * (1.0f / 4410.0f);
                if (++m->lu_pos == MB_LU_BLOCKS)   /* (no divide: RAM code calls no helper) */
                    m->lu_pos = 0;
                if (m->lu_have < MB_LU_BLOCKS)
                    m->lu_have++;
                acc = 0.0f;
                cnt = 0;
            }
        }
        done += k;
    }
    m->lu_acc = acc;
    m->lu_n = cnt;
    m->c_lr = fm_flush(lr);
    m->c_ll = fm_flush(ll);
    m->c_rr = fm_flush(rr);
}

/* ---- the screen's */
float mbus_eq_db(const mbus_t *m, float hz)
{
    float sn = fm_sinf(FM_PI * fm_minf(hz, 21000.0f) / FS), phi = sn * sn, db = 0.0f;
    int b;
    for (b = 0; b < 4; b++)
        if (m->eq_on >> b & 1u)
            db += bq_db(&m->eq[b], phi);
    return db;
}

float mbus_lufs(const mbus_t *m, int momentary)
{
    int want = momentary ? 4 : MB_LU_BLOCKS, have = m->lu_have, n = have < want ? have : want, i, p = m->lu_pos;
    float s = 0.0f;
    if (!m->meter_on || n == 0)
        return -70.0f;
    for (i = 0; i < n; i++) {
        p = p ? p - 1 : MB_LU_BLOCKS - 1;
        s += m->lu_blk[p];
    }
    s /= (float)n;
    if (s < 1e-7f)                               /* under -70 */
        return -70.0f;
    return -0.691f + 3.0103f * fm_log2f(s);
}

float mbus_corr(const mbus_t *m)
{
    float d = m->c_ll * m->c_rr;
    if (d < 1e-12f)
        return 1.0f;                             /* silence: nothing out of phase */
    return fm_clampf(m->c_lr / fm_sqrtf(d), -1.0f, 1.0f);
}

int mbus_active(const mbus_t *m, int stage)
{
    return stage == 0 ? m->eq_on != 0 : stage == 1 ? m->tape_on : m->st_on;
}

/* ---- units */
static char *put_num(char *b, float v, int decimals, int plus, const char *unit)
{
    int neg = v < 0.0f, n = 0, i = 0;
    char t[16];
    uint32_t scale = decimals ? 10u : 1u, u = (uint32_t)((neg ? -v : v) * (float)scale + 0.5f);
    if (u && (neg || plus))                      /* the sign: only on what shows as not zero */
        b[i++] = neg ? '-' : '+';
    if (decimals) {
        t[n++] = (char)('0' + u % 10u);
        t[n++] = '.';
        u /= 10u;
    }
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    while (n)
        b[i++] = t[--n];
    while (*unit)
        b[i++] = *unit++;
    b[i] = 0;
    return b + i;
}

static void put_hz(char *b, float hz)
{
    if (hz >= 1000.0f)
        put_num(b, hz * 0.001f, 1, 0, "k");
    else
        put_num(b, hz, 0, 0, "Hz");
}

void mbus_format(const mbus_t *m, int i, char *b)
{
    int p = m->pot[i];
    b[0] = 0;
    switch (i) {
    case MB_EQ_LOW: case MB_EQ_MID: case MB_EQ_HIGH:
        if (p == 24) {
            b[0] = '0'; b[1] = 'd'; b[2] = 'B'; b[3] = 0;
        } else
            put_num(b, gain_db(p), 1, 1, "dB");
        return;
    case MB_EQ_MFREQ: put_hz(b, mid_hz(p)); return;
    case MB_TP_DRIVE:
        if (!p) {
            b[0] = 'O'; b[1] = 'F'; b[2] = 'F'; b[3] = 0;
        } else
            put_num(b, drive_db(p), 1, 1, "dB");
        return;
    case MB_TP_WARM: case MB_TP_MIX: put_num(b, (float)p * (100.0f / 127.0f), 0, 0, "%"); return;
    case MB_TP_TONE:
        if (p >= 127) {
            b[0] = 'O'; b[1] = 'P'; b[2] = 'E'; b[3] = 'N'; b[4] = 0;
        } else
            put_hz(b, tone_hz(p));
        return;
    case MB_ST_WIDTH: put_num(b, (float)(p * 5), 0, 0, "%"); return;
    case MB_ST_BAL:
        if (p == 64) {
            b[0] = 'C'; b[1] = 0;
        } else {
            b[0] = p < 64 ? 'L' : 'R';
            put_num(b + 1, (float)(p < 64 ? 64 - p : p - 64), 0, 0, "");
        }
        return;
    default: {
        const char *s = MB_P[i].names ? MB_P[i].names[p] : "";
        while ((*b = *s++) != 0)
            b++;
        return;
    }
    }
}
