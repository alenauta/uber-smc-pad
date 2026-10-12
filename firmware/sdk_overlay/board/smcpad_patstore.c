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
#define PAYLOAD_MAX     (PATSTORE_SLOT_SIZE - PATSTORE_HDR)
#define STAGE_HEADER    (2 + SEQ_TRACKS)

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

struct hdr {
    u32 magic;
    u8  ver, pver;
    u16 len;
    u32 crc, count;
};

static void put32le(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 get32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 slot_addr(u8 slot)
{
    return PATSTORE_BASE + (u32)slot * PATSTORE_SLOT_SIZE;
}

static int rd(struct patstore *ps, u32 addr, u8 *dst, u32 len)
{
    if (!ps->port || !ps->port->read || addr < PATSTORE_BASE || addr + len > PATSTORE_END) {
        return PATSTORE_E_IO;
    }
    return ps->port->read(ps->port->ctx, addr, dst, len) ? PATSTORE_E_IO : PATSTORE_OK;
}

static int read_hdr(struct patstore *ps, u8 slot, struct hdr *h)
{
    u8 b[PATSTORE_HDR];
    if (rd(ps, slot_addr(slot), b, sizeof(b))) {
        return PATSTORE_E_IO;
    }
    h->magic = get32le(b);
    h->ver = b[4];
    h->pver = b[5];
    h->len = (u16)(b[6] | (b[7] << 8));
    h->crc = get32le(b + 8);
    h->count = get32le(b + 12);
    if (h->magic != PATSTORE_MAGIC) {
        return PATSTORE_E_EMPTY;
    }
    if (h->ver != PATSTORE_VER || h->pver != SEQ_PATTERN_VER || h->len < SEQ_TRACKS * TRACK_FIXED ||
        h->len > PAYLOAD_MAX) {
        return PATSTORE_E_FORMAT;
    }
    return PATSTORE_OK;
}

static u8 popcount8(u8 v)
{
    u8 c = 0;
    for (; v; v &= (u8)(v - 1)) {
        c++;
    }
    return c;
}

/* walks the payload; dst = NULL: structure check only */
static int walk(struct patstore *ps, u8 slot, const struct hdr *h, struct seq_pattern *dst)
{
    u32 base = slot_addr(slot) + PATSTORE_HDR, off = 0;
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        u8 fixed[TRACK_FIXED];
        if (off + TRACK_FIXED > h->len || rd(ps, base + off, fixed, TRACK_FIXED)) {
            return PATSTORE_E_FORMAT;
        }
        off += TRACK_FIXED;
        u32 n = 0;
        for (u8 i = 0; i < 8; i++) {
            n += popcount8(fixed[PATSTORE_TRACK_HDR + i]);
        }
        if (off + n * sizeof(struct seq_step) > h->len) {
            return PATSTORE_E_FORMAT;
        }
        if (dst) {
            struct seq_track *tr = &dst->track[t];
            memcpy(tr, fixed, PATSTORE_TRACK_HDR);
            for (u8 i = 0; i < SEQ_STEPS; i++) {
                if (fixed[PATSTORE_TRACK_HDR + i / 8] & (1u << (i % 8))) {
                    if (rd(ps, base + off, (u8 *)&tr->step[i], sizeof(struct seq_step))) {
                        return PATSTORE_E_IO;
                    }
                    off += sizeof(struct seq_step);
                }
            }
        } else {
            off += n * sizeof(struct seq_step);
        }
    }
    return off == h->len ? PATSTORE_OK : PATSTORE_E_FORMAT;
}

static int check(struct patstore *ps, u8 slot, struct hdr *h)
{
    if (slot >= PATSTORE_SLOTS) {
        return PATSTORE_E_SLOT;
    }
    int r = read_hdr(ps, slot, h);
    if (r) {
        return r;
    }
    u8 b[64];
    u32 crc = 0, base = slot_addr(slot) + PATSTORE_HDR;
    for (u32 off = 0; off < h->len; off += sizeof(b)) {
        u32 n = h->len - off < sizeof(b) ? h->len - off : sizeof(b);
        if (rd(ps, base + off, b, n)) {
            return PATSTORE_E_IO;
        }
        crc = patstore_crc32(crc, b, n);
    }
    if (crc != h->crc) {
        return PATSTORE_E_CRC;
    }
    return walk(ps, slot, h, NULL);
}

int patstore_check(struct patstore *ps, u8 slot)
{
    struct hdr h;
    return check(ps, slot, &h);
}

static void sanitize(struct seq_track *t)          /* a valid slot from a future/older layout */
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

int patstore_load(struct patstore *ps, u8 slot, struct seq_pattern *dst)
{
    struct hdr h;
    if (ps->busy && ps->slot == slot) {
        return PATSTORE_E_BUSY;
    }
    int r = check(ps, slot, &h);
    if (r) {
        return r;
    }
    memset(dst, 0, sizeof(*dst));
    dst->magic = SEQ_PATTERN_MAGIC;
    dst->ver = SEQ_PATTERN_VER;
    r = walk(ps, slot, &h, dst);                    /* checked above: only a read error left */
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        sanitize(&dst->track[t]);
    }
    return r;
}

void patstore_init(struct patstore *ps, const struct patstore_port *port)
{
    memset(ps, 0, sizeof(*ps));
    ps->port = port;
    for (u8 s = 0; s < PATSTORE_SLOTS; s++) {
        struct hdr h;
        if (check(ps, s, &h) == PATSTORE_OK) {
            ps->used |= (u16)(1u << s);
            if (h.count > ps->count) {
                ps->count = h.count;
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
    return TRACK_FIXED + n * sizeof(struct seq_step);
}

u32 patstore_pattern_size(const struct seq_pattern *p)
{
    u32 n = 0;
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
            memcpy(out + o, &t->step[i], sizeof(struct seq_step));
            o += sizeof(struct seq_step);
        }
    }
    return o;
}

int patstore_save_begin(struct patstore *ps, u8 slot, const struct seq_pattern *src)
{
    if (ps->busy) {
        return PATSTORE_E_BUSY;
    }
    if (slot >= PATSTORE_SLOTS) {
        return ps->err = PATSTORE_E_SLOT;
    }
    if (!ps->port || !ps->port->erase || !ps->port->write) {
        return ps->err = PATSTORE_E_IO;
    }
    if (patstore_pattern_size(src) > PAYLOAD_MAX) {
        return ps->err = PATSTORE_E_FULL;           /* nothing erased: the slot keeps its pattern */
    }
    ps->busy = 1;
    ps->slot = slot;
    ps->stage = 0;
    ps->off = 0;
    ps->crc = 0;
    ps->src = src;
    return PATSTORE_OK;
}

static int abort_save(struct patstore *ps, int e)
{
    ps->busy = 0;
    ps->used &= (u16)~(1u << ps->slot);
    ps->err = (s8)e;
    return e;
}

int patstore_save_step(struct patstore *ps)
{
    if (!ps->busy) {
        return ps->err;
    }
    u32 addr = slot_addr(ps->slot);
    if (ps->stage < 2) {                            /* the two sectors */
        ps->used &= (u16)~(1u << ps->slot);
        if (ps->port->erase(ps->port->ctx, addr + (u32)ps->stage * PATSTORE_SECTOR)) {
            return abort_save(ps, PATSTORE_E_IO);
        }
        ps->stage++;
        return 1;
    }
    if (ps->stage < STAGE_HEADER) {                 /* one track */
        u32 n = patstore_encode_track(&ps->src->track[ps->stage - 2], ps->buf);
        if (ps->off + n > PAYLOAD_MAX) {
            return abort_save(ps, PATSTORE_E_FULL); /* grew while saving */
        }
        if (ps->port->write(ps->port->ctx, addr + PATSTORE_HDR + ps->off, ps->buf, n)) {
            return abort_save(ps, PATSTORE_E_IO);
        }
        ps->crc = patstore_crc32(ps->crc, ps->buf, n);
        ps->off = (u16)(ps->off + n);
        ps->stage++;
        return 1;
    }
    u8 b[PATSTORE_HDR];                             /* header last: the slot becomes valid */
    put32le(b, PATSTORE_MAGIC);
    b[4] = PATSTORE_VER;
    b[5] = SEQ_PATTERN_VER;
    b[6] = (u8)ps->off;
    b[7] = (u8)(ps->off >> 8);
    put32le(b + 8, ps->crc);
    put32le(b + 12, ps->count + 1);
    if (ps->port->write(ps->port->ctx, addr, b, sizeof(b))) {
        return abort_save(ps, PATSTORE_E_IO);
    }
    struct hdr h;
    if (check(ps, ps->slot, &h) || h.crc != ps->crc) {
        return abort_save(ps, PATSTORE_E_VERIFY);   /* read back differs: flash protected? */
    }
    ps->count++;
    ps->used |= (u16)(1u << ps->slot);
    ps->busy = 0;
    ps->err = PATSTORE_OK;
    return 0;
}
