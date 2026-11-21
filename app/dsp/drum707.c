/* SPDX-License-Identifier: GPL-3.0-only */
/* Voice synthesis adapted from JimmyKaps' TR-707C (260d07d, GPL-3.0-only).
 * The oscillator curve came from Dinghy; see drum707.md for all credits.
 * This file does not contain Roland sample data, platform/UI/storage code, or ROM. */
#include "drum707.h"

typedef struct {
    uint32_t inc0, sweep;
    uint16_t ratio;
    uint8_t pdec, metal, noise, hp;
} drum707_spec_t;

#include "drum707_tables.h"

static const x0x_param_t PARAM[D7P_NUM] = {
    {"LEVEL",127,100,0}, {"TUNE",127,64,0}, {"DECY",127,64,0},
    {"PAN",127,64,0}, {"REV",127,0,0}, {"DLY",127,0,0},
};
static const x0x_param_t LEVEL_CHANGED[D7_NUM] = {
    [D7_CH]={"LEVEL",127,85,0}, [D7_CR]={"LEVEL",127,70,0}, [D7_RD]={"LEVEL",127,60,0},
    [D7_CB]={"LEVEL",127,80,0}, [D7_TB]={"LEVEL",127,75,0},
};
static const char *const CHOKE[] = {"OFF","CH>OH"};
static const x0x_param_t KIT_PARAM[D7K_NUM] = {{"CHOKE",1,1,CHOKE}};

int drum707_nparams(int v)
{
    return v >= 0 && v < D7_NUM ? D7P_NUM : v == D7_KIT ? D7K_NUM : 0;
}

const x0x_param_t *drum707_param(int v, int p)
{
    if (p < 0 || p >= drum707_nparams(v)) return 0;
    if (v == D7_KIT) return &KIT_PARAM[p];
    return p == D7P_LEVEL && LEVEL_CHANGED[v].name ? &LEVEL_CHANGED[v] : &PARAM[p];
}

void drum707_set(drum707_t *d, int v, int p, int value)
{
    const x0x_param_t *desc=drum707_param(v,p);
    if (!desc) return;
    if (value < 0) value = 0;
    if (value > desc->max) value = desc->max;
    if (v == D7_KIT) d->choke = (uint8_t)value;
    else {
        d->voice[v].pot[p] = (uint8_t)value;
        if (p == D7P_PAN) x0x_pan_gains(value,&d->voice[v].pan_l,&d->voice[v].pan_r);
    }
}

int drum707_get(const drum707_t *d, int v, int p)
{
    if (!drum707_param(v,p)) return 0;
    return v == D7_KIT ? d->choke : d->voice[v].pot[p];
}

void drum707_init(drum707_t *d)
{
    for(int v=0; v<D7_NUM; ++v) {
        d->voice[v] = (drum707_voice_t){0};
        d->voice[v].rng=0x2545f491u ^ ((uint32_t)v * 0x9e3779b9u);
        for (int p=0; p<D7P_NUM; ++p) drum707_set(d,v,p,drum707_param(v,p)->def);
    }
    d->choke=KIT_PARAM[D7K_CHOKE].def;
}

static inline int32_t sine707(uint32_t phase)
{
    int32_t x = (int32_t)phase >> 16, ax=x<0 ? -x : x, y, ay;
    y=(x*(32768-ax))>>13;
    ay=y<0 ? -y : y;
    y+=((((y*ay)>>15)-y)*7373)>>15;
    return y>32767 ? 32767 : y< -32767 ? -32767 : y;
}

/* Pi32v2 erratum: keep the wide multiply away from surrounding loads.
 * The fm1-base device audit checks printed instructions, not just source. */
static __attribute__((noinline)) uint32_t mulshift(uint32_t x, uint32_t y, int shift)
{
    return (uint32_t)(((uint64_t)x*y)>>shift);
}

static uint32_t decay_factor(int v, uint32_t p)
{
    if (p == 127u) return DECAY[v][16];
    const uint32_t q=p>>3, x=DECAY[v][q], delta=DECAY[v][q+1u]-x, frac=p&7u;
    return x+(q==15u ? (delta*frac+3u)/7u : (delta*frac+4u)>>3);
}

void drum707_trigger(drum707_t *d, int v, float vel)
{
    if (v < 0 || v >= D7_NUM || !(vel>0.f)) return;
    if (v==D7_CH && d->choke) d->voice[D7_OH].active=0;
    drum707_voice_t *s=&d->voice[v];
    const drum707_spec_t *a=&SPEC[v];
    const uint32_t tune=TUNE[s->pot[D7P_TUNE]];
    s->inc1=mulshift(a->inc0,tune,16);
    s->inc2=mulshift(s->inc1,a->ratio,8);
    s->pitch_sweep=mulshift(a->sweep,tune,16);
    s->increment=s->inc1+s->pitch_sweep;
    uint32_t hp=a->hp;
    if (!a->inc0) {
        hp=hp*tune>>16;
        hp=hp<4u?4u:hp>250u?250u:hp;
    }
    s->noise_hp=hp;
    s->decay=decay_factor(v,s->pot[D7P_DECY]);
    s->vel=vel>1.f?1.f:vel;
    s->env=1<<24;
    s->pe=32767;
    s->lp=0;
    s->ph1=s->ph2=s->at=0;
    s->active=1;
}

void drum707_cut(drum707_t *d, int v)
{
    if (v>=0 && v<D7_NUM) d->voice[v].active=0;
}

int drum707_active(const drum707_t *d)
{
    for(int v=0;v<D7_NUM;++v)
        if(d->voice[v].active) return 1;
    return 0;
}

void drum707_render_st(drum707_t *d,float *l,float *r,float *rev,float *dly,int n)
{
    for (int v=0; v<D7_NUM; ++v) {
        drum707_voice_t *s=&d->voice[v];
        if (!s->active) continue;
        const drum707_spec_t *a=&SPEC[v];
        int32_t env=s->env, lp=s->lp, pe=s->pe;
        uint32_t at=s->at, rng=s->rng, ph1=s->ph1, ph2=s->ph2, inc=s->increment;
        const float gain=(0.54f/(127.f*32768.f))*s->vel*s->pot[D7P_LEVEL];
        const float lg=gain*s->pan_l, rg=gain*s->pan_r;
        const float rv=gain*((1.f/127.f)*s->pot[D7P_REV]), dl=gain*((1.f/127.f)*s->pot[D7P_DLY]);
        for(int i=0;i<n;++i) {
            if (!(at&3u)) {
                if (v==D7_CP && (at==440u || at==880u)) env=1<<24;   /* triple transient */
                env=(int32_t)mulshift((uint32_t)env,s->decay,24);
                if (a->sweep) {
                    pe-=pe>>a->pdec;
                    inc=s->inc1+mulshift(s->pitch_sweep,(uint32_t)pe,15);
                }
                if (env<16384) { s->active=0; break; }
            }
            ++at;
            int32_t tone=0, nz=0;
            if (a->inc0) {
                ph1+=inc;
                if (a->metal) {
                    ph2+=s->inc2;
                    tone=((ph1>>31)?9000:-9000)+((ph2>>31)?9000:-9000);
                } else tone=sine707(ph1)*3>>2;
            }
            if (a->noise) {
                rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
                nz=(int32_t)(rng>>16)-32768;
                lp+=((nz-lp)*(int32_t)s->noise_hp)>>8;
                nz-=lp;
            }
            const int32_t mix=(tone*(255-(int32_t)a->noise)+nz*(int32_t)a->noise)>>8;
            const float x=(float)((mix*(env>>9))>>15);
            l[i]+=lg*x; r[i]+=rg*x;
            rev[i]+=rv*x; dly[i]+=dl*x;
        }
        s->env=env; s->lp=lp; s->pe=pe; s->at=at; s->rng=rng;
        s->ph1=ph1; s->ph2=ph2; s->increment=inc;
    }
}
