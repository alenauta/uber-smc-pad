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
#define HIST_VER                3           /* 3: + crash flag per boot, latest crash record */
#define CRUMB_MAGIC             0x534D4352u /* "SMCR" */
#define CRASH_MAGIC             0x43525348u /* "CRSH" */

extern u8 power_reset_src;                  /* cpu/br25/setup.c, set at boot */
extern void chip_reset(void);
extern const char *os_current_task();

static struct smcpad_boot_hist hist;
/* 4 words: magic, ms, stage|ev|vbat, ev_ms. NOLOAD section placed by sdk_ld.c (apply.py):
 * not zeroed at boot, so the previous run's words are still there after a warm reset */
static volatile u32 crumb_keep[4] __attribute__((section(".smcpad_keep"), aligned(16)));
static volatile struct smcpad_crash_rec crash_keep __attribute__((section(".smcpad_keep"), aligned(4)));
static volatile u32 *crumb;

void smcpad_crash_init(void)
{
    crumb = crumb_keep;

    if (syscfg_read(SMCPAD_BOOTS_VM_ID, (u8 *)&hist, sizeof(hist)) != sizeof(hist) || hist.ver != HIST_VER) {
        memset(&hist, 0, sizeof(hist));
        hist.ver = HIST_VER;
    }
    memmove(&hist.e[1], &hist.e[0], sizeof(hist.e[0]) * (SMCPAD_BOOT_HIST - 1));
    struct smcpad_boot_entry *e = &hist.e[0];
    memset(e, 0, sizeof(*e));
    e->rst = (u8)JL_RST->SRC;
    e->pwr = power_reset_src;
    if (crumb && crumb[0] == CRUMB_MAGIC) {     /* the previous run's last words */
        u32 w2 = crumb[2];
        u32 age = (crumb[1] - crumb[3]) / 100;
        e->prev = 1;
        e->ms = crumb[1];
        e->stage = (u8)w2;
        e->ev_kind = (u8)(w2 >> 8);
        e->ev_id = (u8)(w2 >> 16);
        e->vbat2 = (u8)(w2 >> 24);
        e->ev_age = (u8)(age > 255 ? 255 : age);
    }
    if (crash_keep.magic == CRASH_MAGIC && e->pwr != 0x01) {    /* the exception hook ran */
        for (u32 i = 0; i < sizeof(hist.crash); i++) {
            ((u8 *)&hist.crash)[i] = ((volatile u8 *)&crash_keep)[i];
        }
        e->crash = 1;
    }
    crash_keep.magic = 0;
    if (hist.n < SMCPAD_BOOT_HIST) {
        hist.n++;
    }
    hist.boots++;
    syscfg_write(SMCPAD_BOOTS_VM_ID, (u8 *)&hist, sizeof(hist));

    if (crumb) {
        crumb[1] = 0;
        crumb[2] = 0;
        crumb[3] = 0;
        crumb[0] = CRUMB_MAGIC;
    }
}

static u8 ptr_ok(const void *p)                 /* readable: RAM or flash */
{
    u32 a = (u32)p;
    return (u8)(a < 0x40000 || (a >= 0x1E00000 && a < 0x2000000));
}

/* Overrides the SDK's weak hook (cpu.a debug.c), which its exception handler calls after saving
 * the registers; the default only calls chip_reset(). Runs in exception context: register
 * reads and plain stores only, then the same reset. */
void exception_analyze_release(void)
{
    volatile struct smcpad_crash_rec *c = &crash_keep;
    c->emu_msg = q32DSP(0)->EMU_MSG;
    c->dbg_msg = JL_DEBUG->MSG;
    c->prp_mmu = JL_DEBUG->PRP_MMU_MSG;
    c->prp_wrl = JL_DEBUG->PRP_WR_LIMIT_MSG;
    c->rete = q32DSP(0)->RETE;
    c->reti = q32DSP(0)->RETI;
    c->rets = q32DSP(0)->RETS;
    c->retx = q32DSP(0)->RETX;
    c->psr = q32DSP(0)->PSR;
    c->sp = q32DSP(0)->SP;
    c->etm[0] = q32DSP(0)->ETM_PC0;
    c->etm[1] = q32DSP(0)->ETM_PC1;
    c->etm[2] = q32DSP(0)->ETM_PC2;
    c->etm[3] = q32DSP(0)->ETM_PC3;
    c->ms = crumb_keep[1];
    c->stage = (u8)crumb_keep[2];
    const char *n = os_current_task();
    for (u8 i = 0; i < sizeof(c->task); i++) {
        c->task[i] = (n && ptr_ok(n + i)) ? n[i] : 0;
        if (!c->task[i]) {
            n = NULL;                           /* stop at the end of the name */
        }
    }
    c->magic = CRASH_MAGIC;
    chip_reset();
}

void smcpad_crumb_stage(u8 stage, u32 now_ms)
{
    if (crumb) {
        crumb[1] = now_ms;
        crumb[2] = (crumb[2] & ~0xFFu) | stage;
    }
}

void smcpad_crumb_event(u8 kind, u8 id, u32 now_ms)
{
    if (crumb) {
        crumb[2] = (crumb[2] & 0xFF0000FFu) | ((u32)kind << 8) | ((u32)id << 16);
        crumb[3] = now_ms;
    }
}

void smcpad_crumb_vbat(u16 vbat_10mv)
{
    if (crumb) {
        u32 v = vbat_10mv / 2;
        crumb[2] = (crumb[2] & 0x00FFFFFFu) | ((v > 255 ? 255 : v) << 24);
    }
}

const struct smcpad_boot_hist *smcpad_crash_hist(void)
{
    return &hist;
}

const struct smcpad_crash_rec *smcpad_crash_last(void)
{
    return hist.crash.magic == CRASH_MAGIC ? &hist.crash : NULL;
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
