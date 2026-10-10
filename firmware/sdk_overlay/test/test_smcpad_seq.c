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
    pat.track[2].rate = SEQ_RATE_8;
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
    pat.track[2].step[0].ratchet = (3 << 4) | 3;               /* 4 hits, 1/4 step apart */
    pat.track[3].step[2].vel = 100;
    pat.track[3].step[2].micro = 24;
    pat.track[3].quantize = 100;                               /* fully quantized: on grid */
    seq_start(&S, now);
    run_to(1000 + 600);
    CHECK(near(nth_on(36, 0), 1188, 1));
    CHECK(near(nth_on(37, 0), 1438, 1));
    CHECK(nth_on(38, 0) == 1000 && near(nth_on(38, 1), 1031, 1) && near(nth_on(38, 2), 1063, 1) &&
          near(nth_on(38, 3), 1094, 1));
    CHECK(near(nth_off(38, 0), 1000 + 23, 1));                 /* ratchet gate 3/4 of interval */
    CHECK(near(nth_on(39, 0), 1250, 1));
}

static void test_probability(void)
{
    setup();
    seq_set_tempo(&S, 30000);
    struct seq_track *t = &pat.track[0];
    t->len = 1;
    t->rate = SEQ_RATE_32T;                                    /* 60 steps per second */
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
}

static void test_record(void)
{
    setup();
    seq_start(&S, now);
    run_to(1000 + 2 * 125 + 10);                               /* 10 ms after step 2 */
    u8 k = seq_record(&S, 5, 90, 0);
    CHECK(k == 2 && pat.track[5].step[2].vel == 90);
    CHECK(pat.track[5].step[2].micro == 7);                    /* 10/125 x 96 = 7.68 */
    run_to(1000 + 4 * 125 - 20);                               /* 20 ms before step 4 */
    k = seq_record(&S, 5, 80, 3);
    CHECK(k == 4 && pat.track[5].step[4].micro == -15 && pat.track[5].step[4].note == 3);
    CHECK(seq_playhead(&S, 5) == 3);
    seq_stop(&S);
    CHECK(seq_record(&S, 5, 80, 0) == 0xFF && seq_playhead(&S, 5) == 0xFF);
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
    test_record();
    printf(failures ? "test_smcpad_seq: %d FAILED\n" : "test_smcpad_seq: ALL PASSED\n", failures);
    return failures != 0;
}
