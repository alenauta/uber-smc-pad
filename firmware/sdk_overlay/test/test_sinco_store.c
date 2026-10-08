/* Host unit tests for sinco/sinco_store.c with a fake NOR flash and VM .
 *   gcc -Wall -Wextra -Werror -std=c11 -I sinco test/test_sinco_store.c -o /tmp/t && /tmp/t
 */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sinco/sinco_proto.c"
#include "../sinco/sinco_store.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- fake NOR flash: erase sets 0xFF, program can only clear bits ---- */
#define FLASH_SIZE 0x80000u
static u8 flash[FLASH_SIZE];
static u32 erases, bad_programs, erased_mask[FLASH_SIZE / SINCO_FLASH_SECTOR];
static int fl_read(void *ctx, u32 addr, u8 *dst, u32 len)
{
    (void)ctx;
    if (addr + len > FLASH_SIZE) {
        return -1;
    }
    memcpy(dst, flash + addr, len);
    return 0;
}
static int fl_erase(void *ctx, u32 addr)
{
    (void)ctx;
    if (addr % SINCO_FLASH_SECTOR || addr >= FLASH_SIZE) {
        return -1;
    }
    memset(flash + addr, 0xFF, SINCO_FLASH_SECTOR);
    erases++;
    erased_mask[addr / SINCO_FLASH_SECTOR]++;
    return 0;
}
static int fl_write(void *ctx, u32 addr, const u8 *src, u32 len)
{
    (void)ctx;
    if (addr + len > FLASH_SIZE) {
        return -1;
    }
    for (u32 i = 0; i < len; i++) {
        if ((flash[addr + i] & src[i]) != src[i]) {
            bad_programs++;                     /* would need an erase first */
        }
        flash[addr + i] &= src[i];
    }
    return 0;
}
/* ---- fake VM: one item ---- */
static u8 vm_item[64];
static int vm_len = -1, vm_writes;
static u16 vm_id_seen;
static int vm_rd(void *ctx, u16 id, u8 *dst, u32 len)
{
    (void)ctx;
    vm_id_seen = id;
    if (vm_len < 0) {
        return -1;
    }
    u32 n = (u32)vm_len < len ? (u32)vm_len : len;
    memcpy(dst, vm_item, n);
    return (int)n;
}
static int vm_wr(void *ctx, u16 id, const u8 *src, u32 len)
{
    (void)ctx;
    vm_id_seen = id;
    memcpy(vm_item, src, len);
    vm_len = (int)len;
    vm_writes++;
    return (int)len;
}
static const struct sinco_flash_port PORT = {NULL, fl_read, fl_erase, fl_write, vm_rd, vm_wr};

static u8 presets[SMCPAD_PRESETS_SIZE];
static struct sinco_store st;

static void blank(void)
{
    memset(flash, 0xFF, sizeof(flash));
    memset(erased_mask, 0, sizeof(erased_mask));
    erases = bad_programs = 0;
    vm_len = -1;
    vm_writes = 0;
}

static void test_blank_boot(void)
{
    blank();
    sinco_store_init(&st, &PORT, presets, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    CHECK(memcmp(st.globals, smcpad_globals_default, 12) == 0);
    CHECK(st.globals_dirty == 1 && st.presets_from_flash == 0);
    CHECK(st.calib[0] == 50 && st.calib[1] == 0 && st.calib[2] == (550 & 0xFF) && st.calib[3] == (550 >> 8));
    CHECK(st.calib[150] == 50 && st.calib[158] == (950 & 0xFF));
    CHECK(sinco_store_flush_globals(&st) == 1 && vm_writes == 1 && vm_len == 14);
    CHECK(vm_id_seen == 0x2C && vm_item[0] == 0xA3 && vm_item[1] == 0x3A && vm_item[2] == 120);
    CHECK(sinco_store_flush_globals(&st) == 0 && vm_writes == 1);       /* clean */
}

static void test_globals(void)
{
    u8 v;
    /* VM content survives a reboot */
    v = 3;
    CHECK(sinco_store_write(&st, 4, GLOB_PRESET, &v, 1) == 0 && st.globals_dirty);
    CHECK(sinco_store_flush_globals(&st) == 1);
    sinco_store_init(&st, &PORT, presets, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    CHECK(st.globals[GLOB_PRESET] == 3 && st.globals_dirty == 0);

    /* stock rule: octave 0 or 6 forces transpose 0 */
    v = 5;
    CHECK(sinco_store_write(&st, 4, GLOB_TRANSPOSE, &v, 1) == 0 && st.globals[GLOB_TRANSPOSE] == 5);
    v = 6;
    CHECK(sinco_store_write(&st, 4, GLOB_OCTAVE, &v, 1) == 0 && st.globals[GLOB_TRANSPOSE] == 0);
    v = 2;
    sinco_store_write(&st, 4, GLOB_OCTAVE, &v, 1);
    v = (u8)-20;
    sinco_store_write(&st, 4, GLOB_TRANSPOSE, &v, 1);
    CHECK((signed char)st.globals[GLOB_TRANSPOSE] == -16);
    v = 9;
    sinco_store_write(&st, 4, GLOB_PRESET, &v, 1);
    CHECK(st.globals[GLOB_PRESET] == 7);
    /* tempo: clamped only when both bytes are written together */
    u8 t[2] = {0x2C, 0x01};                     /* 300 */
    CHECK(sinco_store_write(&st, 4, 0, t, 2) == 0 && st.globals[0] == 0x2C && st.globals[1] == 1);
    t[0] = 0x90;                                /* 400 -> 300 */
    sinco_store_write(&st, 4, 0, t, 2);
    CHECK(st.globals[0] == 0x2C && st.globals[1] == 1);
    t[0] = 10; t[1] = 0;                        /* 10 -> 30 */
    sinco_store_write(&st, 4, 0, t, 2);
    CHECK(st.globals[0] == 30 && st.globals[1] == 0);
    /* Midi Suite factory reset: write all 12 */
    CHECK(sinco_store_write(&st, 4, 0, smcpad_globals_default, 12) == 0);
    CHECK(memcmp(st.globals, smcpad_globals_default, 12) == 0);
    u8 r[12];
    CHECK(sinco_store_read(&st, 4, 0, r, 12) == 0 && memcmp(r, smcpad_globals_default, 12) == 0);
    CHECK(sinco_store_read(&st, 4, 11, r, 2) == -1);
    CHECK(sinco_store_write(&st, 4, 12, r, 1) == -1);
    /* bad VM magic -> defaults */
    vm_item[0] = 0;
    sinco_store_init(&st, &PORT, presets, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    CHECK(memcmp(st.globals, smcpad_globals_default, 12) == 0 && st.globals_dirty);
}

static void test_presets(void)
{
    static u8 img[SMCPAD_PRESETS_SIZE];
    srand(7);
    for (u32 i = 0; i < sizeof(img); i++) {
        img[i] = (u8)rand();
    }
    for (u32 p = 0; p < SMCPAD_PRESET_COUNT; p++) {  /* plausible record types */
        img[p * SMCPAD_PRESET_SIZE] = 1;
        img[p * SMCPAD_PRESET_SIZE + 115] = 2;
        img[p * SMCPAD_PRESET_SIZE + 211] = 0;
    }
    u8 calib_sector[SINCO_FLASH_SECTOR];
    memcpy(calib_sector, flash + SMCPAD_CALIB_FLASH, sizeof(calib_sector));
    flash[SMCPAD_PRESETS_FLASH - 1] = 0x5A;     /* neighbours must survive */

    /* Midi Suite factory reset / import: 512-B chunks */
    u32 gen0 = st.presets_gen;
    for (u32 off = 0; off < sizeof(img); off += 512) {
        u32 n = sizeof(img) - off < 512 ? sizeof(img) - off : 512;
        CHECK(sinco_store_write(&st, 5, off, img + off, n) == 0);
    }
    CHECK(st.presets_dirty && st.presets_gen > gen0);
    CHECK(memcmp(flash + SMCPAD_PRESETS_FLASH, img, 64) != 0);    /* not committed yet */
    u8 r[1024];
    CHECK(sinco_store_read(&st, 5, 3539, r, 1009) == 0 && memcmp(r, img + 3539, 1009) == 0);

    /* Save */
    erases = 0;
    CHECK(sinco_store_write(&st, 5, 0, NULL, 0) == 0);
    CHECK(erases == 7 && bad_programs == 0 && !st.presets_dirty && st.presets_from_flash);
    CHECK(memcmp(flash + SMCPAD_PRESETS_FLASH, img, sizeof(img)) == 0);
    CHECK(flash[SMCPAD_PRESETS_FLASH - 1] == 0x5A);
    CHECK(memcmp(flash + SMCPAD_CALIB_FLASH, calib_sector, sizeof(calib_sector)) == 0);
    for (u32 s = 0; s < FLASH_SIZE / SINCO_FLASH_SECTOR; s++) {
        u32 a = s * SINCO_FLASH_SECTOR;
        CHECK(!erased_mask[s] || (a >= SMCPAD_PRESETS_FLASH && a < SMCPAD_PRESETS_FLASH + 7 * SINCO_FLASH_SECTOR));
    }

    /* reboot loads them */
    memset(presets, 0, sizeof(presets));
    sinco_store_init(&st, &PORT, presets, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    CHECK(memcmp(presets, img, sizeof(img)) == 0 && st.presets_from_flash);
    CHECK(sinco_store_preset(&st, 2) == presets + 2 * 3539);

    /* refused */
    u8 v = 1;
    CHECK(sinco_store_write(&st, 5, SMCPAD_PRESETS_SIZE, &v, 1) == -1);
    CHECK(sinco_store_write(&st, 5, SMCPAD_PRESETS_SIZE - 1, r, 2) == -1);
    CHECK(sinco_store_write(&st, 5, 0x80000000u, &v, 1) == -1);   /* stock mode flag */
    CHECK(sinco_store_read(&st, 5, 0x80000000u, r, 1) == -1);
    CHECK(sinco_store_write(&st, 5, 0xFFFFFFFFu, &v, 1) == -1);   /* overflow */
    CHECK(sinco_store_write(&st, 5, 5, NULL, 0) == -1);
    CHECK(sinco_store_write(&st, 9, 0, &v, 1) == -1 && sinco_store_read(&st, 1, 0, r, 1) == -1);
}

static void test_raw_and_calib(void)
{
    u8 r[16];
    memcpy(flash + 0x100, "JLFLASH", 7);
    CHECK(sinco_store_read(&st, 0, 0x100, r, 7) == 0 && memcmp(r, "JLFLASH", 7) == 0);
    CHECK(sinco_store_read(&st, 0, 0x7FFFF, r, 1) == 0);
    CHECK(sinco_store_read(&st, 0, 0x7FFFF, r, 2) == -1);
    CHECK(sinco_store_read(&st, 0, 0xFFFFE0, r, 16) == -1);         /* the stock link killer */
    u32 e0 = erases;
    CHECK(sinco_store_write(&st, 0, 0x100, r, 1) == -1);
    CHECK(sinco_store_write(&st, 0, 0, NULL, 0) == -1 && erases == e0);

    /* calibration: Midi Suite sets pad 3 to level 5 (u16 = 550), then flashSave0(3) */
    u8 lv[2] = {550 & 0xFF, 550 >> 8};
    CHECK(sinco_store_write(&st, 3, 30, lv, 2) == 0);
    CHECK(sinco_store_read(&st, 3, 30, r, 2) == 0 && r[0] == lv[0] && r[1] == lv[1]);
    CHECK(sinco_store_read(&st, 3, 0, r, 16) == 0);
    CHECK(sinco_store_read(&st, 3, 150, r, 11) == -1);
    CHECK(sinco_store_write(&st, 3, 1, NULL, 0) == 0 && st.calib_mode == 1);
    CHECK(sinco_store_write(&st, 3, 2, NULL, 0) == 0 && st.calib_mode == 0);
    CHECK(sinco_store_write(&st, 3, 3, NULL, 0) == -1);
    e0 = erases;
    CHECK(sinco_store_write(&st, 3, 0, NULL, 0) == 0 && erases == e0 + 1 && bad_programs == 0);
    CHECK(memcmp(flash + SMCPAD_CALIB_FLASH + 160, "trimdat", 8) == 0);
    CHECK(flash[SMCPAD_CALIB_FLASH + 30] == lv[0] && flash[SMCPAD_CALIB_FLASH + 168] == 0xFF);
    st.calib[30] = 0;
    sinco_store_init(&st, &PORT, presets, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    CHECK(st.calib[30] == lv[0]);
}

static void test_via_dispatcher(void)
{
    struct sinco_ops ops = {&st, "SMC-PAD_901", sinco_store_read, sinco_store_write, NULL, NULL};
    static u8 pkt[SINCO_MAX_PKT], reply[SINCO_MAX_PKT];
    /* Midi Suite _load: read 4:0:12 */
    const u8 rq[8] = {4, 0, 0, 0, 0, 12, 0, 0};
    u32 n = sinco_build(0x23, rq, 8, pkt);
    n = sinco_handle(pkt, n, reply, &ops);
    CHECK(n == 27 && reply[2] == 0x23 && memcmp(reply + 14, st.globals, 12) == 0);
    /* a 1009-B preset chunk (Midi Suite USB chunking) */
    const u8 rp[8] = {5, 0xD3, 0x0D, 0, 0, 0xF1, 0x03, 0};      /* addr 3539, len 1009 */
    n = sinco_build(0x23, rp, 8, pkt);
    n = sinco_handle(pkt, n, reply, &ops);
    CHECK(n == 6 + 8 + 1009 + 1 && memcmp(reply + 14, presets + 3539, 1009) == 0);
    /* type-0 write refused with the error packet */
    const u8 w0[9] = {0, 0, 0x10, 0, 0, 1, 0, 0, 0xAA};
    n = sinco_build(0x22, w0, 9, pkt);
    CHECK(sinco_handle(pkt, n, reply, &ops) == 8 && reply[2] == 0 && reply[6] == 1);
}

int main(void)
{
    test_blank_boot();
    test_globals();
    test_presets();
    test_raw_and_calib();
    test_via_dispatcher();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_sinco_store: all tests passed\n");
    return 0;
}
