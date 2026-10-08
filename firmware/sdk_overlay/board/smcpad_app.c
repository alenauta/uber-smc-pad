/* SMC-PAD application glue: one task turns pad / knob / button input into MIDI on
 * USB (cable 0), TRS and BLE-MIDI, drives the LEDs, answers a small diagnostic SysEx and
 * (with TCFG_SMCPAD_BLE_ENABLE) serves the Sinco config protocol (Midi Suite / SincoOTA)
 * over USB SysEx and BLE AE41/AE42 with the stock storage model (sinco_store.c).
 *
 * Default mapping = the stock factory preset (docs/stock_midi_map.md):
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
#endif

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[SMCPAD]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

#define APP_MIDI_CABLES     BIT(0)              /* stock duplicates on cable 0 and 1 */
#define APP_AT_PERIOD_MS    10
#define APP_RGB_PERIOD_MS   10
#define APP_PAD_HELD_RGB    0x606060
#define APP_HOST_RGB        0x0040ff            /* colour for host-lit pads, scaled by vel */
#define APP_TASK_PRIO       2
#define APP_TASK_STACK      512                 /* u32 words */

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

static void host_midi_in(const u8 *msg, u32 len);

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
    globals_gen_seen = store.globals_gen;
    log_info("presets %s, globals preset %d", store.presets_from_flash ? "loaded" : "blank",
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

static void midi3(u8 st, u8 d1, u8 d2)
{
    u8 m[3] = {st, (u8)(d1 & 0x7f), (u8)(d2 & 0x7f)};
    midi_out(m, 3);
}

static void pad_led_update(u8 pad)
{
    smcpad_hw_rgb_set(pad, (app.held & BIT(pad)) ? APP_PAD_HELD_RGB : app.host_rgb[pad]);
    app.rgb_dirty = 1;
}

/* ---- input -> MIDI ------------------------------------------------------------------ */
static void service_pads(u32 now)
{
    struct smcpad_pad_ev e;
    while (smcpad_hw_pad_pop(&e)) {
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
        pad_led_update(e.pad);
    }

    if (app.held && now - app.last_at_ms >= APP_AT_PERIOD_MS) {
        u16 p = 0;
        for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
            if (app.held & BIT(i)) {
                u16 v = smcpad_hw_pad_pressure(i);
                p = v > p ? v : p;
            }
        }
        u32 at = (u32)p * 127u / smcpad_pad_cfg_default.vel_full;
        u8 v = (u8)(at > 127 ? 127 : at);
        if (v != app.last_at) {
            u8 m[2] = {0xD0 | SMCPAD_MIDI_PAD_CH, v};
            midi_out(m, 2);
            app.last_at = v;
        }
        app.last_at_ms = now;
    }
}

static void service_knobs(void)
{
    for (u8 k = 0; k < SMCPAD_NUM_KNOBS; k++) {
        s32 diff = smcpad_hw_knob_count(k) - app.knob_rep[k];
        s32 steps = diff / 2;                       /* stock: count >> 1 per step */
        if (!steps) {
            continue;
        }
        app.knob_rep[k] += steps * 2;
        u8 v = smcpad_knob_value(app.knob_val[k], steps);
        if (v != app.knob_val[k]) {
            app.knob_val[k] = v;
            midi3(0xB0 | SMCPAD_MIDI_KNOB_CH, (u8)(SMCPAD_MIDI_KNOB_CC0 + k), v);
        }
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
            midi3(0xB0 | SMCPAD_MIDI_BTN_CH, (u8)(SMCPAD_MIDI_BTN_CC0 + b), (now & BIT(b)) ? 127 : 0);
        }
    }
    app.buttons = now;
    smcpad_hw_button_leds(now);                     /* local feedback for now */
}

/* ---- USB MIDI in ----------------------------------------------------------------------- */
static void put14(u8 **p, u32 v)
{
    *(*p)++ = (u8)(v & 0x7f);
    *(*p)++ = (u8)((v >> 7) & 0x7f);
}

static void diag_reply(u8 cmd)
{
    static u8 m[6 + SMCPAD_NUM_PADS * 6 + 1];
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
    } else {
        return;
    }
    *p++ = 0xF7;
#if TCFG_USB_SLAVE_MIDI_ENABLE
    usb_midi_send_msg(APP_MIDI_CABLES, m, (u32)(p - m));
#endif
}

/* MIDI from the host (USB short messages, BLE-MIDI, Sinco 0x15): pad LEDs */
static void host_midi_in(const u8 *msg, u32 len)
{
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
}

#if TCFG_USB_SLAVE_MIDI_ENABLE
static void on_usb_midi(u8 cable, const u8 *msg, u32 len)
{
    (void)cable;
    if (len == 6 && msg[0] == 0xF0 && msg[1] == 0x7D && msg[2] == 0x53 && msg[3] == 0x50 &&
        msg[5] == 0xF7) {
        diag_reply(msg[4]);
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
        service_knobs();
        service_buttons();
#if TCFG_USB_SLAVE_MIDI_ENABLE
        usb_midi_poll(on_usb_midi);
#endif
#if TCFG_SMCPAD_BLE_ENABLE
        service_ble(now);
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
