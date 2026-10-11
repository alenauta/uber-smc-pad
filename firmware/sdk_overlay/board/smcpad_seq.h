/* SMC-PAD step sequencer engine (pure C, host-tested by test/test_smcpad_seq.c).
 *
 * Model: a pattern = 16 tracks x 64 steps. Every track has its own base note, MIDI channel,
 * loop length ("steps"), rate (1/4 .. 1/32T), gate, probability, record quantize and ratchet
 * rate; steps can override gate / probability / ratchet rate and add micro-timing, a note offset
 * (in scale steps), a trig condition (A:B, first loop, FILL, NOT FILL) and tie.
 *
 * Time: the caller passes a millisecond clock (seq_run(now_ms)). The play position is kept in
 * units of 1/960 quarter note with a remainder accumulator, so tempo changes are glitch-free
 * and the resolution does not depend on how often seq_run() is called. Hits of a step are put
 * in a small time-sorted queue as soon as the position is within half a step of the step's
 * grid time (negative micro-timing / swing), and sent when due.
 *
 * Clock: internal tempo (centi-BPM) or, after MIDI clock (F8) arrives, follows it: every F8 is
 * 40 units (24 PPQN). The host tempo is measured over the last 24 pulses (one beat), so bunched
 * Bluetooth packets do not upset it; between pulses the position runs at that tempo and each F8
 * pulls it towards the pulse's position (snap if far off, otherwise a quarter of the error).
 * "Play early" (lead_ms) sends notes that many ms ahead of the host's clock to cancel the
 * round trip through the DAW. Clock out (F8 / FA / FC) only while internal.
 */
#ifndef SMCPAD_SEQ_H
#define SMCPAD_SEQ_H

#ifdef SMCPAD_HOST_TEST
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
#else
#include "typedef.h"
#endif

#define SEQ_TRACKS          16
#define SEQ_STEPS           64
#define SEQ_PPQN            960             /* position units per quarter note */
#define SEQ_CLOCK_UNITS     (SEQ_PPQN / 24) /* one MIDI clock */
#define SEQ_MICRO_MAX       48              /* micro-timing: +-48/96 step */
#define SEQ_QUEUE           192             /* pending note on/off events, kept in time order */
#define SEQ_ACTIVE          64              /* notes currently sounding */

enum { SEQ_RATE_4 = 0, SEQ_RATE_4T, SEQ_RATE_8, SEQ_RATE_8T, SEQ_RATE_16, SEQ_RATE_16T,
       SEQ_RATE_32, SEQ_RATE_32T, SEQ_RATES };

/* trig conditions (step.cond & SEQ_COND_MASK) */
enum { SEQ_COND_ALWAYS = 0,
       SEQ_COND_1_2, SEQ_COND_2_2, SEQ_COND_1_3, SEQ_COND_2_3, SEQ_COND_3_3,
       SEQ_COND_1_4, SEQ_COND_2_4, SEQ_COND_3_4, SEQ_COND_4_4,
       SEQ_COND_FIRST, SEQ_COND_FILL, SEQ_COND_NOT_FILL, SEQ_CONDS };
#define SEQ_COND_MASK       0x0F
#define SEQ_COND_TIE        0x80

#define SEQ_GATES           15              /* gate table index 0..14 */
#define SEQ_RATCHET_RATES   7               /* auto (split the step), 1/8 1/16 1/16T 1/32 1/32T 1/64 */
#define SEQ_EXT_RING        24              /* F8 timestamps kept for the tempo (one beat) */
#define SEQ_LEAD_MAX_MS     63              /* play early: 0..63 ms, 1 ms steps */

#define SEQ_CHORD           4               /* notes per step */

struct seq_step {
    u8 vel;         /* 0 = off, 1..127 */
    s8 note;        /* first note: offset from the track's base note, in scale steps */
    u8 prob_gate;   /* prob << 4 | gate; prob 0 = track, 1..15 = 0..100 %; gate 0 = track, n = table[n-1] */
    s8 micro;       /* -48..+48 in 1/96 step */
    u8 ratchet;     /* (chord notes-1) << 6 | (ratchet hits-1) << 4 | rate (0 = track, n = table[n-1]) */
    u8 cond;        /* SEQ_COND_* | SEQ_COND_TIE */
    s8 extra[SEQ_CHORD - 1];                /* more chord notes (offsets), count in ratchet bits 6-7 */
};
#define SEQ_RATCHET_HITS(r)  ((u8)((((r) >> 4) & 3) + 1))
#define SEQ_CHORD_NOTES(r)   ((u8)((((r) >> 6) & 3) + 1))

struct seq_track {
    u8 chan;        /* 0..15 */
    u8 note;        /* base note */
    u8 len;         /* 1..64 steps (of res) */
    u8 rate;        /* SEQ_RATE_*: the editing grid (step view, recording); a multiple of res */
    u8 res;         /* SEQ_RATE_*: how long a stored step is (playback). Changing the grid to a
                     * coarser rate keeps res, so notes between grid steps stay (seq_set_rate) */
    u8 gate;        /* gate table index */
    u8 vol;         /* 0..127, scales velocities */
    u8 prob;        /* 0..100 % */
    u8 quantize;    /* 0..100 %: how far live recording pulls hits to the grid */
    u8 ratchet_rate;/* 0..SEQ_RATCHET_RATES-1, 0 = auto */
    u8 mute;
    u8 root;        /* 0 = follow the global root, 1..12 = C..B */
    u8 scale;       /* 0 = follow the global scale, 1.. = smcpad_scale index + 1 (1 = chromatic) */
    struct seq_step step[SEQ_STEPS];
};

#define SEQ_PATTERN_MAGIC   0x5A
#define SEQ_PATTERN_VER     3
struct seq_pattern {
    u8 magic, ver;
    u8 rsv[6];
    struct seq_track track[SEQ_TRACKS];
};

struct seq_out {
    void *ctx;
    void (*midi)(void *ctx, const u8 *m, u32 n);
};

struct seq_event {
    u32 at;         /* position */
    u8  on;         /* 1 = note on, 0 = note off */
    u8  chan, note, vel;
    u8  track;
};

struct seq_track_state {
    u32 next_grid;  /* position of the next step to schedule */
    u8  step;       /* index of that step */
    u8  rsv;
    u16 loop;       /* completed loops (trig conditions) */
};

struct seq {
    const struct seq_out *out;
    struct seq_pattern *pat;
    u8  running;
    u8  sync;       /* follow external clock when a host sends Start/Continue */
    u8  following;  /* external clock drives the position */
    u8  self_started; /* Play was pressed on the pad: ignore host Start (could be our own echo) */
    u8  fill;
    u8  swing;      /* 50..75 % */
    u8  groot;      /* global root: 0 = follow the preset (each track's base note), 1..12 = C..B */
    u8  gscale;     /* global scale: 0 = follow the preset (chromatic here), 1.. = smcpad_scale index + 1 */
    u32 centibpm;   /* internal tempo */
    u32 pos;        /* current position */
    u32 rem;        /* remainder of the ms -> units conversion */
    u32 last_ms;
    u32 clock_next; /* next F8 to send (internal) */
    u32 rng;
    /* external clock */
    u32 ext_last_ms, ext_interval_q8;   /* ms << 8 between F8s, averaged over the ring */
    u32 ext_pulses;                     /* F8s since Start */
    u32 ext_ts[SEQ_EXT_RING];           /* arrival times of the last F8s */
    u8  ext_n, ext_i;                   /* entries in use, next slot */
    u8  ext_seen;
    u8  lead_ms;                        /* play early while following (0..SEQ_LEAD_MAX_MS) */
    s16 ext_err;                        /* diag: pulse position - play position at the last F8 */
    u16 ext_err_max;                    /* diag: largest |ext_err| since the host's Start */
    struct seq_track_state ts[SEQ_TRACKS];
    struct seq_event q[SEQ_QUEUE];          /* q[qh .. qh+nq-1], earliest first */
    u8  qh, nq;
    struct { u8 chan, note; } active[SEQ_ACTIVE];
    u8  nactive;
    u16 hits;       /* bit t: track t sent a note on since the UI last cleared it */
    u32 late;       /* events emitted more than ~2 ms late (diagnostics) */
    u32 dropped;    /* queue overflows */
    u8  rec_created;/* the last seq_record() call started a new step (vs. added a chord note) */
};

/* tables (exposed for UI and tests) */
extern const u16 seq_rate_units[SEQ_RATES];    /* step length in units */
extern const u8  seq_gate_eighths[SEQ_GATES];  /* gate length in 1/8 step */
extern const u16 seq_ratchet_units[SEQ_RATCHET_RATES];  /* hit spacing, 0 = auto (step / count) */
u32  seq_step_units(u8 rate);
/* stored steps per grid step (rate / res, >= 1) */
u8   seq_track_grid(const struct seq_track *t);
/* change a track's rate. keep_timing = 1: the notes stay where they are in time (finer: the
 * stored steps are spread out, coarser: only the grid changes, so notes between grid steps stay;
 * straight <-> triplet via a resolution both divide). keep_timing = 0: the same steps play at
 * the new rate (faster / slower). Returns 0 (nothing changed) if it would need > 64 steps or no
 * common resolution exists (1/32 <-> triplets). */
u8   seq_set_rate(struct seq *s, u8 track, u8 rate, u8 keep_timing);

void seq_pattern_default(struct seq_pattern *p, const u8 *notes, const u8 *chans);
void seq_init(struct seq *s, const struct seq_out *out, struct seq_pattern *pat, u32 seed);
void seq_set_tempo(struct seq *s, u32 centibpm);   /* clamped 30.00..300.00 BPM */
void seq_start(struct seq *s, u32 now_ms);          /* from the top */
void seq_continue(struct seq *s, u32 now_ms);
void seq_stop(struct seq *s);                       /* all sounding notes off */
void seq_run(struct seq *s, u32 now_ms);            /* emit everything due */
void seq_clock_in(struct seq *s, u8 rt, u32 now_ms);/* F8 FA FB FC from a host */
void seq_clock_timeout(struct seq *s, u32 now_ms);  /* call regularly: lost clock -> internal */
u32  seq_ext_centibpm(const struct seq *s);         /* measured external tempo (0 = none) */

/* step / note helpers */
u8   seq_step_prob(const struct seq_track *t, const struct seq_step *st);   /* 0..100 */
u32  seq_step_gate_units(const struct seq_track *t, const struct seq_step *st);
u8   seq_track_scale(const struct seq *s, const struct seq_track *t);       /* effective scale */
u8   seq_track_root(const struct seq *s, const struct seq_track *t);        /* effective root */
u8   seq_track_note(const struct seq *s, const struct seq_track *t, s8 offset);
u8   seq_step_count(const struct seq_step *st);                             /* chord notes 1..4 */
s8   seq_step_offset(const struct seq_step *st, u8 i);
s32  seq_step_micro_units(const struct seq_track *t, const struct seq_step *st);

/* live recording: writes the hit into the nearest step of the track at the current position.
 * An empty step takes the note, velocity and micro-timing (offset scaled by (100 - track
 * quantize) %, so the default 100 % snaps to the step); a step that already plays adds the note
 * to its chord (up to SEQ_CHORD, a note already there is not added twice). Returns the step
 * index (0xFF when stopped). */
u8   seq_record(struct seq *s, u8 track, u8 vel, s8 note);
u8   seq_playhead(const struct seq *s, u8 track);   /* step whose grid time last passed */

#endif
