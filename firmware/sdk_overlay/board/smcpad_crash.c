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

static struct smcpad_crash_rec *rec;        /* survives the reset (best effort) */
static struct smcpad_crash_rec last;        /* what this boot found */
static u8 last_valid;
static struct smcpad_boot_hist hist;

static u32 rec_sum(const struct smcpad_crash_rec *r)
{
    return r->magic ^ r->reti ^ r->rets ^ r->msg ^ r->emu ^ r->mmu ^ r->wrl ^ r->ms ^ r->task ^ 0x5AA5C33Cu;
}

___interrupt
static void smcpad_crash_isr(void)
{
    struct smcpad_crash_rec *r = rec;
    if (r) {
        u32 reti, rets;
        __asm__ volatile("%0 = reti" : "=r"(reti));
        __asm__ volatile("%0 = rets" : "=r"(rets));
        r->reti = reti;
        r->rets = rets;
        r->msg = JL_DEBUG->MSG;
        r->emu = q32DSP(0)->EMU_MSG;
        r->mmu = JL_DEBUG->PRP_MMU_MSG;
        r->wrl = JL_DEBUG->PRP_WR_LIMIT_MSG;
        r->ms = smcpad_hw_ms();
        u32 tn = 0;
        const char *t = os_current_task();
        for (u8 i = 0; t && i < 4 && t[i]; i++) {
            tn |= (u32)(u8)t[i] << (8 * i);
        }
        r->task = tn;
        r->magic = CRASH_MAGIC;
        r->check = rec_sum(r);
    }
    chip_reset();
    while (1) {
    }
}

void smcpad_crash_init(void)
{
    rec = (struct smcpad_crash_rec *)malloc(sizeof(*rec));
    if (rec && rec->magic == CRASH_MAGIC && rec->check == rec_sum(rec)) {
        last = *rec;
        last_valid = 1;
    }
    if (rec) {
        memset(rec, 0, sizeof(*rec));
    }

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

    request_irq(0, 2, smcpad_crash_isr, 0); /* CPU exceptions: record, then reset */
    request_irq(1, 2, smcpad_crash_isr, 0);
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
