/* SMC-PAD application glue: one task turns pad / knob / button input into MIDI on
 * USB (cable 0), TRS and BLE-MIDI, drives the LEDs, answers a small diagnostic SysEx and
 * (with TCFG_SMCPAD_BLE_ENABLE) serves the Sinco config protocol (Midi Suite / SincoOTA)
 * over USB SysEx and BLE AE41/AE42 with the stock storage model (sinco_store.c).
 *
 * With TCFG_SMCPAD_BLE_ENABLE the mapping is the active stock preset (smcpad_preset.c: the
 * records Midi Suite edits, globals octave / transpose / curve / aftertouch / preset / bank);
 * blank flash gets the factory Performance preset (= the mapping below).
 * Without it, the hard-coded stock factory mapping (docs/stock_midi_map.md):
 *   pads    Note On/Off ch10, notes 36..51 (pad index order, to be confirmed), velocity;
 *           channel aftertouch (D9) from the strongest held pad, at most every 10 ms
 *   knobs   CC 30..37 ch1, endless knob kept as an absolute 0..127 value
 *   buttons CC 25..34 ch1, 127 on press / 0 on release
 * Incoming Note On ch10 36..51 from USB or BLE-MIDI lights the pad LED (velocity = brightness).
 *
 * Diagnostics (non-commercial ID 0x7D, "SP"):  F0 7D 53 50 01 F7  -> pad dump
 *                                              F0 7D 53 50 02 F7  -> status dump
 */
#include "app_config.h"

#if defined(CONFIG_BOARD_SMCPAD) && TCFG_SMCPAD_HW_ENABLE

#include "system/includes.h"
#include "smcpad_board.h"
#if TCFG_USB_SLAVE_MIDI_ENABLE
#include "usb_midi.h"
#endif
#ifndef TCFG_SMCPAD_BLE_ENABLE
#define TCFG_SMCPAD_BLE_ENABLE      0
#endif
#if TCFG_SMCPAD_BLE_ENABLE
#include "sinco_proto.h"
#include "sinco_store.h"
#include "ble_midi.h"
#include "ble_smcpad.h"
#include "smcpad_store_port.h"
#include "smcpad_preset.h"
#define APP_PRESETS         1                   /* preset interpreter (needs the store) */
#else
#define APP_PRESETS         0
#endif

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[SMCPAD]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

#define APP_MIDI_CABLES     BIT(0)              /* stock duplicates on cable 0 and 1 */
#define APP_MIDI_MCP_CABLES BIT(2)              /* "SMC-PAD-MCP": Mackie-class output, as stock */
#define APP_AT_PERIOD_MS    10
#define APP_RGB_PERIOD_MS   10
#define APP_PAD_HELD_RGB    0x606060
#define APP_HOST_RGB        0x0040ff            /* colour for host-lit pads, scaled by vel */
#define APP_TASK_PRIO       2
#define APP_TASK_STACK      512                 /* u32 words */
#define APP_KNOB_PERIOD_MS  50                  /* stock: encoder deltas every 50 ms */
#define APP_KNOB_TICK_MS    10                  /* knob value walker */

static OS_SEM app_sem;

static struct {
    s32 knob_rep[SMCPAD_NUM_KNOBS];
    u8  knob_val[SMCPAD_NUM_KNOBS];
    u16 buttons;
    u16 held;                       /* bit p = pad p held */
    u8  last_at;
    u8  rgb_dirty;
    u32 last_at_ms;
    u32 last_rgb_ms;
    u32 host_rgb[SMCPAD_NUM_PADS];
    u32 last_knob_ms;
    u32 last_knob_tick_ms;
} app;

/* IRQ context, called from the RAM ISRs: keep it in RAM too (os_sem_post is in mask ROM) */
SMCPAD_RAM_CODE static void app_notify(void)
{
    os_sem_post(&app_sem);
}

static void pad_led_update(u8 pad);

#if TCFG_SMCPAD_BLE_ENABLE
#define APP_GLOBALS_FLUSH_MS    2000            /* VM write after the globals stop changing */

static u8 presets_ram[SMCPAD_PRESETS_SIZE];     /* 28 KB, as the stock P block */
static struct sinco_store store;
static u8 sinco_reply[SINCO_MAX_PKT];
static u8 sinco_frame[SINCO_SYSEX_LEN(SINCO_MAX_PKT)];
static struct sinco_sx usb_sx;                 /* USB SysEx -> Sinco packets */
static u8 usb_sx_cable;
static struct sinco_rx ble_sx;                 /* AE41 chunks -> Sinco packets */
static struct ble_midi_enc ble_enc;
static struct ble_midi_dec ble_dec;
static u8  ble_pkt[BLE_MIDI_PKT_MAX];           /* a notification waiting for a free slot */
static u16 ble_pkt_len;
static u32 ble_conn_gen;
static u32 ble_midi_drops;
static u32 globals_gen_seen, globals_change_ms;
static struct smcpad_preset preset;
static u32 led_presets_gen, led_globals_gen;

static void host_midi_in(const u8 *msg, u32 len);
static void midi_out(const u8 *m, u32 n);
static void ble_midi_out(const u8 *m, u32 n);

static void preset_midi(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
    midi_out(m, n);
}

/* Mackie-class messages: USB cable 2 (stock: 0x1E0FEA2 + USB TX unwrap of F0 35 59 .. F7).
 * BLE gets them as plain MIDI (stock wraps them in that non-standard SysEx over BLE). */
static void preset_midi_mcp(void *ctx, const u8 *m, u32 n)
{
    (void)ctx;
#if TCFG_USB_SLAVE_MIDI_ENABLE
    usb_midi_send_msg(APP_MIDI_MCP_CABLES, m, n);
#endif
    smcpad_hw_trs_write(m, n);
    ble_midi_out(m, n);
}

static void preset_globals_changed(void *ctx)  /* pad-bank button: persist like a Sinco write */
{
    (void)ctx;
    store.globals_dirty = 1;
    store.globals_gen++;
}

static const struct smcpad_preset_io preset_io = {NULL, preset_midi, preset_midi_mcp, preset_globals_changed};

static void sinco_midi_in(void *ctx, const u8 *b, u32 n)  /* cmd 0x15: raw MIDI bytes */
{
    (void)ctx;
    for (u32 i = 0; i < n;) {
        u32 l = ble_midi_msg_len(b[i]);
        if (l && i + l <= n) {
            host_midi_in(b + i, l);
            i += l;
        } else {
            i++;
        }
    }
}

static const struct sinco_ops sinco_ops = {
    &store, SMCPAD_NAME_VER, sinco_store_read, sinco_store_write, sinco_midi_in, NULL
};

/* BLE-MIDI out: messages are packed into one notification per task pass */
static void ble_midi_flush(void)
{
    if (ble_pkt_len && ble_smcpad_midi_send(ble_pkt, ble_pkt_len) == 0) {
        ble_pkt_len = 0;
    }
    if (!ble_pkt_len && ble_midi_enc_pending(&ble_enc)) {
        ble_pkt_len = (u16)ble_midi_enc_take(&ble_enc, ble_pkt);
        if (ble_smcpad_midi_send(ble_pkt, ble_pkt_len) == 0) {
            ble_pkt_len = 0;
        }
    }
}

static void ble_midi_out(const u8 *m, u32 n)
{
    if (!ble_smcpad_midi_ready()) {
        return;
    }
    u16 ts = (u16)sys_timer_get_ms();
    u32 done = 0;
    for (u32 guard = 0; done < n && guard < 64; guard++) {
        done += ble_midi_enc_add(&ble_enc, ts, m + done, n - done);
        if (done < n) {
            ble_midi_flush();
            if (ble_pkt_len) {                  /* link busy: drop the oldest packet */
                ble_pkt_len = 0;
                ble_midi_drops++;
            }
        }
    }
}

static void ble_on_midi(void *ctx, const u8 *msg, u32 len)
{
    (void)ctx;
    host_midi_in(msg, len);
}

#if TCFG_USB_SLAVE_MIDI_ENABLE
/* usb_midi_poll() hook: every SysEx byte; Sinco frames are decoded while they stream in */
static void on_usb_sysex_byte(u8 cable, u8 b)
{
    if (b == 0xF0) {
        usb_sx_cable = cable;
    } else if (cable != usb_sx_cable) {
        return;
    }
    u32 n = sinco_sx_byte(&usb_sx, b);
    if (n) {
        u32 r = sinco_handle(usb_sx.rx.buf, n, sinco_reply, &sinco_ops);
        u32 f = r ? sinco_sysex_encode(sinco_reply, r, sinco_frame, sizeof(sinco_frame)) : 0;
        if (f) {
            usb_midi_send_msg((u16)BIT(cable), sinco_frame, f);   /* reply on the same cable */
        }
    }
}
#endif

static void service_ble(u32 now)
{
    static u8 buf[512];                         /* >= one ATT write (local MTU 512) */
    u8 type;
    u32 n;

    if (ble_conn_gen != ble_smcpad_conn_gen()) {   /* new link (or none): fresh state */
        ble_conn_gen = ble_smcpad_conn_gen();
        ble_midi_enc_init(&ble_enc, ble_smcpad_payload_max());
        ble_midi_dec_init(&ble_dec, ble_on_midi, NULL, NULL);
        sinco_rx_reset(&ble_sx);
        ble_pkt_len = 0;
    }
    if (!ble_midi_enc_pending(&ble_enc)) {
        ble_midi_enc_set_max(&ble_enc, ble_smcpad_payload_max());
    }
    while ((n = ble_smcpad_rx_pop(&type, buf, sizeof(buf))) != 0) {
        if (type == BLE_SMCPAD_RX_MIDI) {
            ble_midi_dec_packet(&ble_dec, buf, n);
        } else if (type == BLE_SMCPAD_RX_SINCO) {
            sinco_rx_chunk_start(&ble_sx, now);
            for (u32 i = 0; i < n; i++) {
                u32 p = sinco_rx_byte(&ble_sx, buf[i]);
                if (p) {
                    u32 r = sinco_handle(ble_sx.buf, p, sinco_reply, &sinco_ops);
                    if (r && ble_smcpad_sinco_reply(sinco_reply, r) < 0) {
                        log_error("sinco reply dropped (busy)");
                    }
                }
            }
        }
    }
    ble_midi_flush();
    ble_smcpad_poll(now);

    /* globals: persist to VM once they stop changing (Midi Suite writes them live) */
    if (store.globals_gen != globals_gen_seen) {
        globals_gen_seen = store.globals_gen;
        globals_change_ms = now;
    }
    if (store.globals_dirty && now - globals_change_ms >= APP_GLOBALS_FLUSH_MS) {
        if (sinco_store_flush_globals(&store) < 0) {
            log_error("globals VM write failed");
            globals_change_ms = now;            /* retry later */
        }
    }
}

static void sinco_start(void)
{
    sinco_store_init(&store, smcpad_store_port(), presets_ram, SMCPAD_PRESETS_FLASH, SMCPAD_CALIB_FLASH);
    if (!store.presets_from_flash) {            /* blank USRFLASH: factory presets in RAM only */
        for (u8 i = 0; i < SMCPAD_PRESET_COUNT; i++) {
            smcpad_preset_factory(presets_ram + (u32)i * SMCPAD_PRESET_SIZE);
        }
    }
    smcpad_preset_init(&preset, &preset_io, presets_ram, store.globals);
    led_presets_gen = store.presets_gen;
    led_globals_gen = store.globals_gen;
    globals_gen_seen = store.globals_gen;
    log_info("presets %s, globals preset %d", store.presets_from_flash ? "loaded" : "factory",
             store.globals[GLOB_PRESET]);
    sinco_sx_reset(&usb_sx);
    sinco_rx_reset(&ble_sx);
    ble_conn_gen = ble_smcpad_conn_gen() + 1;  /* forces the BLE state reset */
    ble_smcpad_set_notify(app_notify);
#if TCFG_USB_SLAVE_MIDI_ENABLE
    usb_midi_set_sysex_hook(on_usb_sysex_byte);
#endif
}
#endif /* TCFG_SMCPAD_BLE_ENABLE */

static void midi_out(const u8 *m, u32 n)
{
#if TCFG_USB_SLAVE_MIDI_ENABLE
    usb_midi_send_msg(APP_MIDI_CABLES, m, n);
#endif
    smcpad_hw_trs_write(m, n);
#if TCFG_SMCPAD_BLE_ENABLE
    ble_midi_out(m, n);
#endif
}

#if !APP_PRESETS
static void midi3(u8 st, u8 d1, u8 d2)
{
    u8 m[3] = {st, (u8)(d1 & 0x7f), (u8)(d2 & 0x7f)};
    midi_out(m, 3);
}
#endif

static void pad_led_update(u8 pad)
{
#if APP_PRESETS
    (void)pad;
    preset.led_dirty = 1;                           /* refresh_leds() recomputes all pads */
#else
    smcpad_hw_rgb_set(pad, (app.held & BIT(pad)) ? APP_PAD_HELD_RGB : app.host_rgb[pad]);
    app.rgb_dirty = 1;
#endif
}

#if APP_PRESETS
/* pad colours + button LEDs from the interpreter, when its state, a preset or the globals changed */
static void refresh_leds(void)
{
    if (!preset.led_dirty && led_presets_gen == store.presets_gen && led_globals_gen == store.globals_gen) {
        return;
    }
    preset.led_dirty = 0;
    led_presets_gen = store.presets_gen;
    led_globals_gen = store.globals_gen;
    for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
        smcpad_hw_rgb_set(i, smcpad_preset_pad_rgb(&preset, i));
    }
    app.rgb_dirty = 1;
    smcpad_hw_button_leds(smcpad_preset_button_leds(&preset, app.buttons));
}
#endif

/* ---- input -> MIDI ------------------------------------------------------------------ */
static void service_pads(u32 now)
{
    struct smcpad_pad_ev e;
    while (smcpad_hw_pad_pop(&e)) {
#if APP_PRESETS
        if (e.type == SMCPAD_EV_PAD_ON) {
            app.held |= (u16)BIT(e.pad);
            smcpad_preset_pad(&preset, e.pad, 1, e.velocity ? e.velocity : 1);
        } else if (e.type == SMCPAD_EV_PAD_OFF) {
            app.held &= (u16)~BIT(e.pad);
            smcpad_preset_pad(&preset, e.pad, 0, 0);    /* sends AT 0 before the last Note Off */
        }
#else
        u8 note = (u8)(SMCPAD_MIDI_PAD_NOTE0 + e.pad);
        if (e.type == SMCPAD_EV_PAD_ON) {
            app.held |= (u16)BIT(e.pad);
            midi3(0x90 | SMCPAD_MIDI_PAD_CH, note, e.velocity ? e.velocity : 1);
        } else if (e.type == SMCPAD_EV_PAD_OFF) {
            app.held &= (u16)~BIT(e.pad);
            if (!app.held && app.last_at) {         /* stock: AT 0 before the last Note Off */
                u8 m[2] = {0xD0 | SMCPAD_MIDI_PAD_CH, 0};
                app.last_at = 0;
                midi_out(m, 2);
            }
            midi3(0x80 | SMCPAD_MIDI_PAD_CH, note, 64);
        }
#endif
        pad_led_update(e.pad);
    }

    if (app.held && now - app.last_at_ms >= APP_AT_PERIOD_MS) {
        u16 p = 0;
        u8 strongest = 0;
        for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
            if (app.held & BIT(i)) {
                u16 v = smcpad_hw_pad_pressure(i);
                if (v >= p) {
                    p = v;
                    strongest = i;
                }
            }
        }
        u32 at = (u32)p * 127u / smcpad_pad_cfg_default.vel_full;
        u8 v = (u8)(at > 127 ? 127 : at);
#if APP_PRESETS
        smcpad_preset_pressure(&preset, strongest, v);  /* Note pads, globals AT on, [min, max] */
#else
        (void)strongest;
        if (v != app.last_at) {
            u8 m[2] = {0xD0 | SMCPAD_MIDI_PAD_CH, v};
            midi_out(m, 2);
            app.last_at = v;
        }
#endif
        app.last_at_ms = now;
    }
}

static void service_knobs(u32 now)
{
#if APP_PRESETS
    if (now - app.last_knob_tick_ms >= APP_KNOB_TICK_MS) {
        smcpad_preset_tick(&preset, now);           /* walks values, Mackie touch release */
        app.last_knob_tick_ms = now;
    }
    if (now - app.last_knob_ms < APP_KNOB_PERIOD_MS) {
        return;                                     /* stock: deltas per 50 ms -> speed table */
    }
    app.last_knob_ms = now;
#else
    (void)now;
#endif
    for (u8 k = 0; k < SMCPAD_NUM_KNOBS; k++) {
        s32 diff = smcpad_hw_knob_count(k) - app.knob_rep[k];
        s32 steps = diff / 2;                       /* stock: count >> 1 per step */
        if (!steps) {
            continue;
        }
        app.knob_rep[k] += steps * 2;
#if APP_PRESETS
        smcpad_preset_knob(&preset, k, steps);
#else
        u8 v = smcpad_knob_value(app.knob_val[k], steps);
        if (v != app.knob_val[k]) {
            app.knob_val[k] = v;
            midi3(0xB0 | SMCPAD_MIDI_KNOB_CH, (u8)(SMCPAD_MIDI_KNOB_CC0 + k), v);
        }
#endif
    }
}

static void service_buttons(void)
{
    u16 now = smcpad_hw_buttons();
    u16 changed = now ^ app.buttons;
    if (!changed) {
        return;
    }
    for (u8 b = 0; b < SMCPAD_NUM_BUTTONS; b++) {
        if (changed & BIT(b)) {
#if APP_PRESETS
            smcpad_preset_button(&preset, b, (now & BIT(b)) ? 1 : 0);
#else
            midi3(0xB0 | SMCPAD_MIDI_BTN_CH, (u8)(SMCPAD_MIDI_BTN_CC0 + b), (now & BIT(b)) ? 127 : 0);
#endif
        }
    }
    app.buttons = now;
#if APP_PRESETS
    preset.led_dirty = 1;                           /* refresh_leds(): held + toggle + host state */
#else
    smcpad_hw_button_leds(now);                     /* local feedback for now */
#endif
}

/* ---- USB MIDI in ----------------------------------------------------------------------- */
static void put14(u8 **p, u32 v)
{
    *(*p)++ = (u8)(v & 0x7f);
    *(*p)++ = (u8)((v >> 7) & 0x7f);
}

static void put32(u8 **p, u32 v)                    /* 5 x 7 bits, LSB first */
{
    for (u8 i = 0; i < 5; i++) {
        *(*p)++ = (u8)(v & 0x7f);
        v >>= 7;
    }
}

static u32 get32(const u8 *q)
{
    u32 v = 0;
    for (u8 i = 0; i < 5; i++) {
        v |= (u32)(q[i] & 0x7f) << (7 * i);
    }
    return v;
}

/* peek/poke windows: RAM and the low-speed / high-speed SFR blocks */
static u8 diag_readable(u32 a)
{
    return !(a & 3) && (a < 0x30000 || (a >= 0x1e0000 && a < 0x200000));
}

static u8 diag_writable(u32 a)
{
    return !(a & 3) && a >= 0x1e0000 && a < 0x200000;
}

/* req = whole SysEx F0 7D 53 50 cmd ... F7 */
static void diag_reply(const u8 *req, u32 len)
{
    static u8 m[6 + SMCPAD_NUM_PADS * 6 + 1];
    u8 cmd = req[4];
    u8 *p = m;
    *p++ = 0xF0; *p++ = 0x7D; *p++ = 0x53; *p++ = 0x50; *p++ = cmd;
    if (cmd == 0x01) {
        struct smcpad_hw_status s;
        smcpad_hw_status(&s);
        *p++ = (u8)(s.polarity + 1);                /* 0 = pressing lowers, 2 = raises */
        for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
            put14(&p, smcpad_hw_pad_raw(i));
            put14(&p, smcpad_hw_pad_baseline(i));
            put14(&p, smcpad_hw_pad_pressure(i));
        }
    } else if (cmd == 0x02) {
        struct smcpad_hw_status s;
        smcpad_hw_status(&s);
        put14(&p, s.adc_frames);
        put14(&p, s.adc_frames >> 14);
        put14(&p, s.km_frames);
        put14(&p, s.km_frames >> 14);
        put14(&p, s.pad_drops);
        put14(&p, s.trs_drops);
        put14(&p, smcpad_hw_buttons());
        *p++ = s.pads_running;
        for (u8 k = 0; k < SMCPAD_NUM_KNOBS; k++) {
            put14(&p, (u32)smcpad_hw_knob_count(k));
        }
    } else if (cmd == 0x03) {                       /* raw key-matrix frame */
        u8 f[SMCPAD_KM_STEPS];
        struct smcpad_hw_status s;
        smcpad_hw_km_frame(f);
        smcpad_hw_status(&s);
        for (u8 i = 0; i < SMCPAD_KM_STEPS; i++) {
            *p++ = (u8)(f[i] & 0x7f);
        }
        put14(&p, smcpad_hw_buttons());
        put32(&p, s.km_frames);
    } else if (cmd == 0x10 && len >= 13) {          /* F0 7D 53 50 10 addr5 n 00 F7 */
        u32 a = get32(req + 5);
        u8 n = req[10];
        n = n < 1 ? 1 : (n > 16 ? 16 : n);
        put32(&p, a);
        for (u8 i = 0; i < n; i++, a += 4) {
            put32(&p, diag_readable(a) ? *(volatile u32 *)a : 0xDEADBEEF);
        }
    } else if (cmd == 0x11 && len >= 17) {          /* F0 7D 53 50 11 addr5 val5 00 F7 */
        u32 a = get32(req + 5), v = get32(req + 10);
        put32(&p, a);
        if (diag_writable(a)) {
            *(volatile u32 *)a = v;
            put32(&p, *(volatile u32 *)a);
        } else {
            put32(&p, 0xDEADBEEF);
        }
    } else if (cmd == 0x20 || (cmd == 0x21 && len >= 17)) {
        /* pad detector settings: on off vel_full (2 x 7 bit each), peak shift hold release (7 bit)
         * 0x20 = read, 0x21 = write (F0 7D 53 50 21 <10 bytes> 00 F7), live only (not saved) */
        struct smcpad_pad_cfg c;
        smcpad_hw_pad_cfg(&c, NULL);
        if (cmd == 0x21) {
            const u8 *q = req + 5;
            c.on_thr = (u16)(q[0] | (q[1] << 7));
            c.off_thr = (u16)(q[2] | (q[3] << 7));
            c.vel_full = (u16)(q[4] | (q[5] << 7));
            c.peak_frames = q[6];
            c.baseline_shift = q[7];
            c.hold_frames = q[8];
            c.release_frames = q[9];
            smcpad_hw_pad_cfg(&c, &c);
        }
        put14(&p, c.on_thr);
        put14(&p, c.off_thr);
        put14(&p, c.vel_full);
        *p++ = (u8)(c.peak_frames & 0x7f);
        *p++ = (u8)(c.baseline_shift & 0x7f);
        *p++ = (u8)(c.hold_frames & 0x7f);
        *p++ = (u8)(c.release_frames & 0x7f);
    } else {
        return;
    }
    *p++ = 0x00;                                    /* never end a frame with 7D F7 */
    *p++ = 0xF7;
#if TCFG_USB_SLAVE_MIDI_ENABLE
    usb_midi_send_msg(APP_MIDI_CABLES, m, (u32)(p - m));
#endif
}

/* MIDI from the host (USB short messages, BLE-MIDI, Sinco 0x15): LED / knob feedback */
static void host_midi_in(const u8 *msg, u32 len)
{
#if APP_PRESETS
    smcpad_preset_midi_in(&preset, msg, len);      /* Led bytes, 9F 7F 7F, knob values */
#else
    if (len == 3 && (msg[0] & 0x0f) == SMCPAD_MIDI_PAD_CH &&
        ((msg[0] & 0xf0) == 0x90 || (msg[0] & 0xf0) == 0x80)) {
        u8 pad = (u8)(msg[1] - SMCPAD_MIDI_PAD_NOTE0);
        if (pad < SMCPAD_NUM_PADS) {
            u32 vel = (msg[0] & 0xf0) == 0x90 ? msg[2] : 0;
            u32 c = APP_HOST_RGB, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
            r = r * vel / 127; g = g * vel / 127; b = b * vel / 127;
            app.host_rgb[pad] = (r << 16) | (g << 8) | b;
            pad_led_update(pad);
        }
    }
#endif
}

#if TCFG_USB_SLAVE_MIDI_ENABLE
static void on_usb_midi(u8 cable, const u8 *msg, u32 len)
{
    (void)cable;
    if (len >= 6 && msg[0] == 0xF0 && msg[1] == 0x7D && msg[2] == 0x53 && msg[3] == 0x50 &&
        msg[len - 1] == 0xF7) {
        diag_reply(msg, len);
        return;
    }
    if (msg[0] != 0xF0) {               /* Sinco SysEx goes through on_usb_sysex_byte() */
        host_midi_in(msg, len);
    }
}
#endif

/* ---- task -------------------------------------------------------------------------------- */
static void smcpad_task(void *p)
{
    (void)p;
    u8 pads_started = 0;

    /* boot animation: all pad LEDs dim white for ~0.5 s */
    for (u8 i = 0; i < SMCPAD_NUM_RGB; i++) {
        smcpad_hw_rgb_set(i, 0x202020);
    }
    smcpad_hw_rgb_show();
    os_time_dly(50);
    for (u8 i = 0; i < SMCPAD_NUM_RGB; i++) {
        smcpad_hw_rgb_set(i, 0);
    }
    smcpad_hw_rgb_show();
#if TCFG_SMCPAD_BLE_ENABLE
    sinco_start();
#endif

    for (;;) {
        os_sem_pend(&app_sem, 1);                   /* events, or at least every tick */
        smcpad_hw_notified();
        u32 now = sys_timer_get_ms();

        if (!pads_started && smcpad_hw_pad_calibrated()) {
            smcpad_hw_pad_begin();                  /* hands off pads must be idle at boot */
            pads_started = 1;
        }
        service_pads(now);
        service_knobs(now);
        service_buttons();
#if TCFG_USB_SLAVE_MIDI_ENABLE
        usb_midi_poll(on_usb_midi);
#endif
#if TCFG_SMCPAD_BLE_ENABLE
        service_ble(now);
#endif
#if APP_PRESETS
        refresh_leds();
#endif
        if (app.rgb_dirty && now - app.last_rgb_ms >= APP_RGB_PERIOD_MS) {
            if (smcpad_hw_rgb_show()) {
                app.rgb_dirty = 0;
                app.last_rgb_ms = now;
            }
        }
        smcpad_hw_sync_sdk_adc();
    }
}

void smcpad_start(void)
{
    static u8 started;
    if (started) {
        return;
    }
    started = 1;
    memset(&app, 0, sizeof(app));
    os_sem_create(&app_sem, 0);
    smcpad_hw_init(app_notify);
    os_task_create(smcpad_task, NULL, APP_TASK_PRIO, APP_TASK_STACK, 0, "smcpad");
    log_info("smcpad started");
}

#endif /* CONFIG_BOARD_SMCPAD && TCFG_SMCPAD_HW_ENABLE */
