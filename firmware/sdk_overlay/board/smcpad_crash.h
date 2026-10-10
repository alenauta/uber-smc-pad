/* SMC-PAD crash forensics (target only).
 *
 * The pad has no log output, so when it restarts by itself we need the reason afterwards:
 *  - every boot appends {JL_RST->SRC, P33 reset source, crash flag, fault PC} to a small ring
 *    in VM (id SMCPAD_BOOTS_VM_ID), with a boot counter;
 *  - CPU exceptions (irq 0 / 1) go to our handler, which stores the fault registers in a RAM
 *    record (best effort: a heap block that the next boot allocates at the same address, checked
 *    with a magic + sum) and resets. The next boot reports and clears it.
 * smcdiag.py "boots" shows both; "reboot" / "crashtest" produce a soft reset / a test exception
 * so the reset-source codes can be told apart on the hardware.
 */
#ifndef SMCPAD_CRASH_H
#define SMCPAD_CRASH_H

#include "typedef.h"

#define SMCPAD_BOOT_HIST    8

struct smcpad_boot_entry {
    u8  rst;            /* JL_RST->SRC (low byte) */
    u8  pwr;            /* P33 reset source (power_reset_source_dump) */
    u8  crash;          /* 1 = the previous run ended in an exception (record found) */
    u8  rsv;
    u32 pc;             /* fault PC (RETI) when crash = 1 */
};

struct smcpad_boot_hist {
    u16 boots;          /* boots since the history was created */
    u8  n;              /* entries used, newest first */
    u8  ver;
    struct smcpad_boot_entry e[SMCPAD_BOOT_HIST];
};

struct smcpad_crash_rec {
    u32 magic, reti, rets, msg, emu, mmu, wrl, ms, task, check;
};

void smcpad_crash_init(void);                               /* early in smcpad_start() */
const struct smcpad_boot_hist *smcpad_crash_hist(void);
const struct smcpad_crash_rec *smcpad_crash_last(void);     /* NULL = no exception recorded */
void smcpad_crash_reboot(void);                             /* soft reset (diag) */
void smcpad_crash_test(void);                               /* deliberate exception (diag) */

#endif
