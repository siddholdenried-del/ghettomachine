/* Host only. Include after the pinned SDK plat_host.c, with its main renamed. */
#include <assert.h>

#ifdef WRITE_REFERENCE
static void store_test_sample(int t)
{
    enum { frames = 4096 };
    struct { smp_head_t head; uint8_t data[frames*3/2]; } sample = {{SMP_MAGIC, frames, 22050, 60, {0}}, {0}};
    for (unsigned i = 0; i < frames; i += 2) {
        unsigned first = ((i * (9u + 2u*t)) & 2047u) - 1024u;
        unsigned second = (((i + 1) * (9u + 2u*t)) & 2047u) - 1024u;
        unsigned off = i * 3 / 2;
        sample.data[off] = first & 255u;
        sample.data[off+1] = ((first >> 8) & 15u) | ((second & 15u) << 4);
        sample.data[off+2] = (second >> 4) & 255u;
    }
    const uint8_t *raw = (const uint8_t *)&sample;
    assert(!app_file_begin(t-1, sizeof sample));
    assert(!app_file_write(t-1, 0, raw, sizeof sample));
    assert(!app_file_commit(t-1, sizeof sample, crc32_of(raw, sizeof sample)));
}
static void write_image(const char *path, int active, int sends)
{
    boot();
    for (int t = 1; t < NTRK; ++t) {
        store_test_sample(t);
        tset(t)->s_loop = 1;
        tset(t)->s_lstart = 123;
        tset(t)->s_tune = 5-t;
        proj.mix[t].pan = 33+t*17;
        proj.mix[t].rev = sends ? 70 : 0;
        proj.mix[t].dly = sends ? 83 : 0;
        track_insert_default(&proj.insert[t-1], t);
        proj.insert[t-1].v[3] = active ? 107 : 0;
        color_defaults(&proj.color[t]);
        proj.color[t].v[COL_ON] = active;
        proj.color[t].v[COL_AMOUNT] = 107;
    }
    proj.fx[FXP_RV_LEVEL] = 92;
    proj.fx[FXP_DL_LEVEL] = 100;
    assert(!project_save());
    assert(!himg_dump(path));
}
#endif

static void midi_note(int t, int on)
{
    unsigned status = (on ? 0x90u : 0x80u) | (unsigned)t;
    min_q[mi_w % MQ] = (on ? 9u : 8u) | (status << 8) | ((unsigned)(54+3*t) << 16) | ((on ? 108u : 0u) << 24);
    min_port[mi_w++ % MQ] = PORT_USB;
}
int main(int argc, char **argv)
{
    plat_host_dir = ".";
#ifdef WRITE_REFERENCE
    if (argc == 5 && !strcmp(argv[1],"write")) {
        write_image(argv[2], atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
#endif
    if (argc != 5 || strcmp(argv[1],"read")) return 2;
    unsigned count = 0;
    assert(!himg_load(argv[2],&count));
    boot();
    const int fx_active = atoi(argv[3]);
    for (int t = 1; t < NTRK; ++t) {
        assert(tset(t)->type == TY_SAMPLER && syn_sample_frames(t) == 4096 && smp_rate[t] == 22050);
        assert(proj.color[t].v[COL_ON] == fx_active && proj.insert[t-1].v[3] == (fx_active ? 107 : 0));
        assert(tset(t)->s_loop && proj.mix[t].pan == 33+t*17);
    }
    FILE *file = fopen(argv[4], "wb");
    assert(file);
    for (int b = 0; b < 300; ++b) {
        int32_t lr[2*APP_BLOCK];
        for (int t = 1; t < NTRK; ++t) {
            if (b == 16*t || b == 95) midi_note(t,1);
            if (b == 16*t+11 || b == 125) midi_note(t,0);
        }
        app_render(lr,APP_BLOCK,4096);
        assert(fwrite(lr, sizeof lr, 1, file) == 1);
        for (int t = 1; t < NTRK; ++t) {
            for (int i = 0; i < APP_BLOCK; ++i)
                assert(fwrite(&usb_ch[i][2*t+2], sizeof(int16_t)*2, 1, file) == 1);
        }
    }
    assert(!fclose(file));
}
