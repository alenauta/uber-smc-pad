/* Sinco storage model, see sinco_store.h. */
#include <string.h>
#include "sinco_store.h"

const u8 smcpad_globals_default[SMCPAD_GLOBALS_SIZE] = {
    120, 0, 50, 4, 0, 0, 2, 0, 1, 1, 0, 0       /* stock boot defaults (0x1E11D2A) */
};
/* stock default calibration record: u16 LE breakpoints 50, 550, 720, 850, 950 (0x1E165A4) */
static const u16 calib_default_rec[SMCPAD_CALIB_REC / 2] = {50, 550, 720, 850, 950};

static int in_range(u32 addr, u32 len, u32 size)
{
    return addr <= size && len <= size - addr;
}

static u8 clamp_u8(u8 v, u8 lo, u8 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void sinco_globals_sanitize(u8 *g, u32 touched_lo, u32 touched_hi)
{
    if (touched_lo <= GLOB_TEMPO_LO && touched_hi >= GLOB_TEMPO_HI) {
        u32 t = (u32)g[GLOB_TEMPO_LO] | ((u32)g[GLOB_TEMPO_HI] << 8);
        t = t < 30 ? 30 : (t > 300 ? 300 : t);
        g[GLOB_TEMPO_LO] = (u8)t;
        g[GLOB_TEMPO_HI] = (u8)(t >> 8);
    }
    g[GLOB_SWING] = clamp_u8(g[GLOB_SWING], 50, 75);
    g[GLOB_TIME] = clamp_u8(g[GLOB_TIME], 0, 7);
    g[GLOB_SYNC] = g[GLOB_SYNC] ? 1 : 0;
    g[GLOB_LATCH] = g[GLOB_LATCH] ? 1 : 0;
    g[GLOB_OCTAVE] = clamp_u8(g[GLOB_OCTAVE], 0, 6);
    s32 tr = (s32)(signed char)g[GLOB_TRANSPOSE];
    tr = tr < -16 ? -16 : (tr > 16 ? 16 : tr);
    g[GLOB_TRANSPOSE] = (u8)tr;
    if (g[GLOB_OCTAVE] == 0 || g[GLOB_OCTAVE] == 6) {   /* stock writer 0x1E1151E */
        g[GLOB_TRANSPOSE] = 0;
    }
    g[GLOB_PAD_CURVE] = clamp_u8(g[GLOB_PAD_CURVE], 0, 3);
    g[GLOB_AFTERTOUCH] = g[GLOB_AFTERTOUCH] ? 1 : 0;
    g[GLOB_PRESET] = clamp_u8(g[GLOB_PRESET], 0, SMCPAD_PRESET_COUNT - 1);
    g[GLOB_BANK] = g[GLOB_BANK] ? 1 : 0;
}

static void calib_defaults(u8 *c)
{
    for (u32 p = 0; p < SMCPAD_CALIB_PADS; p++) {
        for (u32 i = 0; i < SMCPAD_CALIB_REC / 2; i++) {
            c[p * SMCPAD_CALIB_REC + 2 * i] = (u8)calib_default_rec[i];
            c[p * SMCPAD_CALIB_REC + 2 * i + 1] = (u8)(calib_default_rec[i] >> 8);
        }
    }
}

/* plausibility of a stock preset block: first button / knob / pad types in range */
static int presets_plausible(const u8 *p)
{
    for (u32 i = 0; i < SMCPAD_PRESET_COUNT; i++) {
        const u8 *r = p + i * SMCPAD_PRESET_SIZE;
        if (r[0] > 5 || r[5 * 23] > 3 || r[5 * 23 + 16 * 6] > 6) {
            return 0;
        }
    }
    return 1;
}

void sinco_store_init(struct sinco_store *s, const struct sinco_flash_port *port, u8 *presets_ram,
                      u32 presets_flash, u32 calib_flash)
{
    memset(s, 0, sizeof(*s));
    s->port = port;
    s->presets = presets_ram;
    s->presets_flash = presets_flash;
    s->calib_flash = calib_flash;

    if (port->read(port->ctx, presets_flash, presets_ram, SMCPAD_PRESETS_SIZE) != 0) {
        memset(presets_ram, 0xFF, SMCPAD_PRESETS_SIZE);
    }
    s->presets_from_flash = (u8)presets_plausible(presets_ram);

    u8 tmp[SMCPAD_CALIB_SIZE + SMCPAD_CALIB_TAG_LEN];
    if (port->read(port->ctx, calib_flash, tmp, sizeof(tmp)) == 0 &&
        memcmp(tmp + SMCPAD_CALIB_SIZE, SMCPAD_CALIB_TAG, SMCPAD_CALIB_TAG_LEN) == 0) {
        memcpy(s->calib, tmp, SMCPAD_CALIB_SIZE);
    } else {
        calib_defaults(s->calib);
    }

    u8 vm[2 + SMCPAD_GLOBALS_SIZE];
    int n = port->vm_read ? port->vm_read(port->ctx, SMCPAD_GLOBALS_VM_ID, vm, sizeof(vm)) : -1;
    if (n == (int)sizeof(vm) && (vm[0] | (vm[1] << 8)) == SMCPAD_GLOBALS_MAGIC) {
        memcpy(s->globals, vm + 2, SMCPAD_GLOBALS_SIZE);
        sinco_globals_sanitize(s->globals, 0, SMCPAD_GLOBALS_SIZE - 1);
    } else {
        memcpy(s->globals, smcpad_globals_default, SMCPAD_GLOBALS_SIZE);
        s->globals_dirty = 1;                   /* stock writes the defaults at boot too */
    }
}

int sinco_store_flush_globals(struct sinco_store *s)
{
    if (!s->globals_dirty) {
        return 0;
    }
    if (!s->port->vm_write) {
        return -1;
    }
    u8 vm[2 + SMCPAD_GLOBALS_SIZE];
    vm[0] = (u8)SMCPAD_GLOBALS_MAGIC;
    vm[1] = (u8)(SMCPAD_GLOBALS_MAGIC >> 8);
    memcpy(vm + 2, s->globals, SMCPAD_GLOBALS_SIZE);
    if (s->port->vm_write(s->port->ctx, SMCPAD_GLOBALS_VM_ID, vm, sizeof(vm)) != (int)sizeof(vm)) {
        return -1;
    }
    s->globals_dirty = 0;
    return 1;
}

static int erase_write(const struct sinco_flash_port *port, u32 addr, const u8 *a, u32 alen,
                       const u8 *b, u32 blen)
{
    u32 total = alen + blen;
    for (u32 off = 0; off < total; off += SINCO_FLASH_SECTOR) {
        if (port->erase_sector(port->ctx, addr + off) != 0) {
            return -1;
        }
    }
    if (port->write(port->ctx, addr, a, alen) != 0) {
        return -1;
    }
    if (blen && port->write(port->ctx, addr + alen, b, blen) != 0) {
        return -1;
    }
    return 0;
}

int sinco_store_read(void *ctx, u8 type, u32 addr, u8 *dst, u32 len)
{
    struct sinco_store *s = ctx;
    switch (type) {
    case SINCO_TYPE_RAW:
        if (!in_range(addr, len, SMCPAD_RAW_FLASH_LIMIT)) {
            return -1;
        }
        return s->port->read(s->port->ctx, addr, dst, len) == 0 ? 0 : -1;
    case SINCO_TYPE_CALIB:
        if (!in_range(addr, len, SMCPAD_CALIB_SIZE)) {
            return -1;
        }
        memcpy(dst, s->calib + addr, len);
        return 0;
    case SINCO_TYPE_GLOBALS:
        if (!in_range(addr, len, SMCPAD_GLOBALS_SIZE)) {
            return -1;
        }
        memcpy(dst, s->globals + addr, len);
        return 0;
    case SINCO_TYPE_PRESETS:
        if (addr == SINCO_PRESET_FLAG_ADDR && len <= 4) {
            memset(dst, 0, len);                /* stock SMC-PAD_005 reads 0 there */
            return 0;
        }
        if (!in_range(addr, len, SMCPAD_PRESETS_SIZE)) {
            return -1;
        }
        memcpy(dst, s->presets + addr, len);
        return 0;
    default:
        return -1;
    }
}

int sinco_store_write(void *ctx, u8 type, u32 addr, const u8 *src, u32 len)
{
    struct sinco_store *s = ctx;
    switch (type) {
    case SINCO_TYPE_CALIB:
        if (len == 0) {
            if (addr == 0) {
                static const u8 tag[SMCPAD_CALIB_TAG_LEN] = SMCPAD_CALIB_TAG;
                return erase_write(s->port, s->calib_flash, s->calib, SMCPAD_CALIB_SIZE,
                                   tag, SMCPAD_CALIB_TAG_LEN);
            }
            if (addr == 1 || addr == 2) {
                s->calib_mode = (addr == 1);
                return 0;
            }
            return -1;
        }
        if (!in_range(addr, len, SMCPAD_CALIB_SIZE)) {
            return -1;
        }
        memcpy(s->calib + addr, src, len);
        return 0;
    case SINCO_TYPE_GLOBALS:
        if (len == 0) {
            return sinco_store_flush_globals(s) < 0 ? -1 : 0;
        }
        if (!in_range(addr, len, SMCPAD_GLOBALS_SIZE)) {
            return -1;
        }
        memcpy(s->globals + addr, src, len);
        sinco_globals_sanitize(s->globals, addr, addr + len - 1);
        s->globals_dirty = 1;
        s->globals_gen++;
        return 0;
    case SINCO_TYPE_PRESETS:
        if (len == 0) {
            if (addr != 0) {
                return -1;
            }
            if (erase_write(s->port, s->presets_flash, s->presets, SMCPAD_PRESETS_SIZE, NULL, 0) != 0) {
                return -1;
            }
            s->presets_dirty = 0;
            s->presets_from_flash = (u8)presets_plausible(s->presets);
            return 0;
        }
        if (addr == SINCO_PRESET_FLAG_ADDR && len <= 4) {
            return 0;                           /* stock mode flag (G+0x23, LED/mode toggle?): ignored */
        }
        if (!in_range(addr, len, SMCPAD_PRESETS_SIZE)) {
            return -1;
        }
        memcpy(s->presets + addr, src, len);
        s->presets_dirty = 1;
        s->presets_gen++;
        return 0;
    default:                                    /* type 0 (raw flash) is read-only */
        return -1;
    }
}
