/* Host unit tests for board/smcpad_logic.c (see README.md).
 *   gcc -Wall -Wextra -Werror -std=c11 -I board test/test_smcpad_logic.c -o /tmp/t && /tmp/t
 */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include "../board/smcpad_logic.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int is_perm16(const u8 *t)
{
    u32 seen = 0;
    for (int i = 0; i < 16; i++) {
        if (t[i] > 15 || (seen & (1u << t[i]))) {
            return 0;
        }
        seen |= 1u << t[i];
    }
    return 1;
}

static void test_tables(void)
{
    CHECK(is_perm16(smcpad_pad_of_mux));
    CHECK(is_perm16(smcpad_rgb_chain_pos));
    u32 zero_bits = 0;
    for (int s = 0; s < 16; s++) {
        u16 inv = (u16)~smcpad_km_column_word[s];
        CHECK(inv && !(inv & (inv - 1)));          /* exactly one column low */
        zero_bits |= inv;
    }
    CHECK(zero_bits == 0xffff);                     /* every column driven once */
    u32 row0 = 0, row1 = 0;
    for (int k = 0; k < SMCPAD_NUM_KNOBS; k++) {
        for (int p = 0; p < 2; p++) {
            u8 s = smcpad_knob_steps[k][p];
            CHECK(s < 16 && !(row0 & (1u << s)));
            row0 |= 1u << s;
        }
    }
    CHECK(row0 == 0xffff);                          /* 8 knobs x 2 phases = all 16 */
    for (int b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        u8 s = smcpad_button_step[b];
        CHECK(s < 16 && !(row1 & (1u << s)));
        row1 |= 1u << s;
    }
    u32 leds = 0;
    for (int b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        u8 l = smcpad_button_led[b];
        CHECK((l >> 4) < SMCPAD_BLED_PHASES && (l & 15) < SMCPAD_BLED_SEGS);
        u32 bit = 1u << ((l >> 4) * 4 + (l & 15));
        CHECK(!(leds & bit));
        leds |= bit;
    }
}

/* feed a sample for two frames (the decoder needs two equal frames) */
static int feed(struct smcpad_knob *k, u8 ab)
{
    int d = smcpad_knob_sample(k, ab);
    d += smcpad_knob_sample(k, ab);
    return d;
}

static void test_knob(void)
{
    struct smcpad_knob k;
    smcpad_knob_init(&k, 0);
    /* 00 -> 10 -> 11 -> 01 -> 00 -> 10: +1 from the 2nd transition on */
    static const u8 fwd[] = {2, 3, 1, 0, 2, 3, 1, 0};
    int sum = 0;
    for (unsigned i = 0; i < sizeof fwd; i++) {
        sum += feed(&k, fwd[i]);
    }
    CHECK(sum == 7 && k.count == 7);

    smcpad_knob_init(&k, 0);
    static const u8 rev[] = {1, 3, 2, 0, 1, 3, 2, 0};
    sum = 0;
    for (unsigned i = 0; i < sizeof rev; i++) {
        sum += feed(&k, rev[i]);
    }
    CHECK(sum == -7 && k.count == -7);

    /* contact bounce back and forth between two states never counts */
    smcpad_knob_init(&k, 0);
    for (int i = 0; i < 10; i++) {
        feed(&k, 2);
        feed(&k, 0);
    }
    CHECK(k.count == 0);

    /* a one-frame glitch is ignored */
    smcpad_knob_init(&k, 0);
    feed(&k, 2);
    smcpad_knob_sample(&k, 1);          /* single frame */
    feed(&k, 3);
    CHECK(k.count == 1);

    /* frame-level decode uses the stock step table on row 0 */
    struct smcpad_knob ks[SMCPAD_NUM_KNOBS];
    for (int i = 0; i < SMCPAD_NUM_KNOBS; i++) {
        smcpad_knob_init(&ks[i], 0);
    }
    u8 frame[16];
    static const u8 seq[] = {2, 3, 1, 0};
    for (int rep = 0; rep < 2; rep++) {
        for (unsigned i = 0; i < sizeof seq; i++) {
            memset(frame, 0xff, sizeof frame);          /* all open */
            u8 a = seq[i] & 1, b = (seq[i] >> 1) & 1;
            if (a) frame[smcpad_knob_steps[2][0]] &= (u8)~1u;   /* knob 2 only */
            if (b) frame[smcpad_knob_steps[2][1]] &= (u8)~1u;
            smcpad_knobs_frame(ks, frame);
            smcpad_knobs_frame(ks, frame);
        }
    }
    CHECK(ks[2].count == 7);
    for (int i = 0; i < SMCPAD_NUM_KNOBS; i++) {
        if (i != 2) {
            CHECK(ks[i].count == 0);
        }
    }
}

static void test_buttons(void)
{
    u8 frame[16];
    memset(frame, 0xff, sizeof frame);
    CHECK(smcpad_km_buttons_raw(frame) == 0);
    frame[3] &= (u8)~2u;                                /* button 0, row 1 */
    frame[2] &= (u8)~2u;                                /* button 9 */
    frame[5] &= (u8)~1u;                                /* row 0 must not count */
    CHECK(smcpad_km_buttons_raw(frame) == ((1u << 0) | (1u << 9)));

    struct smcpad_debounce d;
    memset(&d, 0, sizeof d);
    CHECK(smcpad_debounce_step(&d, 1, 3) == 0);
    CHECK(smcpad_debounce_step(&d, 1, 3) == 0);
    CHECK(smcpad_debounce_step(&d, 0, 3) == 0);         /* bounce resets the count */
    CHECK(smcpad_debounce_step(&d, 1, 3) == 0);
    CHECK(smcpad_debounce_step(&d, 1, 3) == 0);
    CHECK(smcpad_debounce_step(&d, 1, 3) == 1 && d.state == 1);
    CHECK(smcpad_debounce_step(&d, 1, 3) == 0);
}

static void test_pads(void)
{
    const struct smcpad_pad_cfg *c = &smcpad_pad_cfg_default;
    u16 idle_lo[4] = {90, 100, 110, 100}, idle_hi[4] = {900, 950, 1000, 990};
    CHECK(smcpad_pad_polarity(idle_lo, 4) == 1);
    CHECK(smcpad_pad_polarity(idle_hi, 4) == -1);

    struct smcpad_pad p;
    u8 vel = 0;
    smcpad_pad_init(&p, 100);
    CHECK(smcpad_pad_sample(&p, c, 1, 120, &vel) == SMCPAD_EV_NONE);   /* below on_thr */
    CHECK(smcpad_pad_sample(&p, c, 1, 150, &vel) == SMCPAD_EV_NONE);   /* rising, 1 */
    CHECK(smcpad_pad_sample(&p, c, 1, 300, &vel) == SMCPAD_EV_NONE);   /* 2 */
    CHECK(smcpad_pad_sample(&p, c, 1, 500, &vel) == SMCPAD_EV_PAD_ON); /* 3 frames */
    CHECK(vel == smcpad_velocity(c, p.peak) && vel > 60 && vel < 100);
    CHECK(p.pressure > 350);
    CHECK(smcpad_pad_sample(&p, c, 1, 300, &vel) == SMCPAD_EV_NONE && p.pressure > 150);
    /* a short dip below off_thr (soft-hit wobble) does not end the note */
    CHECK(smcpad_pad_sample(&p, c, 1, 105, &vel) == SMCPAD_EV_NONE);
    CHECK(smcpad_pad_sample(&p, c, 1, 105, &vel) == SMCPAD_EV_NONE);
    CHECK(smcpad_pad_sample(&p, c, 1, 200, &vel) == SMCPAD_EV_NONE && p.state == PAD_HELD);
    /* even a sustained dip waits for the minimum hold */
    int n = 0;
    while (smcpad_pad_sample(&p, c, 1, 105, &vel) == SMCPAD_EV_NONE && n < 100) {
        n++;
    }
    CHECK(p.state == PAD_IDLE && p.pressure == 0);
    CHECK(n + 1 + 4 >= c->hold_frames && n < 100);    /* released at hold_frames, not before */
    /* after the hold, release needs release_frames consecutive low samples */
    smcpad_pad_init(&p, 100);
    smcpad_pad_sample(&p, c, 1, 500, &vel);
    smcpad_pad_sample(&p, c, 1, 500, &vel);
    CHECK(smcpad_pad_sample(&p, c, 1, 500, &vel) == SMCPAD_EV_PAD_ON);
    for (int i = 0; i < c->hold_frames; i++) {
        smcpad_pad_sample(&p, c, 1, 400, &vel);
    }
    for (int i = 0; i + 1 < c->release_frames; i++) {
        CHECK(smcpad_pad_sample(&p, c, 1, 105, &vel) == SMCPAD_EV_NONE);
    }
    CHECK(smcpad_pad_sample(&p, c, 1, 105, &vel) == SMCPAD_EV_PAD_OFF);

    /* a hit that peaks and falls fast fires before peak_frames */
    smcpad_pad_init(&p, 100);
    smcpad_pad_sample(&p, c, 1, 700, &vel);
    CHECK(smcpad_pad_sample(&p, c, 1, 400, &vel) == SMCPAD_EV_PAD_ON && vel == 127);

    /* inverted polarity */
    smcpad_pad_init(&p, 900);
    smcpad_pad_sample(&p, c, -1, 600, &vel);
    smcpad_pad_sample(&p, c, -1, 590, &vel);
    CHECK(smcpad_pad_sample(&p, c, -1, 580, &vel) == SMCPAD_EV_PAD_ON);

    /* baseline follows slow drift while idle */
    smcpad_pad_init(&p, 100);
    for (int i = 0; i < 2000; i++) {
        smcpad_pad_sample(&p, c, 1, 130, &vel);
    }
    CHECK((p.baseline_q4 >> 4) >= 125 && p.state == PAD_IDLE);

    CHECK(smcpad_velocity(c, 0) == 1);
    CHECK(smcpad_velocity(c, c->vel_full) == 127);
    CHECK(smcpad_velocity(c, 60000) == 127);
}

static void test_ring(void)
{
    static struct smcpad_ev_ring r;
    struct smcpad_pad_ev e = {SMCPAD_EV_PAD_ON, 0, 0}, o;
    for (int i = 0; i < SMCPAD_EV_RING; i++) {
        e.pad = (u8)i;
        CHECK(smcpad_ring_push(&r, &e) == 1);
    }
    CHECK(smcpad_ring_push(&r, &e) == 0 && r.dropped == 1);
    for (int i = 0; i < SMCPAD_EV_RING; i++) {
        CHECK(smcpad_ring_pop(&r, &o) == 1 && o.pad == i);
    }
    CHECK(smcpad_ring_pop(&r, &o) == 0);
    /* wrap-around of the u8 indices */
    for (int i = 0; i < 1000; i++) {
        e.pad = (u8)i;
        CHECK(smcpad_ring_push(&r, &e) == 1);
        CHECK(smcpad_ring_pop(&r, &o) == 1 && o.pad == (u8)i);
    }
}

static void test_leds(void)
{
    static u8 buf[SMCPAD_WS_BUF_LEN + 4];
    u32 rgb[SMCPAD_NUM_RGB] = {0};
    memset(buf, 0xaa, sizeof buf);
    CHECK(SMCPAD_WS_BUF_LEN == 388);
    rgb[8] = 0xff0000;                                   /* logical LED 8 = chain pos 0 */
    rgb[0] = 0x0000ff;                                   /* logical LED 0 = chain pos 11 */
    smcpad_ws_encode(buf, rgb, 255);
    CHECK(buf[0] == 0 && buf[1] == 0);
    CHECK(buf[SMCPAD_WS_BUF_LEN - 1] == 0 && buf[SMCPAD_WS_BUF_LEN - 2] == 0);
    CHECK(buf[SMCPAD_WS_BUF_LEN] == 0xaa);               /* no overrun */
    const u8 *p0 = buf + SMCPAD_WS_LEAD;                 /* GRB: G=0, R=ff, B=0 */
    for (int i = 0; i < 24; i++) {
        CHECK(p0[i] == ((i >= 8 && i < 16) ? SMCPAD_WS_ONE : SMCPAD_WS_ZERO));
    }
    const u8 *p11 = buf + SMCPAD_WS_LEAD + 11 * 24;      /* blue only */
    for (int i = 0; i < 24; i++) {
        CHECK(p11[i] == (i >= 16 ? SMCPAD_WS_ONE : SMCPAD_WS_ZERO));
    }
    /* brightness scaling: 0x80 at 50 % -> 0x40 */
    rgb[8] = 0x800000;
    smcpad_ws_encode(buf, rgb, 127);
    u8 r = 0;
    for (int i = 8; i < 16; i++) {
        r = (u8)((r << 1) | (p0[i] == SMCPAD_WS_ONE));
    }
    CHECK(r == 0x40);

    u8 seg[SMCPAD_BLED_PHASES];
    smcpad_bled_phases(1u << 3, seg);               /* hw 3 = Left -> LED 0 = 0x10 */
    CHECK(seg[0] == 0 && seg[1] == 1 && seg[2] == 0);
    smcpad_bled_phases(1u << 0, seg);               /* hw 0 = BT -> LED 5 = 0x22 */
    CHECK(seg[0] == 0 && seg[1] == 0 && seg[2] == 4);
    smcpad_bled_phases(1u << 2, seg);               /* hw 2 = knob bank -> LED 7 = 0x20 */
    CHECK(seg[0] == 0 && seg[1] == 0 && seg[2] == 1);
    smcpad_bled_phases(0x3ff, seg);
    CHECK(seg[0] == 0x0f && seg[1] == 0x07 && seg[2] == 0x07);
    u16 seen = 0;                                   /* the map is a permutation of 0..9 */
    for (int b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        CHECK(smcpad_led_of_button[b] < SMCPAD_NUM_BUTTONS && !(seen & (1u << smcpad_led_of_button[b])));
        seen |= (u16)(1u << smcpad_led_of_button[b]);
    }

    CHECK(smcpad_knob_value(126, 5) == 127);
    CHECK(smcpad_knob_value(2, -5) == 0);
    CHECK(smcpad_knob_value(64, -1) == 63);
}

static void test_safe_mode(void)
{
    u8 vm = 0, next;
    /* boot 1 and 2 end early: counter 1, 2; boot 3 = safe mode, counter reset */
    CHECK(smcpad_safe_step(vm, &next) == 0 && next == 1); vm = next;
    CHECK(smcpad_safe_step(vm, &next) == 0 && next == 2); vm = next;
    CHECK(smcpad_safe_step(vm, &next) == 1 && next == 0); vm = next;
    /* the boot after safe mode is normal again */
    CHECK(smcpad_safe_step(vm, &next) == 0 && next == 1);
    /* a boot that survives 5 s writes 0, so normal use never reaches safe mode */
    CHECK(smcpad_safe_step(0, &next) == 0 && next == 1);
    /* blank / junk VM values count as 0 */
    CHECK(smcpad_safe_step(0xFF, &next) == 0 && next == 1);
    CHECK(smcpad_safe_step(SMCPAD_SAFE_BOOTS, &next) == 0 && next == 1);
}

int main(void)
{
    test_tables();
    test_knob();
    test_buttons();
    test_pads();
    test_ring();
    test_leds();
    test_safe_mode();
    if (failures) {
        printf("test_smcpad_logic: %d FAILED\n", failures);
        return 1;
    }
    printf("test_smcpad_logic: ALL PASSED\n");
    return 0;
}
