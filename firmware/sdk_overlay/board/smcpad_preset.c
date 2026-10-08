/* SMC-PAD preset interpreter, see smcpad_preset.h for the behaviour and its evidence. */
#include "smcpad_preset.h"
#include "smcpad_scale.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

/* knob speed multipliers, stock table 0x1E15DD4 (u32[5][5], index min(|delta|, 5) - 1) */
static const u8 knob_speed_mul[5][5] = {
    {1, 1, 1, 1, 1}, {1, 1, 2, 2, 3}, {1, 2, 2, 3, 4}, {1, 2, 3, 3, 4}, {1, 2, 3, 4, 5},
};

/* SPB.bin slot 1 pad colours, one per 16-record group (records 112..127 = bank B) */
static const u32 factory_pad_rgb[8] = {
    0x96c8f0, 0xf0f000, 0xf000f0, 0x0000f0, 0x00f000, 0xf00000, 0x00f0f0, 0x7878f0,
};

static const u8 *preset_base(const struct smcpad_preset *p)
{
    return p->presets + (u32)(p->globals[GLOB_PRESET] % SMCPAD_PRESET_COUNT) * SMCPAD_PRESET_SIZE;
}

static const u8 *pad_rec_ptr(const struct smcpad_preset *p, u8 rec)
{
    return preset_base(p) + SMCPAD_PRESET_OFF_PAD + (u32)rec * SMCPAD_PRESET_PAD_LEN;
}

static const u8 *btn_rec_ptr(const struct smcpad_preset *p, u8 rec)
{
    return preset_base(p) + SMCPAD_PRESET_OFF_BTN + (u32)rec * SMCPAD_PRESET_BTN_LEN;
}

static const u8 *knob_rec_ptr(const struct smcpad_preset *p, u8 rec)
{
    return preset_base(p) + SMCPAD_PRESET_OFF_KNOB + (u32)rec * SMCPAD_PRESET_KNOB_LEN;
}

static void send(struct smcpad_preset *p, const u8 *m, u32 n)
{
    p->io->midi(p->io->ctx, m, n);
}

static void send3(struct smcpad_preset *p, u8 st, u8 d1, u8 d2)
{
    u8 m[3] = {st, (u8)(d1 & 0x7f), (u8)(d2 & 0x7f)};
    send(p, m, 3);
}

static void send2(struct smcpad_preset *p, u8 st, u8 d1)
{
    u8 m[2] = {st, (u8)(d1 & 0x7f)};
    send(p, m, 2);
}

/* Mackie-class output (stock 0x1E0FEA2 -> MCP port) */
static void send3_mcp(struct smcpad_preset *p, u8 st, u8 d1, u8 d2)
{
    u8 m[3] = {st, (u8)(d1 & 0x7f), (u8)(d2 & 0x7f)};
    if (p->io->midi_mcp) {
        p->io->midi_mcp(p->io->ctx, m, 3);
    } else {
        send(p, m, 3);
    }
}

/* Bn 00 MSB, Bn 20 LSB, Cn prog; rec bytes 2 = LSB, 3 = MSB, 4 = program (005 order) */
static void send_program(struct smcpad_preset *p, u8 ch, u8 lsb, u8 msb, u8 prog)
{
    send3(p, (u8)(0xB0 | (ch & 0x0f)), 0x00, msb);
    send3(p, (u8)(0xB0 | (ch & 0x0f)), 0x20, lsb);
    send2(p, (u8)(0xC0 | (ch & 0x0f)), prog);
}

/* Custom SysEx field: only a well-formed F0 .. F7 message goes out */
static void send_sysex(struct smcpad_preset *p, const u8 *sx, u8 len)
{
    if (len < 2 || len > SMCPAD_PRESET_SX_MAX || sx[0] != 0xF0 || sx[len - 1] != 0xF7) {
        return;
    }
    for (u8 i = 1; i + 1 < len; i++) {
        if (sx[i] & 0x80) {
            return;
        }
    }
    send(p, sx, len);
}

/* ---- helpers ---------------------------------------------------------------------- */
u8 smcpad_preset_pad_record(const u8 *globals, u8 pad)
{
    if (pad >= SMCPAD_NUM_PADS) {
        return 0xFF;
    }
    if (globals[GLOB_BANK]) {
        return (u8)(SMCPAD_PRESET_BANK_B_REC + pad);
    }
    s32 idx = (s32)globals[GLOB_OCTAVE] * 16 + pad + (s32)(s8)globals[GLOB_TRANSPOSE];
    return (idx < 0 || idx >= SMCPAD_PRESET_PADS) ? 0xFF : (u8)idx;   /* stock: no clamp */
}

static u32 isqrt(u32 x)
{
    u32 r = 0, bit = 1u << 14;              /* x < 2^16 here */
    while (bit > x) {
        bit >>= 2;
    }
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Velocity curves (global byte 8, Cube Suite / Midi Suite "1"-"4"). Stock had linear, two
 * raised floors (3v/32+32, v/16+64) and fixed 127; ours keep the 4 slots with real curves:
 *   0 linear      v
 *   1 soft        (sqrt(127 v) + v) / 2   more output for soft hits   (20 -> 35, 64 -> 77)
 *   2 hard        (v^2 / 127 + v) / 2     needs harder hits           (20 -> 12, 64 -> 48)
 *   3 fixed       127 */
u8 smcpad_preset_curve(u8 v7, u8 curve)
{
    u32 v = v7, r;
    switch (curve) {
    case 0:  r = v; break;
    case 1:  r = (isqrt(v * 127u) + v + 1) / 2; break;
    case 2:  r = (v * v / 127u + v + 1) / 2; break;
    case 3:  r = 127; break;
    default: r = v; break;
    }
    return (u8)(r < 1 ? 1 : (r > 127 ? 127 : r));
}

/* stock 0x1E0FE62: x = 0x7F -> b, x = 0 -> a, linear in between (either order) */
u8 smcpad_preset_scale(u8 x, u8 a, u8 b)
{
    u32 r;
    if (b > a) {
        r = a + (((u32)x + 1) * (u32)(b - a) >> 7);
    } else {
        r = b + (((u32)0x80 - x) * (u32)(a - b) >> 7);
    }
    return (u8)(r & 0x7f);
}

s32 smcpad_preset_speed(s32 delta, u8 speed)
{
    if (!delta) {
        return 0;
    }
    s32 mag = delta < 0 ? -delta : delta;
    return delta * knob_speed_mul[speed > 4 ? 4 : speed][(mag > 5 ? 5 : mag) - 1];
}

void smcpad_preset_init(struct smcpad_preset *p, const struct smcpad_preset_io *io,
                        const u8 *presets, u8 *globals)
{
    memset(p, 0, sizeof(*p));
    p->io = io;
    p->presets = presets;
    p->globals = globals;
    p->led_dirty = 1;
}

void smcpad_preset_factory(u8 *preset)
{
    memset(preset, 0, SMCPAD_PRESET_SIZE);
    for (u8 b = 0; b < SMCPAD_PRESET_BTNS; b++) {          /* CC Push 25..29, 0 / 127 */
        u8 *r = preset + SMCPAD_PRESET_OFF_BTN + b * SMCPAD_PRESET_BTN_LEN;
        static const u8 mmc[6] = {0xF0, 0x7F, 0x7F, 0x06, 0x00, 0xF7};
        r[0] = BTN_T_CC_PUSH; r[1] = 0; r[2] = (u8)(25 + b); r[3] = 0; r[4] = 127;
        r[5] = sizeof(mmc);
        memcpy(r + 6, mmc, sizeof(mmc));
        r[10] = (u8)(b + 1);                                  /* MMC command 1..5 */
        r[22] = 0xFF;
    }
    for (u8 k = 0; k < SMCPAD_PRESET_KNOBS; k++) {          /* CC 30..45 ch1, speed normal */
        u8 *r = preset + SMCPAD_PRESET_OFF_KNOB + k * SMCPAD_PRESET_KNOB_LEN;
        r[0] = KNOB_T_CC; r[1] = 2; r[2] = 0; r[3] = (u8)(30 + k); r[4] = 0; r[5] = 127;
    }
    for (u32 i = 0; i < SMCPAD_PRESET_PADS; i++) {          /* Note ch10, record + 4 */
        u8 *r = preset + SMCPAD_PRESET_OFF_PAD + i * SMCPAD_PRESET_PAD_LEN;
        u32 rgb = factory_pad_rgb[i / 16];
        r[0] = PAD_T_NOTE; r[1] = 9;
        r[2] = (u8)(i < SMCPAD_PRESET_BANK_B_REC ? i + 4 : 52 + (i - SMCPAD_PRESET_BANK_B_REC));
        r[3] = 0; r[4] = 127;
        r[5] = (u8)(rgb >> 16); r[6] = (u8)(rgb >> 8); r[7] = (u8)rgb;
        r[8] = 0xFF;
    }
}

/* ---- pads ------------------------------------------------------------------------- */
static void at_release(struct smcpad_preset *p)
{
    if (p->at_last) {
        send2(p, (u8)(0xD0 | p->at_ch), 0);
        p->at_last = 0;
    }
}

static void set_global(struct smcpad_preset *p, u8 idx, u8 v);

/* Shift + pad (matches stock SMC-PAD_005 behaviour). Pad = pad index (pad 1 =
 * index 0 = bottom-left; rows go up). 1 if the top-row function can move further (stock limits). */
static u8 shift_can_move(const struct smcpad_preset *p, u8 pad)
{
    s8 oct = (s8)p->globals[GLOB_OCTAVE];
    s8 tr = (s8)p->globals[GLOB_TRANSPOSE];
    switch (pad) {
    case 12: return (oct == 0 && tr > 0) || (oct >= 1 && tr >= -15);      /* 0x1E10E68 */
    case 13: return (oct <= 5 && tr < 16) || (oct == 6 && tr < 0);        /* 0x1E10E98 */
    case 14: return oct >= 1;                                             /* 0x1E10ECA */
    case 15: return oct <= 5;                                             /* 0x1E10EFC */
    default: return 0;
    }
}

static void shift_pad(struct smcpad_preset *p, u8 pad)
{
    s8 oct = (s8)p->globals[GLOB_OCTAVE];
    s8 tr = (s8)p->globals[GLOB_TRANSPOSE];
    if (pad < 8) {                                        /* 0x1E10D90..: preset 1..8 */
        set_global(p, GLOB_PRESET, pad);
    } else if (pad < 12) {                                /* 0x1E10E20..: curve 0..3 */
        set_global(p, GLOB_PAD_CURVE, (u8)(pad - 8));
    } else if (!shift_can_move(p, pad)) {
        return;
    } else if (pad == 12) {
        set_global(p, GLOB_TRANSPOSE, (u8)(tr - 1));
    } else if (pad == 13) {
        set_global(p, GLOB_TRANSPOSE, (u8)(tr + 1));
    } else if (pad == 14) {
        set_global(p, GLOB_OCTAVE, (u8)(oct - 1));
        if (oct - 1 == 0 && tr < 0) {
            set_global(p, GLOB_TRANSPOSE, 0);
        }
    } else {
        set_global(p, GLOB_OCTAVE, (u8)(oct + 1));
        if (oct + 1 == 6 && tr > 0) {
            set_global(p, GLOB_TRANSPOSE, 0);
        }
    }
}

void smcpad_preset_pad(struct smcpad_preset *p, u8 pad, u8 on, u8 vel7)
{
    if (pad >= SMCPAD_NUM_PADS) {
        return;
    }
    p->led_dirty = 1;
    if (on && p->shift && !p->pad_active[pad]) {
        /* stock (manual): Shift + Pad 15 + Pad 16 = default octave range */
        if ((pad == 14 && (p->shift_pads & (1u << 15))) || (pad == 15 && (p->shift_pads & (1u << 14)))) {
            set_global(p, GLOB_OCTAVE, 2);
            set_global(p, GLOB_TRANSPOSE, 0);
            p->shift_pads |= (u16)(1u << pad);
            return;
        }
        p->shift_pads |= (u16)(1u << pad);
        shift_pad(p, pad);
        return;
    }
    if (!on && (p->shift_pads & (1u << pad))) {
        p->shift_pads &= (u16)~(1u << pad);               /* its press was a Shift function */
        return;
    }
    if (!on) {
        if (!p->pad_active[pad]) {
            return;
        }
        const u8 *c = p->pad_rec[pad];
        u8 ch = c[1] & 0x0f;
        p->pad_active[pad] = 0;
        p->held &= (u16)~(1u << pad);
        if (!p->held) {
            at_release(p);                   /* stock: AT 0 before the last Note Off */
        }
        if (c[0] == PAD_T_NOTE) {
            send3(p, (u8)(0x80 | ch), c[2], 0x40);
        } else if (c[0] == PAD_T_CC_MOMENTARY) {
            send3(p, (u8)(0xB0 | ch), c[2], smcpad_preset_scale(0, c[3], c[4]));
        } else if (c[0] == PAD_T_MCP) {
            send3_mcp(p, 0x90, c[2], 0);
        }
        return;
    }

    u8 rec = smcpad_preset_pad_record(p->globals, pad);
    if (rec == 0xFF || p->pad_active[pad]) {
        return;
    }
    const u8 *r = pad_rec_ptr(p, rec);
    u8 ch = r[1] & 0x0f;
    memcpy(p->pad_rec[pad], r, 5);
    switch (r[0]) {
    case PAD_T_NOTE: {
        u8 v = smcpad_preset_scale(smcpad_preset_curve(vel7, p->globals[GLOB_PAD_CURVE]), r[3], r[4]);
        u8 n = smcpad_preset_scaled_note(p, pad, NULL);
        p->pad_rec[pad][2] = n == 0xFF ? r[2] : n;          /* the release ends this note */
        send3(p, (u8)(0x90 | ch), p->pad_rec[pad][2], v ? v : 1);
        p->pad_active[pad] = 1;
        break;
    }
    case PAD_T_CC_TOGGLE:
        p->pad_toggle[pad] ^= 1;
        send3(p, (u8)(0xB0 | ch), r[2], smcpad_preset_scale(p->pad_toggle[pad] ? 0x7f : 0, r[3], r[4]));
        break;
    case PAD_T_CC_MOMENTARY:
        send3(p, (u8)(0xB0 | ch), r[2], smcpad_preset_scale(0x7f, r[3], r[4]));
        p->pad_active[pad] = 1;
        break;
    case PAD_T_PROGRAM:
        send_program(p, ch, r[2], r[3], r[4]);
        break;
    case PAD_T_MCP:
        send3_mcp(p, 0x90, r[2], 0x7f);
        p->pad_active[pad] = 1;
        break;
    case PAD_T_SYSEX:
        send_sysex(p, r + 10, r[9]);
        break;
    default:                                 /* Comb-MCP: deferred */
        break;
    }
    if (p->pad_active[pad]) {
        p->held |= (u16)(1u << pad);
    }
}

void smcpad_preset_pressure(struct smcpad_preset *p, u8 pad, u8 p7)
{
    if (pad >= SMCPAD_NUM_PADS || !p->pad_active[pad] || p->pad_rec[pad][0] != PAD_T_NOTE ||
        !p->globals[GLOB_AFTERTOUCH]) {
        return;
    }
    const u8 *c = p->pad_rec[pad];
    u8 curve = p->globals[GLOB_PAD_CURVE] == 3 ? 0 : p->globals[GLOB_PAD_CURVE];  /* fixed: AT linear */
    u8 v = p7 ? smcpad_preset_scale(smcpad_preset_curve(p7, curve), c[3], c[4]) : 0;
    u8 ch = c[1] & 0x0f;
    if (v != p->at_last || ch != p->at_ch) {
        if (ch != p->at_ch) {
            at_release(p);
        }
        p->at_ch = ch;
        p->at_last = v;
        send2(p, (u8)(0xD0 | ch), v);
    }
}

/* ---- buttons ---------------------------------------------------------------------- */
static void set_global(struct smcpad_preset *p, u8 idx, u8 v)
{
    if (p->globals[idx] != v) {
        p->globals[idx] = v;
        p->led_dirty = 1;
        if (p->io->globals_changed) {
            p->io->globals_changed(p->io->ctx);
        }
    }
}

void smcpad_preset_button(struct smcpad_preset *p, u8 hwbtn, u8 pressed)
{
    p->led_dirty = 1;
    if (hwbtn == SMCPAD_HWBTN_PAD_BANK) {                 /* stock 0x1E11182 */
        u8 momentary = p->globals[GLOB_PRESET] > 3;
        if (pressed) {
            set_global(p, GLOB_BANK, momentary ? 1 : (u8)!p->globals[GLOB_BANK]);
        } else if (momentary) {
            set_global(p, GLOB_BANK, 0);
        }
        return;
    }
    if (hwbtn == SMCPAD_HWBTN_KNOB_BANK) {                /* stock 0x1E111C6 */
        if (pressed) {
            p->knob_bank ^= 1;
        }
        return;
    }
    if (hwbtn == SMCPAD_HWBTN_SHIFT) {                    /* stock 0x1E1135E */
        p->shift = pressed ? 1 : 0;
        p->scale_view = 0;
        return;
    }
    if (hwbtn < SMCPAD_HWBTN_REC0 || hwbtn >= SMCPAD_HWBTN_REC0 + SMCPAD_PRESET_BTNS) {
        return;                                           /* system / shift: not implemented */
    }
    u8 b = (u8)(hwbtn - SMCPAD_HWBTN_REC0);
    if (!pressed) {
        const u8 *c = p->btn_rec[b];
        if (c[0] == BTN_T_CC_PUSH) {
            send3(p, (u8)(0xB0 | (c[1] & 0x0f)), c[2], smcpad_preset_scale(0, c[3], c[4]));
        } else if (c[0] == BTN_T_NOTE) {
            send3_mcp(p, 0x90, c[2], 0);
        }
        p->btn_rec[b][0] = 0xFF;                          /* nothing pending */
        return;
    }
    const u8 *r = btn_rec_ptr(p, b);
    u8 ch = r[1] & 0x0f;
    u8 num = (u8)((r[2] & 0x7f) ^ (p->shift ? 0x40 : 0));
    u8 ti = (u8)(b + (p->shift ? SMCPAD_PRESET_BTNS : 0));
    memcpy(p->btn_rec[b], r, 5);
    p->btn_rec[b][2] = num;
    switch (r[0]) {
    case BTN_T_CC_SINGLE:
        send3(p, (u8)(0xB0 | ch), num, r[3]);
        break;
    case BTN_T_CC_TOGGLE:
        p->btn_toggle[ti] ^= 1;
        send3(p, (u8)(0xB0 | ch), num, smcpad_preset_scale(p->btn_toggle[ti] ? 0x7f : 0, r[3], r[4]));
        break;
    case BTN_T_CC_PUSH:
        send3(p, (u8)(0xB0 | ch), num, smcpad_preset_scale(0x7f, r[3], r[4]));
        break;
    case BTN_T_PROGRAM:
        send_program(p, ch, r[2], r[3], (u8)((r[4] & 0x7f) ^ (p->shift ? 0x40 : 0)));
        break;
    case BTN_T_SYSEX:
        send_sysex(p, r + 6, r[5]);
        break;
    case BTN_T_NOTE:
        send3_mcp(p, 0x90, num, 0x7f);
        break;
    default:
        break;
    }
}

/* ---- knobs ------------------------------------------------------------------------ */
static u8 knob_rec_index(const struct smcpad_preset *p, u8 knob)
{
    return (u8)(knob + (p->knob_bank ? SMCPAD_NUM_KNOBS : 0));
}

void smcpad_preset_knob(struct smcpad_preset *p, u8 knob, s32 steps)
{
    if (knob >= SMCPAD_NUM_KNOBS || !steps) {
        return;
    }
    if (p->shift && (knob == 2 || knob == 3)) {           /* Shift + K3 / K4: global root / scale */
        s32 d = steps > 0 ? 1 : -1;
        if (knob == 2) {
            s32 v = (s32)p->root + d;
            p->root = (u8)(v < 0 ? 0 : (v > 12 ? 12 : v));
            p->scale_view = 1;
        } else {
            s32 v = (s32)p->scale + d;
            p->scale = (u8)(v < 0 ? 0 : (v > SCALE_COUNT ? SCALE_COUNT : v));
            p->scale_view = 2;
        }
        p->led_dirty = 1;
        if (p->io->scale_changed) {
            p->io->scale_changed(p->io->ctx);
        }
        return;
    }
    u8 idx = knob_rec_index(p, knob);
    const u8 *r = knob_rec_ptr(p, idx);
    if (r[0] == KNOB_T_CW) {                              /* Right = byte 4, Left = byte 5 */
        s32 n = steps < 0 ? -steps : steps;
        for (s32 i = 0; i < n && i < 16; i++) {
            send3(p, (u8)(0xB0 | (r[2] & 0x0f)), r[3], steps > 0 ? r[4] : r[5]);
        }
        return;
    }
    u8 lo = r[4] < r[5] ? r[4] : r[5], hi = r[4] < r[5] ? r[5] : r[4];
    s32 t = (s32)p->knob_target[idx] + smcpad_preset_speed(steps, r[1]);
    p->knob_target[idx] = (u8)(t < lo ? lo : (t > hi ? hi : t));
    p->knob_moved |= (u16)(1u << idx);
    p->knob_busy |= (u16)(1u << idx);
}

static void knob_send(struct smcpad_preset *p, u8 idx, const u8 *r, u8 v, u32 now)
{
    u8 ch = r[2] & 0x0f;
    switch (r[0]) {
    case KNOB_T_CC:
        send3(p, (u8)(0xB0 | ch), r[3], v);
        break;
    case KNOB_T_AT:
        send2(p, (u8)(0xD0 | ch), v);
        break;
    case KNOB_T_FADER:                                    /* Mackie fader, stock 0x1E10500 */
        if (!p->fader_touch[idx]) {
            p->fader_touch[idx] = (u8)(0x68 + ch + 1);    /* note + 1, 0 = not touched */
            send3_mcp(p, 0x90, (u8)(0x68 + ch), 0x7f);
        }
        send3_mcp(p, (u8)(0xE0 | ch), 0, v);
        p->fader_ms[idx] = now;
        break;
    default:
        break;
    }
}

void smcpad_preset_tick(struct smcpad_preset *p, u32 now_ms)
{
    for (u8 idx = 0; idx < SMCPAD_PRESET_KNOBS; idx++) {
        u16 bit = (u16)(1u << idx);
        const u8 *r = knob_rec_ptr(p, idx);
        if (p->knob_moved & bit) {
            p->knob_moved &= (u16)~bit;
            p->knob_turn_ms[idx] = now_ms;
        }
        if (r[0] != KNOB_T_CW && p->knob_sent[idx] != p->knob_target[idx]) {
            u8 lo = r[4] < r[5] ? r[4] : r[5], hi = r[4] < r[5] ? r[5] : r[4];
            if (p->knob_sent[idx] < lo || p->knob_sent[idx] > hi) {
                p->knob_sent[idx] = p->knob_target[idx];  /* first move into the range */
                knob_send(p, idx, r, p->knob_sent[idx], now_ms);
            } else {
                for (u8 n = 0; n < SMCPAD_KNOB_STEPS_PER_TICK &&
                     p->knob_sent[idx] != p->knob_target[idx]; n++) {
                    p->knob_sent[idx] += p->knob_sent[idx] < p->knob_target[idx] ? 1 : -1;
                    knob_send(p, idx, r, p->knob_sent[idx], now_ms);
                }
            }
            p->knob_turn_ms[idx] = now_ms;
        } else if ((p->knob_busy & bit) && now_ms - p->knob_turn_ms[idx] >= SMCPAD_KNOB_ECHO_MS) {
            p->knob_busy &= (u16)~bit;
        }
        if (p->fader_touch[idx] && now_ms - p->fader_ms[idx] >= SMCPAD_FADER_RELEASE_MS) {
            send3_mcp(p, 0x90, (u8)(p->fader_touch[idx] - 1), 0);
            p->fader_touch[idx] = 0;
        }
    }
}

/* ---- host MIDI in ----------------------------------------------------------------- */
static void knob_set_from_host(struct smcpad_preset *p, u8 idx, const u8 *r, u8 vv)
{
    if ((p->knob_busy & (1u << idx)) || p->knob_sent[idx] != p->knob_target[idx]) {
        return;                                           /* ignore late echo while turning */
    }
    u8 lo = r[4] < r[5] ? r[4] : r[5], hi = r[4] < r[5] ? r[5] : r[4];
    u8 v = vv < lo ? lo : (vv > hi ? hi : vv);
    p->knob_target[idx] = p->knob_sent[idx] = v;
}

void smcpad_preset_midi_in(struct smcpad_preset *p, const u8 *msg, u32 len)
{
    if (len == 7 && msg[0] == 0xF0 && msg[1] == 0x35 && msg[2] == 0x59 && msg[6] == 0xF7) {
        msg += 3;                                         /* BLE-wrapped MCP 3-byte message */
        len = 3;
    }
    if (len == 2 && (msg[0] & 0xf0) == 0xD0) {
        for (u8 idx = 0; idx < SMCPAD_PRESET_KNOBS; idx++) {
            const u8 *r = knob_rec_ptr(p, idx);
            if (r[0] == KNOB_T_AT && (r[2] & 0x0f) == (msg[0] & 0x0f)) {
                knob_set_from_host(p, idx, r, msg[1] & 0x7f);
            }
        }
        return;
    }
    if (len != 3) {
        return;
    }
    u8 st = msg[0] & 0xf0;
    if (msg[0] == 0x9F && msg[1] == 0x7F && msg[2] == 0x7F) {     /* clear all LED states */
        memset(p->pad_led, 0, sizeof(p->pad_led));
        memset(p->btn_led, 0, sizeof(p->btn_led));
        p->led_dirty = 1;
        return;
    }
    if (st == 0x90 || st == 0x80) {
        u8 kk = msg[1], vv = st == 0x90 ? msg[2] : 0;
        for (u8 i = 0; i < SMCPAD_PRESET_PADS; i++) {
            if (pad_rec_ptr(p, i)[8] == kk && p->pad_led[i] != vv) {
                p->pad_led[i] = vv;
                p->led_dirty = 1;
            }
        }
        for (u8 b = 0; b < SMCPAD_PRESET_BTNS; b++) {
            if (btn_rec_ptr(p, b)[22] == kk && p->btn_led[b] != vv) {
                p->btn_led[b] = vv;
                p->led_dirty = 1;
            }
        }
    } else if (st == 0xB0 || st == 0xE0) {
        for (u8 idx = 0; idx < SMCPAD_PRESET_KNOBS; idx++) {
            const u8 *r = knob_rec_ptr(p, idx);
            if ((r[2] & 0x0f) != (msg[0] & 0x0f)) {
                continue;
            }
            if ((st == 0xB0 && r[0] == KNOB_T_CC && r[3] == msg[1]) ||
                (st == 0xE0 && r[0] == KNOB_T_FADER)) {
                knob_set_from_host(p, idx, r, msg[2] & 0x7f);
            }
        }
    }
}

/* ---- LEDs ------------------------------------------------------------------------- */
static u32 rgb_scale(u32 rgb, u32 num, u32 den)
{
    u32 r = ((rgb >> 16) & 0xff) * num / den, g = ((rgb >> 8) & 0xff) * num / den;
    u32 b = (rgb & 0xff) * num / den;
    return (r << 16) | (g << 8) | b;
}

/* Shift view: bottom rows = presets (selected full, others 1/8), row 3 = curves (green, selected
 * full, others a dim gradient), top row = transpose -/+ (blue), octave -/+ (magenta): dim at the
 * default, brighter the further it moved that way, off at the limit. */
#define SHIFT_RGB_PERFORMANCE   0xFF6000            /* preset 1: amber */
#define SHIFT_RGB_DAW           0x00A0FF            /* preset 2: cyan */
#define SHIFT_RGB_USER          0x8000FF            /* presets 3-8 whose pad 1 has no colour: purple */
#define SHIFT_RGB_CURVE         0x00FF30            /* green */
#define SHIFT_RGB_TRANSPOSE     0x0040FF            /* blue */
#define SHIFT_RGB_OCTAVE        0xFF0080            /* magenta */
#define SHIFT_USER_COLOUR_REC   32                  /* pad 1 at the default octave (2 x 16) */

static u32 shift_rgb(const struct smcpad_preset *p, u8 pad)
{
    if (pad < 8) {
        u32 c = pad == 0 ? SHIFT_RGB_PERFORMANCE : SHIFT_RGB_DAW;
        if (pad >= 2) {                     /* user preset: its own pad-1 colour (Cube Suite) */
            const u8 *r = p->presets + (u32)pad * SMCPAD_PRESET_SIZE + SMCPAD_PRESET_OFF_PAD +
                          SHIFT_USER_COLOUR_REC * SMCPAD_PRESET_PAD_LEN;
            c = ((u32)r[5] << 16) | ((u32)r[6] << 8) | r[7];
            if (!c) {
                c = SHIFT_RGB_USER;
            }
        }
        return pad == p->globals[GLOB_PRESET] ? c : rgb_scale(c, 1, 8);
    }
    if (pad < 12) {
        static const u8 lvl[4] = {10, 14, 20, 28};  /* /128: gradient of the unselected ones */
        u8 k = (u8)(pad - 8);
        return k == p->globals[GLOB_PAD_CURVE] ? SHIFT_RGB_CURVE : rgb_scale(SHIFT_RGB_CURVE, lvl[k], 128);
    }
    if (!shift_can_move(p, pad)) {
        return 0;
    }
    s32 oct = (s8)p->globals[GLOB_OCTAVE], tr = (s8)p->globals[GLOB_TRANSPOSE], dist, span;
    switch (pad) {
    case 12: dist = -tr; span = 16; break;
    case 13: dist = tr; span = 16; break;
    case 14: dist = 2 - oct; span = 2; break;               /* default octave 2 */
    default: dist = oct - 2; span = 4; break;
    }
    if (dist < 0) {
        dist = 0;
    }
    u32 lvl = 16 + (u32)(112 * dist / span);                /* 1/8 (idle-pad level) .. full */
    return rgb_scale(pad < 14 ? SHIFT_RGB_TRANSPOSE : SHIFT_RGB_OCTAVE, lvl, 128);
}

#define SCALE_RGB_PICK          0x00FFC0            /* root / scale pick: teal */

/* the scale keyboard's base: pad 1's note at the current octave, before transpose (bank B:
 * its pad 1); 0xFF = not a Note pad */
static u8 scale_base(const struct smcpad_preset *p)
{
    u32 rec = p->globals[GLOB_BANK] ? SMCPAD_PRESET_BANK_B_REC : (u32)p->globals[GLOB_OCTAVE] * 16;
    if (rec >= SMCPAD_PRESET_PADS) {
        return 0xFF;
    }
    const u8 *r = pad_rec_ptr(p, (u8)rec);
    return r[0] == PAD_T_NOTE ? (u8)(r[2] & 0x7f) : 0xFF;
}

static u8 preset_root(const struct smcpad_preset *p)   /* 0 = follow the preset: pad 1's note */
{
    return p->root ? (u8)((p->root - 1) % 12) : smcpad_preset_own_root(p);
}

u8 smcpad_preset_own_root(const struct smcpad_preset *p)
{
    u8 b = scale_base(p);
    return b == 0xFF ? 0 : (u8)(b % 12);
}

u8 smcpad_preset_default_root(const struct smcpad_preset *p)
{
    u8 n = smcpad_preset_default_note(p, 0, NULL);
    return n == 0xFF ? 0 : (u8)(n % 12);
}

u8 smcpad_preset_default_note(const struct smcpad_preset *p, u8 pad, u8 *chan)
{
    if (pad >= SMCPAD_NUM_PADS) {
        return 0xFF;
    }
    const u8 *r = pad_rec_ptr(p, (u8)(SMCPAD_PRESET_DEFAULT_OCT * 16 + pad));
    if (r[0] != PAD_T_NOTE) {
        return 0xFF;
    }
    if (chan) {
        *chan = r[1] & 0x0f;
    }
    return r[2] & 0x7f;
}

/* Shift + K3 / K4 pick, same pads as in the sequencer: no pad lit = follow the preset,
 * pads 1..12 = C..B / chromatic, major, ... blues */
static u32 scale_pick_rgb(const struct smcpad_preset *p, u8 pad)
{
    s32 v = p->scale_view == 1 ? p->root : p->scale;
    s32 hi = p->scale_view == 1 ? 12 : SCALE_COUNT;
    s32 idx = (s32)pad + 1;
    if (!v || idx > hi) {
        return 0;
    }
    return idx == v ? SCALE_RGB_PICK : rgb_scale(SCALE_RGB_PICK, 1, 12);
}

u32 smcpad_preset_pad_rgb(const struct smcpad_preset *p, u8 pad)
{
    if (p->shift) {
        if (pad >= SMCPAD_NUM_PADS) {
            return 0;
        }
        return p->scale_view ? scale_pick_rgb(p, pad) : shift_rgb(p, pad);
    }
    u8 rec = smcpad_preset_pad_record(p->globals, pad);
    if (rec == 0xFF) {
        return 0;
    }
    const u8 *r = pad_rec_ptr(p, rec);
    u32 rgb = ((u32)r[5] << 16) | ((u32)r[6] << 8) | r[7];
    u32 lit = rgb ? rgb : 0x606060;
    if (p->held & (1u << pad)) {
        return lit;
    }
    if (p->pad_led[rec]) {
        return rgb_scale(lit, (u32)p->pad_led[rec] + 1, 128);
    }
    if (p->scale >= 2) {                                    /* a scale: root pads brighter */
        u8 n = smcpad_preset_scaled_note(p, pad, NULL);
        if (n != 0xFF && n % 12 == preset_root(p)) {
            return rgb_scale(lit, 1, 2);
        }
    }
    return rgb_scale(rgb, 1, 1u << SMCPAD_PAD_IDLE_SHIFT);
}

u16 smcpad_preset_button_leds(const struct smcpad_preset *p, u16 hw_held)
{
    u16 m = hw_held;
    if (p->globals[GLOB_BANK]) {
        m |= 1u << SMCPAD_HWBTN_PAD_BANK;
    }
    if (p->knob_bank) {
        m |= 1u << SMCPAD_HWBTN_KNOB_BANK;
    }
    for (u8 b = 0; b < SMCPAD_PRESET_BTNS; b++) {
        u8 type = btn_rec_ptr(p, b)[0];
        u8 ti = (u8)(b + (p->shift ? SMCPAD_PRESET_BTNS : 0));
        if ((type == BTN_T_CC_TOGGLE && p->btn_toggle[ti]) || p->btn_led[b]) {
            m |= (u16)(1u << (SMCPAD_HWBTN_REC0 + b));
        }
    }
    return m;
}

/* the note a pad plays in the active preset (sequencer track defaults); 0xFF = not a Note pad */
u8 smcpad_preset_pad_note(const struct smcpad_preset *p, u8 pad, u8 *chan)
{
    u8 rec = smcpad_preset_pad_record(p->globals, pad);
    if (rec == 0xFF) {
        return 0xFF;
    }
    const u8 *r = pad_rec_ptr(p, rec);
    if (r[0] != PAD_T_NOTE) {
        return 0xFF;
    }
    if (chan) {
        *chan = r[1] & 0x0f;
    }
    return r[2] & 0x7f;
}

/* global scale on Note pads: chromatic / a scale counted from pad 1's note at the current
 * octave; transpose moves in scale steps (= semitones when chromatic). Pad 1 not a Note pad, or
 * scale 0 = the pad's own note */
u8 smcpad_preset_scaled_note(const struct smcpad_preset *p, u8 pad, u8 *chan)
{
    u8 own = smcpad_preset_pad_note(p, pad, chan);
    if (own == 0xFF || !p->scale || p->scale > SCALE_COUNT) {
        return own;
    }
    u8 base = scale_base(p);
    if (base == 0xFF) {
        return own;
    }
    s32 off = (s32)pad + (p->globals[GLOB_BANK] ? 0 : (s8)p->globals[GLOB_TRANSPOSE]);
    return smcpad_scale_note(base, (s8)off, preset_root(p), (u8)(p->scale - 1));
}
