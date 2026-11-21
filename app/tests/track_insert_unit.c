/* cc -O2 -std=c99 app/tests/track_insert_unit.c app/dsp/track_insert.c -lm && ./a.out */
#include "../dsp/track_insert.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846
#define N 10240
static float l[N], r[N], dry[N];
static track_insert_t fx;

static void run(const track_insert_param_t *p)
{
    unsigned i;
    for (i = 0; i < N; i += 32) track_insert_process(&fx,p,l+i,r+i,32);
}
static double amplitude(float *x, double hz)
{
    double a=0, b=0;
    int i;
    for (i=512;i<N;i++) {
        double win=.5-.5*cos(2*PI*(i-512)/(N-513)), theta = 2*PI*hz*i/44100;
        a += x[i]*win*cos(theta); b += x[i]*win*sin(theta);
    }
    return 2*hypot(a,b)/(N-512);
}
static void test_shift(int direction)
{
    const double f0=3050, f1=5575, delta=337.5*direction;
    track_insert_param_t p;
    int i;
    track_insert_default(&p,TI_SHIFT);
    p.v[0] = (uint8_t)(64+24*direction);
    p.v[1]=p.v[2]=64;
    p.v[3]=127;
    track_insert_reset(&fx);
    for (i=0;i<N;i++) l[i]=r[i]=.29f*cos(2*PI*f0*i/44100)+.18f*cos(2*PI*f1*i/44100);
    run(&p);
    {
        double a=amplitude(l,f0+delta), b=amplitude(l,f1+delta);
        double im0=amplitude(l,f0-delta), im1=amplitude(l,f1-delta);
        printf("shift %+.1f Hz: wanted %.6f %.6f, mirrors %.6f %.6f\n",delta,a,b,im0,im1);
        assert(a>.10 && b>.065 && im0<a*.06 && im1<b*.06);
        assert(amplitude(l,f0)<a*.03 && amplitude(l,f1)<b*.03);
    }
}
int main(void)
{
    track_insert_param_t p;
    int mode, i;
    for (i=0;i<N;i++) dry[i]=.31f*cos(2*PI*1260*i/44100)+.22f*cos(2*PI*2300*i/44100);
    for (mode=0;mode<TI_MODES;mode++) {
        double change=0, power=0;
        memcpy(l,dry,sizeof l); memcpy(r,dry,sizeof r);
        track_insert_default(&p,mode);
        track_insert_reset(&fx);
        run(&p);
        for(i=0;i<N;i++) {
            assert(isfinite(l[i]) && isfinite(r[i]) && fabsf(l[i])<4.f && fabsf(r[i])<4.f);
            change+=fabs(l[i]-dry[i])+fabs(r[i]-dry[i]); power+=fabs(l[i])+fabs(r[i]);
        }
        assert(mode==TI_OFF ? change==0 : change>10 && power>40);
        /* Clear during a live tail, then feed silence. */
        track_insert_reset(&fx);
        memset(l,0,sizeof l); memset(r,0,sizeof r);
        run(&p);
        for(i=0;i<N;i++) assert(l[i]==0 && r[i]==0);
        /* Every mode with MIX=0 must leave the exact bits unchanged. */
        p.v[3]=0;
        memcpy(l,dry,sizeof l); memcpy(r,dry,sizeof r);
        run(&p);
        assert(!memcmp(l,dry,sizeof l) && !memcmp(r,dry,sizeof r));
        printf("%s: wet delta %.2f; hard-cut reset and exact bypass pass\n",track_insert_name(mode),change);
    }
    test_shift(1); test_shift(-1);
    printf("three full track DSP states: %zu bytes\n",3*sizeof fx);
    return 0;
}
