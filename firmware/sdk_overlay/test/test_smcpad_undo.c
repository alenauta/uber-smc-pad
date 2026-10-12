/* Host tests for smcpad_undo.c:
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board \
 *       test/test_smcpad_undo.c -o /tmp/t_undo && /tmp/t_undo */
#define SMCPAD_HOST_TEST 1
#define UNDO_RECS 16
#include <stdio.h>
#include <string.h>
#include "../board/smcpad_undo.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u8 buf[64], shadow[64], small[16];
static struct undo U;
static u8 skip7(u16 off) { return off == 7; }

int main(void)
{
    undo_init(&U, buf, sizeof(buf), shadow, sizeof(shadow), skip7);
    undo_window(&U, 0, sizeof(buf));                /* watch it all */
    CHECK(!undo_commit(&U) && !undo_undo(&U) && !undo_redo(&U) && undo_levels(&U) == 0);
    buf[1] = 10; buf[2] = 20;                   /* group 1: 2 bytes */
    CHECK(undo_commit(&U) && U.n == 2 && undo_levels(&U) == 1);
    buf[1] = 11; buf[7] = 99;                   /* group 2: byte 1 (byte 7 is skipped) */
    CHECK(undo_commit(&U) && U.n == 3);
    buf[7] = 98;                                /* only a skipped byte: nothing recorded */
    CHECK(!undo_commit(&U) && U.n == 3);
    CHECK(undo_undo(&U) && buf[1] == 10 && buf[2] == 20 && buf[7] == 98);
    CHECK(undo_undo(&U) && buf[1] == 0 && buf[2] == 0);
    CHECK(!undo_undo(&U));
    CHECK(undo_redo(&U) && buf[1] == 10 && buf[2] == 20);
    CHECK(undo_redo(&U) && buf[1] == 11 && !undo_redo(&U));
    /* new edit after an undo drops the redo branch */
    CHECK(undo_undo(&U) && buf[1] == 10);
    buf[3] = 3;
    CHECK(undo_commit(&U) && U.n == 3 && !undo_redo(&U));
    /* journal full: the oldest groups go */
    for (u8 i = 0; i < 6; i++) {
        buf[10 + i * 2] = 1; buf[11 + i * 2] = 1;   /* 6 groups x 2 bytes */
        CHECK(undo_commit(&U));
    }
    CHECK(U.n <= UNDO_RECS && undo_levels(&U) == 8);   /* 3 + 12 = 15 records: all kept */
    buf[30] = buf[31] = buf[32] = 5;            /* 3 more: group 1 (2 records) must go */
    CHECK(undo_commit(&U) && U.n == 16 && undo_levels(&U) == 8);
    u8 n = 0;
    while (undo_undo(&U)) {
        n++;
    }
    CHECK(n == 8 && buf[1] == 10 && buf[2] == 20 && buf[3] == 0 && buf[10] == 0);  /* group 1 dropped: stays */
    /* an edit bigger than the journal clears the history */
    memset(buf + 40, 7, 20);
    CHECK(!undo_commit(&U) && U.n == 0 && !undo_undo(&U) && buf[40] == 7);

    /* windows: only the watched part is shadowed; groups undo wherever they are */
    memset(buf, 0, sizeof(buf));
    undo_init(&U, buf, sizeof(buf), small, sizeof(small), skip7);
    undo_window(&U, 16, 16);                    /* "track" 1 */
    buf[17] = 1;
    buf[40] = 9;                                /* outside the window: not journaled */
    undo_window(&U, 32, 40);                    /* commits track 1; window capped at 16 bytes */
    CHECK(U.n == 1 && U.woff == 32 && U.wlen == 16 && undo_levels(&U) == 1);
    buf[33] = 2; buf[34] = 3;
    CHECK(undo_commit(&U) && undo_levels(&U) == 2);
    CHECK(undo_undo(&U) && buf[33] == 0 && buf[34] == 0 && small[1] == 0);  /* shadow follows */
    CHECK(undo_undo(&U) && buf[17] == 0 && buf[40] == 9);
    CHECK(undo_redo(&U) && buf[17] == 1);
    CHECK(!undo_commit(&U));                    /* the undo itself is not a new edit */
    CHECK(undo_redo(&U) && buf[34] == 3 && small[2] == 3);
    printf(failures ? "test_smcpad_undo: %d FAILED\n" : "test_smcpad_undo: ALL PASSED\n", failures);
    return failures != 0;
}
