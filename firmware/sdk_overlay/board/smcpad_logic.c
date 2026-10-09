/* SMC-PAD board logic that does not touch hardware (host-testable). See smcpad_logic.h. */
#include "smcpad_logic.h"

/* ---- stock tables ------------------------------------------------------------------ */
SMCPAD_TABLE u8 smcpad_pad_of_mux[SMCPAD_NUM_PADS] = {
    2, 0, 1, 3, 15, 14, 12, 13,         /* mux A, PA4 */
    6, 4, 5, 7, 8, 9, 11, 10,           /* mux B, PA6 */
};
SMCPAD_TABLE u16 smcpad_km_column_word[SMCPAD_KM_STEPS] = {
    0xfeff, 0xfdff, 0xfbff, 0xf7ff, 0xefff, 0xdfff, 0xbfff, 0x7fff,
    0xfffe, 0xfffd, 0xfffb, 0xfff7, 0xffef, 0xffdf, 0xffbf, 0xff7f,
};
SMCPAD_TABLE u8 smcpad_knob_steps[SMCPAD_NUM_KNOBS][2] = {
    {15, 0}, {11, 12}, {1, 2}, {9, 10}, {7, 8}, {13, 14}, {5, 6}, {3, 4},
};
SMCPAD_TABLE u8 smcpad_button_step[SMCPAD_NUM_BUTTONS] = {
    3, 4, 5, 6, 7, 8, 10, 9, 1, 2,
};
SMCPAD_TABLE u8 smcpad_button_led[SMCPAD_NUM_BUTTONS] = {
    0x10, 0x03, 0x12, 0x01, 0x11, 0x22, 0x21, 0x20, 0x02, 0x00,
};
/* hardware button -> LED number (index into smcpad_button_led). Stock lights LEDs 0-4 for the
 * preset buttons (hw 3-7) and LED 7 for knob bank (hw 2); BT, pad bank, shift and note repeat
 * own LEDs 5, 6, 8, 9 (first hardware test, 2026-10-09). */
SMCPAD_TABLE u8 smcpad_led_of_button[SMCPAD_NUM_BUTTONS] = {
    5, 6, 7, 0, 1, 2, 3, 4, 8, 9,
};
SMCPAD_TABLE u8 smcpad_rgb_chain_pos[SMCPAD_NUM_RGB] = {
    11, 4, 2, 7, 10, 14, 6, 9, 0, 5, 15, 1, 13, 12, 8, 3,
};

/* Guesses until measured on hardware (10-bit SARADC). */
SMCPAD_TABLE struct smcpad_pad_cfg smcpad_pad_cfg_default = {
    .on_thr = 40,
    .off_thr = 20,
    .vel_full = 600,
    .peak_frames = 3,
    .baseline_shift = 6,
    .hold_frames = 12,      /* ~20 ms: first hardware test showed note-offs 2-5 ms after soft hits */
    .release_frames = 12,   /* ~20 ms: 904 test, hard hits dip below off_thr then rebound ~12-17 ms later */
};

/* ---- key matrix -------------------------------------------------------------------- */
SMCPAD_RAM_CODE
u16 smcpad_km_buttons_raw(const u8 *frame)
{
    u16 m = 0;
    for (u8 b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        if (smcpad_km_closed(frame, smcpad_button_step[b], 1)) {
            m |= (u16)(1u << b);
        }
    }
    return m;
}

void smcpad_knob_init(struct smcpad_knob *k, u8 ab)
{
    ab &= 3;
    k->last_raw = ab;
    k->hist = (u8)((ab << 4) | (ab << 2) | ab);
    k->count = 0;
}

/* Stock classification of the 3-sample history (oldest in bits 5:4):
 *   +1: 00-10-11, 01-00-10, 10-11-01, 11-01-00  (hist 11, 18, 45, 52)
 *   -1: 00-01-11, 01-11-10, 10-00-01, 11-10-00  (hist  7, 30, 33, 56) */
SMCPAD_RAM_CODE
s8 smcpad_knob_sample(struct smcpad_knob *k, u8 ab)
{
    ab &= 3;
    if (ab != k->last_raw) {            /* must be seen in two consecutive frames */
        k->last_raw = ab;
        return 0;
    }
    if ((k->hist & 3) == ab) {          /* no new state */
        return 0;
    }
    k->hist = (u8)(((k->hist << 2) | ab) & 0x3f);
    /* if-chains, not switch: a jump table would land in flash .rodata */
    u8 h = k->hist;
    if (h == 11 || h == 18 || h == 45 || h == 52) {
        k->count++;
        return 1;
    }
    if (h == 7 || h == 30 || h == 33 || h == 56) {
        k->count--;
        return -1;
    }
    return 0;
}

SMCPAD_RAM_CODE
u8 smcpad_knobs_frame(struct smcpad_knob *k, const u8 *frame)
{
    u8 moved = 0;
    for (u8 i = 0; i < SMCPAD_NUM_KNOBS; i++) {
        u8 a = smcpad_km_closed(frame, smcpad_knob_steps[i][0], 0);
        u8 b = smcpad_km_closed(frame, smcpad_knob_steps[i][1], 0);
        if (smcpad_knob_sample(&k[i], (u8)(a | (b << 1)))) {
            moved++;
        }
    }
    return moved;
}

SMCPAD_RAM_CODE
u16 smcpad_debounce_step(struct smcpad_debounce *d, u16 raw, u8 n)
{
    u16 changed = 0;
    for (u8 i = 0; i < 16; i++) {
        u16 bit = (u16)(1u << i);
        if ((raw ^ d->state) & bit) {
            if (++d->cnt[i] >= n) {
                d->state ^= bit;
                d->cnt[i] = 0;
                changed |= bit;
            }
        } else {
            d->cnt[i] = 0;
        }
    }
    return changed;
}

/* ---- velocity pads ----------------------------------------------------------------- */
void smcpad_pad_init(struct smcpad_pad *p, u16 idle_raw)
{
    p->baseline_q4 = (u32)idle_raw << 4;
    p->peak = 0;
    p->pressure = 0;
    p->state = PAD_IDLE;
    p->frames = 0;
}

s8 smcpad_pad_polarity(const u16 *idle_raw, u8 n)
{
    u32 sum = 0;
    for (u8 i = 0; i < n; i++) {
        sum += idle_raw[i];
    }
    /* an idle pad sitting low means pressing raises the reading */
    return (n && sum / n >= 512) ? -1 : 1;
}

SMCPAD_RAM_CODE
u8 smcpad_velocity(const struct smcpad_pad_cfg *c, u16 peak)
{
    if (peak <= c->on_thr || c->vel_full <= c->on_thr) {
        return 1;
    }
    u32 v = 1 + (u32)(peak - c->on_thr) * 126u / (u32)(c->vel_full - c->on_thr);
    return (u8)(v > 127 ? 127 : v);
}

SMCPAD_RAM_CODE
u8 smcpad_pad_sample(struct smcpad_pad *p, const struct smcpad_pad_cfg *c,
                     s8 polarity, u16 raw, u8 *vel)
{
    s32 base = (s32)(p->baseline_q4 >> 4);
    s32 d = polarity > 0 ? (s32)raw - base : base - (s32)raw;
    u16 delta = (u16)(d < 0 ? 0 : (d > 0xffff ? 0xffff : d));

    if (p->state == PAD_IDLE) {
        if (delta >= c->on_thr) {
            p->state = PAD_RISING;
            p->peak = delta;
            p->frames = 1;
        } else {
            s32 q = (s32)p->baseline_q4;
            q += (((s32)raw << 4) - q) >> c->baseline_shift;
            p->baseline_q4 = (u32)(q < 0 ? 0 : q);
        }
        return SMCPAD_EV_NONE;
    }
    if (p->state == PAD_RISING) {
        if (delta > p->peak) {
            p->peak = delta;
        }
        p->frames++;
        /* stop when enough frames were seen or the value is clearly falling again */
        if (p->frames >= c->peak_frames || (u32)delta + (p->peak >> 3) < p->peak) {
            p->state = PAD_HELD;
            p->pressure = delta;
            p->frames = 0;
            p->low = 0;
            *vel = smcpad_velocity(c, p->peak);
            return SMCPAD_EV_PAD_ON;
        }
        return SMCPAD_EV_NONE;
    }
    /* PAD_HELD: release after the minimum hold and release_frames consecutive low samples */
    if (p->frames < 0xff) {
        p->frames++;
    }
    if (delta < c->off_thr) {
        if (p->low < 0xff) {
            p->low++;
        }
        if (p->low >= c->release_frames && p->frames >= c->hold_frames) {
            p->state = PAD_IDLE;
            p->pressure = 0;
            p->peak = 0;
            return SMCPAD_EV_PAD_OFF;
        }
    } else {
        p->low = 0;
    }
    p->pressure = delta;
    return SMCPAD_EV_NONE;
}

SMCPAD_RAM_CODE
u8 smcpad_ring_push(struct smcpad_ev_ring *r, const struct smcpad_pad_ev *e)
{
    u8 h = r->head;
    if ((u8)(h - r->tail) >= SMCPAD_EV_RING) {
        r->dropped++;
        return 0;
    }
    r->ev[h & (SMCPAD_EV_RING - 1)] = *e;
    r->head = (u8)(h + 1);
    return 1;
}

u8 smcpad_ring_pop(struct smcpad_ev_ring *r, struct smcpad_pad_ev *e)
{
    u8 t = r->tail;
    if (t == r->head) {
        return 0;
    }
    *e = r->ev[t & (SMCPAD_EV_RING - 1)];
    r->tail = (u8)(t + 1);
    return 1;
}

/* ---- LEDs -------------------------------------------------------------------------- */
void smcpad_ws_encode(u8 *buf, const u32 *rgb, u8 brightness)
{
    u32 scale = (u32)brightness + 1;
    u32 i;
    for (i = 0; i < SMCPAD_WS_LEAD; i++) {
        buf[i] = 0;
    }
    for (i = 0; i < SMCPAD_WS_TAIL; i++) {
        buf[SMCPAD_WS_BUF_LEN - 1 - i] = 0;
    }
    for (u8 led = 0; led < SMCPAD_NUM_RGB; led++) {
        u32 c = rgb[led];
        u32 r = (((c >> 16) & 0xff) * scale) >> 8;
        u32 g = (((c >> 8) & 0xff) * scale) >> 8;
        u32 b = ((c & 0xff) * scale) >> 8;
        u32 grb = (g << 16) | (r << 8) | b;
        u8 *p = buf + SMCPAD_WS_LEAD + (u32)smcpad_rgb_chain_pos[led] * 24;
        for (u8 bit = 0; bit < 24; bit++) {
            p[bit] = (grb & (1ul << (23 - bit))) ? SMCPAD_WS_ONE : SMCPAD_WS_ZERO;
        }
    }
}

void smcpad_bled_phases(u16 on_mask, u8 seg_on[SMCPAD_BLED_PHASES])
{
    for (u8 p = 0; p < SMCPAD_BLED_PHASES; p++) {
        seg_on[p] = 0;
    }
    for (u8 b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        if (on_mask & (1u << b)) {
            u8 led = smcpad_button_led[smcpad_led_of_button[b]];
            seg_on[(led >> 4) % SMCPAD_BLED_PHASES] |= (u8)(1u << (led & 0x0f));
        }
    }
}
