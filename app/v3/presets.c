#include "presets.h"
#include <stddef.h>
#include <stdint.h>

#define COUNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
enum { MAX_PARAMS = 8, CYMBAL_MODE = 65535, TONE_FIELD = 65534, DRIVE_FIELD = 65533 };
typedef struct {
    KitParamSpec spec;
    const char *const *choices;
    uint16_t offset;
    uint16_t scale;   /* 0 = native int; otherwise ticks per native float unit */
} Param;
typedef struct { const char *name; int16_t ticks[MAX_PARAMS]; } Recipe;
typedef struct {
    const Param *params;
    const Recipe *recipes;
    int param_count, recipe_count;
} Bank;

static const char *const shells[] = {"Round", "Hard", "Dry"};
static const char *const tom_models[] = {"Round", "Hard"};
static const char *const profiles[] = {"Low", "High"};
static const char *const articulations[] = {"Crash", "Ride", "Riser", "Noise Drop"};
static const char *const perc_models[] = {"A Single osc", "B FM pair", "C Dual osc",
                                         "D Vel sweep", "E Metal FM", "F Noise"};
#define NUMBER(label,unit,lo,hi,step,dec,type,field,scale) \
    {{label,unit,lo,hi,step,1,dec},NULL,offsetof(type,field),scale}
#define PERCENT(label,type,field) NUMBER(label,"%",0,1000,40,1,type,field,1000)
#define OPTION(label,names,type,field) \
    {{label,"",0,COUNT(names)-1,1,1,0},names,offsetof(type,field),0}
/* Every drum ends with the same two output controls; recipes leave them at 0. */
#define SHAPING {{"Tone","%",-1000,1000,80,1,1},NULL,TONE_FIELD,1000}, \
                {{"Drive","%",0,1000,40,1,1},NULL,DRIVE_FIELD,1000}

static const Param kick_params[] = {
    NUMBER("Body tune","Hz",300,800,20,1,KickPatch,tune_hz,10),
    NUMBER("Body decay","ms",100,2400,80,0,KickPatch,decay_ms,1),
    NUMBER("Pitch drop amount","st",0,360,10,1,KickPatch,drop_st,10),
    NUMBER("Pitch drop time","ms",20,1200,40,1,KickPatch,drop_ms,10),
    PERCENT("Click level",KickPatch,click), PERCENT("Body drive",KickPatch,drive), SHAPING
};
static const Param snare_params[] = {
    OPTION("Model",shells,KitPatch,snare_preset),
    PERCENT("Tune",KitPatch,snare_tune), PERCENT("Body",KitPatch,snare_body),
    PERCENT("Snappy",KitPatch,snare_snappy), PERCENT("Wire",KitPatch,snare_wire), SHAPING
};
static const Param clap_params[] = {
    OPTION("Model",shells,KitPatch,clap_preset), PERCENT("Spread",KitPatch,clap_spread),
    PERCENT("Color",KitPatch,clap_tone), PERCENT("Tail",KitPatch,clap_tail), SHAPING
};
static const Param tom_params[] = {
    OPTION("Model",tom_models,TomPatch,model), PERCENT("Tune",TomPatch,tune),
    PERCENT("Decay",TomPatch,decay), PERCENT("Attack",TomPatch,attack), SHAPING
};
static const Param cymbal_params[] = {
    {{"Mode","",0,3,1,1,0},articulations,CYMBAL_MODE,0},
    OPTION("Profile",profiles,MetalPatch,profile),
    PERCENT("Decay",MetalPatch,decay), PERCENT("Brightness",MetalPatch,brightness), SHAPING
};

static const Param perc_params[] = {
    OPTION("Model",perc_models,PercPatch,mode), PERCENT("Tune",PercPatch,tune),
    PERCENT("Decay",PercPatch,decay), PERCENT("Width",PercPatch,width),
    NUMBER("Sweep","%",-1000,1000,80,1,PercPatch,sweep,1000), PERCENT("Speed",PercPatch,speed), SHAPING
};

/* Stored in display ticks; recipes are complete and independent of edit order. */
static const Recipe kicks[] = {
    {"808 Round",  {520,1500,110,50,160,680}}, {"909 Punch", {530,880,280,140,540,820}},
    {"Short Thump",{620,300,75,30,300,480}},   {"Sub Boom",  {370,2050,50,100,80,260}},
    {"Hard Knock", {700,470,185,80,860,940}},  {"Laser Kick",{460,720,345,580,350,590}}
};
static const Recipe snares[] = {
    {"808 Snare",{0,470,550,600,460}}, {"909 Snare",{1,520,600,740,620}},
    {"Dry Rim",{2,750,290,250,720}},   {"Wire Snap",{0,680,400,900,840}},
    {"Deep Shell",{1,240,900,580,290}}
};
static const Recipe claps[] = {
    {"808 Clap",{0,460,540,480}}, {"909 Clap",{1,520,670,550}},
    {"Dry Hands",{2,180,390,120}}, {"Wide Room",{0,860,490,910}}
};
static const Recipe toms[] = {
    {"Round Tom",{0,500,500,480}}, {"Hard Tom",{1,550,490,670}},
    {"Short Wood",{0,720,140,840}}, {"Deep Skin",{0,250,910,210}}
};
static const Recipe closed_hats[] = {
    {"Closed Hat",{0,500,500}}, {"Tight Tick",{1,160,720}},
    {"Dark Chick",{0,370,180}}, {"Loose Hat",{1,830,580}}
};
static const Recipe open_hats[] = {
    {"Open Hat",{0,500,500}}, {"Short Sizzle",{1,220,790}},
    {"Dark Wash",{0,810,220}}, {"Long Shimmer",{1,920,700}}
};
static const Recipe cymbals[] = {
    {"Crash",{0,0,500,500}}, {"Ride",{1,0,500,500}},
    {"Riser",{2,1,760,730}}, {"Noise Drop",{3,0,650,420}},
    {"Dark Crash",{0,0,840,220}}, {"Bright Ride",{1,1,350,840}}
};
static const Recipe percussion[] = {
    {"Electro Zap",{0,590,440,780,-880,820}},  {"FM Block",{1,420,350,640,-350,640}},
    {"Twin Bell",{2,760,560,800,0,500}},      {"Velocity Tom",{3,330,530,520,0,500}},
    {"Metal Sheet",{4,680,420,550,180,500}},   {"Noise Brush",{5,460,780,680,0,500}},
    {"Round Bongo",{0,290,610,370,-150,560}},  {"High Ping",{1,840,260,780,280,830}}
};

#define BANK(p,r) {p,r,COUNT(p),COUNT(r)}
static const Bank banks[] = {
    BANK(kick_params,kicks), BANK(snare_params,snares), BANK(clap_params,claps),
    BANK(tom_params,toms),
    {cymbal_params+1,closed_hats,COUNT(cymbal_params)-1,COUNT(closed_hats)},
    {cymbal_params+1,open_hats,COUNT(cymbal_params)-1,COUNT(open_hats)},
    BANK(cymbal_params,cymbals), BANK(perc_params,percussion)
};
static const uint8_t pad_bank[] = {0,1,2,3,3,3,4,5,6,7,7};
static const Bank *bank(int pad)
{
    return pad>=1 && pad<=COUNT(pad_bank) ? &banks[pad_bank[pad-1]] : NULL;
}
static const Param *parameter(int pad, int index)
{
    const Bank *b=bank(pad);
    return b && index>=0 && index<b->param_count ? b->params+index : NULL;
}
static const Recipe *recipe(int pad, int index)
{
    const Bank *b=bank(pad);
    if (!b || index<0 || index>=b->recipe_count) return NULL;
    /* Both Perc slots expose the same sounds with their factory sound first. */
    if (pad==11) {
        if (index==0) index=4;
        else if (index==4) index=0;
    }
    return b->recipes+index;
}

int kit_preset_count(int pad) { const Bank *b=bank(pad); return b ? b->recipe_count : 0; }
int kit_param_count(int pad) { const Bank *b=bank(pad); return b ? b->param_count : 0; }
const char *kit_preset_name(int pad, int index)
{
    const Recipe *r=recipe(pad,index);
    return r ? r->name : NULL;
}
const KitParamSpec *kit_param_spec(int pad, int index)
{
    const Param *p=parameter(pad,index);
    return p ? &p->spec : NULL;
}
const char *kit_param_option(int pad, int index, int tick)
{
    const Param *p=parameter(pad,index);
    return p && p->choices && tick>=0 && tick<=p->spec.max_tick ? p->choices[tick] : NULL;
}

typedef union { TomPatch tom; MetalPatch metal; } Effective;
static const void *read_base(const KitPatch *p, int pad, Effective *e)
{
    if (pad==1) return &p->kick;
    if (pad<=3) return p;
    if (pad<=6) { e->tom=kit_tom_controls(p,pad); return &e->tom; }
    if (pad<=9) { e->metal=kit_metal_controls(p,pad); return &e->metal; }
    return &p->perc[pad-10];
}
static void *write_base(KitPatch *p, int pad)
{
    if (pad==1) return &p->kick;
    if (pad<=3) return p;
    if (pad<=9) {
        kit_make_pad_local(p,pad);
        if (pad<=6) return &p->tom[pad-4];
        if (pad<=8) return &p->hat[pad-7];
        return &p->cymbal;
    }
    return &p->perc[pad-10];
}
static float read_value(const KitPatch *p, int pad, const void *base, const Param *param)
{
    if (param->offset==CYMBAL_MODE) return (float)p->metal_mode;
    if (param->offset==TONE_FIELD) return p->tone[pad-1];
    if (param->offset==DRIVE_FIELD) return p->drive[pad-1];
    const unsigned char *field=(const unsigned char *)base+param->offset;
    return param->scale ? *(const float *)field : (float)*(const int *)field;
}
static void write_tick(KitPatch *p, int pad, void *base, const Param *param, int tick)
{
    if (param->offset==CYMBAL_MODE) { p->metal_mode=tick; return; }
    if (param->offset==TONE_FIELD) { p->tone[pad-1]=(float)tick/(float)param->scale; return; }
    if (param->offset==DRIVE_FIELD) { p->drive[pad-1]=(float)tick/(float)param->scale; return; }
    unsigned char *field=(unsigned char *)base+param->offset;
    if (param->scale) *(float *)field=(float)tick/(float)param->scale;
    else *(int *)field=tick;
}

int kit_param_get(const KitPatch *p, int pad, int index)
{
    const Param *param=parameter(pad,index);
    if (!p || !param) return 0;
    Effective e;
    float value=read_value(p,pad,read_base(p,pad,&e),param);
    if (param->scale) value *= param->scale;
    if (!(value>param->spec.min_tick)) return param->spec.min_tick;
    if (value>=param->spec.max_tick) return param->spec.max_tick;
    return (int)(value + (value<0.f ? -.5f : .5f));
}

int kit_param_editable(const KitPatch *p, int pad, int index)
{
    if (!p || !parameter(pad,index)) return 0;
    if (pad<10 || index<4 || index>5) return 1;
    const int model=p->perc[pad-10].mode;
    return model!=3 && model!=5; /* Velocity sweep supplies its own glide. Noise has no pitched source. */
}
int kit_param_set(KitPatch *p, int pad, int index, int tick)
{
    if (!kit_param_editable(p,pad,index)) return 0;
    const Param *param=parameter(pad,index);
    if (tick<param->spec.min_tick) tick=param->spec.min_tick;
    if (tick>param->spec.max_tick) tick=param->spec.max_tick;
    write_tick(p,pad,write_base(p,pad),param,tick);
    return 1;
}
int kit_preset_apply(KitPatch *p, int pad, int index)
{
    const Recipe *r=recipe(pad,index);
    if (!p || !r) return 0;
    const Bank *b=bank(pad);
    void *base=write_base(p,pad);
    for (int i=0; i<b->param_count; ++i) write_tick(p,pad,base,b->params+i,r->ticks[i]);
    return 1;
}
int kit_preset_match(const KitPatch *p, int pad)
{
    const Bank *b=bank(pad);
    if (!p || !b) return -1;
    Effective e;
    const void *base=read_base(p,pad,&e);
    for (int n=0;n<b->recipe_count;++n) {
        const Recipe *r=recipe(pad,n);
        int i=0;
        for (; i<b->param_count; ++i) {
            const Param *param=b->params+i;
            float expected=param->scale ? (float)r->ticks[i]/(float)param->scale : (float)r->ticks[i];
            float difference=read_value(p,pad,base,param)-expected;
            /* Allow one conversion's roundoff, never a half-tick display match. */
            if (!(difference<=.000001f && difference>=-.000001f)) break;
        }
        if (i==b->param_count) return n;
    }
    return -1;
}
