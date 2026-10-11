/* Host tests for smcpad_seq.c / smcpad_scale.c:
 *   gcc -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined -g -I board \
 *       test/test_smcpad_seq.c -o /tmp/t_seq && /tmp/t_seq */
#define SMCPAD_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../board/smcpad_scale.c"
#include "../board/smcpad_seq.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* captured MIDI with the time of the seq_run() call that sent it */
#define LOG_MAX 20000
static struct { u32 t; u8 m[3]; u8 n; } lg[LOG_MAX];
static int nlg;
static u32 now;
static void cap(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    if (nlg < LOG_MAX) {
        lg[nlg].t = now;
        lg[nlg].n = (u8)n;
        memcpy(lg[nlg].m, m, n);
        nlg++;
    }
}
static const struct seq_out out = {NULL, cap};

static struct seq_pattern pat;
static struct seq S;

static void setup(void)
{
    seq_pattern_default(&pat, NULL, NULL);
    seq_init(&S, &out, &pat, 12345);
    nlg = 0;
    now = 1000;                                 /* not 0: wrap-safe arithmetic */
}

static void run_to(u32 t)                       /* 1 ms ticks */
{
    while (now < t) {
        now++;
        seq_run(&S, now);
    }
}

static int count_on(u8 note, u32 t0, u32 t1)
{
    int n = 0;
    for (int i = 0; i < nlg; i++) {
        if (lg[i].n == 3 && (lg[i].m[0] & 0xF0) == 0x90 && lg[i].m[1] == note && lg[i].t >= t0 && lg[i].t < t1) {
            n++;
        }
    }
    return n;
}

static int nth_on(u8 note, int k)               /* time of the k-th note on, -1 if none */
{
    for (int i = 0; i < nlg; i++) {
        if (lg[i].n == 3 && (lg[i].m[0] & 0xF0) == 0x90 && lg[i].m[1] == note && k-- == 0) {
            return (int)lg[i].t;
        }
    }
    return -1;
}

static int nth_off(u8 note, int k)
{
    for (int i = 0; i < nlg; i++) {
        if (lg[i].n == 3 && (lg[i].m[0] & 0xF0) == 0x80 && lg[i].m[1] == note && k-- == 0) {
            return (int)lg[i].t;
        }
    }
    return -1;
}

static int count_byte(u8 b)
{
    int n = 0;
    for (int i = 0; i < nlg; i++) {
        n += lg[i].n == 1 && lg[i].m[0] == b;
    }
    return n;
}

static int near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }

static void test_scale(void)
{
    CHECK(smcpad_scale_size(SCALE_MAJOR) == 7 && smcpad_scale_size(SCALE_CHROMATIC) == 12);
    CHECK(smcpad_scale_size(SCALE_PENTA_MINOR) == 5 && smcpad_scale_size(SCALE_BLUES) == 6);
    CHECK(smcpad_scale_note(60, 0, 0, SCALE_MAJOR) == 60);
    CHECK(smcpad_scale_note(60, 1, 0, SCALE_MAJOR) == 62);
    CHECK(smcpad_scale_note(60, 7, 0, SCALE_MAJOR) == 72);
    CHECK(smcpad_scale_note(60, -1, 0, SCALE_MAJOR) == 59);
    CHECK(smcpad_scale_note(61, 0, 0, SCALE_MAJOR) == 61);      /* base kept even off-scale */
    CHECK(smcpad_scale_note(61, 1, 0, SCALE_MAJOR) == 62);
    CHECK(smcpad_scale_note(57, 1, 9, SCALE_MINOR) == 59);      /* A minor: A -> B */
    CHECK(smcpad_scale_note(57, 2, 9, SCALE_MINOR) == 60);      /*           -> C */
    CHECK(smcpad_scale_note(60, 3, 0, SCALE_CHROMATIC) == 63);
    CHECK(smcpad_scale_note(126, 10, 0, SCALE_MAJOR) == 127 || smcpad_scale_note(126, 10, 0, SCALE_MAJOR) == 126);
    CHECK(smcpad_scale_note(1, -10, 0, SCALE_MAJOR) == 0);
    CHECK(smcpad_scale_in(64, 0, SCALE_MAJOR) && !smcpad_scale_in(63, 0, SCALE_MAJOR));
}

static void test_basic_timeline(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    for (int k = 0; k < 16; k += 4) {
        t->step[k].vel = 100;
    }
    seq_start(&S, now);
    run_to(1000 + 2000);
    /* 120 BPM, 1/16 = 125 ms: steps 0,4,8,12 at 0, 500, 1000, 1500 ms; loop at 2000 */
    CHECK(nth_on(36, 0) == 1000 && near(nth_on(36, 1), 1500, 1));
    CHECK(near(nth_on(36, 2), 2000, 1) && near(nth_on(36, 3), 2500, 1));
    CHECK(count_on(36, 1000, 3000) == 4);
    CHECK(near(nth_off(36, 0), 1000 + 63, 1));                 /* gate 1/2 step = 62.5 ms */
    CHECK(lg[0].n == 1 && lg[0].m[0] == 0xFA);                 /* Start first, then clock */
    CHECK(near(count_byte(0xF8), 96, 1));                      /* 24 PPQN x 4 beats */
    CHECK(S.late == 0 && S.dropped == 0);
    /* velocity scaled by track volume, channel 10 */
    CHECK(lg[2].m[0] == 0x99 || lg[1].m[0] == 0x99);
    int n0 = nlg;
    seq_stop(&S);
    CHECK(nlg == n0 + 2 && lg[n0].m[0] == 0xFC && lg[n0 + 1].m[0] == 0x89);  /* FC, then the sounding note off */
}

static void test_polymeter_rate(void)
{
    setup();
    pat.track[1].len = 3;
    pat.track[1].step[0].vel = 90;                             /* note 37 every 3 steps */
    pat.track[2].rate = pat.track[2].res = SEQ_RATE_8;
    pat.track[2].step[1].vel = 90;                             /* note 38: 1/8 grid, step 1 */
    seq_start(&S, now);
    run_to(1000 + 1200);
    CHECK(nth_on(37, 0) == 1000 && near(nth_on(37, 1), 1375, 1) && near(nth_on(37, 2), 1750, 1));
    CHECK(near(nth_on(38, 0), 1250, 1));                       /* 1/8 = 250 ms */
    CHECK(near(nth_on(38, 1), 1000 + 250 + 16 * 250, 1) || nth_on(38, 1) == -1);
}

static void test_swing_micro_ratchet(void)
{
    setup();
    S.swing = 75;
    pat.track[0].step[1].vel = 100;                            /* swung: 125 + 62.5 */
    pat.track[1].step[4].vel = 100;
    pat.track[1].step[4].micro = -48;                          /* half a step early */
    pat.track[2].step[0].vel = 100;
    pat.track[2].step[0].ratchet = (3 << 4);                   /* 4 hits, track rate auto: 1/4 step */
    pat.track[3].step[2].vel = 100;
    pat.track[3].step[2].micro = 24;                           /* a quarter step late */
    pat.track[3].quantize = 100;                               /* record-only: playback keeps it */
    pat.track[4].step[0].vel = 100;
    pat.track[4].step[0].ratchet = (1 << 4) | 5;               /* 2 hits, fixed 1/32 (table 4) */
    pat.track[5].ratchet_rate = 1;                             /* track: fixed 1/8 */
    pat.track[5].step[0].vel = 100;
    pat.track[5].step[0].ratchet = (2 << 4);                   /* 3 hits, 250 ms apart */
    seq_start(&S, now);
    run_to(1000 + 600);
    CHECK(near(nth_on(36, 0), 1188, 1));
    CHECK(near(nth_on(37, 0), 1438, 1));
    CHECK(nth_on(38, 0) == 1000 && near(nth_on(38, 1), 1031, 1) && near(nth_on(38, 2), 1063, 1) &&
          near(nth_on(38, 3), 1094, 1));
    CHECK(near(nth_off(38, 0), 1000 + 23, 1));                 /* ratchet gate 3/4 of interval */
    CHECK(near(nth_on(39, 0), 1281, 1));
    CHECK(nth_on(40, 0) == 1000 && near(nth_on(40, 1), 1063, 1) && count_on(40, 1000, 1125) == 2);
    CHECK(nth_on(41, 0) == 1000 && near(nth_on(41, 1), 1250, 1) && near(nth_on(41, 2), 1500, 1));
}

static void test_probability(void)
{
    setup();
    seq_set_tempo(&S, 30000);
    struct seq_track *t = &pat.track[0];
    t->len = 1;
    t->rate = t->res = SEQ_RATE_32T;                           /* 60 steps per second */
    t->step[0].vel = 100;
    t->step[0].prob_gate = (u8)(8 << 4);                       /* (8-1)*100/14 = 50 % */
    CHECK(seq_step_prob(t, &t->step[0]) == 50);
    seq_start(&S, now);
    run_to(1000 + 40000);
    int n = count_on(36, 0, 0xFFFFFFFF);
    CHECK(n > 2400 * 45 / 100 && n < 2400 * 55 / 100);
    setup();
    pat.track[0].step[0].vel = 100;
    pat.track[0].prob = 0;                                     /* track probability 0 */
    seq_start(&S, now);
    run_to(1000 + 4000);
    CHECK(count_on(36, 0, 0xFFFFFFFF) == 0);
}

static void test_conditions_tie(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    t->len = 4;
    t->step[0].vel = 100;
    t->step[0].cond = SEQ_COND_1_2;                            /* loops 0, 2, 4 */
    t->step[1].vel = 100;
    t->step[1].note = 1;                                       /* note 37 */
    t->step[1].cond = SEQ_COND_FIRST;
    t->step[2].vel = 100;
    t->step[2].note = 2;                                       /* note 38 */
    t->step[2].cond = SEQ_COND_FILL;
    t->step[3].vel = 100;
    t->step[3].note = 3;                                       /* note 39: tied */
    t->step[3].cond = SEQ_COND_TIE;
    seq_start(&S, now);
    run_to(1000 + 4 * 500 - 1);                                /* 4 loops of 500 ms */
    CHECK(count_on(36, 0, 0xFFFFFFFF) == 2);
    CHECK(count_on(37, 0, 0xFFFFFFFF) == 1);
    CHECK(count_on(38, 0, 0xFFFFFFFF) == 0);
    S.fill = 1;
    run_to(1000 + 5 * 500);
    CHECK(count_on(38, 0, 0xFFFFFFFF) == 1);
    /* tie: 39 on at 375 ms, off just after the next step (500 ms + 10 units = 505.2 ms) */
    CHECK(near(nth_on(39, 0), 1375, 1) && near(nth_off(39, 0), 1505, 1));
}

static void test_stop_flush_retrigger(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    t->gate = 14;                                              /* 4 steps */
    t->step[0].vel = 100;
    t->step[1].vel = 100;                                      /* same key 125 ms later */
    seq_start(&S, now);
    run_to(1000 + 130);
    /* first note must end when the second starts (not 4 steps later, cutting the second) */
    CHECK(nth_on(36, 1) == 1125 && nth_off(36, 0) == 1125);
    int i_off = -1, i_on = -1;
    for (int i = 0; i < nlg; i++) {
        if (lg[i].t == 1125 && lg[i].n == 3) {
            if ((lg[i].m[0] & 0xF0) == 0x80 && i_off < 0) i_off = i;
            if ((lg[i].m[0] & 0xF0) == 0x90) i_on = i;
        }
    }
    CHECK(i_off >= 0 && i_on > i_off);                         /* off before on */
    nlg = 0;
    seq_stop(&S);
    CHECK(nlg == 2 && lg[0].m[0] == 0xFC && lg[1].m[0] == 0x89 && lg[1].m[1] == 36);
    run_to(1000 + 2000);
    CHECK(count_on(36, 0, 0xFFFFFFFF) == 0);                   /* nothing while stopped */
}

static void test_external_clock(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    for (int k = 0; k < 16; k += 4) {
        t->step[k].vel = 100;
    }
    /* host at 100 BPM: one F8 every 25 ms, with +-1 ms jitter */
    seq_clock_in(&S, 0xFA, now);
    u32 next = now;
    int pulse = 0;
    while (now < 1000 + 3000) {
        if (now >= next) {
            seq_clock_in(&S, 0xF8, now);
            pulse++;
            next = 1000 + (u32)pulse * 25 + (pulse % 3 == 0 ? 1 : 0) - (pulse % 5 == 0 ? 1 : 0);
        }
        now++;
        seq_run(&S, now);
        seq_clock_timeout(&S, now);
    }
    /* quarter = 600 ms: steps 0,4,8,12 at 0, 600, 1200, 1800 ms */
    CHECK(near(nth_on(36, 0), 1000, 1));
    CHECK(near(nth_on(36, 1), 1600, 2) && near(nth_on(36, 2), 2200, 2) && near(nth_on(36, 3), 2800, 2));
    CHECK(count_byte(0xF8) == 0 && count_byte(0xFA) == 0);    /* no clock echo while following */
    CHECK(near((int)seq_ext_centibpm(&S), 10000, 200));
    /* host clock disappears: we keep playing at the measured tempo */
    u32 lost = now;
    run_to(lost + 600);
    seq_clock_timeout(&S, now);
    CHECK(!S.following && S.running && near((int)S.centibpm, 10000, 200));
    nlg = 0;
    run_to(now + 1300);
    CHECK(count_on(36, 0, 0xFFFFFFFF) >= 2 && count_byte(0xF8) > 40);
    /* host Stop while following stops us; sync off ignores the host */
    seq_clock_in(&S, 0xFA, now);
    seq_clock_in(&S, 0xFC, now);
    CHECK(!S.running);
    S.sync = 0;
    seq_clock_in(&S, 0xFA, now);
    CHECK(!S.running);
    /* while we are the master, an echoed Start does not take over */
    S.sync = 1;
    seq_start(&S, now);
    run_to(now + 100);
    seq_clock_in(&S, 0xFA, now);
    CHECK(S.running && !S.following && S.pos > 0);
}

/* play early + a Bluetooth-like clock: pulses arrive in 15 ms connection events */
static void test_external_clock_lead_jitter(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    for (int k = 0; k < 16; k += 4) {
        t->step[k].vel = 100;
    }
    S.lead_ms = 21;
    seq_clock_in(&S, 0xFA, now);
    int pulse = 0;
    while (now < 1000 + 3000) {
        while (1000 + (u32)pulse * 25 <= now) {                /* due pulses, delivered on USB time */
            seq_clock_in(&S, 0xF8, now);
            pulse++;
        }
        now++;
        seq_run(&S, now);
        seq_clock_timeout(&S, now);
    }
    /* beats at 1600, 2200, 2800 ms, sent 21 ms early (the first one can't be: Start just came) */
    CHECK(near(nth_on(36, 1), 1579, 2) && near(nth_on(36, 2), 2179, 2) && near(nth_on(36, 3), 2779, 2));
    CHECK(S.ext_err_max <= 4);

    setup();
    for (int k = 0; k < 16; k += 4) {
        pat.track[0].step[k].vel = 100;
    }
    seq_clock_in(&S, 0xFA, now);
    pulse = 0;
    while (now < 1000 + 6000) {
        if (now % 15 == 0) {                                   /* connection event: all pending pulses */
            while (1000 + (u32)pulse * 25 <= now) {
                seq_clock_in(&S, 0xF8, now);
                pulse++;
            }
        }
        now++;
        seq_run(&S, now);
        seq_clock_timeout(&S, now);
    }
    CHECK(near((int)seq_ext_centibpm(&S), 10000, 100));        /* tempo steady despite bunching */
    int worst = 0;
    for (int b = 4; b < 10; b++) {                             /* beats 4..9: settled */
        int d = nth_on(36, b) - (1000 + b * 600);
        worst = d > worst ? d : (-d > worst ? -d : worst);
    }
    CHECK(worst <= 15);                                        /* within one connection interval */
    CHECK(S.following && S.running);
}

static void test_record(void)
{
    setup();
    pat.track[5].quantize = 0;                                 /* keep the feel */
    seq_start(&S, now);
    run_to(1000 + 2 * 125 + 10);                               /* 10 ms after step 2 */
    u8 k = seq_record(&S, 5, 90, 0);
    CHECK(k == 2 && pat.track[5].step[2].vel == 90);
    CHECK(pat.track[5].step[2].micro == 7);                    /* 10/125 x 96 = 7.68 */
    run_to(1000 + 4 * 125 - 20);                               /* 20 ms before step 4 */
    k = seq_record(&S, 5, 80, 3);
    CHECK(k == 4 && pat.track[5].step[4].micro == -15 && pat.track[5].step[4].note == 3);
    CHECK(seq_playhead(&S, 5) == 3);
    /* default quantize 100 %: snapped to the nearest step; 50 %: half the offset */
    run_to(1000 + 6 * 125 + 30);
    k = seq_record(&S, 6, 70, 0);
    CHECK(k == 6 && pat.track[6].step[6].vel == 70 && pat.track[6].step[6].micro == 0);
    pat.track[7].quantize = 50;
    k = seq_record(&S, 7, 70, 0);
    CHECK(k == 6 && pat.track[7].step[6].micro == 11);         /* 30/125 x 96 = 23 -> 11 */
    seq_stop(&S);
    CHECK(seq_record(&S, 5, 80, 0) == 0xFF && seq_playhead(&S, 5) == 0xFF);
}

/* recording onto a step that already plays builds a chord (max 4, no duplicates) */
static void test_record_chord(void)
{
    setup();
    seq_start(&S, now);
    run_to(1000 + 2 * 125 + 5);
    struct seq_step *st = &pat.track[3].step[2];
    CHECK(seq_record(&S, 3, 90, 0) == 2 && seq_step_count(st) == 1);
    CHECK(seq_record(&S, 3, 50, 4) == 2 && seq_step_count(st) == 2 && st->extra[0] == 4);
    CHECK(st->vel == 90);                                      /* one velocity per step */
    seq_record(&S, 3, 50, 4);                                  /* already there */
    seq_record(&S, 3, 50, 0);
    CHECK(seq_step_count(st) == 2);
    seq_record(&S, 3, 50, 7);
    seq_record(&S, 3, 50, 11);
    seq_record(&S, 3, 50, 12);                                 /* a 5th note: full */
    CHECK(seq_step_count(st) == 4 && st->extra[1] == 7 && st->extra[2] == 11);
    CHECK(SEQ_RATCHET_HITS(st->ratchet) == 1);                 /* chord bits don't touch ratchets */
}

static void test_chords_scales(void)
{
    setup();
    /* track 0: C E G chord, offsets in the effective scale (global "follow preset" = chromatic) */
    pat.track[0].note = 60;
    pat.track[0].step[0].vel = 100;
    pat.track[0].step[0].ratchet = (2 << 6) | (1 << 4);       /* 3 notes x 2 hits */
    pat.track[0].step[0].extra[0] = 4;
    pat.track[0].step[0].extra[1] = 7;
    seq_start(&S, now);
    run_to(1000 + 120);
    CHECK(count_on(60, 1000, 1001) == 1 && count_on(64, 1000, 1001) == 1 && count_on(67, 1000, 1001) == 1);
    CHECK(count_on(60, 1000, 1120) == 2 && count_on(67, 1000, 1120) == 2);   /* each note ratcheted */
    seq_stop(&S);
    /* global major: following tracks move in the scale, a chromatic (drum) track does not */
    setup();
    S.gscale = SCALE_MAJOR + 1;
    pat.track[0].note = 60;
    pat.track[0].step[0].vel = 100;
    pat.track[0].step[0].ratchet = 1 << 6;
    pat.track[0].step[0].extra[0] = 2;                        /* 2 scale steps = E */
    pat.track[1].note = 36;
    pat.track[1].scale = SCALE_CHROMATIC + 1;
    pat.track[1].step[0].vel = 100;
    pat.track[1].step[0].note = 1;                             /* 37, not D */
    pat.track[2].note = 62;
    pat.track[2].root = 3;                                     /* own root D, follows global major */
    pat.track[2].step[0].vel = 100;
    pat.track[2].step[0].note = 2;                             /* D E F# */
    seq_start(&S, now);
    run_to(1000 + 50);
    CHECK(count_on(60, 1000, 1050) == 1 && count_on(64, 1000, 1050) == 1);
    CHECK(count_on(37, 1000, 1050) == 1 && count_on(66, 1000, 1050) == 1);
    CHECK(seq_track_scale(&S, &pat.track[1]) == SCALE_CHROMATIC && seq_track_scale(&S, &pat.track[0]) == SCALE_MAJOR);
    S.gscale = SCALE_CHROMATIC + 1;                            /* global chromatic forces it */
    CHECK(seq_track_note(&S, &pat.track[0], 2) == 62);
    S.gscale = SCALE_COUNT + 5;                                /* bad value: chromatic, no crash */
    CHECK(seq_track_scale(&S, &pat.track[0]) == SCALE_CHROMATIC);
}

/* queue: events leave in time order; an overload drops whole hits (never a lone note off) */
static void test_queue_load(void)
{
    setup();
    for (u8 t = 0; t < SEQ_TRACKS; t++) {                      /* 16 tracks x 4-note chords, every step */
        pat.track[t].chan = t;
        for (u8 k = 0; k < 16; k++) {
            struct seq_step *st = &pat.track[t].step[k];
            st->vel = 100;
            st->ratchet = 3 << 6;
            st->extra[0] = 3; st->extra[1] = 7; st->extra[2] = 10;
            st->micro = (s8)((k * 7 + t * 5) % 40 - 20);       /* mixed micro-timing */
        }
    }
    seq_start(&S, now);
    run_to(1000 + 4000);
    CHECK(S.dropped == 0 && S.late == 0);
    int ons = 0;
    for (int i = 0; i < nlg; i++) {
        ons += lg[i].n == 3 && (lg[i].m[0] & 0xF0) == 0x90;
    }
    CHECK(ons >= 16 * 4 * 32 - 64);                            /* 32 steps in 4 s, all notes */
    /* add 4 ratchets everywhere: far over the queue; hits drop, every on still gets its off */
    for (u8 t = 0; t < SEQ_TRACKS; t++) {
        for (u8 k = 0; k < 16; k++) {
            pat.track[t].step[k].ratchet |= 3 << 4;
        }
    }
    run_to(now + 2000);
    CHECK(S.dropped > 0 && S.nq <= SEQ_QUEUE);
    for (int i = S.qh + 1; i < S.qh + S.nq; i++) {
        CHECK((s32)(S.q[i].at - S.q[i - 1].at) >= 0);
    }
    seq_stop(&S);
    int bal[16][128];
    memset(bal, 0, sizeof(bal));
    for (int i = 0; i < nlg; i++) {
        if (lg[i].n == 3) {
            bal[lg[i].m[0] & 15][lg[i].m[1]] += (lg[i].m[0] & 0xF0) == 0x90 ? 1 : -1;
        }
    }
    int stuck = 0;
    for (int c = 0; c < 16; c++) {
        for (int n = 0; n < 128; n++) {
            stuck += bal[c][n] > 0;
        }
    }
    CHECK(stuck == 0);
}

/* rate = the editing grid: changing it keeps every note where it is in time */
static void test_rate_grid(void)
{
    setup();
    struct seq_track *t = &pat.track[0];
    for (u8 k = 0; k < 16; k += 2) {
        t->step[k].vel = 100;                                  /* every 1/8 at a 1/16 rate */
    }
    CHECK(seq_set_rate(&S, 0, SEQ_RATE_32, 1));
    CHECK(t->rate == SEQ_RATE_32 && t->res == SEQ_RATE_32 && t->len == 32 && t->step[4].vel && !t->step[2].vel);
    seq_start(&S, now);
    run_to(1000 + 600);
    CHECK(nth_on(36, 0) == 1000 && near(nth_on(36, 1), 1250, 1) && near(nth_on(36, 2), 1500, 1));
    CHECK(near(nth_off(36, 0), 1000 + 63, 1));                 /* gate length kept */
    seq_stop(&S);

    setup();
    for (u8 k = 0; k < 16; k++) {
        pat.track[0].step[k].vel = 100;                        /* every 1/16 */
    }
    CHECK(seq_set_rate(&S, 0, SEQ_RATE_4, 1));                 /* coarser: notes between stay */
    CHECK(t->rate == SEQ_RATE_4 && t->res == SEQ_RATE_16 && seq_track_grid(t) == 4 && t->len == 16);
    seq_start(&S, now);
    run_to(1000 + 300);
    CHECK(near(nth_on(36, 1), 1125, 1) && near(nth_on(36, 2), 1250, 1));
    seq_stop(&S);
    /* empty track: same loop time at the new rate */
    CHECK(seq_set_rate(&S, 1, SEQ_RATE_8, 1) && pat.track[1].len == 8 && pat.track[1].res == SEQ_RATE_8);
    /* straight -> triplet through the coarsest exact resolution (a lone note: 1/16T itself), and back */
    struct seq_track *t2 = &pat.track[2];
    t2->step[4].vel = 100;
    CHECK(seq_set_rate(&S, 2, SEQ_RATE_16T, 1));
    CHECK(t2->res == SEQ_RATE_16T && t2->len == 24 && t2->step[6].vel && seq_track_grid(t2) == 1);
    CHECK(seq_set_rate(&S, 2, SEQ_RATE_16, 1));
    CHECK(t2->res == SEQ_RATE_16 && t2->len == 16 && t2->step[4].vel && t2->gate == 3);
    /* every 1/16 filled: 1/16T needs the common 1/32T */
    struct seq_track *t8 = &pat.track[8];
    for (u8 k = 0; k < 16; k++) {
        t8->step[k].vel = 100;
    }
    CHECK(seq_set_rate(&S, 8, SEQ_RATE_16T, 1));
    CHECK(t8->res == SEQ_RATE_32T && t8->len == 48 && t8->step[3].vel && !t8->step[1].vel && seq_track_grid(t8) == 2);
    /* micro-timing beyond half a new step moves the note to the neighbouring step */
    struct seq_track *t3 = &pat.track[3];
    seq_set_rate(&S, 3, SEQ_RATE_8, 0);                        /* speed change: res = rate = 1/8 */
    t3->step[1].vel = 100;
    t3->step[1].micro = 40;
    CHECK(seq_set_rate(&S, 3, SEQ_RATE_32, 1));
    CHECK(t3->step[6].vel && t3->step[6].micro == -32 && !t3->step[4].vel);   /* 4 + 160/96 */
    /* refused, untouched: 64 steps of 1/16 can't become 1/32 */
    struct seq_track *t4 = &pat.track[4];
    t4->len = 64;
    for (u8 k = 0; k < 64; k++) {
        t4->step[k].vel = 100;
    }
    CHECK(!seq_set_rate(&S, 4, SEQ_RATE_32, 1) && t4->res == SEQ_RATE_16 && t4->len == 64 && t4->step[1].vel);
    /* 1/32 <-> triplets with notes on every 1/32: no common resolution */
    struct seq_track *t5 = &pat.track[5];
    seq_set_rate(&S, 5, SEQ_RATE_32, 0);
    t5->step[0].vel = t5->step[1].vel = 100;
    CHECK(!seq_set_rate(&S, 5, SEQ_RATE_32T, 1) && t5->rate == SEQ_RATE_32 && t5->step[1].vel);
    /* an auto ratchet keeps its hit spacing when the steps get finer */
    struct seq_track *t6 = &pat.track[6];
    t6->step[0].vel = 100;
    t6->step[0].ratchet = 1 << 4;                              /* 2 hits, auto: 120 units apart */
    CHECK(seq_set_rate(&S, 6, SEQ_RATE_32, 1) && (t6->step[0].ratchet & 0x0f) == 5);   /* table 120 */
    /* recording snaps to the grid, stored at the resolution */
    struct seq_track *t7 = &pat.track[7];
    t7->step[1].vel = 100;                                     /* off the 1/4 grid: res stays 1/16 */
    CHECK(seq_set_rate(&S, 7, SEQ_RATE_4, 1) && t7->res == SEQ_RATE_16);
    seq_start(&S, now);
    run_to(now + 5 * 125 + 10);                                /* nearest 1/4: stored step 4 */
    CHECK(seq_record(&S, 7, 90, 0) == 4 && t7->step[4].vel == 90);
}

int main(void)
{
    test_scale();
    test_basic_timeline();
    test_polymeter_rate();
    test_swing_micro_ratchet();
    test_probability();
    test_conditions_tie();
    test_stop_flush_retrigger();
    test_external_clock();
    test_external_clock_lead_jitter();
    test_record();
    test_record_chord();
    test_chords_scales();
    test_queue_load();
    test_rate_grid();
    printf(failures ? "test_smcpad_seq: %d FAILED\n" : "test_smcpad_seq: ALL PASSED\n", failures);
    return failures != 0;
}
