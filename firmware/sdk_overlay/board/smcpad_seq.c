#include "smcpad_seq.h"
#include "smcpad_scale.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

const u16 seq_rate_units[SEQ_RATES] = {960, 640, 480, 320, 240, 160, 120, 80};
const u8  seq_gate_eighths[SEQ_GATES] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 28, 32};
const u16 seq_ratchet_units[SEQ_RATCHET_RATES] = {0, 480, 240, 160, 120, 80, 60};

#define TIE_OVERLAP     10              /* units a tied note overlaps the next step */
#define EXT_TIMEOUT_MS  500             /* no F8 for this long: back to the internal clock */
#define EXT_IV_MIN_Q8   1600            /* F8 spacing (ms << 8) accepted: 400 BPM ... */
#define EXT_IV_MAX_Q8   32000           /* ... 20 BPM */

u32 seq_step_units(u8 rate)
{
    return seq_rate_units[rate < SEQ_RATES ? rate : SEQ_RATE_16];
}

static void send(struct seq *s, const u8 *m, u32 n)
{
    if (s->out && s->out->midi) {
        s->out->midi(s->out->ctx, m, n);
    }
}

static void send1(struct seq *s, u8 b)
{
    send(s, &b, 1);
}

static u32 rnd(struct seq *s)
{
    u32 x = s->rng ? s->rng : 0x9E3779B9u;      /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

void seq_pattern_default(struct seq_pattern *p, const u8 *notes, const u8 *chans)
{
    memset(p, 0, sizeof(*p));
    p->magic = SEQ_PATTERN_MAGIC;
    p->ver = SEQ_PATTERN_VER;
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        struct seq_track *t = &p->track[i];
        t->chan = chans ? (u8)(chans[i] & 0x0f) : 9;
        t->note = notes ? (u8)(notes[i] & 0x7f) : (u8)(36 + i);
        t->len = 16;
        t->rate = SEQ_RATE_16;
        t->res = SEQ_RATE_16;
        t->gate = 3;                            /* 1/2 step */
        t->vol = 127;
        t->prob = 100;
        t->quantize = 100;                      /* live recording snaps to the step */
        t->ratchet_rate = 0;                    /* auto: hits split the step */
    }
}

void seq_init(struct seq *s, const struct seq_out *out, struct seq_pattern *pat, u32 seed)
{
    memset(s, 0, sizeof(*s));
    s->out = out;
    s->pat = pat;
    s->rng = seed ? seed : 1;
    s->swing = 50;
    s->sync = 1;
    s->centibpm = 12000;
}

void seq_set_tempo(struct seq *s, u32 centibpm)
{
    s->centibpm = centibpm < 3000 ? 3000 : (centibpm > 30000 ? 30000 : centibpm);
}

/* ---- step parameters -------------------------------------------------------------- */
u8 seq_step_prob(const struct seq_track *t, const struct seq_step *st)
{
    u8 n = st->prob_gate >> 4;
    return n ? (u8)((n - 1) * 100 / 14) : (t->prob > 100 ? 100 : t->prob);
}

u32 seq_step_gate_units(const struct seq_track *t, const struct seq_step *st)
{
    u8 g = st->prob_gate & 0x0f;
    u8 idx = g ? (u8)(g - 1) : t->gate;
    if (idx >= SEQ_GATES) {
        idx = SEQ_GATES - 1;
    }
    u32 u = seq_step_units(t->res) * seq_gate_eighths[idx] / 8;
    return u ? u : 1;
}

u8 seq_track_scale(const struct seq *s, const struct seq_track *t)
{
    u8 v = t->scale ? t->scale : s->gscale;     /* track, else global; 0 = chromatic */
    return (v && v - 1 < SCALE_COUNT) ? (u8)(v - 1) : SCALE_CHROMATIC;
}

u8 seq_track_root(const struct seq *s, const struct seq_track *t)
{
    if (t->root) {                              /* track's own */
        return (u8)((t->root - 1) % 12);
    }
    if (s->groot) {                             /* global */
        return (u8)((s->groot - 1) % 12);
    }
    return (u8)(s->proot % 12);                 /* follow the preset: its pad-1 note */
}

u8 seq_track_note(const struct seq *s, const struct seq_track *t, s8 offset)
{
    return smcpad_scale_note(t->note, offset, seq_track_root(s, t), seq_track_scale(s, t));
}

u8 seq_step_count(const struct seq_step *st)
{
    return SEQ_CHORD_NOTES(st->ratchet);
}

s8 seq_step_offset(const struct seq_step *st, u8 i)
{
    return i == 0 ? st->note : st->extra[(i - 1) % (SEQ_CHORD - 1)];
}

s8 seq_step_micro(const struct seq_track *t, const struct seq_step *st)
{
    s32 m = st->micro;
    m = m > SEQ_MICRO_MAX ? SEQ_MICRO_MAX : (m < -SEQ_MICRO_MAX ? -SEQ_MICRO_MAX : m);
    if (st->cond & SEQ_COND_REC) {
        s32 q = t->quantize > 100 ? 100 : t->quantize;
        m = m * (100 - q) / 100;
    }
    return (s8)m;
}

s32 seq_step_micro_units(const struct seq_track *t, const struct seq_step *st)
{
    return (s32)seq_step_micro(t, st) * (s32)seq_step_units(t->res) / 96;
}

/* ---- grid / resolution ---------------------------------------------------------------- */
u8 seq_track_grid(const struct seq_track *t)
{
    u32 ur = seq_step_units(t->res), ug = seq_step_units(t->rate);
    return (u8)((ug >= ur && ug % ur == 0) ? ug / ur : 1);     /* table units: never 0 */
}

static u8 gate_nearest(u32 eighths)             /* gate table index closest to eighths */
{
    u8 best = 0;
    for (u8 i = 1; i < SEQ_GATES; i++) {
        u32 db = eighths > seq_gate_eighths[best] ? eighths - seq_gate_eighths[best] : seq_gate_eighths[best] - eighths;
        u32 di = eighths > seq_gate_eighths[i] ? eighths - seq_gate_eighths[i] : seq_gate_eighths[i] - eighths;
        if (di < db) {
            best = i;
        }
    }
    return best;
}

/* put st at index i, or add its notes to the chord already there (up to SEQ_CHORD) */
static void step_place(struct seq_step *dst, const struct seq_step *st)
{
    if (!dst->vel) {
        *dst = *st;
        return;
    }
    for (u8 n = 0; n < seq_step_count(st); n++) {
        s8 o = seq_step_offset(st, n);
        u8 c = seq_step_count(dst), dup = 0;
        for (u8 j = 0; j < c; j++) {
            dup |= (u8)(seq_step_offset(dst, j) == o);
        }
        if (!dup && c < SEQ_CHORD) {
            dst->extra[c - 1] = o;
            dst->ratchet = (u8)((dst->ratchet & 0x3F) | (c << 6));
        }
    }
}

static struct seq_step rescale_buf[SEQ_STEPS];  /* seq_set_rate scratch (not on the task stack) */
static struct seq_track rate_backup;            /* seq_set_rate: undo a refused change */

static u8 gate_exact(u32 eighths)               /* table index with exactly this many eighths, 0xFF */
{
    for (u8 i = 0; i < SEQ_GATES; i++) {
        if (seq_gate_eighths[i] == eighths) {
            return i;
        }
    }
    return 0xFF;
}

static u8 ratchet_auto(const struct seq_track *t, const struct seq_step *st)   /* hits split the step */
{
    u8 rr = st->ratchet & 0x0f;
    return (u8)(SEQ_RATCHET_HITS(st->ratchet) > 1 && (rr ? rr == 1 : t->ratchet_rate == 0));
}

/* stored steps m times finer: step i moves to i * m (micro-timing beyond half a step moves it to
 * the neighbouring step), gates keep their length (up to 4 steps), auto ratchets their spacing */
static u8 expand(struct seq *s, u8 ti, u8 m)
{
    struct seq_track *t = &s->pat->track[ti];
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    u32 nlen = (u32)len * m;
    u32 uold = seq_step_units(t->res);
    if (m < 2 || nlen > SEQ_STEPS) {
        return m == 1;
    }
    memcpy(rescale_buf, t->step, sizeof(rescale_buf));
    memset(t->step, 0, sizeof(t->step));
    for (u8 i = 0; i < len; i++) {
        struct seq_step st = rescale_buf[i];
        if (!st.vel) {
            continue;
        }
        if (ratchet_auto(t, &st)) {             /* keep the hit spacing if the table has it */
            u32 iv = uold / SEQ_RATCHET_HITS(st.ratchet);
            for (u8 j = 1; j < SEQ_RATCHET_RATES; j++) {
                if (seq_ratchet_units[j] == iv) {
                    st.ratchet = (u8)((st.ratchet & 0xF0) | (j + 1));
                }
            }
        }
        s32 off = (s32)st.micro * m;            /* 1/96 of a new step */
        s32 sh = (st.cond & SEQ_COND_REC) ? 0 : (off >= 0 ? (off + 48) / 96 : -((-off + 48) / 96));
        s32 nm = off - sh * 96;
        st.micro = (s8)(nm > SEQ_MICRO_MAX ? SEQ_MICRO_MAX : (nm < -SEQ_MICRO_MAX ? -SEQ_MICRO_MAX : nm));
        u8 g = st.prob_gate & 0x0f;
        if (g) {
            st.prob_gate = (u8)((st.prob_gate & 0xF0) | (gate_nearest((u32)seq_gate_eighths[g - 1] * m) + 1));
        }
        s32 ni = ((s32)i * m + sh) % (s32)nlen;
        step_place(&t->step[ni < 0 ? ni + (s32)nlen : ni], &st);
    }
    t->gate = gate_nearest((u32)seq_gate_eighths[t->gate < SEQ_GATES ? t->gate : SEQ_GATES - 1] * m);
    t->len = (u8)nlen;
    s->ts[ti].step = (u8)(s->ts[ti].step * m);  /* same point in time (next_grid unchanged) */
    return 1;
}

/* stored steps m times coarser, only if nothing changes: every note on a multiple of m, its
 * micro-timing and gate divide exactly, no auto ratchet (its spacing would change) */
static u8 shrink(struct seq *s, u8 ti, u8 m)
{
    struct seq_track *t = &s->pat->track[ti];
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    u8 tg = t->gate < SEQ_GATES ? t->gate : SEQ_GATES - 1;
    if (m < 2 || len % m || seq_gate_eighths[tg] % m || gate_exact(seq_gate_eighths[tg] / m) == 0xFF) {
        return 0;
    }
    for (u8 i = 0; i < len; i++) {
        const struct seq_step *st = &t->step[i];
        u8 g = st->prob_gate & 0x0f;
        if (st->vel && (i % m || st->micro % m || ratchet_auto(t, st) ||
                        (g && (seq_gate_eighths[g - 1] % m || gate_exact(seq_gate_eighths[g - 1] / m) == 0xFF)))) {
            return 0;
        }
    }
    u8 nlen = (u8)(len / m);
    for (u8 i = 0; i < nlen; i++) {             /* ascending: i * m >= i */
        struct seq_step st = t->step[i * m];
        if (st.vel) {
            u8 g = st.prob_gate & 0x0f;
            st.micro = (s8)(st.micro / m);
            if (g) {
                st.prob_gate = (u8)((st.prob_gate & 0xF0) | (gate_exact(seq_gate_eighths[g - 1] / m) + 1));
            }
        }
        t->step[i] = st;
    }
    memset(&t->step[nlen], 0, sizeof(t->step[0]) * (SEQ_STEPS - nlen));
    t->gate = gate_exact(seq_gate_eighths[tg] / m);
    t->len = nlen;
    struct seq_track_state *ts = &s->ts[ti];    /* next step: the next coarse one, at its grid */
    u32 ns = ((u32)ts->step + m - 1) / m;
    ts->next_grid += (ns * m - ts->step) * seq_step_units(t->res);
    if (ns >= nlen) {
        ns = 0;
        ts->loop++;
    }
    ts->step = (u8)ns;
    return 1;
}

/* make the stored steps as coarse as possible without changing anything; limit = a rate the
 * result must divide (the grid), SEQ_RATES = any */
static void compact(struct seq *s, u8 ti, u8 limit)
{
    struct seq_track *t = &s->pat->track[ti];
    u32 ur = seq_step_units(t->res);
    for (u8 r = 0; r < SEQ_RATES; r++) {        /* coarsest first */
        u32 uc = seq_step_units(r);
        if (uc <= ur || uc % ur || (limit < SEQ_RATES && seq_step_units(limit) % uc)) {
            continue;
        }
        if (shrink(s, ti, (u8)(uc / ur))) {
            t->res = r;
            return;
        }
    }
}

/* storage -> a resolution the new grid is a multiple of (expanding as needed) */
static u8 convert(struct seq *s, u8 ti, u8 rate)
{
    struct seq_track *t = &s->pat->track[ti];
    u32 ur = seq_step_units(t->res), un = seq_step_units(rate);
    if (un % ur == 0) {                         /* coarser grid: storage stays, notes stay */
        return 1;
    }
    for (u8 r = 0; r < SEQ_RATES; r++) {        /* finer / straight <-> triplet: coarsest common */
        u32 ug = seq_step_units(r);
        if (ur % ug == 0 && un % ug == 0) {
            if (!expand(s, ti, (u8)(ur / ug))) {
                return 0;
            }
            t->res = r;
            return 1;
        }
    }
    return 0;                                   /* 1/32 <-> triplets: no common resolution */
}

u8 seq_set_rate(struct seq *s, u8 ti, u8 rate, u8 keep_timing)
{
    if (ti >= SEQ_TRACKS || rate >= SEQ_RATES) {
        return 0;
    }
    struct seq_track *t = &s->pat->track[ti];
    if (!keep_timing) {                         /* same steps, new speed */
        t->rate = t->res = rate;
        return 1;
    }
    u32 ur = seq_step_units(t->res), un = seq_step_units(rate);
    u8 empty = 1;
    for (u8 i = 0; i < SEQ_STEPS && empty; i++) {
        empty = (u8)!t->step[i].vel;
    }
    if (empty) {                                /* nothing to keep: same loop time at the new rate */
        u32 len = t->len ? t->len : 1;
        u32 nl = (len * ur + un / 2) / un;
        t->len = (u8)(nl < 1 ? 1 : (nl > SEQ_STEPS ? SEQ_STEPS : nl));
        s->ts[ti].step = (u8)((u32)s->ts[ti].step * ur / un % t->len);
        t->rate = t->res = rate;
        return 1;
    }
    struct seq_track_state bts = s->ts[ti];
    rate_backup = *t;
    compact(s, ti, SEQ_RATES);                  /* undo what earlier rate changes spread out */
    if (!convert(s, ti, rate)) {
        *t = rate_backup;                       /* refused: exactly as before */
        s->ts[ti] = bts;
        return 0;
    }
    t->rate = rate;
    compact(s, ti, rate);                       /* as coarse as the grid allows */
    return 1;
}

static u8 cond_ok(u8 cond, u16 loop, u8 fill)
{
    static const u8 ab[][2] = {{1, 2}, {2, 2}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {2, 4}, {3, 4}, {4, 4}};
    if (cond >= SEQ_COND_1_2 && cond <= SEQ_COND_4_4) {
        const u8 *c = ab[cond - SEQ_COND_1_2];
        return (u8)(loop % c[1] == (u16)(c[0] - 1));
    }
    switch (cond) {
    case SEQ_COND_FIRST:    return loop == 0;
    case SEQ_COND_FILL:     return fill;
    case SEQ_COND_NOT_FILL: return !fill;
    default:                return 1;
    }
}

/* ---- event queue: q[qh .. qh+nq-1], earliest first, offs before ons at the same time ---- */
static void q_remove(struct seq *s, u8 i)               /* i = index into q[] */
{
    u8 end = (u8)(s->qh + s->nq);
    memmove(&s->q[i], &s->q[i + 1], (u32)(end - i - 1) * sizeof(s->q[0]));
    s->nq--;
}

static void q_insert(struct seq *s, const struct seq_event *ev)
{
    if (s->nq >= SEQ_QUEUE) {
        s->dropped++;
        return;
    }
    if (s->qh + s->nq >= SEQ_QUEUE) {                  /* no room at the end: compact */
        memmove(&s->q[0], &s->q[s->qh], (u32)s->nq * sizeof(s->q[0]));
        s->qh = 0;
    }
    u8 i = (u8)(s->qh + s->nq);
    while (i > s->qh) {                                 /* new events are late: search from the end */
        const struct seq_event *p = &s->q[i - 1];
        s32 d = (s32)(p->at - ev->at);
        if (d < 0 || (d == 0 && (!p->on || ev->on))) {
            break;
        }
        i--;
    }
    memmove(&s->q[i + 1], &s->q[i], (u32)(s->qh + s->nq - i) * sizeof(s->q[0]));
    s->q[i] = *ev;
    s->nq++;
}

static void push(struct seq *s, u32 at, u8 on, u8 chan, u8 note, u8 vel, u8 track)
{
    if (on) {                                   /* an earlier note on the same key ends here */
        for (u8 i = s->qh; i < s->qh + s->nq; i++) {
            const struct seq_event *e = &s->q[i];
            if (!e->on && e->chan == chan && e->note == note && (s32)(e->at - at) > 0) {
                struct seq_event m = *e;
                m.at = at;
                q_remove(s, i);
                q_insert(s, &m);
                i = (u8)(s->qh - 1);            /* rescan (the moved off no longer matches) */
            }
        }
    }
    struct seq_event e = {at, on, chan, note, vel, track};
    q_insert(s, &e);
}

static void active_add(struct seq *s, u8 chan, u8 note)
{
    for (u8 i = 0; i < s->nactive; i++) {
        if (s->active[i].chan == chan && s->active[i].note == note) {
            return;
        }
    }
    if (s->nactive < SEQ_ACTIVE) {
        s->active[s->nactive].chan = chan;
        s->active[s->nactive].note = note;
        s->nactive++;
    }
}

static void active_del(struct seq *s, u8 chan, u8 note)
{
    for (u8 i = 0; i < s->nactive; i++) {
        if (s->active[i].chan == chan && s->active[i].note == note) {
            s->active[i] = s->active[--s->nactive];
            return;
        }
    }
}

static void emit(struct seq *s, u32 upto, u32 late_units)
{
    while (s->nq && (s32)(s->q[s->qh].at - upto) <= 0) {
        struct seq_event e = s->q[s->qh];
        s->qh++;
        s->nq--;
        if (!s->nq) {
            s->qh = 0;
        }
        if ((s32)(upto - e.at) > (s32)late_units) {
            s->late++;
        }
        u8 m[3] = {(u8)((e.on ? 0x90 : 0x80) | e.chan), e.note, e.on ? e.vel : 0x40};
        send(s, m, 3);
        if (e.on) {
            active_add(s, e.chan, e.note);
            s->hits |= (u16)(1u << (e.track & 15));
        } else {
            active_del(s, e.chan, e.note);
        }
    }
}

/* ---- scheduling ----------------------------------------------------------------------- */
static void schedule(struct seq *s, u8 ti)
{
    struct seq_track *t = &s->pat->track[ti];
    struct seq_track_state *ts = &s->ts[ti];
    u32 u = seq_step_units(t->res);
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    u8 k = ts->step < len ? ts->step : 0;
    const struct seq_step *st = &t->step[k];
    u32 g = ts->next_grid;

    if (st->vel && !t->mute && cond_ok(st->cond & SEQ_COND_MASK, ts->loop, s->fill) &&
        rnd(s) % 100 < seq_step_prob(t, st)) {
        s32 off = seq_step_micro_units(t, st);
        if (k & 1) {
            off += (s32)u * (s->swing > 75 ? 25 : (s->swing < 50 ? 0 : s->swing - 50)) / 50;
        }
        u32 at = g + (u32)off;
        if ((s32)(at - s->pos) < 0) {
            at = s->pos;                        /* negative micro-timing at the very start */
        }
        u8 count = SEQ_RATCHET_HITS(st->ratchet);
        u8 rr = st->ratchet & 0x0f;
        u8 ri = rr ? (u8)(rr - 1) : t->ratchet_rate;
        u32 interval = ri < SEQ_RATCHET_RATES ? seq_ratchet_units[ri] : 0;
        if (!interval) {
            interval = u / count;               /* auto: the hits split the step */
        }
        u32 gate = seq_step_gate_units(t, st);
        if (count > 1 && gate > interval * 3 / 4) {
            gate = interval * 3 / 4 ? interval * 3 / 4 : 1;
        }
        u32 vel = (u32)st->vel * (t->vol > 127 ? 127 : t->vol) / 127;
        u8 notes = seq_step_count(st);
        for (u8 n = 0; n < notes; n++) {
            u8 note = seq_track_note(s, t, seq_step_offset(st, n));
            for (u8 h = 0; h < count; h++) {
                u32 on = at + h * interval;
                u32 g_off = on + gate;
                if (h == count - 1 && (st->cond & SEQ_COND_TIE)) {
                    g_off = g + u + TIE_OVERLAP;    /* legato into the next step */
                    if ((s32)(g_off - on) <= 0) {
                        g_off = on + 1;
                    }
                }
                if (s->nq + 2 > SEQ_QUEUE) {    /* full: drop the whole hit, never an off alone */
                    s->dropped++;
                    continue;
                }
                push(s, on, 1, t->chan & 0x0f, note, (u8)(vel ? vel : 1), ti);
                push(s, g_off, 0, t->chan & 0x0f, note, 0, ti);
            }
        }
    }
    ts->next_grid = g + u;
    ts->step = (u8)(k + 1);
    if (ts->step >= len) {
        ts->step = 0;
        ts->loop++;
    }
}

/* play early: lead_ms in position units at the host's tempo (only while following) */
static u32 lead_units(const struct seq *s)
{
    if (!s->following || !s->lead_ms || !s->ext_interval_q8) {
        return 0;
    }
    return (u32)s->lead_ms * SEQ_CLOCK_UNITS * 256u / s->ext_interval_q8;
}

/* a track schedules a step half a step before its time: a cue has to fire before the coarsest
 * track would schedule the bar */
static u32 max_half(const struct seq *s)
{
    u32 m = 1;
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        u32 h = seq_step_units(s->pat->track[i].res) / 2 + 1;
        m = h > m ? h : m;
    }
    return m;
}

u8 seq_cue(struct seq *s, void (*fn)(void *ctx, struct seq *s), void *ctx)
{
    if (!s->running) {
        return 0;
    }
    u32 ahead = s->pos + lead_units(s);
    u32 b = (s->pos / SEQ_BAR_UNITS + 1) * SEQ_BAR_UNITS;
    if ((s32)(b - (ahead + max_half(s))) <= 0) {
        b += SEQ_BAR_UNITS;                     /* too close: tracks may have scheduled it */
    }
    s->cue_at = b;
    s->cue_fn = fn;
    s->cue_ctx = ctx;
    s->cued = 1;
    return 1;
}

void seq_cue_cancel(struct seq *s)
{
    s->cued = 0;
}

static void cue_fire(struct seq *s)
{
    s->cued = 0;
    for (u8 i = 0; i < SEQ_TRACKS; i++) {       /* the old pattern up to the bar (<= half a step) */
        for (u8 guard = 0; guard < SEQ_STEPS && (s32)(s->ts[i].next_grid - s->cue_at) < 0; guard++) {
            schedule(s, i);
        }
    }
    if (s->cue_fn) {
        s->cue_fn(s->cue_ctx, s);               /* the new pattern */
    }
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        s->ts[i].next_grid = s->cue_at;         /* everything restarts on the bar */
        s->ts[i].step = 0;
        s->ts[i].loop = 0;
    }
}

static void process(struct seq *s)
{
    u32 ahead = s->pos + lead_units(s);
    if (s->cued && (s32)(ahead + max_half(s) - s->cue_at) >= 0) {
        cue_fire(s);
    }
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        u32 half = seq_step_units(s->pat->track[i].res) / 2;
        for (u8 guard = 0; guard < 8 && (s32)(ahead + half - s->ts[i].next_grid) >= 0; guard++) {
            schedule(s, i);
        }
    }
    u32 late = (u32)((unsigned long long)s->centibpm * SEQ_PPQN * 2 / 6000000u);
    emit(s, ahead, late ? late : 1);
}

static void reset_tracks(struct seq *s)
{
    memset(s->ts, 0, sizeof(s->ts));
    s->pos = 0;
    s->rem = 0;
    s->clock_next = 0;
}

void seq_start(struct seq *s, u32 now_ms)
{
    seq_stop(s);
    reset_tracks(s);
    s->running = 1;
    s->following = 0;
    s->self_started = 1;
    s->last_ms = now_ms;
    send1(s, 0xFA);
    seq_run(s, now_ms);
}

void seq_continue(struct seq *s, u32 now_ms)
{
    if (s->running) {
        return;
    }
    s->running = 1;
    s->following = 0;
    s->self_started = 1;
    s->last_ms = now_ms;
    s->clock_next = (s->pos + SEQ_CLOCK_UNITS - 1) / SEQ_CLOCK_UNITS * SEQ_CLOCK_UNITS;
    send1(s, 0xFB);
    seq_run(s, now_ms);
}

void seq_stop(struct seq *s)
{
    if (s->running && !s->following) {
        send1(s, 0xFC);
    }
    s->running = 0;
    s->following = 0;
    s->self_started = 0;
    s->cued = 0;
    for (u8 i = 0; i < s->nactive; i++) {
        u8 m[3] = {(u8)(0x80 | s->active[i].chan), s->active[i].note, 0x40};
        send(s, m, 3);
    }
    s->nactive = 0;
    s->nq = 0;
    s->qh = 0;
}

void seq_run(struct seq *s, u32 now_ms)
{
    if (!s->running) {
        s->last_ms = now_ms;
        return;
    }
    u32 dt = now_ms - s->last_ms;
    s->last_ms = now_ms;
    if (dt > 1000) {
        dt = 1000;                              /* stalled: don't burst a second of notes */
    }
    if (s->following) {
        if (s->ext_pulses && s->ext_interval_q8) {
            unsigned long long acc = (unsigned long long)dt * SEQ_CLOCK_UNITS * 256 + s->rem;
            u32 adv = (u32)(acc / s->ext_interval_q8);
            s->rem = (u32)(acc % s->ext_interval_q8);
            /* at most one pulse past the next expected F8: halts if the host clock pauses */
            u32 cap = (s->ext_pulses + 1) * SEQ_CLOCK_UNITS;
            u32 np = s->pos + adv;
            if ((s32)(np - cap) > 0) {
                np = (s32)(s->pos - cap) > 0 ? s->pos : cap;
                s->rem = 0;
            }
            s->pos = np;
        }
    } else {
        unsigned long long acc = (unsigned long long)dt * s->centibpm * SEQ_PPQN + s->rem;
        s->pos += (u32)(acc / 6000000u);
        s->rem = (u32)(acc % 6000000u);
        for (u8 guard = 0; guard < 64 && (s32)(s->pos - s->clock_next) >= 0; guard++) {
            send1(s, 0xF8);
            s->clock_next += SEQ_CLOCK_UNITS;
        }
    }
    process(s);
}

/* ---- external clock ----------------------------------------------------------------- */
static void ext_reset(struct seq *s)
{
    s->ext_n = s->ext_i = 0;
    s->ext_interval_q8 = 0;
}

/* host tempo = average F8 spacing over the last SEQ_EXT_RING pulses */
static void ext_tempo(struct seq *s, u32 now_ms)
{
    if (!s->ext_seen || now_ms - s->ext_last_ms >= EXT_TIMEOUT_MS) {
        ext_reset(s);
    }
    if (s->ext_n) {
        u8 oldest = s->ext_n < SEQ_EXT_RING ? 0 : s->ext_i;
        u32 span = now_ms - s->ext_ts[oldest];
        u32 iv = span * 256u / s->ext_n;
        if (iv >= EXT_IV_MIN_Q8 && iv <= EXT_IV_MAX_Q8) {   /* 20..400 BPM: else a burst */
            s->ext_interval_q8 = iv;
        }
    }
    s->ext_ts[s->ext_i] = now_ms;
    s->ext_i = (u8)((s->ext_i + 1) % SEQ_EXT_RING);
    if (s->ext_n < SEQ_EXT_RING) {
        s->ext_n++;
    }
}

void seq_clock_in(struct seq *s, u8 rt, u32 now_ms)
{
    if (!s->sync) {
        return;
    }
    switch (rt) {
    case 0xF8:
        ext_tempo(s, now_ms);
        s->ext_seen = 1;
        s->ext_last_ms = now_ms;
        if (s->running && s->following) {
            u32 target = s->ext_pulses * SEQ_CLOCK_UNITS;
            s32 err = (s32)(target - s->pos);
            if (s->ext_pulses >= 4 && s->ext_interval_q8) {
                s32 a = err < 0 ? -err : err;
                s->ext_err = (s16)(err > 8191 ? 8191 : (err < -8191 ? -8191 : err));
                if (a > s->ext_err_max) {
                    s->ext_err_max = (u16)(a > 8191 ? 8191 : a);
                }
            }
            if (s->ext_pulses < 4 || !s->ext_interval_q8 ||
                err > 2 * SEQ_CLOCK_UNITS || err < -2 * SEQ_CLOCK_UNITS) {
                s->pos = target;                /* starting, no tempo yet, or far off */
            } else {                            /* pull gently (rounded): smooth over jitter */
                s->pos = (u32)((s32)s->pos + (err + (err > 0 ? 2 : -2)) / 4);
            }
            s->rem = 0;
            s->ext_pulses++;
            s->last_ms = now_ms;
            process(s);
        }
        break;
    case 0xFA:
        if (s->running && s->self_started) {
            break;                              /* started on the pad: maybe our own FA echoed */
        }
        seq_stop(s);
        reset_tracks(s);
        s->running = 1;
        s->following = 1;
        s->ext_pulses = 0;
        s->ext_err = 0;
        s->ext_err_max = 0;
        s->last_ms = now_ms;
        break;
    case 0xFB:
        if (!s->running) {
            s->running = 1;
            s->following = 1;
            s->ext_pulses = (s->pos + SEQ_CLOCK_UNITS - 1) / SEQ_CLOCK_UNITS;
            s->last_ms = now_ms;
        }
        break;
    case 0xFC:
        if (s->following) {
            seq_stop(s);
        }
        break;
    default:
        break;
    }
}

u32 seq_ext_centibpm(const struct seq *s)
{
    return s->ext_interval_q8 ? 64000000u / s->ext_interval_q8 : 0;
}

void seq_clock_timeout(struct seq *s, u32 now_ms)
{
    if (s->ext_seen && now_ms - s->ext_last_ms > EXT_TIMEOUT_MS) {
        s->ext_seen = 0;
        if (s->following) {                     /* host clock vanished: keep playing on ours */
            u32 c = seq_ext_centibpm(s);
            if (c) {
                seq_set_tempo(s, c);
            }
            s->following = 0;
            s->rem = 0;
            s->clock_next = (s->pos + SEQ_CLOCK_UNITS - 1) / SEQ_CLOCK_UNITS * SEQ_CLOCK_UNITS;
        }
        ext_reset(s);
    }
}

/* ---- live recording / playhead ------------------------------------------------------ */
u8 seq_playhead(const struct seq *s, u8 track)
{
    if (!s->running || track >= SEQ_TRACKS) {
        return 0xFF;
    }
    const struct seq_track *t = &s->pat->track[track];
    const struct seq_track_state *ts = &s->ts[track];
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    u32 u = seq_step_units(t->res);
    u8 prev = ts->step ? (u8)(ts->step - 1) : (u8)(len - 1);
    if (ts->next_grid == 0) {
        return 0;
    }
    if ((s32)(s->pos - (ts->next_grid - u)) >= 0) {
        return prev < len ? prev : 0;
    }
    return prev ? (u8)(prev - 1) : (u8)(len - 1);
}

u8 seq_record(struct seq *s, u8 track, u8 vel, s8 note)
{
    s->rec_created = 0;
    if (!s->running || track >= SEQ_TRACKS) {
        return 0xFF;
    }
    struct seq_track *t = &s->pat->track[track];
    struct seq_track_state *ts = &s->ts[track];
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    s32 u = (s32)seq_step_units(t->res);        /* stored step */
    s32 g = u * seq_track_grid(t);              /* grid step (rate) */
    s32 loop = u * len;
    /* time since the loop started: the next step to schedule (ts->step) starts at next_grid */
    s32 rel = ts->next_grid ? (s32)(s->pos - ts->next_grid) + (s32)ts->step * u : (s32)s->pos;
    rel = ((rel % loop) + loop) % loop;
    s32 j = (rel + g / 2) / g;                  /* nearest grid step */
    s32 off = rel - j * g;
    s32 i2 = j * (s32)seq_track_grid(t);        /* the grid step's stored step */
    s32 micro = off * 96 / u;
    u8 idx = (u8)(((i2 % len) + len) % len);
    struct seq_step *st = &t->step[idx];
    if (!st->vel) {                             /* empty step: this hit defines it */
        memset(st, 0, sizeof(*st));
        st->vel = vel ? (vel > 127 ? 127 : vel) : 1;
        st->note = note;
        st->micro = (s8)(micro > SEQ_MICRO_MAX ? SEQ_MICRO_MAX : (micro < -SEQ_MICRO_MAX ? -SEQ_MICRO_MAX : micro));
        st->cond = SEQ_COND_REC;
        s->rec_created = 1;
        return idx;
    }
    u8 n = seq_step_count(st);                  /* playing step: add to its chord */
    for (u8 i = 0; i < n; i++) {
        if (seq_step_offset(st, i) == note) {
            return idx;
        }
    }
    if (n < SEQ_CHORD) {
        st->extra[n - 1] = note;
        st->ratchet = (u8)((st->ratchet & 0x3F) | (n << 6));
    }
    return idx;
}
