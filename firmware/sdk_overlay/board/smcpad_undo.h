/* Undo / redo journal for a byte buffer (the sequencer pattern), pure C, host-tested by
 * test/test_smcpad_undo.c.
 *
 * Keeps a shadow copy of the buffer as it was at the last commit. undo_commit() diffs the
 * buffer against the shadow and appends the changed bytes (offset, old, new) as one group to a
 * fixed journal; undo / redo replay a group backwards / forwards. The oldest groups are dropped
 * when the journal is full, and a single edit bigger than the whole journal clears the history.
 * Bytes for which skip(off) returns 1 (e.g. mutes) are never journaled.
 *
 * RAM: the shadow (len bytes, owned by the caller) + 4 bytes per journal record.
 */
#ifndef SMCPAD_UNDO_H
#define SMCPAD_UNDO_H

#include "smcpad_seq.h"     /* u8 / u16 / u32 for host and target */

#ifndef UNDO_RECS
#define UNDO_RECS       384             /* changed bytes kept (1.5 KB) */
#endif
#define UNDO_GROUP      0x8000u         /* rec.off flag: first record of a group */

struct undo_rec {
    u16 off;                            /* byte offset | UNDO_GROUP */
    u8  old, neu;
};

struct undo {
    u8  *data;                          /* live buffer */
    u8  *shadow;                        /* buffer at the last commit */
    u16 len;                            /* < 0x8000 */
    u8  (*skip)(u16 off);
    u16 n;                              /* records in the journal */
    u16 cur;                            /* records applied; [cur, n) can be redone */
    struct undo_rec rec[UNDO_RECS];
};

void undo_init(struct undo *u, u8 *data, u8 *shadow, u16 len, u8 (*skip)(u16 off));
void undo_reset(struct undo *u);        /* shadow = data, history cleared (pattern loaded) */
u8   undo_commit(struct undo *u);       /* 1 if a group was recorded */
u8   undo_undo(struct undo *u);         /* 1 if something was undone (commit first!) */
u8   undo_redo(struct undo *u);         /* 1 if something was redone */
u16  undo_levels(const struct undo *u); /* groups that can be undone */

#endif
