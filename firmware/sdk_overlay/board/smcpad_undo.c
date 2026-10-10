#include "smcpad_undo.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

void undo_init(struct undo *u, u8 *data, u8 *shadow, u16 len, u8 (*skip)(u16 off))
{
    u->data = data;
    u->shadow = shadow;
    u->len = len;
    u->skip = skip;
    undo_reset(u);
}

void undo_reset(struct undo *u)
{
    memcpy(u->shadow, u->data, u->len);
    u->n = u->cur = 0;
}

static u8 journaled(const struct undo *u, u16 off)
{
    return u->data[off] != u->shadow[off] && !(u->skip && u->skip(off));
}

static void drop_oldest(struct undo *u)
{
    u16 k = 1;
    while (k < u->n && !(u->rec[k].off & UNDO_GROUP)) {
        k++;
    }
    memmove(u->rec, u->rec + k, (u32)(u->n - k) * sizeof(u->rec[0]));
    u->n = (u16)(u->n - k);
}

u8 undo_commit(struct undo *u)
{
    u32 c = 0;
    for (u16 i = 0; i < u->len; i++) {
        c += journaled(u, i);
    }
    if (!c) {
        memcpy(u->shadow, u->data, u->len);     /* skipped bytes follow silently */
        return 0;
    }
    u->n = u->cur;                              /* a new edit ends the redo branch */
    if (c > UNDO_RECS) {                        /* too big to undo: start over */
        undo_reset(u);
        return 0;
    }
    while (u->n && u->n + c > UNDO_RECS) {
        drop_oldest(u);
    }
    u16 first = 1;
    for (u16 i = 0; i < u->len; i++) {
        if (journaled(u, i)) {
            struct undo_rec *r = &u->rec[u->n++];
            r->off = (u16)(i | (first ? UNDO_GROUP : 0));
            r->old = u->shadow[i];
            r->neu = u->data[i];
            first = 0;
        }
    }
    u->cur = u->n;
    memcpy(u->shadow, u->data, u->len);
    return 1;
}

u8 undo_undo(struct undo *u)
{
    if (!u->cur) {
        return 0;
    }
    for (;;) {
        const struct undo_rec *r = &u->rec[--u->cur];
        u16 off = (u16)(r->off & ~UNDO_GROUP);
        u->data[off] = u->shadow[off] = r->old;
        if ((r->off & UNDO_GROUP) || !u->cur) {
            return 1;
        }
    }
}

u8 undo_redo(struct undo *u)
{
    if (u->cur >= u->n) {
        return 0;
    }
    do {
        const struct undo_rec *r = &u->rec[u->cur++];
        u16 off = (u16)(r->off & ~UNDO_GROUP);
        u->data[off] = u->shadow[off] = r->neu;
    } while (u->cur < u->n && !(u->rec[u->cur].off & UNDO_GROUP));
    return 1;
}

u16 undo_levels(const struct undo *u)
{
    u16 g = 0;
    for (u16 i = 0; i < u->cur; i++) {
        g += (u->rec[i].off & UNDO_GROUP) ? 1 : 0;
    }
    return g;
}
