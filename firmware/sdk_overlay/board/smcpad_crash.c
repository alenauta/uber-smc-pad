#include "app_config.h"

#if defined(CONFIG_BOARD_SMCPAD) && TCFG_SMCPAD_HW_ENABLE

#include "system/includes.h"
#include "syscfg_id.h"
#include "smcpad_board.h"
#include "smcpad_crash.h"

#ifndef SMCPAD_BOOTS_VM_ID
#define SMCPAD_BOOTS_VM_ID      46          /* user VM id (globals 44, safe-mode counter 45) */
#endif
#if SMCPAD_BOOTS_VM_ID < CFG_USER_DEFINE_BEGIN || SMCPAD_BOOTS_VM_ID > CFG_USER_DEFINE_END
#error "SMCPAD_BOOTS_VM_ID must be a user VM id"
#endif
#define CRASH_MAGIC             0x43525348u /* "CRSH" */
#define HIST_VER                1

extern u8 power_reset_src;                  /* cpu/br25/setup.c, set at boot */
extern void chip_reset(void);

/* 909 also replaced the CPU exception handlers (irq 0 / 1) to record faults; that build
 * boot-looped into safe mode, so the SDK handler stays. Only the reset sources are kept. */
static struct smcpad_crash_rec last;        /* never filled for now */
static u8 last_valid;
static struct smcpad_boot_hist hist;

void smcpad_crash_init(void)
{
    if (syscfg_read(SMCPAD_BOOTS_VM_ID, (u8 *)&hist, sizeof(hist)) != sizeof(hist) || hist.ver != HIST_VER) {
        memset(&hist, 0, sizeof(hist));
        hist.ver = HIST_VER;
    }
    memmove(&hist.e[1], &hist.e[0], sizeof(hist.e[0]) * (SMCPAD_BOOT_HIST - 1));
    hist.e[0].rst = (u8)JL_RST->SRC;
    hist.e[0].pwr = power_reset_src;
    hist.e[0].crash = last_valid;
    hist.e[0].rsv = 0;
    hist.e[0].pc = last_valid ? last.reti : 0;
    if (hist.n < SMCPAD_BOOT_HIST) {
        hist.n++;
    }
    hist.boots++;
    syscfg_write(SMCPAD_BOOTS_VM_ID, (u8 *)&hist, sizeof(hist));
}

const struct smcpad_boot_hist *smcpad_crash_hist(void)
{
    return &hist;
}

const struct smcpad_crash_rec *smcpad_crash_last(void)
{
    return last_valid ? &last : NULL;
}

void smcpad_crash_reboot(void)
{
    chip_reset();
}

void smcpad_crash_test(void)
{
    volatile u32 zero = 0, one = 1;
    volatile u32 r = one / zero;            /* div0 exception */
    (void)r;
}

#endif /* CONFIG_BOARD_SMCPAD && TCFG_SMCPAD_HW_ENABLE */
