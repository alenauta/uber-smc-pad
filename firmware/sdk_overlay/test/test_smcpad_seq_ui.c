/* Host tests for smcpad_seq_ui.c:
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board \
 *       test/test_smcpad_seq_ui.c -o /tmp/t_sui && /tmp/t_sui */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include "../board/smcpad_scale.c"
#include "../board/smcpad_seq.c"
#include "../board/smcpad_undo.c"
#include "../board/smcpad_seq_ui.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u8 last[3];
static int nmidi, ntempo;
static u32 tempo_seen;
static u8 swing_seen, lead_seen;
static void cap(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    if (n == 3) {
        memcpy(last, m, 3);
    }
    nmidi++;
}
static void cap_tempo(void *ctx, u32 c, u8 sw, u8 lead)
{
    (void)ctx;
    ntempo++;
    tempo_seen = c;
    swing_seen = sw;
    lead_seen = lead;
}
static const struct seq_out sout = {NULL, cap};
static const struct seq_ui_io uio = {NULL, cap, cap_tempo};
#define P(step) seq_ui_step_pad(step)                         /* pad of a step-view step */

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
    /* step order reads like text: steps 0-3 = top row (pads 12-15), 12-15 = bottom row */
    CHECK(P(0) == 12 && P(3) == 15 && P(4) == 8 && P(12) == 0 && P(15) == 3);
    for (u8 i = 0; i < 16; i++) {
        CHECK(P(P(i)) == i);
    }
    tap(12);                                                   /* top-left pad = step 1 */
    CHECK(pat.track[0].step[0].vel == SUI_DEFAULT_VEL);
    tap(12);
    tap(P(3));
    CHECK(pat.track[0].step[3].vel == SUI_DEFAULT_VEL);       /* tap = on */
    tap(P(3));
    CHECK(pat.track[0].step[3].vel == 0);                     /* tap again = off */
    /* hold + knob edits and keeps the step */
    seq_ui_pad(&U, P(5), 1, 90, now);
    seq_ui_knob(&U, 0, -10, now);                              /* K1 velocity */
    seq_ui_knob(&U, 3, 2, now);                                /* K4 micro (small: no accel) */
    seq_ui_knob(&U, 2, -3, now);                               /* K3 probability: from the track's 100 % */
    seq_ui_knob(&U, 6, 2, now);                                /* K7 condition: one per report -> 1:2 */
    seq_ui_knob(&U, 7, 1, now);                                /* K8 tie */
    seq_ui_knob(&U, 5, 3, now);                                /* K6 ratchet rate: auto -> 1/8 */
    seq_ui_knob(&U, 1, 1, now);                                /* K2 gate: from the track's 1/2 step */
    seq_ui_pad(&U, P(5), 0, 0, now);
    struct seq_step *st = &pat.track[0].step[5];
    CHECK(st->vel == SUI_DEFAULT_VEL - 40);                    /* accel: |10| >= 3 -> x4 */
    CHECK(st->micro == 2 && (st->prob_gate >> 4) == 14 && seq_step_prob(&pat.track[0], st) == 92);
    CHECK((st->cond & SEQ_COND_MASK) == SEQ_COND_1_2);
    CHECK((st->cond & SEQ_COND_TIE) && (st->ratchet & 0x0f) == 2);
    CHECK((st->prob_gate & 0x0f) == 5);                        /* track gate index 3 + 1, stored +1 */
    /* an existing step held + edited stays on */
    seq_ui_pad(&U, P(5), 1, 90, now);
    seq_ui_knob(&U, 0, 1, now);
    seq_ui_pad(&U, P(5), 0, 0, now);
    CHECK(st->vel == SUI_DEFAULT_VEL - 39);
    /* ratchet: hold step + Note Repeat cycles 2, 3, 4, 1 */
    seq_ui_pad(&U, P(5), 1, 90, now);
    press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 1);
    press(SUI_NOTE_REPEAT); press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 3);
    press(SUI_NOTE_REPEAT);
    CHECK((st->ratchet >> 4) == 0);
    seq_ui_pad(&U, P(5), 0, 0, now);
    CHECK(st->vel != 0);                                       /* ratchet edit kept it */
    /* steps past the track length do nothing */
    pat.track[0].len = 8;
    tap(P(12));
    CHECK(pat.track[0].step[12].vel == 0);
    /* pages */
    pat.track[0].len = 64;
    press(SUI_RIGHT);
    tap(P(0));
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
    /* kit taps are a preview only, even while recording */
    press(SUI_REC);
    press(SUI_PLAY);
    CHECK(S.running && U.rec);
    for (int i = 0; i < 260; i++) {
        now++;
        seq_run(&S, now);
    }
    tap(7);                                                    /* ~step 2 */
    CHECK(U.track == 7 && last[1] == 43 && pat.track[7].step[2].vel == 0);
    /* keyboard: C major from 60, records while Rec is on */
    pat.root = 0;
    pat.scale = SCALE_MAJOR;
    pat.track[7].note = 60;
    press(SUI_PAD_BANK);
    CHECK(U.view == SUI_VIEW_KEYS && U.track == 7);
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 64 && pat.track[7].step[2].vel == 80 && pat.track[7].step[2].note == 2);
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
    seq_ui_knob(&U, 7, -2, now);                               /* K8 rate: one per report, 1/16 -> 1/8T */
    CHECK(pat.track[0].rate == SEQ_RATE_8T);
    seq_ui_leds(&U, now, rgb);                                 /* pick: pad 4 (printed 1/8T) bright */
    CHECK(rgb[3] == palette[0] && rgb[4] == rgbs(palette[0], 1, 12) && rgb[8] == 0);
    seq_ui_knob(&U, 4, 2, now);                                /* K5 base note + preview */
    CHECK(pat.track[0].note == 37 && last[0] == 0x99 && last[1] == 37);
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
    seq_ui_knob(&U, 2, 1, now);
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
    CHECK(rgb[P(1)] == palette[0] && rgb[P(2)] == rgbs(palette[0], 1, 16) && rgb[1] != palette[0]);
    CHECK(b & (1u << SUI_RIGHT) && !(b & (1u << SUI_LEFT)) && !(b & (1u << SUI_PLAY)));
    press(SUI_PLAY);
    b = seq_ui_leds(&U, now, rgb);
    CHECK(rgb[P(0)] == rgbs(WHITE, 1, 6) && (b & (1u << SUI_PLAY)));   /* playhead on step 0 */
    press(SUI_RIGHT);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[P(3)] != 0 && rgb[P(4)] == 0);                   /* page 2: steps 17-20 only */
    /* pattern K8: play early, 1 ms per slow knob report, faster when spun, persisted */
    press(SUI_KNOB_BANK);
    for (int i = 0; i < 3; i++) {
        seq_ui_knob(&U, 7, 2, now);                            /* slow: 1 ms each */
    }
    CHECK(S.lead_ms == 3 && lead_seen == 3);
    seq_ui_knob(&U, 7, 5, now);                                /* spun: 5 ms */
    CHECK(S.lead_ms == 8);
    for (int i = 0; i < 20; i++) {
        seq_ui_knob(&U, 7, 5, now);
    }
    CHECK(S.lead_ms == SEQ_LEAD_MAX_MS);
    /* pattern K7: MIDI channel, 1 pad lit = ch 1 ... 16 pads = ch 16 */
    pat.track[0].chan = 0;
    seq_ui_knob(&U, 6, 3, now);                                /* -> ch 2 */
    seq_ui_leds(&U, now, rgb);
    CHECK(pat.track[0].chan == 1 && rgb[0] == palette[0] && rgb[1] == palette[0] && rgb[2] == rgbs(palette[0], 1, 24));
    pat.track[0].chan = 14;
    seq_ui_knob(&U, 6, 1, now);                                /* -> ch 16: all lit */
    seq_ui_leds(&U, now, rgb);
    CHECK(pat.track[0].chan == 15 && rgb[15] == palette[0]);
}

static void test_stop_mute_running(void)
{
    setup();
    press(SUI_PAD_BANK);                                       /* kit view */
    press(SUI_PLAY);
    seq_ui_button(&U, SUI_STOP, 1, now);
    CHECK(S.running);                                          /* Stop acts on release */
    tap(3);
    seq_ui_button(&U, SUI_STOP, 0, now);
    CHECK(S.running && pat.track[3].mute == 1);                /* muted, still playing */
    press(SUI_STOP);
    CHECK(!S.running);
}

static void test_beat_led(void)
{
    setup();
    /* stopped: flashes at the internal tempo (120 BPM = 500 ms, on for 1/8) */
    CHECK(seq_ui_beat_led(&S, 1000) && seq_ui_beat_led(&S, 1060) && !seq_ui_beat_led(&S, 1070));
    CHECK(seq_ui_beat_led(&S, 1500) && !seq_ui_beat_led(&S, 1250));
    u16 b = seq_ui_leds(&U, 1000, rgb);
    CHECK(b & (1u << SUI_NOTE_REPEAT));
    b = seq_ui_leds(&U, 1200, rgb);
    CHECK(!(b & (1u << SUI_NOTE_REPEAT)));
    /* running: locked to the position, bar downbeat longer */
    press(SUI_PLAY);
    S.pos = 200;                                               /* beat 1, < 1/4 */
    CHECK(seq_ui_beat_led(&S, 0));
    S.pos = 960 + 200;                                         /* beat 2: only 1/8 */
    CHECK(!seq_ui_beat_led(&S, 0));
    S.pos = 960 + 100;
    CHECK(seq_ui_beat_led(&S, 0));
}

static void test_undo(void)
{
    static struct seq_pattern shadow;
    static struct undo un;
    setup();
    undo_init(&un, (u8 *)&pat, (u8 *)&shadow, (u16)sizeof(pat), seq_ui_undo_skip);
    U.undo = &un;
    tap(P(0));                                                 /* edit 1 */
    now += SUI_UNDO_IDLE_MS;
    seq_ui_leds(&U, now, rgb);                                 /* settles: committed */
    CHECK(undo_levels(&un) == 1);
    tap(P(1));                                                 /* edit 2: two quick taps = one step */
    tap(P(2));
    now += 10;
    seq_ui_knob(&U, 6, -4, now);                               /* ... with a knob turn */
    seq_ui_button(&U, SUI_STOP, 1, now);                       /* Stop + pad in kit view = mute: */
    seq_ui_button(&U, SUI_STOP, 0, now);                       /* not journaled (step view: none) */
    pat.track[3].mute = 1;
    now += SUI_UNDO_IDLE_MS;
    seq_ui_leds(&U, now, rgb);
    CHECK(undo_levels(&un) == 2 && pat.track[0].len == 12);
    /* Shift + Left: undo edit 2 (mute stays) */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    press(SUI_LEFT);
    CHECK(pat.track[0].step[1].vel == 0 && pat.track[0].step[2].vel == 0 && pat.track[0].len == 16);
    CHECK(pat.track[0].step[0].vel == SUI_DEFAULT_VEL && pat.track[3].mute == 1 && U.page == 0);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[0] == rgbs(WHITE, 1, 6));                        /* confirmation flash */
    press(SUI_LEFT);                                           /* undo edit 1 */
    CHECK(pat.track[0].step[0].vel == 0);
    press(SUI_LEFT);                                           /* nothing left: no change */
    press(SUI_RIGHT); press(SUI_RIGHT);                        /* redo both */
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    CHECK(pat.track[0].step[0].vel && pat.track[0].step[2].vel && pat.track[0].len == 12 && U.page == 0);
    /* a new edit after undo drops the redo branch */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    press(SUI_LEFT);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    tap(P(9));
    now += SUI_UNDO_IDLE_MS;
    seq_ui_leds(&U, now, rgb);
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    press(SUI_RIGHT);
    CHECK(pat.track[0].len == 16 && pat.track[0].step[9].vel);  /* redo had nothing */
    press(SUI_LEFT);
    CHECK(!pat.track[0].step[9].vel && pat.track[0].step[0].vel);
}

int main(void)
{
    test_mode_and_steps();
    test_timing_page();
    test_views_kit_keys();
    test_transport();
    test_knob_pages_and_leds();
    test_stop_mute_running();
    test_beat_led();
    test_undo();
    printf(failures ? "test_smcpad_seq_ui: %d FAILED\n" : "test_smcpad_seq_ui: ALL PASSED\n", failures);
    return failures != 0;
}
