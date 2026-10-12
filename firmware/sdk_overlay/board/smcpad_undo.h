/* Undo / redo journal for a byte buffer (the sequencer pattern), pure C, host-tested by
 * test/test_smcpad_undo.c.
 *
 * Only a window of the buffer is watched at a time (the track being edited): the shadow holds
 * that window as it was at the last commit. undo_commit() diffs the window against the shadow
 * and appends the changed bytes (offset in the whole buffer, old, new) as one group to a fixed
 * journal; undo / redo replay a group backwards / forwards, wherever it is. undo_window() moves
 * the window (committing the old one first). The oldest groups are dropped when the journal is
 * full, and a single edit bigger than the whole journal clears the history. Bytes for which
 * skip(off) returns 1 (e.g. mutes) are never journaled.
 *
 * RAM: the shadow (largest window, owned by the caller) + 4 bytes per journal record.
 */
#ifndef SMCPAD_UNDO_H
#define SMCPAD_UNDO_H

#include "smcpad_seq.h"     /* u8 / u16 / u32 for host and target */

#ifndef UNDO_RECS
#define UNDO_RECS       768             /* changed bytes kept (3 KB): a whole track fits */
#endif
#define UNDO_GROUP      0x8000u         /* rec.off flag: first record of a group */

struct undo_rec {
    u16 off;                            /* byte offset | UNDO_GROUP */
    u8  old, neu;
};

struct undo {
    u8  *data;                          /* live buffer */
    u8  *shadow;                        /* the window at the last commit */
    u16 len;                            /* buffer length, < 0x8000 */
    u16 cap;                            /* shadow size */
    u16 woff, wlen;                     /* watched window: data[woff .. woff + wlen) */
    u8  (*skip)(u16 off);
    u16 n;                              /* records in the journal */
    u16 cur;                            /* records applied; [cur, n) can be redone */
    struct undo_rec rec[UNDO_RECS];
};

/* shadow = cap bytes; the window starts empty (nothing watched) */
void undo_init(struct undo *u, u8 *data, u16 len, u8 *shadow, u16 cap, u8 (*skip)(u16 off));
void undo_window(struct undo *u, u16 off, u16 wlen);   /* commit, then watch this window */
void undo_reset(struct undo *u);        /* shadow = window, history cleared (pattern loaded) */
u8   undo_commit(struct undo *u);       /* 1 if a group was recorded */
u8   undo_undo(struct undo *u);         /* 1 if something was undone (commit first!) */
u8   undo_redo(struct undo *u);         /* 1 if something was redone */
u16  undo_levels(const struct undo *u); /* groups that can be undone */

#endif
