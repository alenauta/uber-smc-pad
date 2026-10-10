/* Host tests for smcpad_seq_ui.c:
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board \
 *       test/test_smcpad_seq_ui.c -o /tmp/t_sui && /tmp/t_sui */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include "../board/smcpad_scale.c"
#include "../board/smcpad_seq.c"
#include "../board/smcpad_seq_ui.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u8 last[3];
static int nmidi, ntempo;
static u32 tempo_seen;
static u8 swing_seen;
static void cap(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    if (n == 3) {
        memcpy(last, m, 3);
    }
    nmidi++;
}
static void cap_tempo(void *ctx, u32 c, u8 sw)
{
    (void)ctx;
    ntempo++;
    tempo_seen = c;
    swing_seen = sw;
}
static const struct seq_out sout = {NULL, cap};
static const struct seq_ui_io uio = {NULL, cap, cap_tempo};

static struct seq_pattern pat;
static struct seq S;
static struct seq_ui U;
static u32 now = 1000, rgb[16];

static void setup(void)
{
    seq_pattern_default(&pat, NULL, NULL);
    seq_init(&S, &sout, &pat, 1);
    seq_ui_init(&U, &S, &uio);
    seq_ui_toggle(&U);                          /* Shift + Note Repeat */
    seq_ui_button(&U, SUI_NOTE_REPEAT, 0, now);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    nmidi = ntempo = 0;
}

static void tap(u8 pad) { seq_ui_pad(&U, pad, 1, 90, now); seq_ui_pad(&U, pad, 0, 0, now); }
static void press(u8 b) { seq_ui_button(&U, b, 1, now); seq_ui_button(&U, b, 0, now); }

static void test_mode_and_steps(void)
{
    setup();
    CHECK(U.active && !U.nr && !U.shift && U.view == SUI_VIEW_STEP);
    tap(3);
    CHECK(pat.track[0].step[3].vel == SUI_DEFAULT_VEL);       /* tap = on */
    tap(3);
    CHECK(pat.track[0].step[3].vel == 0);                     /* tap again = off */
    /* hold + knob edits and keeps the step */
    seq_ui_pad(&U, 5, 1, 90, now);
    seq_ui_knob(&U, 0, -10, now);                              /* K1 velocity */
    seq_ui_knob(&U, 3, 2, now);                                /* K4 micro (small: no accel) */
    seq_ui_knob(&U, 2, 4, now);                                /* K3 probability nibble 4 */
    seq_ui_knob(&U, 6, 1, now);                                /* K7 condition 1:2 */
    seq_ui_knob(&U, 7, 1, now);                                /* K8 tie */
    seq_ui_pad(&U, 5, 0, 0, now);
    struct seq_step *st = &pat.track[0].step[5];
    CHECK(st->vel == SUI_DEFAULT_VEL - 40);                    /* accel: |10| >= 3 -> x4 */
    CHECK(st->micro == 2 && (st->prob_gate >> 4) == 4 && (st->cond & SEQ_COND_MASK) == SEQ_COND_1_2);
    CHECK(st->cond & SEQ_COND_TIE);
    /* an existing step held + edited stays on */
    seq_ui_pad(&U, 5, 1, 90, now);
    seq_ui_knob(&U, 0, 1, now);
    seq_ui_pad(&U, 5, 0, 0, now);
    CHECK(st->vel == SUI_DEFAULT_VEL - 39);
    /* ratchet: hold step + Note Repeat cycles 2, 3, 4, 1 */
    seq_ui_pad(&U, 5, 1, 90, now);
    press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 1);
    press(SUI_NOTE_REPEAT); press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 3);
    press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 0);
    seq_ui_pad(&U, 5, 0, 0, now);
    CHECK(st->vel != 0);                                       /* ratchet edit kept it */
    /* steps past the track length do nothing */
    pat.track[0].len = 8;
    tap(12);
    CHECK(pat.track[0].step[12].vel == 0);
    /* pages */
    pat.track[0].len = 64;
    press(SUI_RIGHT);
    tap(0);
    CHECK(U.page == 1 && pat.track[0].step[16].vel == SUI_DEFAULT_VEL);
    press(SUI_LEFT); press(SUI_LEFT);
    CHECK(U.page == 0);
    /* leave with Shift + Note Repeat */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_button(&U, SUI_NOTE_REPEAT, 1, now);
    CHECK(!U.active);
}

static void test_timing_page(void)
{
    setup();
    seq_ui_button(&U, SUI_NOTE_REPEAT, 1, now);
    CHECK(U.nr);
    tap(2);
    CHECK(pat.track[0].rate == SEQ_RATE_8);
    tap(12);
    CHECK(S.swing == 75 && ntempo == 1 && swing_seen == 75);
    seq_ui_pad(&U, 13, 1, 90, now);
    CHECK(S.fill);
    seq_ui_pad(&U, 13, 0, 0, now);
    CHECK(!S.fill);
    tap(14);
    CHECK(S.sync == 0);
    tap(14);
    CHECK(S.sync == 1);
    /* tap tempo: 4 taps 400 ms apart = 150 BPM */
    for (int i = 0; i < 4; i++) {
        tap(15);
        now += 400;
    }
    CHECK(S.centibpm == 15000 && tempo_seen == 15000);
    /* LED page: rate 1/8 lit, swing 75 lit */
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[2] == C_RATE && rgb[12] == C_SWING && rgb[0] != C_RATE);
    seq_ui_button(&U, SUI_NOTE_REPEAT, 0, now);
    CHECK(!U.nr && pat.track[0].step[2].vel == 0);             /* page taps did not edit steps */
}

static void test_views_kit_keys(void)
{
    setup();
    /* PAD BANK hold + pad selects a track without changing the view */
    seq_ui_button(&U, SUI_PAD_BANK, 1, now);
    tap(5);
    seq_ui_button(&U, SUI_PAD_BANK, 0, now);
    CHECK(U.track == 5 && U.view == SUI_VIEW_STEP);
    press(SUI_PAD_BANK);
    CHECK(U.view == SUI_VIEW_KIT);
    /* kit: pad plays its track's note, release stops it */
    seq_ui_pad(&U, 2, 1, 77, now);
    CHECK(last[0] == 0x99 && last[1] == 38 && last[2] == 77 && U.track == 2);
    seq_ui_pad(&U, 2, 0, 0, now);
    CHECK(last[0] == 0x89 && last[1] == 38);
    /* Stop + pad mutes */
    seq_ui_button(&U, SUI_STOP, 1, now);
    tap(4);
    seq_ui_button(&U, SUI_STOP, 0, now);
    CHECK(pat.track[4].mute == 1);
    /* live record while running */
    press(SUI_REC);
    press(SUI_PLAY);
    CHECK(S.running && U.rec);
    for (int i = 0; i < 260; i++) {
        now++;
        seq_run(&S, now);
    }
    tap(7);                                                    /* ~step 2 */
    CHECK(pat.track[7].step[2].vel == 90);
    /* keyboard: C major from 60 */
    pat.root = 0;
    pat.scale = SCALE_MAJOR;
    pat.track[7].note = 60;
    press(SUI_PAD_BANK);
    CHECK(U.view == SUI_VIEW_KEYS && U.track == 7);
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 64);
    seq_ui_pad(&U, 2, 0, 0, now);
    press(SUI_PAD_BANK);
    CHECK(U.view == SUI_VIEW_STEP);
    /* Shift + Stop + pad clears a track */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_button(&U, SUI_STOP, 1, now);
    tap(7);
    seq_ui_button(&U, SUI_STOP, 0, now);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    CHECK(pat.track[7].step[2].vel == 0);
}

static void test_transport(void)
{
    setup();
    press(SUI_PLAY);
    CHECK(S.running);
    press(SUI_STOP);
    CHECK(!S.running && !U.at_start);
    press(SUI_PLAY);                                           /* continue */
    CHECK(S.running);
    press(SUI_STOP);
    press(SUI_STOP);                                           /* twice: to the start */
    CHECK(U.at_start);
    press(SUI_PLAY);
    CHECK(S.running && S.pos == 0);
}

static void test_knob_pages_and_leds(void)
{
    setup();
    seq_ui_knob(&U, 6, -4, now);                               /* K7 steps 16 -> 12 */
    CHECK(pat.track[0].len == 12);
    seq_ui_knob(&U, 7, -1, now);                               /* K8 rate 1/16 -> 1/8T */
    CHECK(pat.track[0].rate == SEQ_RATE_8T);
    seq_ui_knob(&U, 4, 2, now);                                /* K5 base note + preview */
    CHECK(pat.track[0].note == 38 && last[0] == 0x99 && last[1] == 38);
    /* overlay for K1 volume (bar) */
    seq_ui_knob(&U, 0, -64, now);                              /* 127 -> 0 (accel) */
    CHECK(pat.track[0].vol == 0);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[0] == rgbs(palette[0], 1, 24));                  /* empty bar */
    now += SUI_OVERLAY_MS + 1;
    /* pattern page */
    press(SUI_KNOB_BANK);
    seq_ui_knob(&U, 0, 1, now);
    CHECK(S.centibpm == 12100 && tempo_seen == 12100);
    seq_ui_knob(&U, 2, 2, now);
    seq_ui_knob(&U, 3, 1, now);
    CHECK(pat.root == 2 && pat.scale == SCALE_MAJOR);
    seq_ui_leds(&U, now, rgb);                                 /* scale overlay: D major */
    CHECK(rgb[2] == palette[0] && rgb[4] != 0 && rgb[3] == 0 && rgb[1] != 0);
    now += SUI_OVERLAY_MS + 1;
    press(SUI_KNOB_BANK);
    /* step view LEDs */
    pat.track[0].vol = 127;
    pat.track[0].len = 20;
    pat.track[0].step[1].vel = 127;
    u16 b = seq_ui_leds(&U, now, rgb);
    CHECK(rgb[1] == palette[0] && rgb[2] == rgbs(palette[0], 1, 16));
    CHECK(b & (1u << SUI_RIGHT) && !(b & (1u << SUI_LEFT)) && !(b & (1u << SUI_PLAY)));
    press(SUI_PLAY);
    b = seq_ui_leds(&U, now, rgb);
    CHECK(rgb[0] == rgbs(WHITE, 1, 6) && (b & (1u << SUI_PLAY)));   /* playhead on step 0 */
    press(SUI_RIGHT);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[3] != 0 && rgb[4] == 0);                         /* page 2: steps 17-20 only */
}

int main(void)
{
    test_mode_and_steps();
    test_timing_page();
    test_views_kit_keys();
    test_transport();
    test_knob_pages_and_leds();
    printf(failures ? "test_smcpad_seq_ui: %d FAILED\n" : "test_smcpad_seq_ui: ALL PASSED\n", failures);
    return failures != 0;
}
