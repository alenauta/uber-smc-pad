/* SMC-PAD flash / VM port for sinco_store (target only, sdk_overlay).
 *
 * Presets live in the reserved area USRFLASH (0x77000, 28 KB) and the pad calibration in
 * USRTRIM (0x7E000, 4 KB), exactly where the stock firmware keeps them (isd_config.ini;
 * board_smcpad_global_build_cfg.h). The addresses are looked up through the SDK file system
 * at boot (same pattern as custom_cfg.c's EXIF lookup) and must match the stock ones:
 * if they do not, erase/write are refused (reads still work), so a layout mistake can never
 * erase the wrong flash.
 * Erase and write run only in the smcpad task (Sinco commit), never from an IRQ.
 */
#include "app_config.h"

#if defined(CONFIG_BOARD_SMCPAD) && TCFG_SMCPAD_HW_ENABLE

#include "system/includes.h"
#include "syscfg_id.h"
#include "boot.h"
#include "sinco_store.h"
#include "smcpad_patstore.h"
#include "smcpad_store_port.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[SMCPAD-FLASH]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

/* same enum as custom_cfg.c / tuya port: SECTOR_ERASER = 4 KB */
typedef enum _FLASH_ERASER {
    CHIP_ERASER,
    BLOCK_ERASER,
    SECTOR_ERASER,
    PAGE_ERASER,
} FLASH_ERASER;
extern bool sfc_erase(FLASH_ERASER cmd, u32 addr);
extern u32 sfc_write(const u8 *buf, u32 addr, u32 len);
extern u32 sfc_read(u8 *buf, u32 addr, u32 len);
extern u32 sdfile_cpu_addr2flash_addr(u32 offset);

#define USRFLASH_LEN    0x7000u
#define USRTRIM_LEN     0x1000u

static u8 writable;     /* bit 0: USRFLASH confirmed, bit 1: USRTRIM confirmed */

static u32 area_addr(const char *path)
{
    u32 addr = 0;
    FILE *fp = fopen(path, "r");
    if (fp) {
        struct vfs_attr attr = {0};
        fget_attrs(fp, &attr);
        addr = sdfile_cpu_addr2flash_addr(attr.sclust);
        fclose(fp);
    }
    return addr;
}

static int in_area(u32 addr, u32 len, u32 base, u32 size)
{
    return addr >= base && len <= size && addr - base <= size - len;
}

static int may_modify(u32 addr, u32 len)
{
    if ((writable & 1) && in_area(addr, len, SMCPAD_PRESETS_FLASH, USRFLASH_LEN)) {
        return 1;
    }
    if ((writable & 2) && in_area(addr, len, SMCPAD_CALIB_FLASH, USRTRIM_LEN)) {
        return 1;
    }
    log_error("refused flash change @%x+%x", addr, len);
    return 0;
}

static int p_read(void *ctx, u32 addr, u8 *dst, u32 len)
{
    (void)ctx;
    if (addr >= SMCPAD_RAW_FLASH_LIMIT || len > SMCPAD_RAW_FLASH_LIMIT - addr) {
        return -1;
    }
    sfc_read(dst, addr, len);
    return 0;
}

static int p_erase(void *ctx, u32 addr)
{
    (void)ctx;
    if (addr % SINCO_FLASH_SECTOR || !may_modify(addr, SINCO_FLASH_SECTOR)) {
        return -1;
    }
    return sfc_erase(SECTOR_ERASER, addr) ? 0 : -1;
}

static int p_write(void *ctx, u32 addr, const u8 *src, u32 len)
{
    (void)ctx;
    if (!may_modify(addr, len)) {
        return -1;
    }
    return sfc_write(src, addr, len) == len ? 0 : -1;
}

static int p_vm_read(void *ctx, u16 id, u8 *dst, u32 len)
{
    (void)ctx;
    return syscfg_read(id, dst, (u16)len);
}

static int p_vm_write(void *ctx, u16 id, const u8 *src, u32 len)
{
    (void)ctx;
    return syscfg_write(id, (void *)src, (u16)len);
}

static const struct sinco_flash_port port = {
    NULL, p_read, p_erase, p_write, p_vm_read, p_vm_write
};

#if SMCPAD_GLOBALS_VM_ID < CFG_USER_DEFINE_BEGIN || SMCPAD_GLOBALS_VM_ID > CFG_USER_DEFINE_END
#error "SMCPAD_GLOBALS_VM_ID must be a user VM id"
#endif

const struct sinco_flash_port *smcpad_store_port(void)
{
    u32 a = area_addr(SDFILE_APP_ROOT_PATH "USRFLASH");
    u32 t = area_addr(SDFILE_APP_ROOT_PATH "USRTRIM");
    writable = (u8)((a == SMCPAD_PRESETS_FLASH) | ((t == SMCPAD_CALIB_FLASH) << 1));
    log_info("USRFLASH @%x, USRTRIM @%x -> writable %x", a, t, writable);
    return &port;
}

/* ---- sequencer pattern slots (918+): the gap 0x30000-0x4FFFF ------------------------------
 * repack_oldchain.sh pins the code below 0x30000 and the VM at 0x50000 (907+), so the gap is in
 * no JLFS area and updates never touch it. Changes are allowed only if the running image's VM
 * (boot_info, from the flash directory) really starts at or above the end of the gap; with any
 * other layout (e.g. an image that kept the stock one, VM from 0x26000) the slots are refused
 * and nothing is written. test/check_ram_isr.sh checks the code end at build time. */
static u8 pat_writable;

static int pp_read(void *ctx, u32 addr, u8 *dst, u32 len)
{
    (void)ctx;
    if (!in_area(addr, len, PATSTORE_BASE, PATSTORE_END - PATSTORE_BASE)) {
        return -1;
    }
    sfc_read(dst, addr, len);
    return 0;
}

static int pp_erase(void *ctx, u32 addr)
{
    (void)ctx;
    if (!pat_writable || addr % PATSTORE_SECTOR ||
        !in_area(addr, PATSTORE_SECTOR, PATSTORE_BASE, PATSTORE_END - PATSTORE_BASE)) {
        log_error("refused pattern erase @%x", addr);
        return -1;
    }
    return sfc_erase(SECTOR_ERASER, addr) ? 0 : -1;
}

static int pp_write(void *ctx, u32 addr, const u8 *src, u32 len)
{
    (void)ctx;
    if (!pat_writable || !in_area(addr, len, PATSTORE_BASE, PATSTORE_END - PATSTORE_BASE)) {
        log_error("refused pattern write @%x+%x", addr, len);
        return -1;
    }
    return sfc_write(src, addr, len) == len ? 0 : -1;
}

static const struct patstore_port pat_port = {NULL, pp_read, pp_erase, pp_write};

const struct patstore_port *smcpad_pattern_port(void)
{
    u32 vs = boot_info.vm.vm_saddr, vz = boot_info.vm.vm_size;
    pat_writable = (u8)(vs >= PATSTORE_END && vs % 0x1000 == 0 && vz && vs < SMCPAD_PRESETS_FLASH &&
                        vz <= SMCPAD_PRESETS_FLASH - vs);
    log_info("VM @%x+%x -> pattern slots %s", vs, vz, pat_writable ? "writable" : "refused");
    return &pat_port;
}

u8 smcpad_pattern_writable(void)
{
    return pat_writable;
}

u32 smcpad_vm_start(void)
{
    return boot_info.vm.vm_saddr;
}

#endif
