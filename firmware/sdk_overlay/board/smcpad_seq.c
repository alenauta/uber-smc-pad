#include "smcpad_seq.h"
#include "smcpad_scale.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

const u16 seq_rate_units[SEQ_RATES] = {960, 640, 480, 320, 240, 160, 120, 80};
const u8  seq_gate_eighths[SEQ_GATES] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 28, 32};
const u8  seq_ratchet_div[SEQ_RATCHET_RATES] = {2, 3, 4, 6, 8};

#define TIE_OVERLAP     10              /* units a tied note overlaps the next step */
#define EXT_TIMEOUT_MS  500             /* no F8 for this long: back to the internal clock */

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
    p->scale = SCALE_CHROMATIC;
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        struct seq_track *t = &p->track[i];
        t->chan = chans ? (u8)(chans[i] & 0x0f) : 9;
        t->note = notes ? (u8)(notes[i] & 0x7f) : (u8)(36 + i);
        t->len = 16;
        t->rate = SEQ_RATE_16;
        t->gate = 3;                            /* 1/2 step */
        t->vol = 127;
        t->prob = 100;
        t->ratchet_rate = 2;                    /* 1/4 step */
        t->colour = i;
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
    u32 u = seq_step_units(t->rate) * seq_gate_eighths[idx] / 8;
    return u ? u : 1;
}

u8 seq_step_note(const struct seq_pattern *p, const struct seq_track *t, const struct seq_step *st)
{
    return smcpad_scale_note(t->note, st->note, p->root, p->scale);
}

s32 seq_step_micro_units(const struct seq_track *t, const struct seq_step *st)
{
    s32 m = st->micro;
    m = m > SEQ_MICRO_MAX ? SEQ_MICRO_MAX : (m < -SEQ_MICRO_MAX ? -SEQ_MICRO_MAX : m);
    s32 q = t->quantize > 100 ? 100 : t->quantize;
    return m * (s32)seq_step_units(t->rate) / 96 * (100 - q) / 100;
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

/* ---- event queue -------------------------------------------------------------------- */
static void push(struct seq *s, u32 at, u8 on, u8 chan, u8 note, u8 vel, u8 track)
{
    if (on) {                                   /* an earlier note on the same key ends here */
        for (u8 i = 0; i < s->nq; i++) {
            struct seq_event *e = &s->q[i];
            if (!e->on && e->chan == chan && e->note == note && (s32)(e->at - at) > 0) {
                e->at = at;
            }
        }
    }
    if (s->nq >= SEQ_QUEUE) {
        s->dropped++;
        return;
    }
    struct seq_event *e = &s->q[s->nq++];
    e->at = at;
    e->on = on;
    e->chan = chan;
    e->note = note;
    e->vel = vel;
    e->track = track;
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

static void emit(struct seq *s, u32 late_units)
{
    for (;;) {
        s8 best = -1;
        for (u8 i = 0; i < s->nq; i++) {
            const struct seq_event *e = &s->q[i];
            if ((s32)(e->at - s->pos) > 0) {
                continue;
            }
            if (best < 0) {
                best = (s8)i;
                continue;
            }
            const struct seq_event *b = &s->q[best];
            s32 d = (s32)(e->at - b->at);
            if (d < 0 || (d == 0 && !e->on && b->on)) {     /* earliest first, offs before ons */
                best = (s8)i;
            }
        }
        if (best < 0) {
            return;
        }
        struct seq_event e = s->q[best];
        s->q[best] = s->q[--s->nq];
        if ((s32)(s->pos - e.at) > (s32)late_units) {
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
    u32 u = seq_step_units(t->rate);
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
        u8 count = (u8)((st->ratchet >> 4) + 1);
        u8 rr = st->ratchet & 0x0f;
        u8 ri = rr ? (u8)(rr - 1) : t->ratchet_rate;
        u32 interval = u / seq_ratchet_div[ri < SEQ_RATCHET_RATES ? ri : 2];
        u32 gate = seq_step_gate_units(t, st);
        if (count > 1 && gate > interval * 3 / 4) {
            gate = interval * 3 / 4 ? interval * 3 / 4 : 1;
        }
        u32 vel = (u32)st->vel * (t->vol > 127 ? 127 : t->vol) / 127;
        u8 note = seq_step_note(s->pat, t, st);
        for (u8 h = 0; h < count; h++) {
            u32 on = at + h * interval;
            u32 g_off = on + gate;
            if (h == count - 1 && (st->cond & SEQ_COND_TIE)) {
                g_off = g + u + TIE_OVERLAP;    /* legato into the next step */
                if ((s32)(g_off - on) <= 0) {
                    g_off = on + 1;
                }
            }
            push(s, on, 1, t->chan & 0x0f, note, (u8)(vel ? vel : 1), ti);
            push(s, g_off, 0, t->chan & 0x0f, note, 0, ti);
        }
    }
    ts->next_grid = g + u;
    ts->step = (u8)(k + 1);
    if (ts->step >= len) {
        ts->step = 0;
        ts->loop++;
    }
}

static void process(struct seq *s)
{
    for (u8 i = 0; i < SEQ_TRACKS; i++) {
        u32 half = seq_step_units(s->pat->track[i].rate) / 2;
        for (u8 guard = 0; guard < 8 && (s32)(s->pos + half - s->ts[i].next_grid) >= 0; guard++) {
            schedule(s, i);
        }
    }
    u32 late = (u32)((unsigned long long)s->centibpm * SEQ_PPQN * 2 / 6000000u);
    emit(s, late ? late : 1);
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
    for (u8 i = 0; i < s->nactive; i++) {
        u8 m[3] = {(u8)(0x80 | s->active[i].chan), s->active[i].note, 0x40};
        send(s, m, 3);
    }
    s->nactive = 0;
    s->nq = 0;
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
            u32 cap = s->ext_pulses * SEQ_CLOCK_UNITS - 1;  /* never past the next F8 */
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
void seq_clock_in(struct seq *s, u8 rt, u32 now_ms)
{
    if (!s->sync) {
        return;
    }
    switch (rt) {
    case 0xF8:
        if (s->ext_seen) {
            u32 iv = now_ms - s->ext_last_ms;
            if (iv > 0 && iv < EXT_TIMEOUT_MS) {
                s32 q = (s32)(iv << 8);
                s->ext_interval_q8 = s->ext_interval_q8 ?
                    (u32)((s32)s->ext_interval_q8 + (q - (s32)s->ext_interval_q8) / 8) : (u32)q;
            }
        }
        s->ext_seen = 1;
        s->ext_last_ms = now_ms;
        if (s->running && s->following) {
            u32 target = s->ext_pulses * SEQ_CLOCK_UNITS;
            if ((s32)(target - s->pos) > 0) {
                s->pos = target;
            }
            s->rem = 0;
            s->ext_pulses++;
            s->last_ms = now_ms;
            process(s);
        }
        break;
    case 0xFA:
        seq_stop(s);
        reset_tracks(s);
        s->running = 1;
        s->following = 1;
        s->ext_pulses = 0;
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
        s->ext_interval_q8 = 0;
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
    u32 u = seq_step_units(t->rate);
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
    if (!s->running || track >= SEQ_TRACKS) {
        return 0xFF;
    }
    struct seq_track *t = &s->pat->track[track];
    struct seq_track_state *ts = &s->ts[track];
    u8 len = t->len ? (t->len > SEQ_STEPS ? SEQ_STEPS : t->len) : 1;
    s32 u = (s32)seq_step_units(t->rate);
    u8 prev = ts->step ? (u8)(ts->step - 1) : (u8)(len - 1);
    s32 d = (s32)(s->pos - (ts->next_grid - (u32)u));  /* from the last scheduled step's grid */
    if (ts->next_grid == 0) {
        prev = 0;
        d = (s32)s->pos;
    }
    s32 j = d >= 0 ? (d + u / 2) / u : -((-d + u / 2 - 1) / u);
    s32 off = d - j * u;
    u8 idx = (u8)(((s32)prev + j % len + len) % len);
    s32 micro = off * 96 / u;
    struct seq_step *st = &t->step[idx];
    st->vel = vel ? (vel > 127 ? 127 : vel) : 1;
    st->note = note;
    st->micro = (s8)(micro > SEQ_MICRO_MAX ? SEQ_MICRO_MAX : (micro < -SEQ_MICRO_MAX ? -SEQ_MICRO_MAX : micro));
    return idx;
}
