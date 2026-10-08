/* BLE-MIDI packet encoder / decoder, see ble_midi.h. */
#include <string.h>
#include "ble_midi.h"

u32 ble_midi_msg_len(u8 status)
{
    if (status < 0x80) {
        return 0;
    }
    if (status < 0xF0) {
        u8 hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
    switch (status) {
    case 0xF1:
    case 0xF3:
        return 2;
    case 0xF2:
        return 3;
    case 0xF6:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        return 1;
    default:            /* F0 / F7 (SysEx), F4 / F5 undefined */
        return 0;
    }
}

/* ---- encoder ------------------------------------------------------------------------- */
void ble_midi_enc_set_max(struct ble_midi_enc *e, u16 max)
{
    e->max = max < 5 ? 5 : (max > BLE_MIDI_PKT_MAX ? BLE_MIDI_PKT_MAX : max);
}

void ble_midi_enc_init(struct ble_midi_enc *e, u16 max)
{
    memset(e, 0, sizeof(*e));
    ble_midi_enc_set_max(e, max);
}

static void put_header(struct ble_midi_enc *e, u8 hi)
{
    e->buf[0] = (u8)(0x80 | hi);
    e->hdr_hi = hi;
    e->len = 1;
}

/* SysEx bytes from msg[0]; F7 gets its own timestamp byte. Returns bytes consumed. */
static u32 put_sysex(struct ble_midi_enc *e, u8 tsb, const u8 *msg, u32 len)
{
    u32 i = 0;
    while (i < len) {
        if (msg[i] == 0xF7) {
            if (e->len + 2 > e->max) {
                break;
            }
            e->buf[e->len++] = tsb;
            e->buf[e->len++] = 0xF7;
            e->sx_cont = 0;
            return i + 1;
        }
        if (e->len + 1 > e->max) {
            break;
        }
        e->buf[e->len++] = msg[i++];
    }
    e->sx_cont = 1;
    return i;
}

u32 ble_midi_enc_add(struct ble_midi_enc *e, u16 ts_ms, const u8 *msg, u32 len)
{
    if (!msg || !len) {
        return 0;
    }
    const u16 ts = ts_ms & 0x1FFF;
    const u8 hi = (u8)((ts >> 7) & 0x3F);
    const u8 tsb = (u8)(0x80 | (ts & 0x7F));

    if (e->sx_cont) {                           /* continuation: header, then raw data */
        if (e->len) {
            return 0;
        }
        put_header(e, hi);
        return put_sysex(e, tsb, msg, len);
    }
    if (e->len && hi != e->hdr_hi) {            /* keep one ts[12:7] per packet */
        return 0;
    }
    const u32 hdr = e->len ? 0 : 1;
    if (msg[0] == 0xF0) {
        if (e->len + hdr + 3 > e->max) {        /* ts F0 + at least one more byte */
            return 0;
        }
        if (hdr) {
            put_header(e, hi);
        }
        e->buf[e->len++] = tsb;
        return put_sysex(e, tsb, msg, len);
    }
    if (ble_midi_msg_len(msg[0]) != len || e->len + hdr + 1 + len > e->max) {
        return 0;
    }
    if (hdr) {
        put_header(e, hi);
    }
    e->buf[e->len++] = tsb;
    memcpy(e->buf + e->len, msg, len);
    e->len = (u16)(e->len + len);
    return len;
}

u32 ble_midi_enc_take(struct ble_midi_enc *e, u8 *out)
{
    u32 n = e->len;
    if (n) {
        memcpy(out, e->buf, n);
        e->len = 0;
    }
    return n;
}

/* ---- decoder ------------------------------------------------------------------------- */
void ble_midi_dec_init(struct ble_midi_dec *d, ble_midi_msg_cb cb, ble_midi_sysex_byte_cb sx, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->msg_cb = cb;
    d->sysex_byte = sx;
    d->ctx = ctx;
}

static void emit(struct ble_midi_dec *d, const u8 *m, u32 n)
{
    if (d->msg_cb) {
        d->msg_cb(d->ctx, m, n);
    }
}

static void sx_put(struct ble_midi_dec *d, u8 b)
{
    if (d->sysex_byte) {
        d->sysex_byte(d->ctx, b);
    }
    if (d->sx_len < BLE_MIDI_SYSEX_MAX) {
        d->sx[d->sx_len++] = b;
    } else {
        d->sx_overflow = 1;
    }
}

static void on_status(struct ble_midi_dec *d, u8 b)
{
    if (b >= 0xF8) {                            /* real-time: may interleave anything */
        emit(d, &b, 1);
        return;
    }
    if (b == 0xF7) {
        if (d->in_sysex) {
            sx_put(d, b);
            if (d->sx_overflow) {
                d->dropped++;
            } else {
                emit(d, d->sx, d->sx_len);
            }
            d->in_sysex = 0;
        }
        return;
    }
    if (d->in_sysex) {                          /* SysEx aborted by another status */
        d->in_sysex = 0;
        d->dropped++;
    }
    d->n = 0;
    if (b == 0xF0) {
        d->in_sysex = 1;
        d->sx_len = 0;
        d->sx_overflow = 0;
        d->running = 0;
        sx_put(d, b);
        return;
    }
    u32 need = ble_midi_msg_len(b);
    d->running = (b < 0xF0) ? b : 0;
    if (!need) {
        return;
    }
    d->msg[0] = b;
    d->n = 1;
    d->need = (u8)need;
    if (need == 1) {                            /* F6 tune request */
        emit(d, d->msg, 1);
        d->n = 0;
    }
}

static void on_data(struct ble_midi_dec *d, u8 b)
{
    if (d->in_sysex) {
        sx_put(d, b);
        return;
    }
    if (d->n == 0) {
        if (!d->running) {
            return;                             /* stray data byte */
        }
        d->msg[0] = d->running;
        d->n = 1;
        d->need = (u8)ble_midi_msg_len(d->running);
    }
    d->msg[d->n++] = b;
    if (d->n == d->need) {
        emit(d, d->msg, d->n);
        d->n = 0;
    }
}

void ble_midi_dec_packet(struct ble_midi_dec *d, const u8 *pkt, u32 len)
{
    if (len < 2 || !(pkt[0] & 0x80)) {
        d->dropped++;
        return;
    }
    /* After the header and after any status or data byte, a byte with bit 7 set is a
     * timestamp; right after a timestamp it is a status. */
    u8 expect_ts = 1;
    d->n = 0;                                   /* messages never span packets (SysEx does) */
    for (u32 i = 1; i < len; i++) {
        u8 b = pkt[i];
        if (b & 0x80) {
            if (expect_ts) {
                expect_ts = 0;
                continue;
            }
            on_status(d, b);
        } else {
            on_data(d, b);
        }
        expect_ts = 1;
    }
    d->n = 0;
}
