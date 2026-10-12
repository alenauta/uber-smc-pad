/* Host tests for smcpad_patstore.c (pattern slots) on a simulated NOR flash:
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

/* NOR: erase sets 0xFF, programming can only clear bits */
static u8 flash[PATSTORE_END - PATSTORE_BASE];
static int erases, writes, fail_erase, fail_write, protect;

static int f_read(void *ctx, u32 addr, u8 *dst, u32 len)
{
    (void)ctx;
    if (addr < PATSTORE_BASE || addr + len > PATSTORE_END) return -1;
    memcpy(dst, flash + (addr - PATSTORE_BASE), len);
    return 0;
}
static int f_erase(void *ctx, u32 addr)
{
    (void)ctx;
    if (fail_erase || addr % PATSTORE_SECTOR || addr < PATSTORE_BASE || addr >= PATSTORE_END) return -1;
    erases++;
    if (!protect) memset(flash + (addr - PATSTORE_BASE), 0xFF, PATSTORE_SECTOR);
    return 0;
}
static int f_write(void *ctx, u32 addr, const u8 *src, u32 len)
{
    (void)ctx;
    if (fail_write || addr < PATSTORE_BASE || addr + len > PATSTORE_END) return -1;
    writes++;
    if (!protect) {
        for (u32 i = 0; i < len; i++) flash[addr - PATSTORE_BASE + i] &= src[i];
    }
    return 0;                                   /* a protected chip "succeeds" silently */
}
static const struct patstore_port port = {NULL, f_read, f_erase, f_write};

static struct patstore ps;
static struct seq_pattern a, b;

static void make_pattern(struct seq_pattern *p, u8 seed)
{
    seq_pattern_default(p, NULL, NULL);
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        struct seq_track *tr = &p->track[t];
        tr->len = (u8)(8 + (t + seed) % 57);
        tr->chan = (u8)((t + seed) & 15);
        tr->root = (u8)((t * 5 + seed) % 13);
        for (u8 i = 0; i < SEQ_STEPS; i++) {
            if ((i * 7 + t * 3 + seed) % 5 == 0) {
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

static int save(u8 slot, const struct seq_pattern *p)
{
    int r = patstore_save_begin(&ps, slot, p);
    if (r) return r;
    int steps = 0;
    while ((r = patstore_save_step(&ps)) > 0) steps++;
    if (r == 0) {
        CHECK(steps == 2 + SEQ_TRACKS);         /* two erases, one step per track; header last */
    }
    return r;
}

static void test_crc(void)
{
    CHECK(patstore_crc32(0, (const u8 *)"123456789", 9) == 0xCBF43926u);
    u32 c = patstore_crc32(0, (const u8 *)"1234", 4);
    CHECK(patstore_crc32(c, (const u8 *)"56789", 5) == 0xCBF43926u);    /* chunked = whole */
}

static void test_roundtrip(void)
{
    memset(flash, 0xFF, sizeof(flash));
    patstore_init(&ps, &port);
    CHECK(ps.used == 0 && ps.count == 0);
    CHECK(patstore_load(&ps, 2, &b) == PATSTORE_E_EMPTY);
    make_pattern(&a, 1);
    CHECK(save(2, &a) == 0 && ps.used == (1u << 2) && ps.count == 1);
    memset(&b, 0x55, sizeof(b));
    CHECK(patstore_load(&ps, 2, &b) == 0 && memcmp(&a, &b, sizeof(a)) == 0);
    /* the default pattern: smallest payload, every slot independent */
    seq_pattern_default(&b, NULL, NULL);
    CHECK(patstore_pattern_size(&b) == SEQ_TRACKS * 21);
    CHECK(save(15, &b) == 0 && ps.used == ((1u << 2) | (1u << 15)));
    struct seq_pattern c;
    CHECK(patstore_load(&ps, 15, &c) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
    CHECK(patstore_load(&ps, 2, &c) == 0 && memcmp(&a, &c, sizeof(a)) == 0);
    /* boot scan finds both and the newest counter */
    patstore_init(&ps, &port);
    CHECK(ps.used == ((1u << 2) | (1u << 15)) && ps.count == 2);
    /* overwrite a slot */
    make_pattern(&b, 7);
    CHECK(save(2, &b) == 0 && patstore_load(&ps, 2, &c) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
    CHECK(patstore_load(&ps, 16, &c) == PATSTORE_E_SLOT);
}

static void test_corruption(void)
{
    memset(flash, 0xFF, sizeof(flash));
    patstore_init(&ps, &port);
    make_pattern(&a, 3);
    CHECK(save(4, &a) == 0);
    u32 base = 4 * PATSTORE_SLOT_SIZE;
    flash[base + PATSTORE_HDR + 100] ^= 0x01;                  /* one bit in the payload */
    memset(&b, 0x55, sizeof(b));
    struct seq_pattern keep = b;
    CHECK(patstore_load(&ps, 4, &b) == PATSTORE_E_CRC && memcmp(&b, &keep, sizeof(b)) == 0);
    patstore_init(&ps, &port);
    CHECK(!(ps.used & (1u << 4)));
    flash[base + PATSTORE_HDR + 100] ^= 0x01;
    flash[base + 5] = SEQ_PATTERN_VER + 1;                     /* a future layout: refused */
    CHECK(patstore_check(&ps, 4) == PATSTORE_E_FORMAT);
    /* a valid CRC over a broken structure (bitmap claims more steps than stored) */
    memset(flash, 0xFF, sizeof(flash));
    seq_pattern_default(&a, NULL, NULL);
    CHECK(save(0, &a) == 0);
    u8 *pl = flash + PATSTORE_HDR;
    pl[PATSTORE_TRACK_HDR] = 0x01;                             /* track 0: step 0 "plays" */
    u32 crc = patstore_crc32(0, pl, SEQ_TRACKS * 21);
    flash[8] = (u8)crc; flash[9] = (u8)(crc >> 8); flash[10] = (u8)(crc >> 16); flash[11] = (u8)(crc >> 24);
    CHECK(patstore_check(&ps, 0) == PATSTORE_E_FORMAT);
    /* out-of-range track fields from a valid slot are made safe */
    memset(flash, 0xFF, sizeof(flash));
    seq_pattern_default(&a, NULL, NULL);
    a.track[3].len = 0; a.track[3].rate = 99; a.track[3].res = 77; a.track[3].gate = 200;
    a.track[3].prob = 250; a.track[3].root = 40; a.track[3].scale = 200; a.track[3].chan = 0x3F;
    CHECK(save(1, &a) == 0 && patstore_load(&ps, 1, &b) == 0);
    struct seq_track *t = &b.track[3];
    CHECK(t->len == 1 && t->rate == SEQ_RATE_16 && t->res == SEQ_RATE_16 && t->gate == 3);
    CHECK(t->prob == 100 && t->root == 0 && t->scale == 0 && t->chan == 15);
}

static void test_save_failures(void)
{
    memset(flash, 0xFF, sizeof(flash));
    patstore_init(&ps, &port);
    make_pattern(&a, 5);
    CHECK(save(6, &a) == 0);
    /* too big: refused before anything is erased, the slot keeps its pattern */
    memset(&b, 0, sizeof(b));
    seq_pattern_default(&b, NULL, NULL);
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        for (u8 i = 0; i < SEQ_STEPS; i++) b.track[t].step[i].vel = 100;
    }
    CHECK(patstore_pattern_size(&b) > PATSTORE_SLOT_SIZE - PATSTORE_HDR);
    erases = 0;
    CHECK(patstore_save_begin(&ps, 6, &b) == PATSTORE_E_FULL && erases == 0 && !ps.busy);
    struct seq_pattern c;
    CHECK(patstore_load(&ps, 6, &c) == 0 && memcmp(&a, &c, sizeof(a)) == 0);
    /* the largest pattern that fits: 871 playing steps (one more does not) */
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        for (u8 i = 0; i < SEQ_STEPS; i++) b.track[t].step[i].vel = (u8)(t * SEQ_STEPS + i < 872 ? 100 : 0);
    }
    CHECK(patstore_pattern_size(&b) > PATSTORE_SLOT_SIZE - PATSTORE_HDR);
    b.track[871 / SEQ_STEPS].step[871 % SEQ_STEPS].vel = 0;
    CHECK(patstore_pattern_size(&b) <= PATSTORE_SLOT_SIZE - PATSTORE_HDR);
    CHECK(save(7, &b) == 0 && patstore_load(&ps, 7, &c) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
    /* cut short (power off after a few steps): the slot is invalid, never wrong */
    CHECK(patstore_save_begin(&ps, 6, &b) == 0);
    for (int i = 0; i < 6; i++) CHECK(patstore_save_step(&ps) == 1);
    CHECK(patstore_save_begin(&ps, 5, &b) == PATSTORE_E_BUSY);
    patstore_init(&ps, &port);                                 /* reboot */
    CHECK(!(ps.used & (1u << 6)) && (ps.used & (1u << 7)) && patstore_load(&ps, 6, &c) == PATSTORE_E_EMPTY);
    /* erase fails / flash protected: error, the slot is not marked used */
    fail_erase = 1;
    CHECK(save(8, &a) == PATSTORE_E_IO && !(ps.used & (1u << 8)) && !ps.busy);
    fail_erase = 0;
    protect = 1;
    CHECK(save(9, &a) == PATSTORE_E_VERIFY && !(ps.used & (1u << 9)) && ps.err == PATSTORE_E_VERIFY);
    protect = 0;
    /* edits between steps: each track is taken whole at its own step */
    make_pattern(&a, 9);
    CHECK(patstore_save_begin(&ps, 10, &a) == 0);
    CHECK(patstore_save_step(&ps) == 1 && patstore_save_step(&ps) == 1);  /* erased */
    CHECK(patstore_save_step(&ps) == 1);                                  /* track 0 written */
    a.track[0].step[1].vel = 99;                                          /* too late for it */
    a.track[5].step[1].vel = 99;                                          /* in time for this */
    int r;
    while ((r = patstore_save_step(&ps)) > 0) {}
    CHECK(r == 0 && patstore_load(&ps, 10, &c) == 0);
    CHECK(c.track[5].step[1].vel == 99 && c.track[0].step[1].vel != 99);
}

int main(void)
{
    test_crc();
    test_roundtrip();
    test_corruption();
    test_save_failures();
    printf(failures ? "test_smcpad_patstore: %d FAILED\n" : "test_smcpad_patstore: ALL PASSED\n", failures);
    return failures != 0;
}
