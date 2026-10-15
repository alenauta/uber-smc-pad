/* BLE-MIDI packet encoder / decoder (Apple "MIDI over Bluetooth LE" spec), SMC-PAD project.
 *
 * Packet  = header (0x80 | ts[12:7]) then, per message, a timestamp byte (0x80 | ts[6:0])
 *           and the MIDI bytes. Running status is accepted on input, never produced.
 *           SysEx may span packets: continuation packets carry only the header before the
 *           data, and F7 is always preceded by a timestamp byte.
 * Service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3.
 * Pure C: host-tested by test/test_ble_midi.c.
 */
#ifndef BLE_MIDI_H
#define BLE_MIDI_H

#include "sinco_proto.h"        /* u8/u16/u32 for host tests */

#ifndef BLE_MIDI_PKT_MAX
#define BLE_MIDI_PKT_MAX        128     /* largest notification we build (<= ATT MTU - 3) */
#endif
#ifndef BLE_MIDI_SYSEX_MAX
#define BLE_MIDI_SYSEX_MAX      256     /* whole SysEx frames handed to msg_cb, incl. F0/F7 */
#endif

/* length of a non-SysEx message from its status byte; 0 = not a message start */
u32  ble_midi_msg_len(u8 status);

/* ---- encoder ------------------------------------------------------------------------- */
struct ble_midi_enc {
    u16 max;            /* packet size limit (MTU - 3, capped at BLE_MIDI_PKT_MAX) */
    u16 len;
    u16 last_ts;        /* 13-bit ts of the last message added to the current packet */
    u8  hdr_hi;         /* ts[12:7] of the current packet's header */
    u8  sx_cont;        /* a SysEx continues in the next packet */
    u8  buf[BLE_MIDI_PKT_MAX];
};
void ble_midi_enc_init(struct ble_midi_enc *e, u16 max);
void ble_midi_enc_set_max(struct ble_midi_enc *e, u16 max);
/* Appends msg: a complete message, a whole F0..F7 SysEx, or the rest of a SysEx after a
 * partial add. Returns the bytes of msg consumed: len = done, less = the packet is full,
 * take it and call again with msg + consumed. 0 also means "take the packet first". */
u32  ble_midi_enc_add(struct ble_midi_enc *e, u16 ts_ms, const u8 *msg, u32 len);
/* Moves the pending packet to out (BLE_MIDI_PKT_MAX bytes) and returns its size (0 = none). */
u32  ble_midi_enc_take(struct ble_midi_enc *e, u8 *out);
static inline u32 ble_midi_enc_pending(const struct ble_midi_enc *e)
{
    return e->len;
}

/* ---- decoder ------------------------------------------------------------------------- */
typedef void (*ble_midi_msg_cb)(void *ctx, const u8 *msg, u32 len);
typedef void (*ble_midi_sysex_byte_cb)(void *ctx, u8 b);
struct ble_midi_dec {
    ble_midi_msg_cb msg_cb;             /* complete messages (SysEx only if <= SYSEX_MAX) */
    ble_midi_sysex_byte_cb sysex_byte;  /* optional: every SysEx byte F0..F7, streamed */
    void *ctx;
    u8  running;
    u8  n, need;
    u8  msg[3];
    u8  in_sysex;
    u8  sx_overflow;
    u16 sx_len;
    u32 dropped;                        /* malformed packets / oversize SysEx */
    u8  sx[BLE_MIDI_SYSEX_MAX];
};
void ble_midi_dec_init(struct ble_midi_dec *d, ble_midi_msg_cb cb, ble_midi_sysex_byte_cb sx, void *ctx);
void ble_midi_dec_packet(struct ble_midi_dec *d, const u8 *pkt, u32 len);

#endif
