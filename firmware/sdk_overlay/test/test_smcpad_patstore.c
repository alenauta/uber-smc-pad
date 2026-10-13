/* Host tests for smcpad_patstore.c (pattern slots as settings items) on a simulated VM:
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board \
 *       test/test_smcpad_patstore.c -o /tmp/t_ps && /tmp/t_ps */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../board/smcpad_scale.c"
#include "../board/smcpad_seq.c"
#include "../board/smcpad_patstore.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* settings store: items by id, like syscfg (read needs cap >= length, write <= 4095) */
static struct { int len; u8 d[PATSTORE_MAX]; } vm[256];
static int writes, fail_write, drop_write, flip_on_read;

static int v_read(void *ctx, u16 id, u8 *dst, u32 cap)
{
    (void)ctx;
    if (id > 255 || vm[id].len <= 0) return -1;
    if ((u32)vm[id].len > cap) return -2;
    memcpy(dst, vm[id].d, (u32)vm[id].len);
    if (flip_on_read) dst[PATSTORE_HDR + 3] ^= 1;
    return vm[id].len;
}
static int v_write(void *ctx, u16 id, const u8 *src, u32 len)
{
    (void)ctx;
    if (fail_write || id > 255 || len > PATSTORE_MAX) return -1;
    writes++;
    if (drop_write) return (int)len;            /* "succeeds" but nothing changes */
    memcpy(vm[id].d, src, len);
    vm[id].len = (int)len;
    return (int)len;
}
static const struct patstore_port port = {NULL, v_read, v_write};

static struct patstore ps;
static struct seq_pattern a, b, c;
static u8 buf[PATSTORE_MAX];

static void make_pattern(struct seq_pattern *p, u8 seed, u8 density)
{
    seq_pattern_default(p, NULL, NULL);
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        struct seq_track *tr = &p->track[t];
        tr->len = (u8)(8 + (t + seed) % 57);
        tr->chan = (u8)((t + seed) & 15);
        tr->root = (u8)((t * 5 + seed) % 13);
        for (u8 i = 0; i < SEQ_STEPS; i++) {
            if ((i * 7 + t * 3 + seed) % density == 0) {
                struct seq_step *st = &tr->step[i];
                st->vel = (u8)(1 + (i + t + seed) % 127);
                st->note = (s8)((i % 9) - 4);
                st->prob_gate = (u8)(i * 13);
                st->micro = (s8)((i % 7) - 3);
                st->ratchet = (u8)(i * 29);
                st->cond = (u8)(i % 13);
                st->extra[0] = 2; st->extra[1] = 4; st->extra[2] = (s8)-3;
            }
        }
    }
}

static void reset_vm(void)
{
    memset(vm, 0, sizeof(vm));
    writes = fail_write = drop_write = flip_on_read = 0;
}

static void test_crc(void)
{
    CHECK(patstore_crc32(0, (const u8 *)"123456789", 9) == 0xCBF43926u);
    u32 x = patstore_crc32(0, (const u8 *)"1234", 4);
    CHECK(patstore_crc32(x, (const u8 *)"56789", 5) == 0xCBF43926u);
}

static void test_roundtrip(void)
{
    reset_vm();
    patstore_init(&ps, &port, buf);
    CHECK(ps.used == 0 && ps.count == 0);
    CHECK(patstore_load(&ps, 2, &b, buf) == PATSTORE_E_EMPTY);
    make_pattern(&a, 1, 5);
    CHECK(patstore_pattern_size(&a) <= PATSTORE_MAX);
    CHECK(patstore_save(&ps, 2, &a, buf) == 0 && ps.used == (1u << 2) && ps.count == 1 && writes == 1);
    CHECK(vm[PATSTORE_ID0 + 2].len == (int)patstore_pattern_size(&a));
    memset(&b, 0x55, sizeof(b));
    CHECK(patstore_load(&ps, 2, &b, buf) == 0 && memcmp(&a, &b, sizeof(a)) == 0);
    /* the default pattern: smallest item, slots independent */
    seq_pattern_default(&b, NULL, NULL);
    CHECK(patstore_pattern_size(&b) == PATSTORE_HDR + SEQ_TRACKS * 21);
    CHECK(patstore_save(&ps, 15, &b, buf) == 0 && ps.used == ((1u << 2) | (1u << 15)));
    CHECK(patstore_load(&ps, 15, &c, buf) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
    CHECK(patstore_load(&ps, 2, &c, buf) == 0 && memcmp(&a, &c, sizeof(a)) == 0);
    /* boot scan */
    patstore_init(&ps, &port, buf);
    CHECK(ps.used == ((1u << 2) | (1u << 15)) && ps.count == 2);
    /* overwrite */
    make_pattern(&b, 7, 6);
    CHECK(patstore_save(&ps, 2, &b, buf) == 0 && patstore_load(&ps, 2, &c, buf) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
    CHECK(patstore_load(&ps, 16, &c, buf) == PATSTORE_E_SLOT && patstore_save(&ps, 16, &b, buf) == PATSTORE_E_SLOT);
    /* ids: slot n = item PATSTORE_ID0 + n, nothing else touched */
    for (int id = 0; id < 256; id++) {
        if (id != PATSTORE_ID0 + 2 && id != PATSTORE_ID0 + 15) CHECK(vm[id].len == 0);
    }
}

static void test_capacity(void)
{
    reset_vm();
    patstore_init(&ps, &port, buf);
    make_pattern(&a, 3, 5);
    CHECK(patstore_save(&ps, 4, &a, buf) == 0);
    /* 415 playing steps fit, 416 don't (refused before writing: the slot keeps its pattern) */
    seq_pattern_default(&b, NULL, NULL);
    for (u32 k = 0; k < 416; k++) b.track[k / SEQ_STEPS].step[k % SEQ_STEPS].vel = 100;
    CHECK(patstore_pattern_size(&b) > PATSTORE_MAX);
    writes = 0;
    CHECK(patstore_save(&ps, 4, &b, buf) == PATSTORE_E_FULL && writes == 0 && ps.err == PATSTORE_E_FULL);
    CHECK(patstore_load(&ps, 4, &c, buf) == 0 && memcmp(&a, &c, sizeof(a)) == 0);
    b.track[415 / SEQ_STEPS].step[415 % SEQ_STEPS].vel = 0;
    CHECK(patstore_pattern_size(&b) <= PATSTORE_MAX);
    CHECK(patstore_save(&ps, 5, &b, buf) == 0 && patstore_load(&ps, 5, &c, buf) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
}

static void test_failures(void)
{
    reset_vm();
    patstore_init(&ps, &port, buf);
    make_pattern(&a, 5, 5);
    CHECK(patstore_save(&ps, 6, &a, buf) == 0);
    /* write refused: error, the old item stays */
    make_pattern(&b, 9, 4);
    fail_write = 1;
    CHECK(patstore_save(&ps, 6, &b, buf) == PATSTORE_E_IO && (ps.used & (1u << 6)));
    fail_write = 0;
    CHECK(patstore_load(&ps, 6, &c, buf) == 0 && memcmp(&a, &c, sizeof(a)) == 0);
    /* write "ok" but nothing stored (an id the store ignores): caught by the read-back */
    drop_write = 1;
    CHECK(patstore_save(&ps, 9, &b, buf) == PATSTORE_E_VERIFY && !(ps.used & (1u << 9)) && ps.err == PATSTORE_E_VERIFY);
    drop_write = 0;
    /* a corrupted item: CRC error, the pattern in RAM untouched */
    memset(&c, 0x55, sizeof(c));
    struct seq_pattern keep = c;
    flip_on_read = 1;
    CHECK(patstore_load(&ps, 6, &c, buf) == PATSTORE_E_CRC && memcmp(&c, &keep, sizeof(c)) == 0);
    patstore_init(&ps, &port, buf);
    CHECK(!(ps.used & (1u << 6)));
    flip_on_read = 0;
    /* an item from another layout version / truncated: refused */
    vm[PATSTORE_ID0 + 6].d[5] = SEQ_PATTERN_VER + 1;
    CHECK(patstore_check(&ps, 6, buf) == PATSTORE_E_FORMAT);
    vm[PATSTORE_ID0 + 6].d[5] = SEQ_PATTERN_VER;
    vm[PATSTORE_ID0 + 6].len -= 1;
    CHECK(patstore_check(&ps, 6, buf) == PATSTORE_E_FORMAT);
    vm[PATSTORE_ID0 + 6].len += 1;
    CHECK(patstore_check(&ps, 6, buf) == PATSTORE_OK);
    /* the 918 raw-flash format (magic SPT1) is not taken for a 919 item */
    vm[PATSTORE_ID0 + 6].d[3] = '1';
    CHECK(patstore_check(&ps, 6, buf) == PATSTORE_E_EMPTY);
    /* valid CRC over a broken structure: refused */
    reset_vm();
    seq_pattern_default(&a, NULL, NULL);
    CHECK(patstore_save(&ps, 0, &a, buf) == 0);
    u8 *it = vm[PATSTORE_ID0].d;
    it[PATSTORE_HDR + PATSTORE_TRACK_HDR] = 0x01;              /* track 0: step 0 "plays" */
    put32le(it + 8, patstore_crc32(0, it + PATSTORE_HDR, SEQ_TRACKS * 21));
    CHECK(patstore_check(&ps, 0, buf) == PATSTORE_E_FORMAT);
    /* out-of-range track fields from a valid item are made safe */
    reset_vm();
    seq_pattern_default(&a, NULL, NULL);
    a.track[3].len = 0; a.track[3].rate = 99; a.track[3].res = 77; a.track[3].gate = 200;
    a.track[3].prob = 250; a.track[3].root = 40; a.track[3].scale = 200; a.track[3].chan = 0x3F;
    CHECK(patstore_save(&ps, 1, &a, buf) == 0 && patstore_load(&ps, 1, &b, buf) == 0);
    struct seq_track *t = &b.track[3];
    CHECK(t->len == 1 && t->rate == SEQ_RATE_16 && t->res == SEQ_RATE_16 && t->gate == 3);
    CHECK(t->prob == 100 && t->root == 0 && t->scale == 0 && t->chan == 15);
}

int main(void)
{
    test_crc();
    test_roundtrip();
    test_capacity();
    test_failures();
    printf(failures ? "test_smcpad_patstore: %d FAILED\n" : "test_smcpad_patstore: ALL PASSED\n", failures);
    return failures != 0;
}
