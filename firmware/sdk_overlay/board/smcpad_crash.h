/* SMC-PAD crash forensics (target only).
 *
 * The pad has no log output, so when it restarts by itself we need the reason afterwards.
 * Every boot appends an entry to a ring in VM (id SMCPAD_BOOTS_VM_ID):
 *  - JL_RST->SRC and the P33 reset source (measured: power-on 0x01, soft reset / exception
 *    0x40; 0x04 seen once, unexplained);
 *  - a "breadcrumb" left by the previous run in RAM that survives a warm reset: its uptime,
 *    what the main task was doing (stage), the last knob / button / pad event and the battery
 *    voltage. It lives in the alignment padding at the end of the .boot_info section, just
 *    below the IRQ stack (cpu0_sstack_begin - 16), which nothing else uses.
 * smcdiag.py "boots" shows the ring; "reboot" / "crashtest" produce known resets.
 * (909 also replaced the CPU exception handlers to record faults; that build boot-looped, so
 * the SDK handlers stay.)
 */
#ifndef SMCPAD_CRASH_H
#define SMCPAD_CRASH_H

#include "typedef.h"

#define SMCPAD_BOOT_HIST    8

/* main-task stages (breadcrumb) */
enum { CRUMB_IDLE = 0, CRUMB_PADS, CRUMB_KNOBS, CRUMB_BUTTONS, CRUMB_BT, CRUMB_USB, CRUMB_BLE,
       CRUMB_SEQ, CRUMB_LEDS, CRUMB_RGB, CRUMB_ADC, CRUMB_GLOBALS };
/* last user event kinds */
enum { CRUMB_EV_NONE = 0, CRUMB_EV_KNOB, CRUMB_EV_BUTTON, CRUMB_EV_PAD };

struct smcpad_boot_entry {
    u8  rst;            /* JL_RST->SRC (low byte) */
    u8  pwr;            /* P33 reset source (power_reset_source_dump) */
    u8  prev;           /* 1 = the previous run left a breadcrumb (fields below valid) */
    u8  stage;          /* CRUMB_* the main task was in */
    u32 ms;             /* uptime of the previous run when it ended */
    u8  ev_kind, ev_id; /* last user event of the previous run */
    u8  vbat2;          /* battery voltage / 20 mV (e.g. 210 = 4.20 V), 0 = unknown */
    u8  ev_age;         /* that event's age in 100 ms (255 = older) */
};

struct smcpad_boot_hist {
    u16 boots;          /* boots since the history was created */
    u8  n;              /* entries used, newest first */
    u8  ver;
    struct smcpad_boot_entry e[SMCPAD_BOOT_HIST];
};

struct smcpad_crash_rec {               /* unused since 910 (kept for the diag format) */
    u32 magic, reti, rets, msg, emu, mmu, wrl, ms, task, check;
};

void smcpad_crash_init(void);                               /* early in smcpad_start() */
void smcpad_crumb_stage(u8 stage, u32 now_ms);              /* main task: where am I */
void smcpad_crumb_event(u8 kind, u8 id, u32 now_ms);        /* last user event */
void smcpad_crumb_vbat(u16 vbat_10mv);
const struct smcpad_boot_hist *smcpad_crash_hist(void);
const struct smcpad_crash_rec *smcpad_crash_last(void);     /* NULL = no exception recorded */
void smcpad_crash_reboot(void);                             /* soft reset (diag) */
void smcpad_crash_test(void);                               /* deliberate exception (diag) */

#endif
