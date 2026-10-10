/* SMC-PAD sequencer UI (pure C, host-tested by test/test_smcpad_seq_ui.c).
 *
 * Turns pad / button / knob events into sequencer edits and computes the LED frame
 * (16 pad colours + button LEDs). Pad index 0 = bottom-left, rows go up; in the step view the
 * steps read like text: steps 1-4 = top row (pads 13-16), then 9-12, 5-8, 1-4.
 *
 *   Shift + Note Repeat        enter / leave (seq_ui_toggle); Note Repeat LED flashes on the beat
 *   step view pads             tap = step on / off; hold + knobs = step parameters
 *   hold step + Note Repeat    ratchet count 1 -> 2 -> 3 -> 4 -> 1
 *   hold Note Repeat           timing page: 1-8 track rate, 9-13 swing, 14 fill (hold),
 *                              15 sync on/off, 16 tap tempo
 *   PAD BANK tap / hold + pad  view step -> kit -> keyboard / select track
 *   Left / Right               page; Shift + Left / Right = undo / redo
 *   Play / Stop                (Stop on release; twice = to the start); Rec = live record
 *   Stop + pad (kit view)      mute (playback continues); Shift + Stop + pad = clear that track
 *   KNOB BANK                  knob page: track / pattern
 */
#ifndef SMCPAD_SEQ_UI_H
#define SMCPAD_SEQ_UI_H

#include "smcpad_seq.h"
#include "smcpad_undo.h"

/* hardware buttons (smcpad_hw_buttons() bit = stock button id) */
enum { SUI_BT = 0, SUI_PAD_BANK, SUI_KNOB_BANK, SUI_LEFT, SUI_RIGHT, SUI_PLAY, SUI_STOP,
       SUI_REC, SUI_SHIFT, SUI_NOTE_REPEAT };
enum { SUI_VIEW_STEP = 0, SUI_VIEW_KIT, SUI_VIEW_KEYS, SUI_VIEWS };
enum { SUI_KNOBS_TRACK = 0, SUI_KNOBS_PATTERN };

#define SUI_OVERLAY_MS      1000            /* knob value shown on the pads after the last turn */
#define SUI_FLASH_MS        120             /* pads blink after an undo / redo */
#define SUI_UNDO_IDLE_MS    600             /* edits this far apart are separate undo steps */
#define SUI_DEFAULT_VEL     100

struct seq_ui_io {
    void *ctx;
    void (*midi)(void *ctx, const u8 *m, u32 n);           /* live notes from kit / keyboard */
    void (*save)(void *ctx, u32 centibpm, u8 swing, u8 lead_ms);  /* persist (globals) */
};

struct seq_ui {
    struct seq *seq;
    const struct seq_ui_io *io;
    u8  active;
    u8  view, track, page, knob_page, rec;
    u8  at_start;                           /* Play starts from the top (else continues) */
    u8  shift, nr, padbank, stop;           /* buttons held */
    u8  nr_used, padbank_used, stop_used;   /* a combination happened while held */
    u16 held;                               /* step-view pads held */
    u16 keep;                               /* held pads that must not toggle on release */
    u16 lit_pads;                           /* kit / keyboard pads held (white) */
    u8  live_note[17], live_chan[17];       /* notes sounding: pads 0-15, 16 = knob preview */
    /* knob value overlay */
    u32 ov_until;
    u8  ov_kind;                            /* SUI_OV_* */
    u8  ov_colour;
    s16 ov_val, ov_min, ov_max;
    /* tap tempo */
    u32 tap[4];
    u8  ntap;
    /* kit-view hit flashes */
    u32 hit_ms[16];
    /* undo: optional journal over the pattern (NULL = off) */
    struct undo *undo;
    u8  dirty;                              /* something may have changed since the commit */
    u32 act_ms;                             /* last pad / knob / button event */
};

enum { SUI_OV_NONE = 0, SUI_OV_BAR, SUI_OV_SIGNED, SUI_OV_KEYS, SUI_OV_SCALE, SUI_OV_COUNT, SUI_OV_FLASH,
       SUI_OV_PICK };

void seq_ui_init(struct seq_ui *u, struct seq *s, const struct seq_ui_io *io);
void seq_ui_toggle(struct seq_ui *u);                       /* enter / leave sequencer mode */
void seq_ui_button(struct seq_ui *u, u8 hw, u8 pressed, u32 now_ms);
void seq_ui_pad(struct seq_ui *u, u8 pad, u8 on, u8 vel, u32 now_ms);
void seq_ui_knob(struct seq_ui *u, u8 knob, s32 steps, u32 now_ms);
/* LED frame: rgb[16] = 0x00RRGGBB per pad, returns the button LED mask (bit = hw button).
 * Also commits pending edits to the undo journal once they settle. */
u16  seq_ui_leds(struct seq_ui *u, u32 now_ms, u32 rgb[16]);
u32  seq_ui_track_rgb(u8 track);                             /* palette */
u8   seq_ui_step_pad(u8 i);         /* step i (0-15 on a page) <-> pad; its own inverse */
u8   seq_ui_beat_led(const struct seq *s, u32 now_ms);       /* 1 = beat flash on */
u8   seq_ui_undo_skip(u16 off);     /* undo_init() skip rule over struct seq_pattern: mutes */

#endif
