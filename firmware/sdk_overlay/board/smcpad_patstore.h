/* Sequencer pattern slots in raw flash (pure C, host-tested by test/test_smcpad_patstore.c).
 *
 * Area: 0x30000-0x4FFFF, the gap between the code and the VM that repack_oldchain.sh keeps
 * outside every JLFS area since 907 (updates never touch it). 16 slots of 8 KB (2 sectors).
 *
 * Slot = header (16 B) + payload. Payload, per track: the 13 track bytes (chan .. scale), an
 * 8-byte bitmap of the steps that play (vel != 0), then those steps (9 B each). A full pattern
 * would need 9.5 KB; a slot holds 871 playing steps of the 1024 (bigger: the save is refused
 * before anything is erased).
 *
 * Saving is split into short steps (erase sector 0, erase sector 1, one track per step, header
 * last) so the caller can spread it over its main loop: the longest stall is one sector erase.
 * The header goes last, so a save cut short (power off) leaves an invalid slot, never a wrong
 * pattern. Tracks are encoded at their own step from the live pattern, so edits made meanwhile
 * can't tear a track. Loading checks the CRC and the structure before touching the pattern.
 */
#ifndef SMCPAD_PATSTORE_H
#define SMCPAD_PATSTORE_H

#include "smcpad_seq.h"

#define PATSTORE_BASE       0x30000u
#define PATSTORE_END        0x50000u
#define PATSTORE_SECTOR     0x1000u
#define PATSTORE_SLOT_SIZE  0x2000u
#define PATSTORE_SLOTS      16
#define PATSTORE_HDR        16
#define PATSTORE_MAGIC      0x31545053u         /* "SPT1" little-endian */
#define PATSTORE_VER        1
#define PATSTORE_TRACK_HDR  13                  /* struct seq_track up to step[] */
#define PATSTORE_TRACK_MAX  (PATSTORE_TRACK_HDR + 8 + SEQ_STEPS * sizeof(struct seq_step))

enum { PATSTORE_OK = 0, PATSTORE_E_IO = -1, PATSTORE_E_EMPTY = -2, PATSTORE_E_CRC = -3,
       PATSTORE_E_FORMAT = -4, PATSTORE_E_FULL = -5, PATSTORE_E_SLOT = -6, PATSTORE_E_BUSY = -7,
       PATSTORE_E_VERIFY = -8 };

struct patstore_port {
    void *ctx;
    int (*read)(void *ctx, u32 addr, u8 *dst, u32 len);         /* 0 = ok */
    int (*erase)(void *ctx, u32 addr);                          /* one sector; 0 = ok */
    int (*write)(void *ctx, u32 addr, const u8 *src, u32 len);  /* 0 = ok */
};

struct patstore {
    const struct patstore_port *port;
    u16 used;                                   /* bit n: slot n holds a valid pattern */
    u32 count;                                  /* highest save counter seen */
    s8  err;                                    /* last error (PATSTORE_E_*), 0 = none */
    /* save in progress */
    u8  busy, slot, stage;
    u16 off;                                    /* payload bytes written */
    u32 crc;
    const struct seq_pattern *src;
    u8  buf[PATSTORE_TRACK_MAX];
};

u32  patstore_crc32(u32 crc, const u8 *p, u32 n);   /* start with 0 */
u32  patstore_track_size(const struct seq_track *t);
u32  patstore_pattern_size(const struct seq_pattern *p);   /* payload bytes */
u32  patstore_encode_track(const struct seq_track *t, u8 *out); /* returns bytes written */

void patstore_init(struct patstore *ps, const struct patstore_port *port);   /* scans the slots */
int  patstore_check(struct patstore *ps, u8 slot);         /* PATSTORE_OK if loadable */
/* dst is only written when the slot is fully valid */
int  patstore_load(struct patstore *ps, u8 slot, struct seq_pattern *dst);
/* starts a save; src must stay valid until done. PATSTORE_E_FULL: too big, nothing erased */
int  patstore_save_begin(struct patstore *ps, u8 slot, const struct seq_pattern *src);
/* one step: 1 = call again, 0 = saved and verified, < 0 = failed (slot now empty) */
int  patstore_save_step(struct patstore *ps);

#endif
