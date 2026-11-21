/* cc -O2 -std=c99 app/tests/color_unit.c app/dsp/color.c -lm && ./a.out */
#include "../dsp/color.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define N 1024
static float l[N],r[N],dry[N];
static color_state_t state;
static color_sends_t sends;
static float rev[N], dly[N];

int main(void)
{
    color_params_t p;
    int i,j,mode;
    color_defaults(&p);
    color_reset(&state);
    for(i=0;i<N;i++)dry[i]=.28f*sinf(i*.102f)+.22f*cosf(i*.451f);
    memcpy(l,dry,sizeof l);memcpy(r,dry,sizeof r);
    color_run(&state,&p,l,r,N);
    assert(!memcmp(l,dry,sizeof l) && !memcmp(r,dry,sizeof r));
    p.v[COL_ON]=1; p.v[COL_AMOUNT]=0;
    color_run(&state,&p,l,r,N);
    assert(!memcmp(l,dry,sizeof l) && !memcmp(r,dry,sizeof r));
    p.v[COL_AMOUNT]=127;
    for(mode=COL_WEAR;mode<=COL_DROP;mode++){
        double delta=0;
        color_reset(&state);
        for(i=COL_WEAR;i<=COL_DROP;i++)p.v[i]=i==mode?127:0;
        for(j=0;j<24;j++) {
            memcpy(l,dry,sizeof l);memcpy(r,dry,sizeof r);
            color_run(&state,&p,l,r,N);
            for(i=0;i<N;i++) {
                assert(isfinite(l[i]) && isfinite(r[i]));
                delta+=fabs(l[i]-dry[i])+fabs(r[i]-dry[i]);
            }
        }
        assert(delta>2.);
        /* Force silence over the full history, including any generated crackle. */
        for(j=0;j<32;j++){
            memset(l,0,sizeof l);memset(r,0,sizeof r);
            color_run(&state,&p,l,r,N);
        }
        for (i=0;i<N;i++)assert(l[i]==0.f && r[i]==0.f);
        if(mode==COL_WEAR) for(j=0;j<8;j++){
            color_reset(&state);
            color_run(&state,&p,l,r,N);
            for (i=0;i<N;i++)assert(l[i]==0.f && r[i]==0.f);
        }
        printf("module %d applies audible processing and drains to silence (delta %.1f)\n",mode,delta);
    }
    for(i=COL_WEAR;i<=COL_DROP;i++)p.v[i]=127;
    for(j=0;j<12;j++){
        for(i=0;i<N;i++){l[i]=2.f*dry[i];r[i]=-2.f*dry[i];}
        color_run(&state,&p,l,r,N);
        for(i=0;i<N;i++)assert(isfinite(l[i]) && isfinite(r[i]) && fabsf(l[i])<2.f && fabsf(r[i])<2.f);
    }
    {
        double delta = 0;
        color_reset(&state);
        memcpy(l,dry,sizeof l); memcpy(r,dry,sizeof r);
        for(i=0;i<N;i++) { rev[i] = 0; dly[i] = dry[i]*.4f; }
        color_run_drums(&state,&sends,&p,l,r,rev,dly,N);
        for(i=0;i<N;i++) {
            assert(rev[i] == 0.f); /* a lane with zero sends doesn't gain one */
            delta += fabs(dly[i]-dry[i]*.4f);
        }
        assert(delta>2);
        color_reset(&state);
        memset(l,0,sizeof l); memset(r,0,sizeof r); memset(rev,0,sizeof rev); memset(dly,0,sizeof dly);
        color_run_drums(&state,&sends,&p,l,r,rev,dly,N);
        for(i=0;i<N;i++) assert(l[i]==0.f && r[i]==0.f && rev[i]==0.f && dly[i]==0.f);
    }
    printf("exact bypass, per-module bounds and silence gate pass; state %zu bytes\n",sizeof state);
    return 0;
}
