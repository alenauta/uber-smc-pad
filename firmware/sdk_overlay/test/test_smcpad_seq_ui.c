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
static u8 swing_seen, lead_seen, groot_seen, gscale_seen;
static void cap(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    if (n == 3) {
        memcpy(last, m, 3);
    }
    nmidi++;
}
static void cap_tempo(void *ctx, const struct seq *s)
{
    (void)ctx;
    ntempo++;
    tempo_seen = s->centibpm;
    swing_seen = s->swing;
    lead_seen = s->lead_ms;
    groot_seen = s->groot;
    gscale_seen = s->gscale;
}
static u8 slot_op, slot_n, slot_calls;
static void cap_slot(void *ctx, u8 op, u8 slot)
{
    (void)ctx;
    slot_op = op;
    slot_n = slot;
    slot_calls++;
}
static const struct seq_out sout = {NULL, cap};
static const struct seq_ui_io uio = {NULL, cap, cap_tempo, cap_slot};
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
    seq_ui_knob(&U, 4, -3, now);                               /* K5 probability: from the track's 100 % */
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
    /* ratchet: hold step + Note Repeat cycles 2, 3, 4, 1; the chord bits stay */
    st->ratchet |= 1u << 6;                                    /* a 2-note chord */
    st->extra[0] = 4;
    seq_ui_pad(&U, P(5), 1, 90, now);
    press(SUI_NOTE_REPEAT);
    CHECK(((st->ratchet >> 4) & 3) == 1 && seq_step_count(st) == 2);
    press(SUI_NOTE_REPEAT); press(SUI_NOTE_REPEAT);
    CHECK(((st->ratchet >> 4) & 3) == 3 && SEQ_RATCHET_HITS(st->ratchet) == 4);
    press(SUI_NOTE_REPEAT);
    CHECK(((st->ratchet >> 4) & 3) == 0 && seq_step_count(st) == 2 && (st->ratchet & 0x0f) == 2);
    /* K3 on a held step moves the whole chord (preview = the first note) */
    seq_ui_knob(&U, 2, 1, now);
    CHECK(st->note == 1 && st->extra[0] == 5 && last[0] == 0x99 && last[1] == 37);
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

static void test_views_play_and_tracks(void)
{
    setup();
    /* PAD BANK hold + pad selects a track without changing the view */
    seq_ui_button(&U, SUI_PAD_BANK, 1, now);
    tap(5);
    seq_ui_button(&U, SUI_PAD_BANK, 0, now);
    CHECK(U.track == 5 && U.view == SUI_VIEW_STEP);
    press(SUI_PAD_BANK);                                       /* tap: step <-> play */
    u16 b = seq_ui_leds(&U, now, rgb);
    CHECK(U.view == SUI_VIEW_PLAY && (b & (1u << SUI_PAD_BANK)));
    /* global "follow preset" = chromatic here: pad n = base + n (a drum kit) */
    seq_ui_pad(&U, 2, 1, 77, now);
    CHECK(last[0] == 0x99 && last[1] == 43 && last[2] == 77 && U.track == 5);
    seq_ui_pad(&U, 2, 0, 0, now);
    CHECK(last[0] == 0x89 && last[1] == 43);
    /* global C major: a following track plays it */
    S.gscale = SCALE_MAJOR + 1;
    pat.track[5].note = 60;
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 64);
    seq_ui_pad(&U, 2, 0, 0, now);
    /* track set to chromatic (drum track): stays chromatic whatever the global scale */
    pat.track[5].scale = SCALE_CHROMATIC + 1;
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 62);
    seq_ui_pad(&U, 2, 0, 0, now);
    /* track's own root D + major from D */
    pat.track[5].scale = SCALE_MAJOR + 1;
    pat.track[5].root = 3;
    pat.track[5].note = 62;
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 66 && seq_track_root(&S, &pat.track[5]) == 2);
    seq_ui_pad(&U, 2, 0, 0, now);
    /* global chromatic forces following tracks chromatic */
    pat.track[5].scale = pat.track[5].root = 0;
    pat.track[5].note = 60;
    S.gscale = SCALE_CHROMATIC + 1;
    seq_ui_pad(&U, 2, 1, 80, now);
    CHECK(last[1] == 62);
    /* play-view LEDs: held white, root (C) brighter, a sounding note full colour */
    S.active[0].chan = 9;
    S.active[0].note = 61;
    S.nactive = 1;
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[2] == WHITE && rgb[0] == rgbs(seq_ui_track_rgb(&U, 5), DIM_ROOT) && rgb[12] == rgbs(seq_ui_track_rgb(&U, 5), DIM_ROOT));
    CHECK(rgb[1] == seq_ui_track_rgb(&U, 5) && rgb[3] == rgbs(seq_ui_track_rgb(&U, 5), DIM_PLAY));
    S.nactive = 0;
    seq_ui_pad(&U, 2, 0, 0, now);
    /* live record while running: an empty step takes the hit, the same step adds chord notes */
    press(SUI_REC);
    press(SUI_PLAY);
    CHECK(S.running && U.rec);
    for (int i = 0; i < 260; i++) {
        now++;
        seq_run(&S, now);
    }
    tap(3);                                                    /* ~step 2 */
    struct seq_step *st = &pat.track[5].step[2];
    CHECK(st->vel == 90 && st->note == 3 && seq_step_count(st) == 1);
    tap(7);
    tap(3);                                                    /* already there: not added */
    CHECK(seq_step_count(st) == 2 && st->extra[0] == 7);
    /* track layer (hold PAD BANK): Stop + pad mutes, playback continues */
    seq_ui_button(&U, SUI_PAD_BANK, 1, now);
    seq_ui_button(&U, SUI_STOP, 1, now);
    tap(4);
    seq_ui_button(&U, SUI_STOP, 0, now);
    CHECK(pat.track[4].mute == 1 && S.running);
    /* its LEDs: activity flash, mutes dim, selected track white */
    S.hits = 1u << 6;
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[6] == seq_ui_track_rgb(&U, 6) && rgb[4] == rgbs(seq_ui_track_rgb(&U, 4), DIM_MUTED) && rgb[7] == rgbs(seq_ui_track_rgb(&U, 7), DIM_TRACK));
    CHECK(rgb[5] == WHITE || rgb[5] == rgbs(WHITE, 1, 4));
    /* Shift + Stop + pad clears a track */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_button(&U, SUI_STOP, 1, now);
    tap(5);
    seq_ui_button(&U, SUI_STOP, 0, now);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    seq_ui_button(&U, SUI_PAD_BANK, 0, now);
    CHECK(st->vel == 0 && U.view == SUI_VIEW_PLAY && S.running);
    press(SUI_PAD_BANK);
    CHECK(U.view == SUI_VIEW_STEP);
}

/* Shift + K3 / K4: global root / scale, persisted through the save callback. Picks: no pad lit =
 * follow the preset, pads 1..12 = C..B / chromatic, major, ... */
static void test_global_scale(void)
{
    setup();
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_knob(&U, 2, -1, now);                               /* clamped at follow */
    CHECK(S.groot == 0);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[11] == 0);         /* follow: no pad lit */
    seq_ui_knob(&U, 2, 1, now);                                /* C = pad 1 */
    seq_ui_leds(&U, now, rgb);
    CHECK(S.groot == 1 && rgb[0] == seq_ui_track_rgb(&U, 0) && rgb[1] == rgbs(seq_ui_track_rgb(&U, 0), DIM_CHOICE));
    seq_ui_knob(&U, 2, 3, now);                                /* one per report: C# = pad 2 */
    CHECK(S.groot == 2 && groot_seen == 2 && pat.track[0].note == 36);   /* not the base note */
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[1] == seq_ui_track_rgb(&U, 0) && rgb[0] == rgbs(seq_ui_track_rgb(&U, 0), DIM_CHOICE) && rgb[11] != 0 && rgb[12] == 0);
    CHECK(seq_track_root(&S, &pat.track[0]) == 1);
    seq_ui_knob(&U, 3, 1, now);
    CHECK(S.gscale == SCALE_CHROMATIC + 1 && gscale_seen == 1);
    seq_ui_knob(&U, 3, 1, now);
    CHECK(S.gscale == SCALE_MAJOR + 1);
    seq_ui_leds(&U, now, rgb);                                 /* major = pad 2 */
    CHECK(rgb[1] == seq_ui_track_rgb(&U, 0) && rgb[0] == rgbs(seq_ui_track_rgb(&U, 0), DIM_CHOICE) && rgb[11] != 0 && rgb[12] == 0);
    for (int i = 0; i < 20; i++) {
        seq_ui_knob(&U, 3, 1, now);
    }
    CHECK(S.gscale == SCALE_COUNT);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    /* root "follow the preset": the preset's pad-1 note, the same for every track */
    S.groot = 0;
    S.proot = 2;
    pat.track[0].note = 61;
    pat.track[1].note = 66;
    CHECK(seq_track_root(&S, &pat.track[0]) == 2 && seq_track_root(&S, &pat.track[1]) == 2);
    pat.track[0].root = 6;                                     /* the track's own F wins */
    CHECK(seq_track_root(&S, &pat.track[0]) == 5);
}

static void run_ms(int n)
{
    for (int i = 0; i < n; i++) {
        now++;
        seq_run(&S, now);
    }
}

/* play view: Shift-menu octave / transpose; Shift page; recorded note length = step gate */
static void test_play_shift_and_length(void)
{
    setup();
    press(SUI_PAD_BANK);                                       /* play view */
    pat.track[0].note = 60;
    S.gscale = SCALE_MAJOR + 1;
    U.play_oct = 1;
    seq_ui_pad(&U, 0, 1, 90, now);
    CHECK(last[1] == 72);                                      /* octave up */
    seq_ui_pad(&U, 0, 0, 0, now);
    U.play_oct = 0;
    U.play_tr = 1;
    seq_ui_pad(&U, 0, 1, 90, now);
    CHECK(last[1] == 62);                                      /* transpose: one scale step */
    seq_ui_pad(&U, 0, 0, 0, now);
    U.play_tr = -1;
    seq_ui_pad(&U, 0, 1, 90, now);
    CHECK(last[1] == 59);
    seq_ui_pad(&U, 0, 0, 0, now);
    pat.track[0].scale = SCALE_CHROMATIC + 1;                  /* chromatic: semitones */
    U.play_oct = -1;
    U.play_tr = 2;
    CHECK(seq_ui_play_offset(&U, 3) == 3 - 12 + 2);
    seq_ui_pad(&U, 3, 1, 90, now);
    CHECK(last[1] == 53);
    seq_ui_pad(&U, 3, 0, 0, now);
    U.play_oct = U.play_tr = 0;
    /* Shift page: Shift held, nothing else claiming the pads */
    CHECK(!seq_ui_shift_page(&U));
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    CHECK(seq_ui_shift_page(&U));
    U.padbank = 1;
    CHECK(!seq_ui_shift_page(&U));
    U.padbank = 0;
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    /* note length: held 1 step -> gate 8/8 (table index 7, stored +1) */
    press(SUI_REC);
    press(SUI_PLAY);
    run_ms(255);                                               /* ~step 2 */
    seq_ui_pad(&U, 3, 1, 90, now);
    run_ms(125);
    seq_ui_pad(&U, 3, 0, 0, now);
    struct seq_step *st = &pat.track[0].step[2];
    CHECK(st->vel == 90 && (st->prob_gate & 0x0f) == 8);
    /* a chord played together: the longest note sets the gate (2 steps = 16/8, index 10) */
    run_ms(250 - 125);                                         /* ~step 4 */
    seq_ui_pad(&U, 3, 1, 90, now);
    seq_ui_pad(&U, 5, 1, 90, now);
    run_ms(31);
    seq_ui_pad(&U, 3, 0, 0, now);
    run_ms(250 - 31);
    seq_ui_pad(&U, 5, 0, 0, now);
    st = &pat.track[0].step[4];
    CHECK(seq_step_count(st) == 2 && (st->prob_gate & 0x0f) == 11);
    /* a note added to a step recorded earlier keeps that step's gate */
    run_ms(2000 - 250 - 250);                                  /* next loop, ~step 2 */
    seq_ui_pad(&U, 9, 1, 90, now);
    run_ms(400);
    seq_ui_pad(&U, 9, 0, 0, now);
    st = &pat.track[0].step[2];
    CHECK(seq_step_count(st) == 2 && (st->prob_gate & 0x0f) == 8);
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
    CHECK(rgb[3] == seq_ui_track_rgb(&U, 0) && rgb[4] == rgbs(seq_ui_track_rgb(&U, 0), DIM_CHOICE) && rgb[8] == 0);
    seq_ui_knob(&U, 2, 2, now);                                /* K3 base note + preview */
    CHECK(pat.track[0].note == 37 && last[0] == 0x99 && last[1] == 37);
    /* overlay for K1 volume (bar) */
    seq_ui_knob(&U, 0, -64, now);                              /* 127 -> 0 (accel) */
    CHECK(pat.track[0].vol == 0);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[0] == rgbs(seq_ui_track_rgb(&U, 0), DIM_BAR));                  /* empty bar */
    now += SUI_OVERLAY_MS + 1;
    /* pattern page */
    press(SUI_KNOB_BANK);
    seq_ui_knob(&U, 0, 1, now);
    CHECK(S.centibpm == 12100 && tempo_seen == 12100);
    seq_ui_knob(&U, 2, 2, now);                                /* K3 track root: one per report */
    seq_ui_knob(&U, 2, 1, now);
    seq_ui_knob(&U, 2, 1, now);
    seq_ui_knob(&U, 3, 1, now);                                /* K4 track scale: chromatic, major */
    seq_ui_knob(&U, 3, 1, now);
    CHECK(pat.track[0].root == 3 && pat.track[0].scale == SCALE_MAJOR + 1);
    CHECK(seq_track_root(&S, &pat.track[0]) == 2 && seq_track_scale(&S, &pat.track[0]) == SCALE_MAJOR);
    seq_ui_leds(&U, now, rgb);                                 /* pick: major = pad 2 */
    CHECK(rgb[1] == seq_ui_track_rgb(&U, 0) && rgb[0] == rgbs(seq_ui_track_rgb(&U, 0), DIM_CHOICE) && rgb[11] != 0 && rgb[12] == 0);
    now += SUI_OVERLAY_MS + 1;
    press(SUI_KNOB_BANK);
    /* step view LEDs */
    pat.track[0].vol = 127;
    pat.track[0].len = 20;
    pat.track[0].step[1].vel = 127;
    u16 b = seq_ui_leds(&U, now, rgb);
    CHECK(rgb[P(1)] == seq_ui_track_rgb(&U, 0) && rgb[P(2)] == rgbs(seq_ui_track_rgb(&U, 0), DIM_EMPTY) && rgb[1] != seq_ui_track_rgb(&U, 0));
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
    CHECK(pat.track[0].chan == 1 && rgb[0] == seq_ui_track_rgb(&U, 0) && rgb[1] == seq_ui_track_rgb(&U, 0) && rgb[2] == rgbs(seq_ui_track_rgb(&U, 0), DIM_BAR));
    pat.track[0].chan = 14;
    seq_ui_knob(&U, 6, 1, now);                                /* -> ch 16: all lit */
    seq_ui_leds(&U, now, rgb);
    CHECK(pat.track[0].chan == 15 && rgb[15] == seq_ui_track_rgb(&U, 0));
}

static void test_stop_mute_running(void)
{
    setup();
    press(SUI_PLAY);
    seq_ui_button(&U, SUI_PAD_BANK, 1, now);                   /* track layer */
    seq_ui_button(&U, SUI_STOP, 1, now);
    CHECK(S.running);                                          /* Stop acts on release */
    tap(3);
    seq_ui_button(&U, SUI_STOP, 0, now);
    seq_ui_button(&U, SUI_PAD_BANK, 0, now);
    CHECK(S.running && pat.track[3].mute == 1 && U.view == SUI_VIEW_STEP);   /* muted, playing */
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
    static u8 shadow[sizeof(struct seq_track)];                /* one track watched at a time */
    static struct undo un;
    setup();
    undo_init(&un, (u8 *)&pat, (u16)sizeof(pat), shadow, (u16)sizeof(shadow), seq_ui_undo_skip);
    U.undo = &un;
    tap(P(0));                                                 /* edit 1 */
    now += SUI_UNDO_IDLE_MS;
    seq_ui_leds(&U, now, rgb);                                 /* settles: committed */
    CHECK(undo_levels(&un) == 1);
    tap(P(1));                                                 /* edit 2: two quick taps = one step */
    tap(P(2));
    now += 10;
    seq_ui_knob(&U, 6, -4, now);                               /* ... with a knob turn */
    seq_ui_button(&U, SUI_STOP, 1, now);                       /* mutes (track layer) are */
    seq_ui_button(&U, SUI_STOP, 0, now);                       /* not journaled: set directly */
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
    /* another track is its own step, undone without touching track 0 */
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    seq_ui_button(&U, SUI_PAD_BANK, 1, now);
    tap(2);
    seq_ui_button(&U, SUI_PAD_BANK, 0, now);
    tap(P(4));
    now += SUI_UNDO_IDLE_MS;
    seq_ui_leds(&U, now, rgb);
    CHECK(U.track == 2 && pat.track[2].step[4].vel && undo_levels(&un) == 2);
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    press(SUI_LEFT);
    CHECK(!pat.track[2].step[4].vel && pat.track[0].step[0].vel);
    press(SUI_LEFT);                                           /* back to track 0's edit 1 */
    CHECK(!pat.track[0].step[0].vel);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
}

static u8 rec_count(const struct seq_track *t)
{
    u8 c = 0;
    for (u32 i = 0; i < SEQ_STEPS; i++) {
        c += t->step[i].vel ? 1 : 0;
    }
    return c;
}

static void run_ui_ms(u32 ms)
{
    for (u32 i = 0; i < ms; i++) {
        now++;
        seq_run(&S, now);
        if (!(i % 20)) {
            seq_ui_leds(&U, now, rgb);
        }
    }
    seq_ui_leds(&U, now, rgb);
}

/* live recording: a pass with SUI_UNDO_REC_IDLE_MS without a hit after it is one undo step, so
 * a bad overdub goes without the phrase under it (a shorter pause stays in the same step) */
static void test_undo_rec_passes(void)
{
    static u8 shadow[sizeof(struct seq_track)];
    static struct undo un;
    setup();
    undo_init(&un, (u8 *)&pat, (u16)sizeof(pat), shadow, (u16)sizeof(shadow), seq_ui_undo_skip);
    U.undo = &un;
    press(SUI_PAD_BANK);                                       /* play view */
    press(SUI_REC);
    press(SUI_PLAY);
    CHECK(U.view == SUI_VIEW_PLAY && U.rec && S.running);
    run_ui_ms(100);
    tap(3);                                                    /* pass 1 */
    run_ui_ms(SUI_UNDO_IDLE_MS + 100);                         /* longer than the edit idle */
    tap(5);
    CHECK(undo_levels(&un) == 0 && rec_count(&pat.track[0]) == 2);
    run_ui_ms(SUI_UNDO_REC_IDLE_MS);
    CHECK(undo_levels(&un) == 1);
    tap(7);                                                    /* pass 2 (overdub) */
    run_ui_ms(200);
    tap(9);
    run_ui_ms(SUI_UNDO_REC_IDLE_MS);
    CHECK(undo_levels(&un) == 2 && rec_count(&pat.track[0]) == 4);
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    press(SUI_LEFT);                                           /* the overdub goes, pass 1 stays */
    CHECK(rec_count(&pat.track[0]) == 2 && S.running);
    press(SUI_LEFT);
    CHECK(rec_count(&pat.track[0]) == 0);
    press(SUI_RIGHT);
    CHECK(rec_count(&pat.track[0]) == 2);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
}

/* pattern slots (918+): hold Shift + Rec = save page, hold Shift + Play = load page */
static void test_slots(void)
{
    setup();
    slot_calls = 0;
    U.slot_used = (1u << 0) | (1u << 3);
    U.slot_cur = 3;
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_button(&U, SUI_REC, 1, now);
    CHECK(U.slot_page == SUI_SLOT_SAVE && !U.rec && !seq_ui_shift_page(&U));
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[3] == C_SLOT_SAVE && rgb[0] == rgbs(C_SLOT_SAVE, DIM_CHOICE) && rgb[1] == 0);
    tap(5);                                                    /* save into slot 6 */
    CHECK(slot_calls == 1 && slot_op == SUI_SLOT_SAVE && slot_n == 5 && rec_count(&pat.track[0]) == 0);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[5] == WHITE);                                    /* tapped: busy at once */
    seq_ui_notify(&U, 0, now);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[5] == C_FAIL);                                   /* then the result on the pad */
    now += SUI_MARK_MS;
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[5] == 0 && U.slot_mark == 0xFF);
    seq_ui_button(&U, SUI_REC, 0, now);
    CHECK(slot_calls == 1 && !U.slot_page);                    /* a pad was used: no extra save */
    seq_ui_button(&U, SUI_REC, 1, now);                        /* quick save: no pad */
    seq_ui_button(&U, SUI_REC, 0, now);
    CHECK(slot_calls == 2 && slot_op == SUI_SLOT_SAVE && slot_n == 3);
    seq_ui_notify(&U, 1, now);                                 /* the app reports every result */
    seq_ui_leds(&U, now, rgb);
    CHECK(U.slot_mark == 3 && U.slot_mark_state == SUI_MARK_OK);
    now += SUI_MARK_MS;
    /* load page: Play doesn't start */
    seq_ui_button(&U, SUI_PLAY, 1, now);
    CHECK(U.slot_page == SUI_SLOT_LOAD && !S.running);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[3] == C_SLOT_LOAD && rgb[0] == rgbs(C_SLOT_LOAD, DIM_CHOICE));
    seq_ui_pad(&U, 0, 1, 90, now);
    CHECK(slot_calls == 3 && slot_op == SUI_SLOT_LOAD && slot_n == 0);
    seq_ui_button(&U, SUI_PLAY, 0, now);
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    seq_ui_pad(&U, 0, 0, 0, now);                              /* released after the page: eaten */
    CHECK(!U.slot_page && rec_count(&pat.track[0]) == 0 && !S.running && slot_calls == 3);
    press(SUI_REC);                                            /* without Shift: record, as before */
    CHECK(U.rec && slot_calls == 3);
    press(SUI_REC);
    /* results: red = failed, white = done */
    seq_ui_notify(&U, 0, now);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[7] == rgbs(C_FAIL, 1, 3));
    now += 3 * SUI_FLASH_MS + 1;
    seq_ui_notify(&U, 1, now);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[7] == rgbs(WHITE, 1, 6));
    /* another pattern in place: held steps and recording state restart */
    seq_ui_pad(&U, P(2), 1, 90, now);
    seq_ui_loaded(&U);
    seq_ui_pad(&U, P(2), 0, 0, now);
    CHECK(!U.held && !U.keep && U.page == 0 && U.rec_step[0] == 0xFF);
}

/* rate = editing grid: a coarser grid keeps the notes between its steps; Shift + K8 = speed;
 * KNOB BANK K6 = track colour */
static void test_grid_rate_colour(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    for (u8 i = 0; i < 16; i++) {
        t->step[i].vel = 100;                                  /* every 1/16 */
    }
    for (int i = 0; i < 4; i++) {
        seq_ui_knob(&U, 7, -1, now);                           /* K8: 1/16 -> 1/4, one per report */
    }
    CHECK(t->rate == SEQ_RATE_4 && t->res == SEQ_RATE_16 && seq_track_grid(t) == 4 && t->len == 16);
    now += SUI_OVERLAY_MS + 1;
    t->step[8].vel = 0;                                        /* grid step 3 now only has notes between */
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[P(0)] != 0 && rgb[P(1)] != rgbs(WHITE, 1, 12) && rgb[P(2)] == rgbs(WHITE, 1, 12));
    CHECK(rgb[P(4)] == 0 && rgb[P(15)] == 0);                  /* 4 grid steps in the loop */
    tap(P(2));                                                 /* grid step 3 = stored step 8 */
    CHECK(t->step[8].vel == SUI_DEFAULT_VEL && t->step[9].vel == 100);
    seq_ui_knob(&U, 6, 1, now);                                /* K7 length in grid steps: 4 -> 5 */
    CHECK(t->len == 20);
    seq_ui_knob(&U, 6, 20, now);
    CHECK(t->len == 64);                                       /* 16 grid steps of 4 */
    t->len = 16;
    /* finer than the stored steps: they spread out; passing the triplet rates doesn't pile up */
    for (int i = 0; i < 6; i++) {
        seq_ui_knob(&U, 7, 1, now);                            /* 1/4 -> 1/32 */
    }
    CHECK(t->rate == SEQ_RATE_32 && t->res == SEQ_RATE_32 && t->len == 32 && t->step[2].vel && !t->step[1].vel);
    /* a rate the track can't take is skipped: 64 steps of 1/32 -> 1/16T would need 96 */
    t->len = 64;
    seq_ui_knob(&U, 7, -1, now);
    CHECK(t->rate == SEQ_RATE_16 && t->res == SEQ_RATE_16 && t->len == 32 && t->step[1].vel);
    /* picked directly on the timing page: refused, nothing changes, pads flash */
    t->len = 64;
    seq_ui_button(&U, SUI_NOTE_REPEAT, 1, now);
    tap(5);                                                    /* 1/16T */
    seq_ui_button(&U, SUI_NOTE_REPEAT, 0, now);
    CHECK(t->rate == SEQ_RATE_16 && t->len == 64 && U.ov_kind == SUI_OV_FLASH);
    t->len = 16;
    /* Shift + K8: the old speed change, steps unchanged */
    seq_ui_button(&U, SUI_SHIFT, 1, now);
    seq_ui_knob(&U, 7, -2, now);                               /* 1/16 -> 1/8T */
    seq_ui_button(&U, SUI_SHIFT, 0, now);
    CHECK(t->rate == SEQ_RATE_8T && t->res == SEQ_RATE_8T && t->len == 16 && t->step[1].vel);
    /* colour: KNOB BANK K6, every colour shown on its pad */
    now += SUI_OVERLAY_MS + 1;
    press(SUI_KNOB_BANK);
    int n0 = ntempo;
    seq_ui_knob(&U, 5, 1, now);
    CHECK(U.colour[0] == 1 && seq_ui_track_rgb(&U, 0) == palette[1] && ntempo == n0 + 1);
    seq_ui_leds(&U, now, rgb);
    CHECK(rgb[1] == palette[1] && rgb[0] == rgbs(palette[0], 1, 6) && rgb[15] == rgbs(palette[15], 1, 6));
    seq_ui_knob(&U, 5, -1, now);
    seq_ui_knob(&U, 5, -3, now);                               /* one per report, wraps */
    CHECK(U.colour[0] == 15);
}

int main(void)
{
    test_mode_and_steps();
    test_timing_page();
    test_views_play_and_tracks();
    test_global_scale();
    test_play_shift_and_length();
    test_transport();
    test_knob_pages_and_leds();
    test_stop_mute_running();
    test_beat_led();
    test_undo();
    test_undo_rec_passes();
    test_slots();
    test_grid_rate_colour();
    printf(failures ? "test_smcpad_seq_ui: %d FAILED\n" : "test_smcpad_seq_ui: ALL PASSED\n", failures);
    return failures != 0;
}
