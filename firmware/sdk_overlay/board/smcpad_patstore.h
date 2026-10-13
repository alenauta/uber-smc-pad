/* Sequencer pattern slots (pure C, host-tested by test/test_smcpad_patstore.c).
 *
 * 919+: one settings item (VM, syscfg ids PATSTORE_ID0 .. +15, the SDK's "app extension" range)
 * per slot. 918 used the raw flash gap 0x30000-0x4FFFF, but the SDK write-protects the flash
 * from 0 up to the VM at boot, so nothing could be written there.
 *
 * Item = header (16 B) + payload. Payload, per track: the 13 track bytes (chan .. scale), an
 * 8-byte bitmap of the steps that play (vel != 0), then those steps (9 B each). An item holds
 * at most PATSTORE_MAX bytes (vm_write's limit), i.e. 415 playing steps of the 1024; a bigger
 * pattern is refused before anything is written, so the slot keeps its pattern.
 *
 * A save is one item write, read back and checked. Loading checks the CRC and the structure
 * before touching the pattern. The caller lends a PATSTORE_MAX scratch buffer.
 */
#ifndef SMCPAD_PATSTORE_H
#define SMCPAD_PATSTORE_H

#include "smcpad_seq.h"

#define PATSTORE_SLOTS      16
#define PATSTORE_ID0        128                 /* VM ids 128..143 */
#define PATSTORE_MAX        4095                /* bytes per item (vm_write refuses more) */
#define PATSTORE_HDR        16
#define PATSTORE_MAGIC      0x32545053u         /* "SPT2" little-endian */
#define PATSTORE_VER        2
#define PATSTORE_TRACK_HDR  13                  /* struct seq_track up to step[] */

enum { PATSTORE_OK = 0, PATSTORE_E_IO = -1, PATSTORE_E_EMPTY = -2, PATSTORE_E_CRC = -3,
       PATSTORE_E_FORMAT = -4, PATSTORE_E_FULL = -5, PATSTORE_E_SLOT = -6, PATSTORE_E_BUSY = -7,
       PATSTORE_E_VERIFY = -8, PATSTORE_E_MEM = -9 };

struct patstore_port {
    void *ctx;
    int (*read)(void *ctx, u16 id, u8 *dst, u32 cap);           /* bytes read, <= 0: none */
    int (*write)(void *ctx, u16 id, const u8 *src, u32 len);    /* bytes written, <= 0: failed */
};

struct patstore {
    const struct patstore_port *port;
    u16 used;                                   /* bit n: slot n holds a valid pattern */
    u32 count;                                  /* highest save counter seen */
    s8  err;                                    /* last error (PATSTORE_E_*), 0 = none */
};

u32  patstore_crc32(u32 crc, const u8 *p, u32 n);   /* start with 0 */
u32  patstore_track_size(const struct seq_track *t);
u32  patstore_pattern_size(const struct seq_pattern *p);   /* item bytes (header included) */
u32  patstore_encode_track(const struct seq_track *t, u8 *out); /* returns bytes written */

/* buf: PATSTORE_MAX bytes of scratch, only used during the call */
void patstore_init(struct patstore *ps, const struct patstore_port *port, u8 *buf);  /* scans */
int  patstore_check(struct patstore *ps, u8 slot, u8 *buf);              /* PATSTORE_OK: loadable */
int  patstore_load(struct patstore *ps, u8 slot, struct seq_pattern *dst, u8 *buf);  /* dst only
                                                                              written when valid */
int  patstore_save(struct patstore *ps, u8 slot, const struct seq_pattern *src, u8 *buf);

#endif
