/* Sinco configuration protocol, see sinco_proto.h. */
#include <string.h>
#include "sinco_proto.h"

/* ---- packets --------------------------------------------------------------------- */
u8 sinco_checksum(const u8 *payload, u32 len)
{
    u32 s = 0;
    for (u32 i = 0; i < len; i++) {
        s += payload[i];
    }
    return (u8)~s;
}

u32 sinco_build(u8 cmd, const u8 *payload, u32 len, u8 *out)
{
    if (len && payload != out + SINCO_HDR_LEN) {
        memmove(out + SINCO_HDR_LEN, payload, len);
    }
    out[0] = 0x00;
    out[1] = 0x59;
    out[2] = cmd;
    out[3] = (u8)len;
    out[4] = (u8)(len >> 8);
    out[5] = (u8)(len >> 16);
    out[SINCO_HDR_LEN + len] = sinco_checksum(out + SINCO_HDR_LEN, len);
    return SINCO_HDR_LEN + len + 1;
}

u32 sinco_build_error(u8 code, u8 *out)
{
    return sinco_build(SINCO_CMD_ERROR, &code, 1, out);
}

int sinco_verify(const u8 *pkt, u32 n, u8 *cmd, const u8 **payload, u32 *len)
{
    if (n < SINCO_HDR_LEN + 1 || pkt[0] != 0x00 || pkt[1] != 0x59) {
        return 0;
    }
    u32 l = sinco_get_le(pkt + 3, 3);
    if (n != SINCO_HDR_LEN + l + 1) {
        return 0;
    }
    if (sinco_checksum(pkt + SINCO_HDR_LEN, l) != pkt[SINCO_HDR_LEN + l]) {
        return 0;
    }
    *cmd = pkt[2];
    *payload = pkt + SINCO_HDR_LEN;
    *len = l;
    return 1;
}

/* ---- reassembler ---------------------------------------------------------------------- */
void sinco_rx_reset(struct sinco_rx *rx)
{
    rx->n = 0;
    rx->need = 0;
}

void sinco_rx_chunk_start(struct sinco_rx *rx, u32 now_ms)
{
    if (rx->n && (rx->need == 0 || rx->n < rx->need) && now_ms - rx->last_ms > SINCO_RX_TIMEOUT_MS) {
        rx->dropped++;
        sinco_rx_reset(rx);
    }
    rx->last_ms = now_ms;
}

u32 sinco_rx_byte(struct sinco_rx *rx, u8 b)
{
    if (rx->need && rx->n >= rx->need) {        /* previous packet was handed out */
        sinco_rx_reset(rx);
    }
    switch (rx->n) {
    case 0:
        if (b != 0x00) {
            return 0;
        }
        break;
    case 1:
        if (b != 0x59) {
            rx->n = (b == 0x00) ? 1 : 0;        /* "00 00 59" still syncs */
            return 0;
        }
        break;
    default:
        break;
    }
    rx->buf[rx->n++] = b;
    if (rx->n == SINCO_HDR_LEN) {
        u32 l = sinco_get_le(rx->buf + 3, 3);
        if (l > SINCO_MAX_PAYLOAD) {
            rx->dropped++;
            sinco_rx_reset(rx);
            return 0;
        }
        rx->need = SINCO_HDR_LEN + l + 1;
    }
    if (rx->need && rx->n == rx->need) {
        return rx->need;
    }
    return 0;
}

/* ---- USB 7-bit framing (Midi Suite SysexCodec.toMidi / fromMidi) ----------------------- */
u32 sinco_sysex_encode(const u8 *pkt, u32 n, u8 *out, u32 out_max)
{
    if (out_max < SINCO_SYSEX_LEN(n)) {
        return 0;
    }
    u32 o = 0, acc = 0, nbits = 0;
    out[o++] = 0xF0;
    for (u32 i = 0; i < n; i++) {
        acc |= (u32)pkt[i] << nbits;
        nbits += 8;
        while (nbits >= 7) {
            out[o++] = (u8)(acc & 0x7F);
            acc >>= 7;
            nbits -= 7;
        }
    }
    if (nbits) {
        out[o++] = (u8)(acc & 0x7F);
    }
    out[o++] = 0xF7;
    return o;
}

void sinco_sx_reset(struct sinco_sx *sx)
{
    sx->acc = 0;
    sx->nbits = 0;
    sx->active = 0;
    sinco_rx_reset(&sx->rx);
}

u32 sinco_sx_byte(struct sinco_sx *sx, u8 b)
{
    if (b == 0xF0) {
        sinco_sx_reset(sx);
        sx->active = 1;
        return 0;
    }
    if (!sx->active) {
        return 0;
    }
    if (b == 0xF7) {                            /* frame ends; a partial packet is dropped */
        sx->active = 0;
        if (sx->rx.n && !(sx->rx.need && sx->rx.n == sx->rx.need)) {
            sx->rx.dropped++;
            sinco_rx_reset(&sx->rx);
        }
        return 0;
    }
    if (b & 0x80) {                             /* real-time bytes may interleave: ignore */
        return 0;
    }
    sx->acc |= (u32)b << sx->nbits;
    sx->nbits += 7;
    if (sx->nbits >= 8) {
        u8 v = (u8)sx->acc;
        sx->acc >>= 8;
        sx->nbits -= 8;
        return sinco_rx_byte(&sx->rx, v);
    }
    return 0;
}

/* ---- dispatcher ------------------------------------------------------------------------ */
u32 sinco_handle(const u8 *pkt, u32 n, u8 *reply, const struct sinco_ops *ops)
{
    u8 cmd;
    const u8 *p;
    u32 len;

    if (!sinco_verify(pkt, n, &cmd, &p, &len)) {
        return sinco_build_error(SINCO_ERR_GENERIC, reply);
    }
    switch (cmd) {
    case SINCO_CMD_NAME_VER: {
        u8 *o = reply + SINCO_HDR_LEN;
        u32 i = 0;
        memset(o, 0, SINCO_NAME_LEN);
        if (ops->name_ver) {
            for (; i < SINCO_NAME_LEN && ops->name_ver[i]; i++) {
                o[i] = (u8)ops->name_ver[i];
            }
        }
        return sinco_build(SINCO_CMD_NAME_VER, o, SINCO_NAME_LEN, reply);
    }
    case SINCO_CMD_STATUS: {
        u8 *o = reply + SINCO_HDR_LEN;
        memset(o, 0, SINCO_STATUS_LEN);
        if (ops->status) {
            ops->status(ops->ctx, o);
        }
        return sinco_build(SINCO_CMD_STATUS, o, SINCO_STATUS_LEN, reply);
    }
    case SINCO_CMD_MIDI:
        if (ops->midi_in && len) {
            ops->midi_in(ops->ctx, p, len);
        }
        return sinco_build_error(0, reply);     /* stock acks 0x15 too (0x1E11666 -> 0x1E1166E) */
    case SINCO_CMD_READ: {
        if (len != SINCO_RW_HDR_LEN || !ops->read) {
            break;
        }
        u8 type = p[0];
        u32 addr = sinco_get_le(p + 1, 4);
        u32 dlen = sinco_get_le(p + 5, 3);
        if (dlen > SINCO_MAX_DATA) {
            break;
        }
        u8 *o = reply + SINCO_HDR_LEN;
        memmove(o, p, SINCO_RW_HDR_LEN);        /* pkt may alias reply */
        if (dlen && ops->read(ops->ctx, type, addr, o + SINCO_RW_HDR_LEN, dlen) < 0) {
            break;
        }
        return sinco_build(SINCO_CMD_READ, o, SINCO_RW_HDR_LEN + dlen, reply);
    }
    case SINCO_CMD_WRITE: {
        if (len < SINCO_RW_HDR_LEN || !ops->write) {
            break;
        }
        u8 type = p[0];
        u32 addr = sinco_get_le(p + 1, 4);
        u32 dlen = sinco_get_le(p + 5, 3);
        if (dlen != len - SINCO_RW_HDR_LEN) {
            break;
        }
        if (ops->write(ops->ctx, type, addr, p + SINCO_RW_HDR_LEN, dlen) < 0) {
            break;
        }
        /* Stock SMC-PAD_005 write acknowledgement: 00 59 00 01 00 00 00 FF = cmd 0, status 0.
         * Cube Suite (iOS) rejects an 0x22 echo ("failed to send data"); Midi Suite only
         * checks payload[0] == 0. */
        return sinco_build_error(0, reply);
    }
    default:                                    /* 0x21 erase, 0x24 aux, unknown */
        break;
    }
    return sinco_build_error(SINCO_ERR_GENERIC, reply);
}
