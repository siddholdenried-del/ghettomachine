#include "color.h"
#include <stddef.h>
_Static_assert(sizeof(color_state_t) <= 576, "color DSP memory budget");

void color_reset(color_state_t *s)
{
    unsigned i;
    for (i = 0; i < offsetof(color_state_t, ring); i++) ((uint8_t *)s)[i] = 0;
    s->rand = 0xB429AC58u;
    s->dropout = 1.f;
}
void color_defaults(color_params_t *p)
{
    const color_params_t d = {{110, 26, 33, 23, 24, 0}, {0,0}};
    *p = d;
}

static float absf(float x) { return x < 0.f ? -x : x; }
static float tri(float p) { return p < .5f ? p * 4.f - 1.f : 3.f - p * 4.f; }
static float unit(float p) { return p >= 1.f ? p-1.f : p; }
static float limit(float f) { return f > 1.f ? 1.f : f < 0.f ? 0.f : f; }
static uint32_t step_rand(color_state_t *s)
{
    uint32_t r = s->rand;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    return s->rand = r;
}
static float random_sample(uint32_t v) { return (int32_t)(v & 65535u)*(1.f/32768.f)-1.f; }
static int16_t fixed(float x)
{
    return x >= 7.99f ? 32727 : x <= -7.99f ? -32727 : (int16_t)(x*4096.f);
}

static float shift(color_state_t *s, int16_t *ring, float x, float d)
{
    unsigned i = (unsigned)d, a = (s->pos-i)&(COL_RING-1), b = (a-1)&(COL_RING-1);
    float f = d-(float)i;
    float y = x;
    if (i < s->valid) y = (ring[a]*(1.f-f)+ring[b]*f)*(1.f/4096.f);
    ring[s->pos] = fixed(x);
    return y;
}

int color_active(const color_params_t *p)
{
    return p->v[COL_ON] && p->v[COL_AMOUNT] &&
           (p->v[COL_WEAR] | p->v[COL_WOW] | p->v[COL_DIGITAL] | p->v[COL_DROP]);
}
static float digital_sample(float x, float dg, float dgi)
{
    float v = limit(absf(x)*(1.f/8.f))*8.f;
    return (x < 0.f ? -1.f : 1.f)*(float)(int)(v*dg+.5f)*dgi;
}
static void run(color_state_t *s, color_sends_t *send, const color_params_t *p,
                float *l, float *r, float *rev, float *dly, unsigned n)
{
    unsigned i;
    float amount, wear, wow, digital, drop, dg, dgi;
    int hold;
    if (!color_active(p)) return;
    amount = p->v[COL_AMOUNT]*(1.f/127.f);
    wear = p->v[COL_WEAR]*(1.f/127.f);
    wow = p->v[COL_WOW]*(1.f/127.f);
    digital = p->v[COL_DIGITAL]*(1.f/127.f);
    drop = p->v[COL_DROP]*(1.f/127.f);
    dg = (float)(1u << (15-(int)(11.f*digital)));
    dgi = 1.f/dg;
    hold = 1+(int)(digital*digital*24.f);
    for (i = 0; i < n; i++) {
        float x = l[i], y = r[i], a = x, b = y;
        float xrv = send ? rev[i] : 0.f, xdl = send ? dly[i] : 0.f, ar = xrv, ad = xdl;
        float v = .5f*(absf(x)+absf(y)), gate, dt = 1.f;
        uint32_t rnd = (wear || drop) ? step_rand(s) : 0;
        s->envelope += (v > s->envelope ? .02f : .001f)*(v-s->envelope);
        gate = limit((s->envelope-.001f)*40.f);
        if (wow) {
            float wa = tri(s->slow), fl = tri(s->fast);
            float da = 63.f+wow*(45.f*wa+8.f*fl);
            float db = 63.f+wow*(41.f*wa+9.f*tri(unit(s->fast+.13f)));
            a = shift(s,s->ring[0],x,da); b = shift(s,s->ring[1],y,db);
            if (send) {
                float d = .5f*(da+db);
                ar = shift(s,send->ring[0],xrv,d);
                ad = shift(s,send->ring[1],xdl,d);
            }
            s->slow = unit(s->slow+(.57f/44100.f));
            s->fast = unit(s->fast+(7.7f/44100.f));
            s->pos = (uint16_t)((s->pos+1)&(COL_RING-1));
            if (s->valid < COL_RING-1) s->valid++;
        }
        if (digital) {
            if (!s->clock) {
                s->hold_l = digital_sample(a,dg,dgi); s->hold_r = digital_sample(b,dg,dgi);
                if (send) { send->hold[0] = digital_sample(ar,dg,dgi); send->hold[1] = digital_sample(ad,dg,dgi); }
                s->clock = (uint16_t)hold;
            }
            s->clock--;
            a += digital*(s->hold_l-a); b += digital*(s->hold_r-b);
            if (send) { ar += digital*(send->hold[0]-ar); ad += digital*(send->hold[1]-ad); }
        }
        if (drop) {
            if (!s->countdown) {
                if (gate > .05f && (rnd & 255u) < (unsigned)(3.f*drop+1.f))
                    s->remain = (uint16_t)(384u + ((rnd >> 8)&1023u));
                s->countdown = 128;
            }
            s->countdown--;
            if (s->remain) { s->remain--; dt = 1.f-.88f*drop; }
            s->dropout += (dt < s->dropout ? .015f : .003f)*(dt-s->dropout);
            a *= s->dropout; b *= s->dropout;
            if (send) { ar *= s->dropout; ad *= s->dropout; }
        }
        if (wear) {
            const float nl = random_sample(rnd), nr = random_sample(rnd >> 16);
            float noise_l, noise_r;
            s->hiss_lp[0] += .24f*(nl-s->hiss_lp[0]);
            s->hiss_lp[1] += .24f*(nr-s->hiss_lp[1]);
            if ((rnd & 4095u) < (unsigned)(5.f*wear) && gate > .05f)
                s->crack += random_sample(rnd >> 16)*.10f*wear;
            s->crack *= .87f;
            noise_l = gate*(.035f*wear*(nl-s->hiss_lp[0]) + s->crack);
            noise_r = gate*(.035f*wear*(nr-s->hiss_lp[1]) + .84f*s->crack);
            a += noise_l; b += noise_r;
            if (send) { /* Never create a send on a lane whose gain is zero. */
                ar += .5f*(noise_l+noise_r)*limit(absf(xrv)*12.f);
                ad += .5f*(noise_l+noise_r)*limit(absf(xdl)*12.f);
            }
        }
        l[i] = x+amount*(a-x); r[i] = y+amount*(b-y);
        if (send) { rev[i] = xrv+amount*(ar-xrv); dly[i] = xdl+amount*(ad-xdl); }
    }
}
void color_run(color_state_t *s, const color_params_t *p, float *l, float *r, unsigned n)
{
    run(s,0,p,l,r,0,0,n);
}
void color_run_drums(color_state_t *s, color_sends_t *sends, const color_params_t *p,
                     float *l, float *r, float *rev, float *dly, unsigned n)
{
    run(s,sends,p,l,r,rev,dly,n);
}
