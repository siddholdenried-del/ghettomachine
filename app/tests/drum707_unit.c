/* SPDX-License-Identifier: GPL-3.0-only */
#include "drum707.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 256
static float l[BLOCK],r[BLOCK],rev[BLOCK],dly[BLOCK];
static void clear(void)
{
    memset(l,0,sizeof l); memset(r,0,sizeof r); memset(rev,0,sizeof rev); memset(dly,0,sizeof dly);
}
static float energy(const float *a)
{
    float e=0;
    for(int i=0;i<BLOCK;i++) { assert(isfinite(a[i])); e+=a[i]*a[i]; }
    return e;
}
int main(void)
{
    drum707_t a;
    float attacks[D7_NUM];
    assert(!drum707_nparams(-1));
    assert(!drum707_param(D7_NUM+1,0));
    assert(!drum707_param(D7_BD,-1));
    for(int v=0;v<D7_NUM;v++) {
        drum707_init(&a);
        assert(!drum707_active(&a));
        drum707_trigger(&a,v,0);
        assert(!drum707_active(&a));
        for(int p=0;p<drum707_nparams(v);p++)
            assert(drum707_get(&a,v,p)==drum707_param(v,p)->def);
        clear(); l[0]=0.125f;
        drum707_render_st(&a,l,r,rev,dly,BLOCK);
        assert(l[0]==0.125f && r[0]==0);
        clear();
        drum707_trigger(&a,v,1);
        assert(drum707_active(&a));
        drum707_render_st(&a,l,r,rev,dly,BLOCK);
        attacks[v]=energy(l);
        assert(attacks[v]>0.001f && fabsf(attacks[v]-energy(r))<1e-7f);
        drum707_cut(&a,v);
        assert(!drum707_active(&a));
        clear();
        drum707_set(&a,v,D7P_PAN,127); drum707_set(&a,v,D7P_REV,127); drum707_set(&a,v,D7P_DLY,127);
        drum707_trigger(&a,v,1);
        drum707_render_st(&a,l,r,rev,dly,BLOCK);
        assert(energy(l)<1e-7f && energy(r)>0.001f && energy(rev)>0.001f && energy(dly)>0.001f);
        for(int j=0;j<1024&&drum707_active(&a);j++) {
            clear(); drum707_render_st(&a,l,r,rev,dly,BLOCK);
        }
        assert(!drum707_active(&a));
    }
    assert(attacks[D7_BD]!=attacks[D7_SD] && attacks[D7_LT]!=attacks[D7_MT]);
    drum707_init(&a);
    drum707_trigger(&a,D7_OH,1);
    assert(a.voice[D7_OH].active);
    drum707_trigger(&a,D7_CH,1);
    assert(a.voice[D7_CH].active && !a.voice[D7_OH].active);
    printf("707 voice bank: %d voices, %zu B instance; audio, bounds, pan/sends, cutoff and choke passed\n",D7_NUM,sizeof a);
}
