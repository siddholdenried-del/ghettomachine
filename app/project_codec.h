/* Formats 4/5/7: header, metadata tail (pots through ACID settings), then 16
 * independently compressed pattern records. Each record starts with a
 * little-endian payload byte count, then literal runs (0..127 => 1..128
 * bytes), or back-references (128..255 => 3..130 bytes, then LE16 distance).
 * This layout keeps sound reload random-access and compresses repeated notes.
 * A preflight counts every byte before erasing the inactive atomic save slot. */
#define CODEC_BUCKETS 512
#define CODEC_MAX ((uint32_t)(sizeof(pattern_t) + (sizeof(pattern_t) + 127u) / 128u + 2u))
static uint8_t codec_out[CODEC_MAX] PLAT_POOL;
static uint16_t codec_prev[CODEC_BUCKETS] PLAT_POOL;
_Static_assert(sizeof(pattern_t) < 65535u, "pattern offsets fit the codec");

static uint32_t codec_hash(const uint8_t *p)
{
    return (((uint32_t)p[0] * 73u + p[1]) * 73u + p[2]) % CODEC_BUCKETS;
}

/* The match flag costs three bytes; use runs of >= 4. Flush literals every
 * 128 and update the dictionary for interior positions in either kind of run. */
static uint32_t codec_pack(const pattern_t *p)
{
    const uint8_t *in = (const uint8_t *)p;
    uint32_t n = sizeof *p, at = 0, put = 2, lit = 0, j;
    for (j = 0; j < CODEC_BUCKETS; j++) codec_prev[j] = 0;
    while (at < n) {
        uint32_t match = 0, dist = 0, take = 1;
        if (at + 4u <= n) {
            uint32_t prev = codec_prev[codec_hash(in + at)];
            if (prev) {
                prev--;
                while (match < 130u && at + match < n && in[prev + match] == in[at + match])
                    match++;
                dist = at - prev;
            }
        }
        if (match >= 4u) {
            codec_out[put++] = (uint8_t)(128u + match - 3u);
            codec_out[put++] = (uint8_t)dist;
            codec_out[put++] = (uint8_t)(dist >> 8);
            lit = 0;
            take = match;
        } else {
            if (!lit || codec_out[lit] == 127u) {
                lit = put++;
                codec_out[lit] = 0;
            } else {
                codec_out[lit]++;
            }
            codec_out[put++] = in[at];
        }
        for (j = at; j < at + take && j + 3u <= n; j++)
            codec_prev[codec_hash(in + j)] = (uint16_t)(j + 1u);
        at += take;
    }
    codec_out[0] = (uint8_t)(put - 2u);
    codec_out[1] = (uint8_t)((put - 2u) >> 8);
    return put;
}

static int project_decode(const uint8_t *in, uint32_t size, int fmt)
{
    uint32_t tail_bytes = fmt == 4 ? PROJ_V4_TAIL_BYTES : fmt == 5 ? PROJ_V5_TAIL_BYTES : PROJ_TAIL_BYTES;
    uint32_t meta_bytes = fmt == 4 ? offsetof(project_t, insert) - PROJ_TAIL :
                          fmt == 5 ? PROJ_V5_META_BYTES : tail_bytes;
    uint32_t at = PROJ_PREFIX + tail_bytes, j, i;
    uint8_t *raw = (uint8_t *)&proj, *tail = raw + PROJ_TAIL;
    if (!in || size < at)
        return -1;
    for (j = 0; j < PROJ_PREFIX; j++) raw[j] = in[j];
    for (j = 0; j < meta_bytes; j++) tail[j] = in[PROJ_PREFIX + j];
    for (i = 0; i < NPAT; i++) {
        uint8_t *out = (uint8_t *)&proj.p[i];
        uint32_t end, put = 0;
        if (size - at < 2u) return -1;
        end = (uint32_t)in[at] | (uint32_t)in[at + 1u] << 8;
        at += 2u;
        if (end > size - at) return -1;
        end += at;
        while (at < end) {
            uint32_t tag = in[at++], count = (tag & 127u) + (tag >= 128u ? 3u : 1u);
            if (count > sizeof(pattern_t) - put) return -1;
            if (tag < 128u) {
                if (count > end - at) return -1;
                for (j = 0; j < count; j++) out[put++] = in[at++];
            } else {
                uint32_t dist;
                if (end - at < 2u) return -1;
                dist = (uint32_t)in[at] | (uint32_t)in[at + 1u] << 8;
                at += 2u;
                if (!dist || dist > put) return -1;
                for (j = 0; j < count; j++, put++) out[put] = out[put - dist];
            }
        }
        if (put != sizeof(pattern_t)) return -1;
    }
    return at == size ? 0 : -1;
}

static int project_encode(void)
{
    const uint8_t *raw = (const uint8_t *)&proj;
    uint32_t size = PROJ_META, off = PROJ_META, crc, n, i;
    for (i = 0; i < NPAT; i++) {
        size += codec_pack(&proj.p[i]);
        if (size > PROJ_CAPACITY) return -2; /* prior save entirely untouched */
    }
    if (plat_file_begin(&PROJ_FILE, size) ||
        plat_file_write(&PROJ_FILE, 0, raw, PROJ_PREFIX) ||
        plat_file_write(&PROJ_FILE, PROJ_PREFIX, raw + PROJ_TAIL, PROJ_TAIL_BYTES))
        return -1;
    crc = crc32_more(0xFFFFFFFFu, raw, PROJ_PREFIX);
    crc = crc32_more(crc, raw + PROJ_TAIL, PROJ_TAIL_BYTES);
    for (i = 0; i < NPAT; i++) {
        n = codec_pack(&proj.p[i]);
        if (plat_file_write(&PROJ_FILE, off, codec_out, n))
            return -1;
        off += n;
        crc = crc32_more(crc, codec_out, n);
    }
    return size == off ? plat_file_commit(&PROJ_FILE, size, ~crc) : -1;
}
