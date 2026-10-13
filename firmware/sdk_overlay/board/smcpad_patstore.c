#include "smcpad_patstore.h"
#include "smcpad_scale.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

/* the raw track / step copies rely on this layout (u8 fields only: no padding) */
typedef char patstore_track_hdr_ok[(__builtin_offsetof(struct seq_track, step) == PATSTORE_TRACK_HDR) ? 1 : -1];
typedef char patstore_step_ok[(sizeof(struct seq_step) == 9) ? 1 : -1];

#define TRACK_FIXED     (PATSTORE_TRACK_HDR + 8)        /* track bytes + step bitmap */
#define STEP_SIZE       ((u32)sizeof(struct seq_step))

/* CRC-32 (IEEE, reflected 0xEDB88320), 4 bits at a time */
static const u32 crc_nib[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

u32 patstore_crc32(u32 crc, const u8 *p, u32 n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nib[crc & 15];
        crc = (crc >> 4) ^ crc_nib[crc & 15];
    }
    return ~crc;
}

static void put32le(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 get32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u8 popcount8(u8 v)
{
    u8 c = 0;
    for (; v; v &= (u8)(v - 1)) {
        c++;
    }
    return c;
}

/* walks the payload pl[0..len); dst = NULL: structure check only */
static int walk(const u8 *pl, u32 len, struct seq_pattern *dst)
{
    u32 off = 0;
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        if (off + TRACK_FIXED > len) {
            return PATSTORE_E_FORMAT;
        }
        const u8 *fixed = pl + off;
        off += TRACK_FIXED;
        u32 n = 0;
        for (u8 i = 0; i < 8; i++) {
            n += popcount8(fixed[PATSTORE_TRACK_HDR + i]);
        }
        if (off + n * STEP_SIZE > len) {
            return PATSTORE_E_FORMAT;
        }
        if (dst) {
            struct seq_track *tr = &dst->track[t];
            memcpy(tr, fixed, PATSTORE_TRACK_HDR);
            for (u8 i = 0; i < SEQ_STEPS; i++) {
                if (fixed[PATSTORE_TRACK_HDR + i / 8] & (1u << (i % 8))) {
                    memcpy(&tr->step[i], pl + off, STEP_SIZE);
                    off += STEP_SIZE;
                }
            }
        } else {
            off += n * STEP_SIZE;
        }
    }
    return off == len ? PATSTORE_OK : PATSTORE_E_FORMAT;
}

/* item in buf[0..n): PATSTORE_OK if it is a valid pattern; *crc = its CRC */
static int check_item(const u8 *buf, int n, u32 *crc)
{
    if (n <= 0) {
        return PATSTORE_E_EMPTY;
    }
    if (n < PATSTORE_HDR || get32le(buf) != PATSTORE_MAGIC) {
        return PATSTORE_E_EMPTY;
    }
    u32 len = (u32)buf[6] | ((u32)buf[7] << 8);
    if (buf[4] != PATSTORE_VER || buf[5] != SEQ_PATTERN_VER || len < SEQ_TRACKS * TRACK_FIXED ||
        len + PATSTORE_HDR != (u32)n) {
        return PATSTORE_E_FORMAT;
    }
    u32 c = patstore_crc32(0, buf + PATSTORE_HDR, len);
    if (c != get32le(buf + 8)) {
        return PATSTORE_E_CRC;
    }
    if (crc) {
        *crc = c;
    }
    return walk(buf + PATSTORE_HDR, len, NULL);
}

static int rd(struct patstore *ps, u8 slot, u8 *buf)
{
    if (!ps->port || !ps->port->read) {
        return PATSTORE_E_IO;
    }
    int n = ps->port->read(ps->port->ctx, (u16)(PATSTORE_ID0 + slot), buf, PATSTORE_MAX);
    return n > PATSTORE_MAX ? PATSTORE_E_FORMAT : n;
}

int patstore_check(struct patstore *ps, u8 slot, u8 *buf)
{
    if (slot >= PATSTORE_SLOTS) {
        return PATSTORE_E_SLOT;
    }
    return check_item(buf, rd(ps, slot, buf), NULL);
}

static void sanitize(struct seq_track *t)          /* a valid item from another layout version */
{
    t->chan &= 0x0f;
    t->note &= 0x7f;
    if (!t->len || t->len > SEQ_STEPS) t->len = t->len ? SEQ_STEPS : 1;
    if (t->rate >= SEQ_RATES) t->rate = SEQ_RATE_16;
    if (t->res >= SEQ_RATES) t->res = SEQ_RATE_16;
    if (t->gate >= SEQ_GATES) t->gate = 3;
    if (t->vol > 127) t->vol = 127;
    if (t->prob > 100) t->prob = 100;
    if (t->quantize > 100) t->quantize = 100;
    if (t->ratchet_rate >= SEQ_RATCHET_RATES) t->ratchet_rate = 0;
    t->mute &= 1;
    if (t->root > 12) t->root = 0;
    if (t->scale > SCALE_COUNT) t->scale = 0;
}

int patstore_load(struct patstore *ps, u8 slot, struct seq_pattern *dst, u8 *buf)
{
    int r = patstore_check(ps, slot, buf);
    if (r) {
        return ps->err = (s8)r;
    }
    u32 len = (u32)buf[6] | ((u32)buf[7] << 8);
    memset(dst, 0, sizeof(*dst));
    dst->magic = SEQ_PATTERN_MAGIC;
    dst->ver = SEQ_PATTERN_VER;
    walk(buf + PATSTORE_HDR, len, dst);              /* checked above */
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        sanitize(&dst->track[t]);
    }
    ps->err = PATSTORE_OK;
    return PATSTORE_OK;
}

void patstore_init(struct patstore *ps, const struct patstore_port *port, u8 *buf)
{
    memset(ps, 0, sizeof(*ps));
    ps->port = port;
    for (u8 s = 0; s < PATSTORE_SLOTS; s++) {
        if (patstore_check(ps, s, buf) == PATSTORE_OK) {
            ps->used |= (u16)(1u << s);
            u32 c = get32le(buf + 12);
            if (c > ps->count) {
                ps->count = c;
            }
        }
    }
}

u32 patstore_track_size(const struct seq_track *t)
{
    u32 n = 0;
    for (u8 i = 0; i < SEQ_STEPS; i++) {
        n += t->step[i].vel ? 1 : 0;
    }
    return TRACK_FIXED + n * STEP_SIZE;
}

u32 patstore_pattern_size(const struct seq_pattern *p)
{
    u32 n = PATSTORE_HDR;
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        n += patstore_track_size(&p->track[t]);
    }
    return n;
}

u32 patstore_encode_track(const struct seq_track *t, u8 *out)
{
    memcpy(out, t, PATSTORE_TRACK_HDR);
    memset(out + PATSTORE_TRACK_HDR, 0, 8);
    u32 o = TRACK_FIXED;
    for (u8 i = 0; i < SEQ_STEPS; i++) {
        if (t->step[i].vel) {
            out[PATSTORE_TRACK_HDR + i / 8] |= (u8)(1u << (i % 8));
            memcpy(out + o, &t->step[i], STEP_SIZE);
            o += STEP_SIZE;
        }
    }
    return o;
}

int patstore_save(struct patstore *ps, u8 slot, const struct seq_pattern *src, u8 *buf)
{
    if (slot >= PATSTORE_SLOTS) {
        return ps->err = PATSTORE_E_SLOT;
    }
    if (!ps->port || !ps->port->write) {
        return ps->err = PATSTORE_E_IO;
    }
    u32 n = patstore_pattern_size(src);
    if (n > PATSTORE_MAX) {
        return ps->err = PATSTORE_E_FULL;           /* nothing written: the slot keeps its pattern */
    }
    u32 off = PATSTORE_HDR;
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        off += patstore_encode_track(&src->track[t], buf + off);
    }
    u32 len = off - PATSTORE_HDR, crc = patstore_crc32(0, buf + PATSTORE_HDR, len);
    put32le(buf, PATSTORE_MAGIC);
    buf[4] = PATSTORE_VER;
    buf[5] = SEQ_PATTERN_VER;
    buf[6] = (u8)len;
    buf[7] = (u8)(len >> 8);
    put32le(buf + 8, crc);
    put32le(buf + 12, ps->count + 1);
    int w = ps->port->write(ps->port->ctx, (u16)(PATSTORE_ID0 + slot), buf, off);
    if (w <= 0) {
        return ps->err = PATSTORE_E_IO;             /* the old item (if any) is still there */
    }
    u32 back = 0;
    if (check_item(buf, rd(ps, slot, buf), &back) || back != crc) {
        ps->used &= (u16)~(1u << slot);
        return ps->err = PATSTORE_E_VERIFY;
    }
    ps->count++;
    ps->used |= (u16)(1u << slot);
    return ps->err = PATSTORE_OK;
}
