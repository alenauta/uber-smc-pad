/* SMC-PAD crash forensics (target only).
 *
 * The pad has no log output, so when it restarts by itself we need the reason afterwards.
 * Every boot appends an entry to a ring in VM (id SMCPAD_BOOTS_VM_ID):
 *  - JL_RST->SRC and the P33 reset source (measured: power-on 0x01, soft reset / exception
 *    0x40; 0x04 seen in sequencer mode, probably the watchdog);
 *  - a "breadcrumb" left by the previous run in RAM that survives a warm reset: its uptime,
 *    what the main task was doing (stage), the last knob / button / pad event and the battery
 *    voltage. It lives in its own NOLOAD section .smcpad_keep just below the heap (apply.py
 *    patches cpu/br25/sdk_ld.c), which the startup code never zeroes. (Until 913 it sat at
 *    cpu0_sstack_begin - 16, which turned out to be wiped at boot.)
 * smcdiag.py "boots" shows the ring; "reboot" / "crashtest" produce known resets.
 * (909 also replaced the CPU exception handlers to record faults; that build boot-looped, so
 * the SDK handlers stay.) Since 916 the SDK's weak hook exception_analyze_release() (cpu.a
 * debug.c, called by its exception handler; default = chip_reset) is overridden instead: it
 * stores the cause and return addresses in .smcpad_keep, then resets as before.
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
    u8  crash;          /* 1 = the previous run ended in a CPU exception (hist.crash) */
    u8  rsv[3];
};

/* what the exception hook saw (registers as read in the SDK's exception handler) */
struct smcpad_crash_rec {
    u32 magic;
    u32 emu_msg;        /* q32DSP EMU_MSG: CPU exception cause bits */
    u32 dbg_msg;        /* JL_DEBUG MSG: bus / memory protection cause */
    u32 prp_mmu, prp_wrl;   /* peripheral MMU / write-limit messages */
    u32 rete, reti, rets, retx; /* exception / interrupt / call / ... return addresses */
    u32 psr, sp;
    u32 etm[4];         /* last branch PCs (if the trace is running) */
    u32 ms;             /* breadcrumb uptime when it happened */
    u8  stage, rsv[3];  /* breadcrumb stage */
    char task[12];      /* running task */
};

struct smcpad_boot_hist {
    u16 boots;          /* boots since the history was created */
    u8  n;              /* entries used, newest first */
    u8  ver;
    struct smcpad_boot_entry e[SMCPAD_BOOT_HIST];
    struct smcpad_crash_rec crash;          /* the latest exception (magic 0 = none yet) */
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
