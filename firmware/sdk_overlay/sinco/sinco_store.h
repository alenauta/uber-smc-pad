/* Sinco storage model (stock SMC-PAD parity), SMC-PAD project.
 *
 *   type 0  raw SPI flash, absolute address    READ-ONLY here, addr + len <= 0x80000
 *   type 3  pad calibration, 16 x 10 B          RAM copy; commit = zero-length write at addr 0
 *                                               -> erase USRTRIM 4K, write 160 B + "trimdat\0"
 *   type 4  globals, 12 B                       RAM copy; persisted in VM (magic 0x3AA3 + 12 B)
 *                                               by sinco_store_flush_globals() (rate-limited
 *                                               by the caller)
 *   type 5  presets, 8 x 3539 B = 28312 B       RAM copy, edits live; commit = zero-length
 *                                               write at addr 0 -> erase 7 x 4K, write 28312 B
 * Refused (decision 3 in the BLE plan): type-0 writes, the 0x21 erase command (in the
 * dispatcher), and the stock "type 5 addr 0x80000000" mode flag. Everything else out of range
 * is refused too. Specification: docs/midisuite_protocol.md, docs/preset_format.md.
 * Pure C: flash and VM go through a port struct (fake flash in test/test_sinco_store.c).
 */
#ifndef SINCO_STORE_H
#define SINCO_STORE_H

#include "sinco_proto.h"

#define SINCO_TYPE_RAW          0
#define SINCO_TYPE_CALIB        3
#define SINCO_TYPE_GLOBALS      4
#define SINCO_TYPE_PRESETS      5
#define SINCO_PRESET_FLAG_ADDR  0x80000000u     /* type 5 "mode flag": accepted, ignored */

#define SMCPAD_PRESET_SIZE      3539
#define SMCPAD_PRESET_COUNT     8
#define SMCPAD_PRESETS_SIZE     (SMCPAD_PRESET_SIZE * SMCPAD_PRESET_COUNT)    /* 28312 */
#define SMCPAD_GLOBALS_SIZE     12
#define SMCPAD_CALIB_PADS       16
#define SMCPAD_CALIB_REC        10
#define SMCPAD_CALIB_SIZE       (SMCPAD_CALIB_PADS * SMCPAD_CALIB_REC)        /* 160 */
#define SMCPAD_CALIB_TAG        "trimdat"                                     /* + '\0' */
#define SMCPAD_CALIB_TAG_LEN    8
#define SMCPAD_RAW_FLASH_LIMIT  0x80000u
#define SINCO_FLASH_SECTOR      4096u

/* stock flash layout (isd_config.ini; docs/build_host.md) */
#define SMCPAD_PRESETS_FLASH    0x77000u                                      /* USRFLASH */
#define SMCPAD_CALIB_FLASH      0x7E000u                                      /* USRTRIM */
#ifndef SMCPAD_GLOBALS_VM_ID
#define SMCPAD_GLOBALS_VM_ID    0x2C                                          /* as stock */
#endif
#define SMCPAD_GLOBALS_MAGIC    0x3AA3

/* globals byte offsets (docs/midisuite_protocol.md 3.1) */
enum {
    GLOB_TEMPO_LO = 0, GLOB_TEMPO_HI, GLOB_SWING, GLOB_TIME, GLOB_SYNC, GLOB_LATCH,
    GLOB_OCTAVE, GLOB_TRANSPOSE, GLOB_PAD_CURVE, GLOB_AFTERTOUCH, GLOB_PRESET, GLOB_BANK
};
extern const u8 smcpad_globals_default[SMCPAD_GLOBALS_SIZE];

struct sinco_flash_port {
    void *ctx;
    int (*read)(void *ctx, u32 addr, u8 *dst, u32 len);          /* 0 = ok */
    int (*erase_sector)(void *ctx, u32 addr);                     /* 4 KB, aligned */
    int (*write)(void *ctx, u32 addr, const u8 *src, u32 len);
    int (*vm_read)(void *ctx, u16 id, u8 *dst, u32 len);          /* bytes read, <0 error */
    int (*vm_write)(void *ctx, u16 id, const u8 *src, u32 len);   /* bytes written */
};

struct sinco_store {
    const struct sinco_flash_port *port;
    u8  *presets;                       /* SMCPAD_PRESETS_SIZE bytes, owned by the caller */
    u32 presets_flash, calib_flash;
    u8  globals[SMCPAD_GLOBALS_SIZE];
    u8  calib[SMCPAD_CALIB_SIZE];
    u8  globals_dirty;                  /* changed since the last flush */
    u8  presets_dirty;                  /* RAM differs from flash (not saved yet) */
    u8  calib_mode;                     /* stock: type 3 addr 1 / 2, len 0 (no effect yet) */
    u8  presets_from_flash;             /* 1 if the boot load looked like stock data */
    u32 presets_gen;                    /* ++ on every preset change (RAM or commit) */
    u32 globals_gen;                    /* ++ on every globals change */
};

/* Loads presets (flash), calibration (flash, defaults if the tag is missing) and globals (VM,
 * defaults if missing). */
void sinco_store_init(struct sinco_store *s, const struct sinco_flash_port *port, u8 *presets_ram,
                      u32 presets_flash, u32 calib_flash);
/* sinco_ops callbacks; ctx = struct sinco_store * */
int  sinco_store_read(void *ctx, u8 type, u32 addr, u8 *dst, u32 len);
int  sinco_store_write(void *ctx, u8 type, u32 addr, const u8 *src, u32 len);
/* Writes the globals to VM if dirty. Returns 1 if written, 0 if clean, <0 on error. */
int  sinco_store_flush_globals(struct sinco_store *s);
/* Stock globals writer rules: clamps, and octave 0 or 6 forces transpose 0. */
void sinco_globals_sanitize(u8 *g, u32 touched_lo, u32 touched_hi);

static inline const u8 *sinco_store_preset(const struct sinco_store *s, u8 idx)
{
    return s->presets + (u32)(idx % SMCPAD_PRESET_COUNT) * SMCPAD_PRESET_SIZE;
}

#endif
