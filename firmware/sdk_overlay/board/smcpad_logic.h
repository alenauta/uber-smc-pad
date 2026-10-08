/* SMC-PAD board logic that does not touch hardware (host-testable).
 *
 * Hardware scan tables and pin mappings match stock firmware SMC-PAD_005 (see docs/pinmap.md).
 * The SDK glue that drives the SFRs lives in smcpad_hw.c.
 */
#ifndef SMCPAD_LOGIC_H
#define SMCPAD_LOGIC_H

#ifdef SMCPAD_HOST_TEST
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#define SMCPAD_RAM_CODE
#define SMCPAD_TABLE        const
#else
#include "typedef.h"
/* Code and tables used from the pad / key-matrix IRQs must not live in flash: the
 * stock firmware also runs its scan ISRs from RAM (0x1D654, 0x1D8BA, 0x1D97A).
 * Tables need an explicit .data section: the SDK builds with LTO, which otherwise
 * turns never-written arrays into .rodata in flash. */
#define SMCPAD_RAM_CODE     __attribute__((section(".volatile_ram_code")))
#define SMCPAD_TABLE        __attribute__((section(".data")))
#endif
/* header helpers called from RAM ISRs: never let the compiler outline them into flash */
#define SMCPAD_INLINE       static inline __attribute__((always_inline))

#define SMCPAD_NUM_PADS         16
#define SMCPAD_NUM_KNOBS        8
#define SMCPAD_NUM_BUTTONS      10
#define SMCPAD_NUM_RGB          16
#define SMCPAD_KM_STEPS         16      /* key-matrix columns per frame */
#define SMCPAD_BLED_PHASES      3       /* button-LED multiplex commons */
#define SMCPAD_BLED_SEGS        4       /* button-LED multiplex segments */

/* ---- stock tables (docs/pinmap.md) ----------------------------------------------- */
/* mux position (0..7 = mux A on PA4, 8..15 = mux B on PA6) -> logical pad   0x1E16CF0 */
extern SMCPAD_TABLE u8  smcpad_pad_of_mux[SMCPAD_NUM_PADS];
/* key-matrix walking-zero column word sent for step n                       0x1E16914 */
extern SMCPAD_TABLE u16 smcpad_km_column_word[SMCPAD_KM_STEPS];
/* knob k: frame steps of phase A and B, both on row 0 (PB0)                 0x1E166CC */
extern SMCPAD_TABLE u8  smcpad_knob_steps[SMCPAD_NUM_KNOBS][2];
/* button b: frame step, row 1 (PC1)                                         0x1E15DC9 */
extern SMCPAD_TABLE u8  smcpad_button_step[SMCPAD_NUM_BUTTONS];
/* LED number n: (phase << 4) | segment                                     0x1E1659A */
extern SMCPAD_TABLE u8  smcpad_button_led[SMCPAD_NUM_BUTTONS];
/* hardware button b -> its LED number (stock numbers LEDs by function, not by button) */
extern SMCPAD_TABLE u8  smcpad_led_of_button[SMCPAD_NUM_BUTTONS];
/* logical pad LED -> position in the WS2812 chain                           0x1E16D00 */
extern SMCPAD_TABLE u8  smcpad_rgb_chain_pos[SMCPAD_NUM_RGB];

/* ---- key matrix -------------------------------------------------------------------- */
/* A frame is 16 bytes, byte[step] = (PB0 level) | (PC1 level << 1), sampled by the SPI2
 * IRQ exactly as the stock firmware does. Lines are active low. */
SMCPAD_INLINE u8 smcpad_km_closed(const u8 *frame, u8 step, u8 row)
{
    return (u8)(((frame[step & 15] >> (row & 1)) & 1u) == 0);
}
u16  smcpad_km_buttons_raw(const u8 *frame);          /* bit b = button b closed */

/* Quadrature decoder, a re-implementation of the stock SOFT2 decoder (0x1D97A):
 * a sample is accepted when two consecutive frames agree and it differs from the last
 * accepted one; the last three accepted samples are then classified. */
struct smcpad_knob {
    u8  last_raw;       /* previous frame's sample, (A | B << 1) */
    u8  hist;           /* last three accepted samples, 2 bits each, newest in bits 1:0 */
    s32 count;          /* +1 / -1 per valid triple; stock divides by 2 for one step */
};
void smcpad_knob_init(struct smcpad_knob *k, u8 ab);
/* returns -1, 0 or +1 (the change applied to k->count) */
s8   smcpad_knob_sample(struct smcpad_knob *k, u8 ab);
u8   smcpad_knobs_frame(struct smcpad_knob *k, const u8 *frame);   /* knobs that moved */

/* Per-bit integrator debounce: a bit changes after `n` consecutive frames disagree. */
struct smcpad_debounce {
    u16 state;
    u8  cnt[16];
};
/* returns the bits that changed this frame */
u16  smcpad_debounce_step(struct smcpad_debounce *d, u16 raw, u8 n);

/* ---- velocity pads ----------------------------------------------------------------- */
struct smcpad_pad_cfg {
    u16 on_thr;         /* delta above baseline that starts a hit */
    u16 off_thr;        /* delta below which a held pad is released (< on_thr) */
    u16 vel_full;       /* peak delta that maps to velocity 127 */
    u8  peak_frames;    /* frames to keep tracking the peak after on_thr */
    u8  baseline_shift; /* idle baseline EMA: baseline += (raw - baseline) >> shift */
    u8  hold_frames;    /* minimum note length in pad frames (~1.7 ms each) */
    u8  release_frames; /* consecutive frames below off_thr that end a note */
};
extern SMCPAD_TABLE struct smcpad_pad_cfg smcpad_pad_cfg_default;

enum { PAD_IDLE = 0, PAD_RISING, PAD_HELD };
struct smcpad_pad {
    u32 baseline_q4;    /* baseline in 1/16 ADC counts */
    u16 peak;
    u16 pressure;       /* current delta while held, 0 otherwise */
    u8  state;
    u8  frames;         /* RISING: frames since on_thr; HELD: frames since note on (sat.) */
    u8  low;            /* HELD: consecutive frames below off_thr */
};
enum { SMCPAD_EV_NONE = 0, SMCPAD_EV_PAD_ON, SMCPAD_EV_PAD_OFF };
struct smcpad_pad_ev {
    u8 type;
    u8 pad;
    u8 velocity;
};
/* polarity: +1 if a press raises the ADC value, -1 if it lowers it */
void smcpad_pad_init(struct smcpad_pad *p, u16 idle_raw);
s8   smcpad_pad_polarity(const u16 *idle_raw, u8 n);
/* feed one sample; returns an event type (SMCPAD_EV_*) and fills *vel for PAD_ON */
u8   smcpad_pad_sample(struct smcpad_pad *p, const struct smcpad_pad_cfg *c,
                       s8 polarity, u16 raw, u8 *vel);
u8   smcpad_velocity(const struct smcpad_pad_cfg *c, u16 peak);

/* Single-producer / single-consumer event ring (ISR -> task). */
#define SMCPAD_EV_RING  32      /* power of two */
struct smcpad_ev_ring {
    volatile u8 head;           /* written by the producer */
    volatile u8 tail;           /* written by the consumer */
    struct smcpad_pad_ev ev[SMCPAD_EV_RING];
    volatile u16 dropped;
};
u8   smcpad_ring_push(struct smcpad_ev_ring *r, const struct smcpad_pad_ev *e);
u8   smcpad_ring_pop(struct smcpad_ev_ring *r, struct smcpad_pad_ev *e);

/* ---- LEDs -------------------------------------------------------------------------- */
/* WS2812 over SPI at ~6 MHz: one SPI byte per LED bit, 1 = 0xFC, 0 = 0xC0 (stock). */
#define SMCPAD_WS_ONE           0xFC
#define SMCPAD_WS_ZERO          0xC0
#define SMCPAD_WS_LEAD          2       /* zero bytes before the first LED (stock) */
#define SMCPAD_WS_TAIL          2
#define SMCPAD_WS_BUF_LEN       (SMCPAD_WS_LEAD + SMCPAD_NUM_RGB * 24 + SMCPAD_WS_TAIL)
/* rgb[i] = 0x00RRGGBB for logical LED i; brightness 0..255 scales every channel */
void smcpad_ws_encode(u8 *buf, const u32 *rgb, u8 brightness);

/* Button LEDs: for each phase, which segment bits are lit (bit s = segment s). */
void smcpad_bled_phases(u16 on_mask, u8 seg_on[SMCPAD_BLED_PHASES]);

/* ---- MIDI defaults (match the stock factory preset, docs/stock_midi_map.md) -------- */
#define SMCPAD_MIDI_PAD_CH      9       /* channel 10 */
#define SMCPAD_MIDI_PAD_NOTE0   36
#define SMCPAD_MIDI_KNOB_CH     0
#define SMCPAD_MIDI_KNOB_CC0    30
#define SMCPAD_MIDI_BTN_CH      0
#define SMCPAD_MIDI_BTN_CC0     25
/* Stock 'endless as absolute' knob: clamp value + delta to 0..127 */
SMCPAD_INLINE u8 smcpad_knob_value(u8 value, s32 delta)
{
    s32 v = (s32)value + delta;
    return (u8)(v < 0 ? 0 : (v > 127 ? 127 : v));
}

/* ---- safe mode (smcpad_safe.c) ------------------------------------------------------ */
/* A boot counter in VM is incremented at every boot and cleared after SMCPAD_SAFE_CLEAR_MS
 * of uptime. If SMCPAD_SAFE_BOOTS - 1 boots in a row end early (crash loop, watchdog reset, or
 * the user switching the pad off within 5 s), the next boot runs without the SMC-PAD drivers.
 * By hand: on, off within 5 s; on, off within 5 s; on = safe mode.
 * Returns 1 for safe mode; *store = the value to write back. */
#define SMCPAD_SAFE_BOOTS       3
#define SMCPAD_SAFE_CLEAR_MS    5000
SMCPAD_INLINE u8 smcpad_safe_step(u8 stored, u8 *store)
{
    u8 n = (u8)((stored >= SMCPAD_SAFE_BOOTS ? 0 : stored) + 1);   /* junk / blank VM -> 0 */
    if (n >= SMCPAD_SAFE_BOOTS) {
        *store = 0;                 /* one safe boot, then normal again */
        return 1;
    }
    *store = n;
    return 0;
}

#endif
