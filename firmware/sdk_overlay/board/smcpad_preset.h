/* SMC-PAD preset interpreter (plan Q2 option b): turns pad / button / knob events into MIDI
 * from the active stock preset record, and host MIDI into LED / knob-value feedback.
 *
 * Record layout (docs/preset_format.md), preset = 3539 B:
 *   +0x000 buttons 5 x 23  type, ch, num, v1, v2, sxlen, sx[16], Led
 *   +0x073 knobs  16 x 6   type, speed, ch, num, min, max          record = knob + bank * 8
 *   +0x0D3 pads  128 x 26  type, ch, num, v1, v2, R, G, B, Led, sxlen, sx[16]
 * Behaviour matches stock SMC-PAD_005 MIDI semantics:
 *   - value mapping: x = 0x7F -> byte 4, x = 0 -> byte 3, in between interpolated.
 *     Push / Momentary: press = byte 4 ("Down"), release = byte 3 ("Up"); Toggle: first
 *     press = byte 4; Single = byte 3 (matches the stock capture: factory 0/127 -> 127 press).
 *   - Note: velocity -> curve (globals byte 8) -> scaled into [byte 3, byte 4]; release 8n kk 40.
 *   - Program: Bn 00 MSB, Bn 20 LSB, Cn prog with byte 2 = LSB, byte 3 = MSB (SMC-PAD_005 order,
 *     matches Midi Suite's labels).
 *   - MCP pad / Note button: 90 kk 7F / 90 kk 00 on channel 1.
 *   - knobs: delta * speed table 0x1E15DD4, clamped to [min, max]; the sent value walks one
 *     step at a time to the target (every intermediate value is sent). CW = Right (byte 4) /
 *     Left (byte 5) repeated |delta|. "Note" = Mackie fader: touch 90 68+ch 7F on ch1,
 *     En 00 val, touch release SMCPAD_FADER_RELEASE_MS after the last move (stock: ~200 ms).
 *   - MCP pads, Note buttons and the fader go out through io->midi_mcp (stock: MCP port).
 *   - host 9n kk vv: every pad / button record with Led == kk takes LED state vv;
 *     9F 7F 7F clears all; Bn cc vv sets the matching knob's value (no jump).
 *   - hardware buttons (stock table 0x1E1699C): 3..7 = preset button records 0..4,
 *     1 = pad bank (toggle; momentary in presets 5..8 as stock), 2 = knob bank toggle.
 *     0, 8, 9 are stock system/shift functions: not implemented (no MIDI).
 * Deferred (plan): Comb-MCP pads, Shift functions, note repeat / arp, MCP LED special cases.
 * Pure C, no SDK calls: host-tested by test/test_smcpad_preset.c.
 */
#ifndef SMCPAD_PRESET_H
#define SMCPAD_PRESET_H

#include "smcpad_logic.h"
#include "sinco_store.h"

#define SMCPAD_PRESET_BTNS          5
#define SMCPAD_PRESET_KNOBS         16
#define SMCPAD_PRESET_PADS          128
#define SMCPAD_PRESET_OFF_BTN       0x000
#define SMCPAD_PRESET_OFF_KNOB      0x073
#define SMCPAD_PRESET_OFF_PAD       0x0D3
#define SMCPAD_PRESET_BTN_LEN       23
#define SMCPAD_PRESET_KNOB_LEN      6
#define SMCPAD_PRESET_PAD_LEN       26
#define SMCPAD_PRESET_SX_MAX        16
#define SMCPAD_PRESET_BANK_B_REC    112

enum { PAD_T_NOTE = 0, PAD_T_CC_TOGGLE, PAD_T_CC_MOMENTARY, PAD_T_PROGRAM, PAD_T_MCP,
       PAD_T_SYSEX, PAD_T_COMB_MCP };
enum { BTN_T_CC_SINGLE = 0, BTN_T_CC_TOGGLE, BTN_T_CC_PUSH, BTN_T_PROGRAM, BTN_T_SYSEX,
       BTN_T_NOTE };
enum { KNOB_T_CC = 0, KNOB_T_AT, KNOB_T_FADER, KNOB_T_CW };

/* hardware button indices (smcpad_hw_buttons() bit = stock button id) */
#define SMCPAD_HWBTN_BT             0       /* handled by smcpad_app.c (BLE on/off, status LED) */
#define SMCPAD_HWBTN_PAD_BANK       1
#define SMCPAD_HWBTN_KNOB_BANK      2
#define SMCPAD_HWBTN_REC0           3       /* 3..7 -> preset button records 0..4 */
#define SMCPAD_HWBTN_SHIFT          8       /* stock 0x1E1135E: modifier bit 0 of G+0x1D */

#ifndef SMCPAD_FADER_RELEASE_MS
#define SMCPAD_FADER_RELEASE_MS     200     /* measured on stock SMC-PAD_005 (docs/stock_checks.md, check D) */
#endif
#ifndef SMCPAD_KNOB_STEPS_PER_TICK
#define SMCPAD_KNOB_STEPS_PER_TICK  8       /* intermediate values sent per tick and knob */
#endif
#ifndef SMCPAD_KNOB_ECHO_MS
#define SMCPAD_KNOB_ECHO_MS         250     /* ignore host echo while / just after turning a knob */
#endif
#define SMCPAD_PAD_IDLE_SHIFT       3       /* idle pad colour = record colour >> 3 */

struct smcpad_preset_io {
    void *ctx;
    void (*midi)(void *ctx, const u8 *m, u32 n);
    /* Mackie-class messages (MCP pads, Note buttons, fader touch + pitch bend). Stock sends
     * them through 0x1E0FEA2: USB cable 2 ("SMC-PAD-MCP"), and over BLE wrapped as
     * F0 35 59 ss d1 d2 F7. NULL = use midi(). */
    void (*midi_mcp)(void *ctx, const u8 *m, u32 n);
    void (*globals_changed)(void *ctx);     /* a bank button changed globals[] */
    void (*scale_changed)(void *ctx);       /* Shift + K3 / K4 changed root / scale */
};

struct smcpad_preset {
    const struct smcpad_preset_io *io;
    const u8 *presets;                      /* SMCPAD_PRESETS_SIZE */
    u8 *globals;                            /* SMCPAD_GLOBALS_SIZE */
    u16 held;                               /* pads held */
    u8  pad_rec[SMCPAD_NUM_PADS][5];        /* type, ch, num, v1, v2 captured at press */
    u8  pad_active[SMCPAD_NUM_PADS];        /* the press produced something to release */
    u8  pad_toggle[SMCPAD_NUM_PADS];
    u8  btn_rec[SMCPAD_PRESET_BTNS][5];
    u8  btn_toggle[2 * SMCPAD_PRESET_BTNS];
    u8  pad_led[SMCPAD_PRESET_PADS];        /* host LED state per pad record */
    u8  btn_led[SMCPAD_PRESET_BTNS];
    u8  knob_bank;
    u8  knob_target[SMCPAD_PRESET_KNOBS];
    u8  knob_sent[SMCPAD_PRESET_KNOBS];
    u16 knob_moved, knob_busy;              /* bit idx: turned since tick / within echo window */
    u32 knob_turn_ms[SMCPAD_PRESET_KNOBS];
    u8  fader_touch[SMCPAD_PRESET_KNOBS];
    u32 fader_ms[SMCPAD_PRESET_KNOBS];
    u8  at_last, at_ch;
    u8  led_dirty;                          /* pad colours / button LEDs need a refresh */
    u8  shift;                              /* Shift held: pads run the stock Shift functions */
    u16 shift_pads;                         /* pads pressed as Shift functions (no note to end) */
    /* global scale (shared with the sequencer): root 0 = follow the preset (pad 1's note),
     * 1..12 = C..B; scale 0 = the preset's own pad notes, 1.. = smcpad_scale index + 1
     * (1 = chromatic) from pad 1's note at the current octave, transpose in scale steps.
     * Applies to Note pads; channel and velocity range stay the pad's own. */
    u8  root, scale;
    u8  scale_view;                         /* Shift held after K3 (1) / K4 (2): pick display */
};

void smcpad_preset_init(struct smcpad_preset *p, const struct smcpad_preset_io *io,
                        const u8 *presets, u8 *globals);
/* Stock factory "Performance" preset (Midi Suite SPB.bin slot 1, byte-exact). */
void smcpad_preset_factory(u8 *preset);

/* helpers (exposed for the tests) */
u8   smcpad_preset_pad_record(const u8 *globals, u8 pad);         /* 0xFF = out of range */
u8   smcpad_preset_curve(u8 v7, u8 curve);
u8   smcpad_preset_scale(u8 x, u8 a, u8 b);                       /* stock 0x1E0FE62 */
s32  smcpad_preset_speed(s32 delta, u8 speed);                    /* stock 0x1E0FC6C */

/* events */
void smcpad_preset_pad(struct smcpad_preset *p, u8 pad, u8 on, u8 vel7);
void smcpad_preset_pressure(struct smcpad_preset *p, u8 pad, u8 p7);  /* strongest held pad */
void smcpad_preset_button(struct smcpad_preset *p, u8 hwbtn, u8 pressed);
void smcpad_preset_knob(struct smcpad_preset *p, u8 knob, s32 steps);
void smcpad_preset_tick(struct smcpad_preset *p, u32 now_ms);
void smcpad_preset_midi_in(struct smcpad_preset *p, const u8 *msg, u32 len);

/* LEDs */
u32  smcpad_preset_pad_rgb(const struct smcpad_preset *p, u8 pad);   /* 0x00RRGGBB */
u16  smcpad_preset_button_leds(const struct smcpad_preset *p, u16 hw_held);
u8   smcpad_preset_pad_note(const struct smcpad_preset *p, u8 pad, u8 *chan);  /* 0xFF = not Note */
/* the note a Note pad plays with the global scale applied (0xFF = not a Note pad) */
u8   smcpad_preset_scaled_note(const struct smcpad_preset *p, u8 pad, u8 *chan);
/* the preset's own root: pad 1's note at the current octave, 0..11 ("follow preset") */
u8   smcpad_preset_own_root(const struct smcpad_preset *p);
/* the same at the default octave (sequencer: its octave moves the play view by 12 instead) */
#define SMCPAD_PRESET_DEFAULT_OCT   2
u8   smcpad_preset_default_root(const struct smcpad_preset *p);
/* a pad's note at the default octave, no transpose (sequencer track defaults); 0xFF = not Note */
u8   smcpad_preset_default_note(const struct smcpad_preset *p, u8 pad, u8 *chan);

#endif
