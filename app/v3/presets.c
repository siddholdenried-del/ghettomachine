#include "presets.h"
#include <stddef.h>
#include <stdint.h>

#define COUNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
enum { MAX_PARAMS = 10, CYMBAL_MODE = 65535, TONE_FIELD = 65534, DRIVE_FIELD = 65533,
       PERC_MODEL_F = 65532, PERC_RESO_F = 65531, PERC_FILTER_F = 65530 };   /* (the three packed in PercPatch.mode) */
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
                                         "D Vel sweep", "E Metal FM", "F Noise", "G Zap", "H Sync", "I Ring"};
static const char *const perc_filters[] = {"Low-pass", "Band-pass", "High-pass"};
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
    PERCENT("Snappy",KitPatch,snare_snappy), PERCENT("Wire",KitPatch,snare_wire), SHAPING,
    /* (after the shaping pair: a recipe that leaves it out has it at 0, the drum as it was) */
    NUMBER("Decay","%",-1000,1000,80,1,KitPatch,snare_decay,1000)
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
    {{"Model","",0,COUNT(perc_models)-1,1,1,0},perc_models,PERC_MODEL_F,0}, PERCENT("Tune",PercPatch,tune),
    PERCENT("Decay",PercPatch,decay), PERCENT("Width",PercPatch,width),
    NUMBER("Sweep","%",-1000,1000,80,1,PercPatch,sweep,1000), PERCENT("Speed",PercPatch,speed), SHAPING,
    /* (0.5, after the shaping so the indices a project's locks and LFO use keep their meaning) */
    {{"Reso","%",0,1000,40,1,1},NULL,PERC_RESO_F,1000}, {{"Filter","",0,2,1,1,0},perc_filters,PERC_FILTER_F,0}
};

/* Stored in display ticks; recipes are complete and independent of edit order. Every drum has one preset per genre,
 * in the genre kits' order (kit_genre_*); the last two ticks of each are its Tone and Drive (0 when left out).
 * ELECTRO is the factory kit as it was (the 808-style recipes renamed), HOUSE the 909's. */
static const Recipe kicks[] = {
    {"Electro",    {520,1500,110,50,300,680}},       {"House",       {530,880,280,140,540,820}},   /* (0.5: Electro's click 16 -> 30 %: its bite) */
    {"Techno",     {520,560,260,110,640,900,0,150}}, {"Trap",        {440,2100,120,90,120,600}},
    {"Boom Bap",   {600,380,90,40,420,560,-150,0}},  {"Drum & Bass", {580,420,200,70,720,800}},
    {"Garage",     {560,520,180,90,460,660}},        {"Disco",       {600,640,150,80,340,450}},
    {"Dub",        {370,2050,50,100,80,260}},        {"Lo-Fi",       {580,420,80,40,220,520,-420,0}},
    {"Industrial", {700,470,185,80,860,940,0,500}},  {"Minimal",     {620,300,75,30,300,480}}
};
static const Recipe snares[] = {
    {"Electro",    {0,470,550,600,460}},             {"House",       {1,520,600,740,620}},
    {"Techno",     {1,560,480,800,700,150,0}},       {"Trap",        {0,680,400,900,840}},
    {"Boom Bap",   {1,240,900,580,290}},             {"Drum & Bass", {1,650,500,850,750,200,0}},
    {"Garage",     {2,750,290,250,720}},             {"Disco",       {1,480,650,600,500}},
    {"Dub",        {0,380,700,500,300,-250,0}},      {"Lo-Fi",       {0,420,650,450,350,-450,0}},
    {"Industrial", {1,300,900,900,900,0,550}},       {"Minimal",     {2,640,320,300,600,100,0}}
};
static const Recipe claps[] = {
    {"Electro",    {0,460,540,480}},                 {"House",       {1,520,670,550}},
    {"Techno",     {1,600,560,650,100,0}},           {"Trap",        {0,700,620,500,150,0}},
    {"Boom Bap",   {2,300,450,250,-200,0}},          {"Drum & Bass", {1,450,700,350,200,0}},
    {"Garage",     {2,180,390,120}},                 {"Disco",       {1,550,600,600}},
    {"Dub",        {0,860,490,910}},                 {"Lo-Fi",       {0,500,400,450,-400,0}},
    {"Industrial", {1,800,750,700,0,450}},           {"Minimal",     {2,260,520,180}}
};
static const Recipe toms[] = {
    {"Electro",    {0,500,500,480}},                 {"House",       {1,550,490,670}},
    {"Techno",     {1,480,350,720,0,150}},           {"Trap",        {0,420,720,380}},
    {"Boom Bap",   {0,420,600,450,-150,0}},          {"Drum & Bass", {1,620,380,760}},
    {"Garage",     {0,720,140,840}},                 {"Disco",       {0,560,650,520}},
    {"Dub",        {0,250,910,210}},                 {"Lo-Fi",       {0,400,550,350,-400,0}},
    {"Industrial", {1,380,500,850,0,500}},           {"Minimal",     {0,680,220,800}}
};
static const Recipe closed_hats[] = {
    {"Electro",    {0,500,500}},                     {"House",       {1,400,650}},
    {"Techno",     {1,250,700}},                     {"Trap",        {1,160,720}},
    {"Boom Bap",   {0,370,180}},                     {"Drum & Bass", {1,220,780}},
    {"Garage",     {1,300,800}},                     {"Disco",       {1,450,600}},
    {"Dub",        {0,450,300}},                     {"Lo-Fi",       {0,380,200,-350,0}},
    {"Industrial", {1,200,900,0,450}},               {"Minimal",     {1,140,680}}
};
static const Recipe open_hats[] = {
    {"Electro",    {0,500,500}},                     {"House",       {1,600,650}},
    {"Techno",     {1,450,700}},                     {"Trap",        {1,220,790}},
    {"Boom Bap",   {0,550,350}},                     {"Drum & Bass", {1,400,720}},
    {"Garage",     {1,500,780}},                     {"Disco",       {1,920,700}},
    {"Dub",        {0,810,220}},                     {"Lo-Fi",       {0,600,250,-350,0}},
    {"Industrial", {1,550,900,0,450}},               {"Minimal",     {1,260,700}}
};
static const Recipe cymbals[] = {
    {"Electro",    {0,0,500,500}},                   {"House",       {1,0,500,500}},
    {"Techno",     {1,1,350,840}},                   {"Trap",        {2,1,760,730}},
    {"Boom Bap",   {0,0,840,220}},                   {"Drum & Bass", {1,0,450,700}},
    {"Garage",     {1,1,400,750}},                   {"Disco",       {0,1,700,650}},
    {"Dub",        {3,0,650,420}},                   {"Lo-Fi",       {0,0,700,250,-350,0}},
    {"Industrial", {3,1,700,800,0,500}},             {"Minimal",     {1,0,300,600}}
};
static const Recipe percussion[] = {
    /* (0.5: Electro is a laser zap and Techno a resonant metal ping; the rest are as they were) */
    {"Electro",    {6,440,400,900,-820,800,0,0,640,0}}, {"House",    {1,420,350,640,-350,640}},
    {"Techno",     {4,680,420,700,180,500,0,0,560,1}},  {"Trap",     {1,840,260,780,280,830}},
    {"Boom Bap",   {0,290,610,370,-150,560}},        {"Drum & Bass", {0,700,300,600,-500,700}},
    {"Garage",     {1,600,300,700,200,700}},         {"Disco",       {3,330,530,520,0,500}},
    {"Dub",        {2,500,700,700,-200,400}},        {"Lo-Fi",       {5,460,780,680,0,500}},
    {"Industrial", {4,500,600,800,400,800,0,400}},   {"Minimal",     {2,760,560,800,0,500}},
    /* past the genres (0.5): the electro set. Model, Tune, Decay, Width, Sweep, Speed, Tone, Drive, Reso, Filter */
    {"Laser Zap",  {6,520,360,1000,-1000,860,0,0,700,0}},  {"Acid Blip",  {6,300,300,1000,-460,900,0,250,900,0}},
    {"Reso Sweep", {5,500,820,1000,0,500,0,0,920,1}},      {"Sync Tear",  {7,360,520,820,-900,560,0,0,320,0}},
    {"Sync Rise",  {7,300,620,700,800,320,0,0,420,2}},     {"Ring Ping",  {8,640,460,620,-420,800,0,0,520,1}},
    {"Pew",        {6,700,240,1000,-700,920,0,0,780,1}},   {"Bubble",     {0,360,300,900,900,860,0,0,880,0}}
};
_Static_assert(sizeof percussion / sizeof percussion[0] <= KIT_MAX_RECIPES, "the level trims' table has a place for every recipe");

/* the genre kits: every drum on its genre's preset, but PERC 2 on another genre's percussion so the two differ */
static const char *const genres[] = {"Electro", "House", "Techno", "Trap", "Boom Bap", "Drum & Bass", "Garage", "Disco",
                                     "Dub", "Lo-Fi", "Industrial", "Minimal"};
static const unsigned char perc2_of[] = {2, 4, 0, 9, 1, 3, 11, 4, 11, 4, 2, 1};
_Static_assert(sizeof genres / sizeof genres[0] == KIT_GENRES, "KIT_GENRES genres");
_Static_assert(sizeof perc2_of == KIT_GENRES, "a second percussion for every genre");

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
    if (param->offset==PERC_MODEL_F) return (float)PERC_MODEL(p->perc[pad-10].mode);
    if (param->offset==PERC_RESO_F) return (float)PERC_RESO(p->perc[pad-10].mode) * .001f;
    if (param->offset==PERC_FILTER_F) return (float)PERC_FILTER(p->perc[pad-10].mode);
    const unsigned char *field=(const unsigned char *)base+param->offset;
    return param->scale ? *(const float *)field : (float)*(const int *)field;
}
static void write_tick(KitPatch *p, int pad, void *base, const Param *param, int tick)
{
    if (param->offset==CYMBAL_MODE) { p->metal_mode=tick; return; }
    if (param->offset==TONE_FIELD) { p->tone[pad-1]=(float)tick/(float)param->scale; return; }
    if (param->offset==DRIVE_FIELD) { p->drive[pad-1]=(float)tick/(float)param->scale; return; }
    if (param->offset>=PERC_FILTER_F && param->offset<=PERC_MODEL_F) {
        int *m=&p->perc[pad-10].mode;
        if (param->offset==PERC_MODEL_F) *m=(*m & ~15) | (tick & 15);
        else if (param->offset==PERC_RESO_F) *m=(*m & ~(1023<<8)) | ((tick > 1000 ? 1000 : tick < 0 ? 0 : tick)<<8);
        else *m=(*m & ~(3<<4)) | ((tick & 3)<<4);
        return;
    }
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
    const int model=PERC_MODEL(p->perc[pad-10].mode);
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

int kit_genre_count(void) { return KIT_GENRES; }
const char *kit_genre_name(int genre) { return genre >= 0 && genre < KIT_GENRES ? genres[genre] : NULL; }
int kit_genre_recipe(int genre, int pad)
{
    if (genre < 0 || genre >= KIT_GENRES || pad < 1 || pad > 11) return -1;
    return pad == 11 ? perc2_of[genre] : genre;
}
int kit_genre_apply(KitPatch *patch, int genre)
{
    if (!patch || genre < 0 || genre >= KIT_GENRES) return 0;
    for (int pad = 1; pad <= 11; ++pad)
        if (!kit_preset_apply(patch, pad, kit_genre_recipe(genre, pad))) return 0;
    return 1;
}
