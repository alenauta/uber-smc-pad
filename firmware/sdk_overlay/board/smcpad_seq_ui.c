#include "smcpad_seq_ui.h"
#include "smcpad_scale.h"

#ifdef SMCPAD_HOST_TEST
#include <string.h>
#else
#include "system/includes.h"
#endif

#define WHITE       0xFFFFFF
#define C_RATE      0xFF8000                /* timing page colours */
#define C_SWING     0x00FF40
#define C_FILL      0x00FFFF
#define C_SYNC      0x0040FF
#define C_TAP       0xFF0020

static const u32 palette[16] = {
    0xFF2000, 0xFF6000, 0xFFB000, 0xC0FF00, 0x40FF00, 0x00FF40, 0x00FFB0, 0x00C0FF,
    0x0060FF, 0x2000FF, 0x8000FF, 0xD000FF, 0xFF00B0, 0xFF0050, 0xFF8080, 0x80FFC0,
};
static const u8 swing_levels[5] = {50, 56, 62, 68, 75};

u32 seq_ui_track_rgb(u8 t)
{
    return palette[t & 15];
}

static u32 rgbs(u32 c, u32 num, u32 den)
{
    u32 r = ((c >> 16) & 0xff) * num / den, g = ((c >> 8) & 0xff) * num / den, b = (c & 0xff) * num / den;
    return (r << 16) | (g << 8) | b;
}

static s32 clampi(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static s32 accel(s32 steps, s32 range)          /* big ranges move faster when spun */
{
    s32 a = steps < 0 ? -steps : steps;
    return (range > 32 && a >= 3) ? steps * 4 : steps;
}

static void midi3(struct seq_ui *u, u8 st, u8 d1, u8 d2)
{
    if (u->io && u->io->midi) {
        u8 m[3] = {st, (u8)(d1 & 0x7f), (u8)(d2 & 0x7f)};
        u->io->midi(u->io->ctx, m, 3);
    }
}

static void live_off(struct seq_ui *u, u8 slot)
{
    if (slot < 17 && u->live_note[slot] != 0xFF) {
        midi3(u, (u8)(0x80 | u->live_chan[slot]), u->live_note[slot], 0x40);
        u->live_note[slot] = 0xFF;
    }
}

static void live_on(struct seq_ui *u, u8 slot, u8 chan, u8 note, u8 vel)
{
    live_off(u, slot);
    midi3(u, (u8)(0x90 | (chan & 0x0f)), note, vel ? vel : 1);
    u->live_note[slot] = note;
    u->live_chan[slot] = (u8)(chan & 0x0f);
}

static void all_live_off(struct seq_ui *u)
{
    for (u8 i = 0; i < 17; i++) {
        live_off(u, i);
    }
    u->lit_pads = 0;
    memset(u->rec_step, 0xFF, sizeof(u->rec_step));
}

static void tempo_changed(struct seq_ui *u)       /* any persisted setting changed */
{
    if (u->io && u->io->save) {
        u->io->save(u->io->ctx, u->seq);
    }
}

/* step view: steps read like text, top row first (pad 12 = step 0); the map is an involution */
u8 seq_ui_step_pad(u8 i)
{
    return (u8)((3 - ((i >> 2) & 3)) * 4 + (i & 3));
}

u8 seq_ui_beat_led(const struct seq *s, u32 now_ms)
{
    if (s->running) {                           /* locked to the play position: bar 1 longer */
        return (u8)(s->pos % (4 * SEQ_PPQN) < SEQ_PPQN / 4 || s->pos % SEQ_PPQN < SEQ_PPQN / 8);
    }
    u32 c = s->sync && s->ext_seen && s->ext_interval_q8 ? seq_ext_centibpm(s) : s->centibpm;
    c = c < 2000 ? 2000 : (c > 40000 ? 40000 : c);      /* div0 traps on this CPU */
    u32 beat = 6000000u / c;
    return (u8)(now_ms % beat < beat / 8);
}

/* undo journal: mutes are performance state, not edits */
u8 seq_ui_undo_skip(u16 off)
{
    u32 base = (u32)__builtin_offsetof(struct seq_pattern, track);
    if (off < base) {
        return 0;
    }
    return (u8)((off - base) % sizeof(struct seq_track) == __builtin_offsetof(struct seq_track, mute));
}

static void undo_settle(struct seq_ui *u, u32 now)
{
    if (u->undo && u->dirty && !u->held && !(u->rec && u->seq->running) &&
        now - u->act_ms >= SUI_UNDO_IDLE_MS) {
        undo_commit(u->undo);
        u->dirty = 0;
    }
}

static void overlay(struct seq_ui *u, u8 kind, u8 colour, s32 v, s32 lo, s32 hi, u32 now)
{
    u->ov_kind = kind;
    u->ov_colour = colour;
    u->ov_val = (s16)v;
    u->ov_min = (s16)lo;
    u->ov_max = (s16)hi;
    u->ov_until = now + SUI_OVERLAY_MS;
    if (!u->ov_until) {
        u->ov_until = 1;
    }
}

void seq_ui_init(struct seq_ui *u, struct seq *s, const struct seq_ui_io *io)
{
    memset(u, 0, sizeof(*u));
    u->seq = s;
    u->io = io;
    u->at_start = 1;
    memset(u->live_note, 0xFF, sizeof(u->live_note));
    memset(u->rec_step, 0xFF, sizeof(u->rec_step));
}

u8 seq_ui_shift_page(const struct seq_ui *u)
{
    return (u8)(u->active && u->shift && !u->padbank && !u->nr);
}

s8 seq_ui_play_offset(const struct seq_ui *u, u8 pad)
{
    const struct seq_track *t = &u->seq->pat->track[u->track & 15];
    s32 o = (s32)pad + (s32)u->play_oct * smcpad_scale_size(seq_track_scale(u->seq, t)) + u->play_tr;
    return (s8)clampi(o, -64, 63);
}

void seq_ui_toggle(struct seq_ui *u)
{
    if (u->undo && u->dirty) {
        undo_commit(u->undo);
        u->dirty = 0;
    }
    u->active ^= 1;
    all_live_off(u);
    u->held = u->keep = 0;
    u->padbank = u->stop = 0;
    u->ov_until = 0;
    u->seq->fill = 0;
    u->shift = 1;                               /* Shift and Note Repeat are held right now */
    u->nr = u->active;
    u->nr_used = 1;
}

/* ---- knobs ---------------------------------------------------------------------------- */
static s32 one(s32 d)                           /* discrete values: one per knob report */
{
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

static u8 prob_nibble(u8 pct)                   /* 0..100 % -> step nibble 1..15 */
{
    return (u8)((pct > 100 ? 100 : pct) * 14 / 100 + 1);
}

static void knob_step(struct seq_ui *u, u8 k, s32 d, u32 now)
{
    struct seq_pattern *p = u->seq->pat;
    struct seq_track *t = &p->track[u->track];
    u8 first = 1;
    for (u8 i = 0; i < 16; i++) {
        if (!(u->held & (1u << i))) {
            continue;
        }
        u8 si = (u8)(u->page * 16 + i);
        if (si >= SEQ_STEPS) {
            continue;
        }
        struct seq_step *st = &t->step[si];
        s32 v = 0;
        switch (k) {
        case 0:
            v = st->vel = (u8)clampi(st->vel + accel(d, 127), 1, 127);
            if (first) overlay(u, SUI_OV_BAR, u->track, v, 0, 127, now);
            break;
        case 1: {                               /* gate: starts from the track's gate */
            u8 g = st->prob_gate & 0x0f;
            v = clampi((g ? g - 1 : t->gate) + one(d), 0, SEQ_GATES - 1);
            st->prob_gate = (u8)((st->prob_gate & 0xF0) | (v + 1));
            if (first) overlay(u, SUI_OV_BAR, u->track, v, -1, SEQ_GATES - 1, now);
            break;
        }
        case 4: {                               /* K5 probability: starts from the track's */
            u8 n = st->prob_gate >> 4;
            v = clampi((n ? n : prob_nibble(t->prob)) + one(d), 1, 15);
            st->prob_gate = (u8)((st->prob_gate & 0x0F) | (v << 4));
            if (first) overlay(u, SUI_OV_BAR, u->track, seq_step_prob(t, st), 0, 100, now);
            break;
        }
        case 3:
            v = st->micro = (s8)clampi(st->micro + accel(d, 96), -SEQ_MICRO_MAX, SEQ_MICRO_MAX);
            if (first) overlay(u, SUI_OV_SIGNED, u->track, v, -SEQ_MICRO_MAX, SEQ_MICRO_MAX, now);
            break;
        case 2: {                               /* K3 note offset: moves the whole chord */
            s32 nd = one(d);
            u8 n = seq_step_count(st);
            v = st->note = (s8)clampi(st->note + nd, -48, 48);
            for (u8 c = 1; c < n; c++) {
                st->extra[c - 1] = (s8)clampi(st->extra[c - 1] + nd, -48, 48);
            }
            if (first) {
                u8 note = seq_track_note(u->seq, t, st->note);
                live_on(u, 16, t->chan, note, st->vel);
                overlay(u, SUI_OV_KEYS, u->track, note, 0, 127, now);
            }
            break;
        }
        case 5: {                               /* ratchet rate: starts from the track's */
            u8 rr = st->ratchet & 0x0f;
            v = clampi((rr ? rr - 1 : t->ratchet_rate) + one(d), 0, SEQ_RATCHET_RATES - 1);
            st->ratchet = (u8)((st->ratchet & 0xF0) | (v + 1));
            if (first) overlay(u, SUI_OV_PICK, u->track, v, 0, SEQ_RATCHET_RATES - 1, now);
            break;
        }
        case 6:
            v = clampi((st->cond & SEQ_COND_MASK) + one(d), 0, SEQ_CONDS - 1);
            st->cond = (u8)((st->cond & SEQ_COND_TIE) | v);
            if (first) overlay(u, SUI_OV_PICK, u->track, v, 0, SEQ_CONDS - 1, now);
            break;
        default:
            if (d > 0) st->cond |= SEQ_COND_TIE;
            if (d < 0) st->cond &= (u8)~SEQ_COND_TIE;
            if (first) overlay(u, SUI_OV_BAR, u->track, (st->cond & SEQ_COND_TIE) ? 1 : 0, 0, 1, now);
            break;
        }
        first = 0;
    }
    u->keep |= u->held;                         /* edited: don't toggle off on release */
}

static void knob_track(struct seq_ui *u, u8 k, s32 d, u32 now)
{
    struct seq_track *t = &u->seq->pat->track[u->track];
    u8 c = u->track;
    switch (k) {
    case 0: t->vol = (u8)clampi(t->vol + accel(d, 127), 0, 127); overlay(u, SUI_OV_BAR, c, t->vol, 0, 127, now); break;
    case 1: t->gate = (u8)clampi(t->gate + one(d), 0, SEQ_GATES - 1); overlay(u, SUI_OV_BAR, c, t->gate, -1, SEQ_GATES - 1, now); break;
    case 4: t->prob = (u8)clampi(t->prob + accel(d, 100), 0, 100); overlay(u, SUI_OV_BAR, c, t->prob, 0, 100, now); break;
    case 3: t->quantize = (u8)clampi(t->quantize + accel(d, 100), 0, 100); overlay(u, SUI_OV_BAR, c, t->quantize, 0, 100, now); break;
    case 2:                                     /* K3 base note */
        t->note = (u8)clampi(t->note + one(d), 0, 127);
        live_on(u, 16, t->chan, t->note, SUI_DEFAULT_VEL);
        overlay(u, SUI_OV_KEYS, c, t->note, 0, 127, now);
        break;
    case 5: t->ratchet_rate = (u8)clampi(t->ratchet_rate + one(d), 0, SEQ_RATCHET_RATES - 1); overlay(u, SUI_OV_PICK, c, t->ratchet_rate, 0, SEQ_RATCHET_RATES - 1, now); break;
    case 6: t->len = (u8)clampi(t->len + d, 1, SEQ_STEPS); overlay(u, SUI_OV_BAR, c, t->len, 0, SEQ_STEPS, now); break;
    default: t->rate = (u8)clampi(t->rate + one(d), 0, SEQ_RATES - 1); overlay(u, SUI_OV_PICK, c, t->rate, 0, SEQ_RATES - 1, now); break;
    }
}

static void knob_pattern(struct seq_ui *u, u8 k, s32 d, u32 now)
{
    struct seq *s = u->seq;
    struct seq_pattern *p = s->pat;
    switch (k) {
    case 0:
        seq_set_tempo(s, (u32)clampi((s32)s->centibpm + accel(d, 270) * 100, 3000, 30000));
        overlay(u, SUI_OV_BAR, u->track, (s32)(s->centibpm / 100), 30, 300, now);
        tempo_changed(u);
        break;
    case 1:
        s->swing = (u8)clampi(s->swing + d, 50, 75);
        overlay(u, SUI_OV_BAR, u->track, s->swing, 50, 75, now);
        tempo_changed(u);
        break;
    case 2: {                                   /* track root: no pad = follow global, 1..12 = C..B */
        struct seq_track *t = &p->track[u->track];
        t->root = (u8)clampi(t->root + one(d), 0, 12);
        overlay(u, SUI_OV_PICK, u->track, t->root, 1, 12, now);
        break;
    }
    case 3: {                                   /* track scale: no pad = follow global, 1 = chromatic.. */
        struct seq_track *t = &p->track[u->track];
        t->scale = (u8)clampi(t->scale + one(d), 0, SCALE_COUNT);
        overlay(u, SUI_OV_PICK, u->track, t->scale, 1, SCALE_COUNT, now);
        break;
    }
    case 6: {                                   /* MIDI channel: 1 pad = ch 1 ... 16 pads = ch 16 */
        struct seq_track *t = &p->track[u->track];
        t->chan = (u8)clampi(t->chan + one(d), 0, 15);
        overlay(u, SUI_OV_BAR, u->track, t->chan, -1, 15, now);
        break;
    }
    case 7: {                                   /* play early: 1 ms per report, faster when spun */
        s32 step = (d >= 3 || d <= -3) ? d : one(d);
        s->lead_ms = (u8)clampi(s->lead_ms + step, 0, SEQ_LEAD_MAX_MS);
        overlay(u, SUI_OV_BAR, u->track, s->lead_ms, 0, SEQ_LEAD_MAX_MS, now);
        tempo_changed(u);
        break;
    }
    default:
        break;                                  /* K5 / K6: later (chaining) */
    }
}

/* Shift + K3 / K4: the global root and scale, shared with normal mode. Same pads as the track
 * pick: no pad lit = follow the preset (root = each track's base note, scale = chromatic here),
 * pads 1..12 = C..B / chromatic, major, ... blues */
static u8 knob_global(struct seq_ui *u, u8 k, s32 d, u32 now)
{
    struct seq *s = u->seq;
    if (k == 2) {
        s->groot = (u8)clampi(s->groot + one(d), 0, 12);
        overlay(u, SUI_OV_PICK, u->track, s->groot, 1, 12, now);
    } else if (k == 3) {
        s->gscale = (u8)clampi(s->gscale + one(d), 0, SCALE_COUNT);
        overlay(u, SUI_OV_PICK, u->track, s->gscale, 1, SCALE_COUNT, now);
    } else {
        return 0;
    }
    tempo_changed(u);
    return 1;
}

void seq_ui_knob(struct seq_ui *u, u8 knob, s32 steps, u32 now_ms)
{
    if (!u->active || !steps || knob > 7) {
        return;
    }
    u->dirty = 1;
    u->act_ms = now_ms;
    if (u->shift && knob_global(u, knob, steps, now_ms)) {
        return;
    }
    if (u->held && u->view == SUI_VIEW_STEP) {
        knob_step(u, knob, steps, now_ms);
    } else if (u->knob_page == SUI_KNOBS_PATTERN) {
        knob_pattern(u, knob, steps, now_ms);
    } else {
        knob_track(u, knob, steps, now_ms);
    }
}

/* ---- pads ----------------------------------------------------------------------------- */
static void tap_tempo(struct seq_ui *u, u32 now)
{
    if (u->ntap && now - u->tap[u->ntap - 1] > 2000) {
        u->ntap = 0;
    }
    if (u->ntap == 4) {
        memmove(u->tap, u->tap + 1, 3 * sizeof(u->tap[0]));
        u->ntap = 3;
    }
    u->tap[u->ntap++] = now;
    if (u->ntap >= 2) {
        u32 avg = (u->tap[u->ntap - 1] - u->tap[0]) / (u32)(u->ntap - 1);
        if (avg >= 200 && avg <= 2000) {
            seq_set_tempo(u->seq, 6000000u / avg);
            tempo_changed(u);
        }
    }
}

static void timing_pad(struct seq_ui *u, u8 pad, u32 now)
{
    struct seq *s = u->seq;
    if (pad < 8) {
        s->pat->track[u->track].rate = pad;
    } else if (pad < 13) {
        s->swing = swing_levels[pad - 8];
        tempo_changed(u);
    } else if (pad == 13) {
        s->fill = 1;
    } else if (pad == 14) {
        s->sync ^= 1;
    } else {
        tap_tempo(u, now);
    }
}

/* note-length recording: the time a play-view pad was held becomes the gate of the step its
 * press created (nearest table value, 1/8 step .. 4 steps); chord notes added while the chord
 * is being played keep the longest */
static void rec_gate(struct seq_ui *u, u8 pad)
{
    struct seq *s = u->seq;
    u8 si = u->rec_step[pad];
    u->rec_step[pad] = 0xFF;
    if (!s->running || si >= SEQ_STEPS) {
        return;
    }
    struct seq_track *t = &s->pat->track[u->rec_trk[pad] & 15];
    struct seq_step *st = &t->step[si];
    u32 held = s->pos - u->rec_pos[pad];
    u32 su = seq_step_units(t->rate);           /* never 0 */
    if (!st->vel || held > 0x01000000u) {
        return;                                 /* cleared meanwhile / position jumped back */
    }
    u32 e8 = (held * 8 + su / 2) / su;          /* eighths of a step */
    u8 best = 0;
    for (u8 i = 1; i < SEQ_GATES; i++) {
        u32 db = e8 > seq_gate_eighths[best] ? e8 - seq_gate_eighths[best] : seq_gate_eighths[best] - e8;
        u32 di = e8 > seq_gate_eighths[i] ? e8 - seq_gate_eighths[i] : seq_gate_eighths[i] - e8;
        if (di < db) {
            best = i;
        }
    }
    u8 cur = st->prob_gate & 0x0f;              /* 0 = new step; else set by a chord note */
    if (cur && (u8)(cur - 1) > best) {
        best = (u8)(cur - 1);
    }
    st->prob_gate = (u8)((st->prob_gate & 0xF0) | (best + 1));
}

void seq_ui_pad(struct seq_ui *u, u8 pad, u8 on, u8 vel, u32 now_ms)
{
    if (!u->active || pad > 15) {
        return;
    }
    u->dirty = 1;
    u->act_ms = now_ms;
    struct seq *s = u->seq;
    struct seq_pattern *p = s->pat;
    struct seq_track *t = &p->track[u->track];
    u16 bit = (u16)(1u << pad);
    u8 sp = seq_ui_step_pad(pad);               /* step-view step of this pad */
    u16 sbit = (u16)(1u << sp);
    if (!on) {
        if (pad == 13) {
            s->fill = 0;
        }
        if (u->held & sbit) {
            u8 si = (u8)(u->page * 16 + sp);
            if (!(u->keep & sbit) && si < SEQ_STEPS) {
                memset(&t->step[si], 0, sizeof(t->step[si]));
            }
            u->held &= (u16)~sbit;
            u->keep &= (u16)~sbit;
            if (!u->held) {
                live_off(u, 16);
            }
        }
        if (u->rec_step[pad] != 0xFF) {
            rec_gate(u, pad);
        }
        if (u->lit_pads & bit) {
            u->lit_pads &= (u16)~bit;
            live_off(u, pad);
        }
        return;
    }
    if (u->nr && !u->held) {
        timing_pad(u, pad, now_ms);
        u->nr_used = 1;
        return;
    }
    if (u->padbank) {                           /* track layer: select / mute / clear */
        if (u->stop && u->shift) {
            memset(p->track[pad].step, 0, sizeof(p->track[pad].step));
            u->stop_used = 1;
        } else if (u->stop) {
            p->track[pad].mute ^= 1;
            u->stop_used = 1;
        } else {
            u->track = pad;
        }
        u->padbank_used = 1;
        return;
    }
    if (u->view == SUI_VIEW_STEP) {
        u8 si = (u8)(u->page * 16 + sp);
        if (si >= t->len) {
            return;
        }
        u->held |= sbit;
        if (!t->step[si].vel) {
            t->step[si].vel = SUI_DEFAULT_VEL;
            u->keep |= sbit;                    /* just created: stays on at release */
        } else {
            u->keep &= (u16)~sbit;
        }
        return;
    }
    /* play view: pad = scale step above the track's base note (chromatic = drum kit), moved by
     * the Shift-menu octave / transpose */
    s8 off = seq_ui_play_offset(u, pad);
    live_on(u, pad, t->chan, seq_track_note(s, t, off), vel);
    u->lit_pads |= bit;
    if (u->rec && s->running) {
        u8 k = seq_record(s, u->track, vel, off);
        u8 mine = s->rec_created;
        for (u8 j = 0; j < 16 && !mine && k != 0xFF; j++) {     /* joins a chord being played */
            mine = (u8)(u->rec_step[j] == k && u->rec_trk[j] == u->track);
        }
        if (mine && k != 0xFF) {
            u->rec_step[pad] = k;
            u->rec_trk[pad] = u->track;
            u->rec_pos[pad] = s->pos;
        }
    }
}

/* ---- buttons -------------------------------------------------------------------------- */
static void history_step(struct seq_ui *u, u8 redo, u32 now)
{
    if (!u->undo) {
        return;
    }
    undo_commit(u->undo);                       /* pending edits become the newest step */
    u->dirty = 0;
    if (redo ? undo_redo(u->undo) : undo_undo(u->undo)) {
        overlay(u, SUI_OV_FLASH, u->track, 0, 0, 0, now);
        u->ov_until = now + SUI_FLASH_MS ? now + SUI_FLASH_MS : 1;
    }
}

void seq_ui_button(struct seq_ui *u, u8 hw, u8 pressed, u32 now_ms)
{
    struct seq *s = u->seq;
    if (!u->active) {
        return;
    }
    u->dirty = 1;
    u->act_ms = now_ms;
    switch (hw) {
    case SUI_SHIFT:
        u->shift = pressed;
        break;
    case SUI_NOTE_REPEAT:
        if (!pressed) {
            u->nr = 0;
            s->fill = 0;
        } else if (u->shift) {
            seq_ui_toggle(u);                   /* leave sequencer mode */
        } else if (u->held && u->view == SUI_VIEW_STEP) {
            struct seq_track *t = &s->pat->track[u->track];
            u8 c = 0;
            for (u8 i = 0; i < 16; i++) {
                u8 si = (u8)(u->page * 16 + i);
                if ((u->held & (1u << i)) && si < SEQ_STEPS) {
                    struct seq_step *st = &t->step[si];
                    c = (u8)((((st->ratchet >> 4) & 3) + 1) & 3);   /* bits 6-7 = chord: keep */
                    st->ratchet = (u8)((st->ratchet & 0xCF) | (c << 4));
                }
            }
            u->keep |= u->held;
            overlay(u, SUI_OV_COUNT, u->track, c + 1, 1, 4, now_ms);
        } else {
            u->nr = 1;
            u->nr_used = 0;
        }
        break;
    case SUI_PAD_BANK:
        if (pressed) {
            u->padbank = 1;
            u->padbank_used = 0;
        } else {
            u->padbank = 0;
            if (!u->padbank_used) {
                all_live_off(u);
                u->held = u->keep = 0;
                u->view = (u8)((u->view + 1) % SUI_VIEWS);
            }
        }
        break;
    case SUI_KNOB_BANK:
        if (pressed) {
            u->knob_page ^= 1;
        }
        break;
    case SUI_LEFT:
        if (pressed && u->shift) {
            history_step(u, 0, now_ms);
        } else if (pressed && u->page) {
            u->page--;
        }
        break;
    case SUI_RIGHT:
        if (pressed && u->shift) {
            history_step(u, 1, now_ms);
        } else if (pressed && u->page < SEQ_STEPS / 16 - 1) {
            u->page++;
        }
        break;
    case SUI_PLAY:
        if (pressed && !s->running) {
            if (u->at_start) {
                seq_start(s, now_ms);
            } else {
                seq_continue(s, now_ms);
            }
            u->at_start = 0;
        }
        break;
    case SUI_STOP:                              /* acts on release: Stop + pad = mute, no stop */
        u->stop = pressed;
        if (pressed) {
            u->stop_used = 0;
        } else if (!u->stop_used) {
            if (s->running) {
                seq_stop(s);
            } else {
                u->at_start = 1;                /* second Stop: back to the start */
            }
        }
        break;
    case SUI_REC:
        if (pressed && !u->shift) {
            u->rec ^= 1;                        /* Shift + Rec = save (Flash B) */
        }
        break;
    default:
        break;
    }
}

/* ---- LEDs ----------------------------------------------------------------------------- */
static void draw_overlay(const struct seq_ui *u, u32 rgb[16])
{
    u32 c = palette[u->ov_colour & 15];
    s32 lo = u->ov_min, hi = u->ov_max, v = u->ov_val;
    if (u->ov_kind == SUI_OV_FLASH) {           /* undo / redo done */
        for (u8 i = 0; i < 16; i++) {
            rgb[i] = rgbs(WHITE, 1, 6);
        }
        return;
    }
    if (u->ov_kind == SUI_OV_PICK) {            /* one pad per choice, from pad 1 */
        for (u8 i = 0; i < 16; i++) {
            s32 idx = (s32)i + lo;
            rgb[i] = (v < lo || idx > hi) ? 0 : (idx == v ? c : rgbs(c, 1, 12));   /* v < lo: follow */
        }
        return;
    }
    if (u->ov_kind == SUI_OV_SIGNED) {          /* centre-out: pads 9.. positive, 8.. negative */
        s32 m = hi > 0 ? hi : 1;
        s32 fill = (v < 0 ? -v : v) * 64 / m;   /* eighths over 8 pads */
        for (u8 i = 0; i < 8; i++) {
            s32 part = clampi(fill - i * 8, 0, 8);
            u8 pad = v < 0 ? (u8)(7 - i) : (u8)(8 + i);
            rgb[pad] = part ? rgbs(c, (u32)part, 8) : rgbs(c, 1, 24);
            rgb[v < 0 ? 8 + i : 7 - i] = rgbs(c, 1, 24);
        }
        if (!v) {
            rgb[7] = rgb[8] = rgbs(WHITE, 1, 4);
        }
        return;
    }
    if (u->ov_kind == SUI_OV_KEYS) {            /* a note: its key in pads 1-12, octave 13-16 */
        static const u16 black = 0x54A;         /* C# D# F# G# A# */
        for (u8 i = 0; i < 12; i++) {
            rgb[i] = (u8)(v % 12) == i ? c : ((black >> i) & 1 ? 0x101010 : 0x303030);
        }
        u8 oct = (u8)(v / 12);
        for (u8 i = 12; i < 16; i++) {
            rgb[i] = (u8)((i - 12) * 3) < oct ? rgbs(WHITE, 1, 3) : 0;
        }
        return;
    }
    if (u->ov_kind == SUI_OV_COUNT) {
        for (u8 i = 0; i < 16; i++) {
            rgb[i] = i < 4 ? (i < v ? c : rgbs(c, 1, 24)) : 0;
        }
        return;
    }
    s32 span = hi - lo > 0 ? hi - lo : 1;
    s32 fill = (v - lo) * 128 / span;           /* eighths over 16 pads */
    for (u8 i = 0; i < 16; i++) {
        s32 part = clampi(fill - i * 8, 0, 8);
        rgb[i] = part ? rgbs(c, (u32)part, 8) : rgbs(c, 1, 24);
    }
}

u16 seq_ui_leds(struct seq_ui *u, u32 now_ms, u32 rgb[16])
{
    struct seq *s = u->seq;
    struct seq_pattern *p = s->pat;
    struct seq_track *t = &p->track[u->track];
    u16 hits = s->hits;
    s->hits = 0;
    for (u8 i = 0; i < 16; i++) {
        if (hits & (1u << i)) {
            u->hit_ms[i] = now_ms;
        }
        rgb[i] = 0;
    }
    u16 btn = 0;
    if (!u->active) {
        return 0;
    }
    undo_settle(u, now_ms);
    if (s->running) btn |= 1u << SUI_PLAY;
    if (u->rec) btn |= 1u << SUI_REC;
    if (u->page) btn |= 1u << SUI_LEFT;
    if ((u->page + 1) * 16 < t->len) btn |= 1u << SUI_RIGHT;
    if (u->view == SUI_VIEW_PLAY) btn |= 1u << SUI_PAD_BANK;
    if (u->knob_page) btn |= 1u << SUI_KNOB_BANK;
    if (u->shift) btn |= 1u << SUI_SHIFT;
    if (seq_ui_beat_led(s, now_ms)) btn |= 1u << SUI_NOTE_REPEAT;   /* sequencer mode + tempo */
    if (u->stop) btn |= 1u << SUI_STOP;

    if (u->ov_until && (s32)(u->ov_until - now_ms) > 0) {
        draw_overlay(u, rgb);
        return btn;
    }
    u->ov_until = 0;
    if (u->nr && !u->held) {                    /* timing page */
        for (u8 i = 0; i < 8; i++) {
            rgb[i] = i == t->rate ? C_RATE : rgbs(C_RATE, 1, 10);
        }
        u8 sw = 0;
        for (u8 i = 1; i < 5; i++) {
            if (s->swing >= swing_levels[i]) sw = i;
        }
        for (u8 i = 0; i < 5; i++) {
            rgb[8 + i] = i == sw ? C_SWING : rgbs(C_SWING, 1, 10);
        }
        rgb[13] = s->fill ? WHITE : rgbs(C_FILL, 1, 10);
        rgb[14] = s->sync ? C_SYNC : rgbs(C_SYNC, 1, 10);
        u32 beat = 6000000u / (s->centibpm ? s->centibpm : 12000);
        rgb[15] = now_ms % beat < beat / 4 ? C_TAP : rgbs(C_TAP, 1, 10);
        return btn;
    }
    if (u->padbank) {                           /* track layer: activity, mutes, selection */
        for (u8 i = 0; i < 16; i++) {
            u32 pc = i == u->track ? WHITE : palette[i];
            if (u->hit_ms[i] && now_ms - u->hit_ms[i] < 80) {
                rgb[i] = pc;
            } else {
                rgb[i] = rgbs(pc, 1, p->track[i].mute ? 32 : (i == u->track ? 4 : 6));
            }
        }
        return btn;
    }
    u32 c = palette[u->track & 15];
    if (u->view == SUI_VIEW_STEP) {
        u8 ph = seq_playhead(s, u->track);
        for (u8 i = 0; i < 16; i++) {           /* i = step on the page */
            u8 si = (u8)(u->page * 16 + i);
            u8 pad = seq_ui_step_pad(i);
            const struct seq_step *st = &t->step[si];
            if (si >= t->len) {
                rgb[pad] = 0;
            } else if (u->held & (1u << i)) {
                rgb[pad] = WHITE;
            } else if (ph == si) {
                rgb[pad] = st->vel ? WHITE : rgbs(WHITE, 1, 6);
            } else if (st->vel) {
                rgb[pad] = rgbs(c, 24 + (u32)st->vel * 104 / 127, 128);
            } else {
                rgb[pad] = rgbs(c, 1, 16);
            }
        }
        return btn;
    }
    /* play view: root pads brighter, held pads white, full colour while the note sounds */
    u8 root = seq_track_root(s, t), ch = (u8)(t->chan & 0x0f);
    for (u8 i = 0; i < 16; i++) {
        u8 n = seq_track_note(s, t, seq_ui_play_offset(u, i));
        u8 on = 0;
        for (u8 a = 0; a < s->nactive && !on; a++) {
            on = (u8)(s->active[a].chan == ch && s->active[a].note == n);
        }
        if (u->lit_pads & (1u << i)) {
            rgb[i] = WHITE;
        } else if (on) {
            rgb[i] = c;
        } else {
            rgb[i] = (u8)(n % 12) == root ? rgbs(c, 1, 3) : rgbs(c, 1, 12);
        }
    }
    return btn;
}
