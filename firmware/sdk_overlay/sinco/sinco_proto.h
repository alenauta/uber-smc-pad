/* Sinco configuration protocol (M-VAVE / Sinco "00 59" packets), SMC-PAD project.
 *
 * Packet:   00 59 | cmd | len u24 LE | payload[len] | ~sum(payload) & 0xFF
 * BLE:      written to AE41 in MTU-sized chunks, replies notified on AE42 (also chunked).
 * USB-MIDI: F0 | packet as an LSB-first 7-bit bit stream | F7  (no manufacturer ID; frames
 *           start "F0 00 32" only because packets start "00 59").
 * Specification: docs/midisuite_protocol.md.
 * Pure C, no SDK calls: host-tested by test/test_sinco_proto.c.
 */
#ifndef SINCO_PROTO_H
#define SINCO_PROTO_H

#ifdef SMCPAD_HOST_TEST
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
#else
#include "typedef.h"
#endif

#define SINCO_CMD_ERROR         0x00    /* reply only, status: 00 59 00 01 00 00 <code> <~code>;
                                         * code 0 = ack (write, 0x15), 1 = error (stock 0x1E16532) */
#define SINCO_CMD_NAME_VER      0x11
#define SINCO_CMD_STATUS        0x12
#define SINCO_CMD_MIDI          0x15    /* payload = raw MIDI bytes injected as "host MIDI in" */
#define SINCO_CMD_ERASE         0x21    /* refused */
#define SINCO_CMD_WRITE         0x22
#define SINCO_CMD_READ          0x23
#define SINCO_CMD_AUX           0x24    /* refused (stock: u16, CRC?) */

#define SINCO_ERR_GENERIC       0x01    /* the only code seen from stock firmware */

#define SINCO_HDR_LEN           6       /* 00 59 cmd len3 */
#define SINCO_RW_HDR_LEN        8       /* type u8, addr u32 LE, len u24 LE */
#define SINCO_NAME_LEN          27      /* 0x11 reply payload, "SMC-PAD_005" + zero padding */
#define SINCO_STATUS_LEN        13      /* 0x12 reply payload (layout not decoded) */
#ifndef SINCO_MAX_DATA
#define SINCO_MAX_DATA          1024    /* Midi Suite reads up to 1009 B per request */
#endif
#define SINCO_MAX_PAYLOAD       (SINCO_RW_HDR_LEN + SINCO_MAX_DATA)
#define SINCO_MAX_PKT           (SINCO_HDR_LEN + SINCO_MAX_PAYLOAD + 1)
/* 7-bit SysEx size of an n-byte packet, including F0/F7 */
#define SINCO_SYSEX_LEN(n)      (2 + ((n) * 8 + 6) / 7)
#define SINCO_RX_TIMEOUT_MS     500     /* a partial BLE packet older than this is dropped */

/* ---- packets --------------------------------------------------------------------- */
u8   sinco_checksum(const u8 *payload, u32 len);
/* Builds a packet into out (needs len + 7 bytes; payload may alias out + 6). Returns its size. */
u32  sinco_build(u8 cmd, const u8 *payload, u32 len, u8 *out);
/* 1 if pkt[0..n) holds exactly one valid packet; fills cmd / payload / len. */
int  sinco_verify(const u8 *pkt, u32 n, u8 *cmd, const u8 **payload, u32 *len);
u32  sinco_build_error(u8 code, u8 *out);
static inline u32 sinco_get_le(const u8 *p, u32 nbytes)
{
    u32 v = 0;
    while (nbytes--) {
        v = (v << 8) | p[nbytes];
    }
    return v;
}

/* ---- reassembler (bytes -> packets) -------------------------------------------------
 * Resyncs on "00 59", drops a partial packet after SINCO_RX_TIMEOUT_MS of silence and any
 * packet whose length field exceeds SINCO_MAX_PAYLOAD. */
struct sinco_rx {
    u32 n;              /* bytes collected */
    u32 need;           /* total packet size once the header is in, else 0 */
    u32 last_ms;
    u32 dropped;        /* oversize / resync / timeout counter (diagnostics) */
    u8  buf[SINCO_MAX_PKT];
};
void sinco_rx_reset(struct sinco_rx *rx);
/* Feeds one byte. Returns the packet size when buf holds a complete packet (it is consumed
 * by the next call), 0 otherwise. */
u32  sinco_rx_byte(struct sinco_rx *rx, u8 b);
/* Call at the start of every transport chunk (BLE write) with the current time. */
void sinco_rx_chunk_start(struct sinco_rx *rx, u32 now_ms);

/* ---- USB 7-bit framing ---------------------------------------------------------------- */
/* Encodes pkt into F0 .. F7. Returns the frame size, 0 if out_max is too small. */
u32  sinco_sysex_encode(const u8 *pkt, u32 n, u8 *out, u32 out_max);
/* Streaming decoder: F0 resets, F7 ends; decoded bytes go through the reassembler, so a
 * frame never has to be buffered whole. Non-Sinco SysEx is simply rejected by the resync. */
struct sinco_sx {
    u32 acc;
    u8  nbits;
    u8  active;         /* inside F0 .. F7 */
    struct sinco_rx rx;
};
void sinco_sx_reset(struct sinco_sx *sx);
/* Returns the packet size when sx->rx.buf holds a complete packet, else 0. */
u32  sinco_sx_byte(struct sinco_sx *sx, u8 b);

/* ---- dispatcher ------------------------------------------------------------------------ */
struct sinco_ops {
    void *ctx;
    const char *name_ver;                                   /* e.g. "SMC-PAD_901" */
    /* 0 = ok, <0 = refused. len 0 on write = commit. */
    int  (*read)(void *ctx, u8 type, u32 addr, u8 *dst, u32 len);
    int  (*write)(void *ctx, u8 type, u32 addr, const u8 *src, u32 len);
    void (*midi_in)(void *ctx, const u8 *bytes, u32 len);   /* optional (0x15) */
    void (*status)(void *ctx, u8 out[SINCO_STATUS_LEN]);    /* optional (0x12) */
};
/* Handles one complete packet (as returned by the reassembler). Writes the reply packet to
 * reply (SINCO_MAX_PKT bytes) and returns its size, or 0 when no reply is due (0x15). */
u32  sinco_handle(const u8 *pkt, u32 n, u8 *reply, const struct sinco_ops *ops);

#endif
