/* SMC-PAD sequencer UI (pure C, host-tested by test/test_smcpad_seq_ui.c).
 *
 * Turns pad / button / knob events into sequencer edits and computes the LED frame
 * (16 pad colours + button LEDs). Pad index 0 = bottom-left, rows go up; in the step view the
 * steps read like text: steps 1-4 = top row (pads 13-16), then 9-12, 5-8, 1-4.
 *
 *   Shift + Note Repeat        enter / leave (seq_ui_toggle); Note Repeat LED flashes on the beat
 *   step view pads             tap = step on / off; hold + knobs = step parameters
 *   play view pads             play the track in its scale from its base note (chromatic = drum
 *                              kit); with Rec on and running they record (same step = chord)
 *   hold step + Note Repeat    ratchet count 1 -> 2 -> 3 -> 4 -> 1
 *   hold Note Repeat           timing page: 1-8 track rate, 9-13 swing, 14 fill (hold),
 *                              15 sync on/off, 16 tap tempo
 *   PAD BANK tap               view step <-> play
 *   hold PAD BANK              track layer (activity, mutes): pad = select track,
 *                              + Stop + pad = mute, + Shift + Stop + pad = clear that track
 *   Left / Right               page; Shift + Left / Right = undo / redo
 *   Play / Stop                (Stop on release; twice = to the start); Rec = live record
 *   KNOB BANK                  knob page: track / pattern (K3 / K4 = track root / scale,
 *                              K6 = track colour)
 *   K8 (track page) / timing   rate = the editing grid: notes keep their timing (seq_set_rate);
 *   pads 1-8                   a coarser grid keeps the notes between its steps (dim white);
 *                              Shift + K8 = old speed change (same steps, faster / slower)
 *   Shift + K3 / K4            global root / scale (shared with normal mode)
 *   Shift + pad                the normal-mode Shift menu (preset, curve, transpose, octave;
 *                              drawn and handled by the caller, see seq_ui_shift_page());
 *                              octave / transpose move the play view (play_oct, play_tr)
 *   hold Shift + Rec           pattern slots, save: pad = save into that slot; released with no
 *                              pad = save into the current slot (918+)
 *   hold Shift + Play          pattern slots, load: pad = load that slot (running: on the next
 *                              bar line). Slot page: off = empty, dim = used, full = current
 */
#ifndef SMCPAD_SEQ_UI_H
#define SMCPAD_SEQ_UI_H

#include "smcpad_seq.h"
#include "smcpad_undo.h"

/* hardware buttons (smcpad_hw_buttons() bit = stock button id) */
enum { SUI_BT = 0, SUI_PAD_BANK, SUI_KNOB_BANK, SUI_LEFT, SUI_RIGHT, SUI_PLAY, SUI_STOP,
       SUI_REC, SUI_SHIFT, SUI_NOTE_REPEAT };
enum { SUI_VIEW_STEP = 0, SUI_VIEW_PLAY, SUI_VIEWS };
enum { SUI_KNOBS_TRACK = 0, SUI_KNOBS_PATTERN };

#define SUI_OVERLAY_MS      1000            /* knob value shown on the pads after the last turn */
#define SUI_FLASH_MS        120             /* pads blink after an undo / redo */
#define SUI_UNDO_IDLE_MS    600             /* edits this far apart are separate undo steps */
#define SUI_UNDO_REC_IDLE_MS 1000           /* while recording: this long without a hit */
#define SUI_DEFAULT_VEL     100

enum { SUI_SLOT_NONE = 0, SUI_SLOT_SAVE, SUI_SLOT_LOAD };
enum { SUI_MARK_BUSY = 0, SUI_MARK_OK, SUI_MARK_FAIL };
#define SUI_MARK_MS         1500            /* slot page: result shown on the tapped pad */

struct seq_ui_io {
    void *ctx;
    void (*midi)(void *ctx, const u8 *m, u32 n);           /* live notes from the play view */
    /* persist tempo, swing, play early, global root / scale */
    void (*save)(void *ctx, const struct seq *s);
    /* pattern slots: op = SUI_SLOT_SAVE / SUI_SLOT_LOAD; the caller updates slot_used /
     * slot_cur and reports with seq_ui_notify() / seq_ui_loaded() */
    void (*slot)(void *ctx, u8 op, u8 slot);
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
    u16 lit_pads;                           /* play-view pads held (white) */
    u8  live_note[17], live_chan[17];       /* notes sounding: pads 0-15, 16 = knob preview */
    s8  play_oct, play_tr;                  /* play view: octaves / scale steps (Shift menu) */
    u8  colour[16];                         /* palette index per track (KNOB BANK K6), saved */
    /* note-length recording: the step a held play-view pad wrote (0xFF = none) */
    u8  rec_step[16], rec_trk[16];
    u32 rec_pos[16];
    /* knob value overlay */
    u32 ov_until;
    u8  ov_kind;                            /* SUI_OV_* */
    u8  ov_colour;
    s16 ov_val, ov_min, ov_max;
    /* tap tempo */
    u32 tap[4];
    u8  ntap;
    /* track-layer hit flashes */
    u32 hit_ms[16];
    /* undo: optional journal over the pattern (NULL = off) */
    struct undo *undo;
    u8  dirty;                              /* something may have changed since the commit */
    u32 act_ms;                             /* last pad / knob / button event */
    /* pattern slots (caller-owned state: slot_used, slot_cur) */
    u8  slot_page;                          /* SUI_SLOT_*: page shown while Rec / Play held */
    u8  slot_picked;                        /* a pad was used on the page */
    u8  slot_cur;                           /* current slot 0..15 */
    u16 slot_used;                          /* bit n: slot n holds a pattern */
    u16 slot_pads;                          /* pads pressed on the page (their release is eaten) */
    u8  slot_mark, slot_mark_state;         /* tapped slot (0xFF none): SUI_MARK_* */
    u32 slot_mark_ms;                       /* when the result came */
};

/* PICK: one pad per choice from pad 1 (ov_min = value of pad 1); a value below ov_min ("follow")
 * leaves every pad dark */
enum { SUI_OV_NONE = 0, SUI_OV_BAR, SUI_OV_SIGNED, SUI_OV_KEYS, SUI_OV_COUNT, SUI_OV_FLASH,
       SUI_OV_PICK, SUI_OV_COLOUR };

void seq_ui_init(struct seq_ui *u, struct seq *s, const struct seq_ui_io *io);
void seq_ui_toggle(struct seq_ui *u);                       /* enter / leave sequencer mode */
void seq_ui_button(struct seq_ui *u, u8 hw, u8 pressed, u32 now_ms);
void seq_ui_pad(struct seq_ui *u, u8 pad, u8 on, u8 vel, u32 now_ms);
void seq_ui_knob(struct seq_ui *u, u8 knob, s32 steps, u32 now_ms);
/* LED frame: rgb[16] = 0x00RRGGBB per pad, returns the button LED mask (bit = hw button).
 * Also commits pending edits to the undo journal once they settle. */
u16  seq_ui_leds(struct seq_ui *u, u32 now_ms, u32 rgb[16]);
u32  seq_ui_track_rgb(const struct seq_ui *u, u8 track);     /* the track's colour */
void seq_ui_default_colours(u8 colour[16]);
u8   seq_ui_step_pad(u8 i);         /* step i (0-15 on a page) <-> pad; its own inverse */
u8   seq_ui_beat_led(const struct seq *s, u32 now_ms);       /* 1 = beat flash on */
u8   seq_ui_undo_skip(u16 off);     /* undo_init() skip rule over struct seq_pattern: mutes */
/* 1 = Shift is held with nothing else claiming the pads: the caller shows / runs the Shift menu */
u8   seq_ui_shift_page(const struct seq_ui *u);
s8   seq_ui_play_offset(const struct seq_ui *u, u8 pad);    /* play-view pad -> scale steps */
/* result of a slot operation: ok = white flash, else red flash */
void seq_ui_notify(struct seq_ui *u, u8 ok, u32 now_ms);
/* a different pattern is now in place: held steps, pages, recording state and undo restart */
void seq_ui_loaded(struct seq_ui *u);

#endif
