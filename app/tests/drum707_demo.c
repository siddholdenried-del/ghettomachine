/* SPDX-License-Identifier: GPL-3.0-only */
/* Standalone WAV audition for the synthesized engine, no other files/samples. */
#include "drum707.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

enum { RATE=44100, BLOCK=256, STEP=5292, BARS=3, SLOT=23153, LEN=16*BARS*STEP+D7_NUM*SLOT+RATE/2 };
static void le(FILE *f, uint32_t n, int bytes) {
    for(int i=0;i<bytes;i++) { fputc(n & 255u,f); n >>= 8; }
}
static int pcm(float x) {
    x=x>0.987f ? 0.987f : x< -0.987f ? -0.987f : x;
    return (int)(x * 32767.0f);
}
static void batch(drum707_t *d, int step, int bar) {
    if (!(step&3)) drum707_trigger(d,D7_BD,0.94f);
    if (step==4||step==12) { drum707_trigger(d,D7_CP,0.76f); drum707_trigger(d,D7_SD,0.70f); }
    if ((step&3)==2) drum707_trigger(d,D7_OH,0.53f);
    if ((step&1)==1||step==0) drum707_trigger(d,D7_CH,(step&3)==1 ? 0.38f : 0.57f);
    if (step==3||step==10||step==14) drum707_trigger(d,D7_RS,0.52f);
    if (bar==1 && (step==7||step==11)) drum707_trigger(d,D7_CB,0.48f);
    if (bar==2) {
        if (step==0) drum707_trigger(d,D7_CR,0.50f);
        if (step==3||step==7||step==11||step==15) drum707_trigger(d,D7_RD,0.49f);
        if (step>=13) drum707_trigger(d,D7_LT+(step-13),0.74f);
        if (step==9) drum707_trigger(d,D7_TB,0.59f);
    }
}
int main(int argc, char **argv) {
    if(argc!=2) return 2;
    FILE *f=fopen(argv[1],"wb"); if(!f) return 1;
    fwrite("RIFF",1,4,f); le(f,LEN*4u+36u,4);
    fwrite("WAVEfmt ",1,8,f); le(f,16,4); le(f,1,2); le(f,2,2); le(f,RATE,4); le(f,RATE*4,4); le(f,4,2); le(f,16,2);
    fwrite("data",1,4,f); le(f,LEN*4u,4);
    drum707_t d; drum707_init(&d);
    drum707_set(&d,D7_LT,D7P_PAN,43); drum707_set(&d,D7_HT,D7P_PAN,86);
    drum707_set(&d,D7_CH,D7P_PAN,99); drum707_set(&d,D7_OH,D7P_PAN,46);
    int pos=0, stop=0;
    while(pos<LEN) {
        if(pos==stop) {
            if(pos<BARS*16*STEP) {
                const int step=pos/STEP;
                batch(&d,step%16,step/16);
                stop=pos+STEP;
            } else {
                const int v=(pos-(BARS*16*STEP))/SLOT;
                if (v==0) drum707_init(&d);
                if (v<D7_NUM) {
                    if(v>0) drum707_cut(&d,v-1);
                    drum707_trigger(&d,v,0.90f);
                    stop=pos+SLOT;
                }
                else stop=LEN;
            }
        }
        const int n=stop-pos<BLOCK ? stop-pos : BLOCK;
        float l[BLOCK]={0},r[BLOCK]={0},a[BLOCK]={0},b[BLOCK]={0};
        drum707_render_st(&d,l,r,a,b,n);
        for(int i=0;i<n;i++) { le(f,(uint32_t)pcm(l[i]*1.28f),2); le(f,(uint32_t)pcm(r[i]*1.28f),2); }
        pos+=n;
    }
    if(fclose(f)!=0) return 1;

    /* Worst simultaneous bank: open+closed hats ring with choke off. Run every
     * voice active at the beginning of every block, all sends enabled. */
    float l[BLOCK]={0},r[BLOCK]={0},a[BLOCK]={0},b[BLOCK]={0};
    const int reps=3500;
    float peak=0;
    drum707_init(&d); drum707_set(&d,D7_KIT,D7K_CHOKE,0);
    for(int t=0;t<D7_NUM;t++) {
        drum707_set(&d,t,D7P_REV,96); drum707_set(&d,t,D7P_DLY,96);
    }
    const clock_t start=clock();
    for(int c=0;c<reps;c++) {
        for(int t=0;t<D7_NUM;t++) drum707_trigger(&d,t,1.0f);
        for(int i=0;i<BLOCK;i++) l[i]=r[i]=a[i]=b[i]=0;
        drum707_render_st(&d,l,r,a,b,BLOCK);
        for(int i=0;i<BLOCK;i++) {
            float x=l[i]<0?-l[i]:l[i];
            if (x>peak) peak=x;
        }
    }
    const double secs=(double)(clock()-start)/CLOCKS_PER_SEC;
    printf("WAV: %.2f s; all %d voices + FX benchmark: %.2f%% real-time on this host; raw worst aligned-hit peak %.3f\n",
           (double)LEN/RATE,D7_NUM,100.0*secs*RATE/(reps*BLOCK),peak);
    return 0;
}
