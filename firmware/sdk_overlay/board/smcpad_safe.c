/* SMC-PAD safe mode: decides at boot whether the SMC-PAD drivers (pads, key matrix, LEDs,
 * TRS, Sinco/preset task) start at all. In safe mode only the SDK runs: BLE (GATT server,
 * JieLi RCSP OTA for SincoOTA) and USB, so a firmware whose drivers crash or hang can still
 * be replaced over the air. Rule and manual trigger: smcpad_safe_step() in smcpad_logic.h.
 * SincoOTA asks for the Sinco name/version (0x11) before an update, so safe mode keeps a
 * minimal Sinco responder on a 20 ms SDK timer: 0x11 / 0x12 answered, storage refused.
 *
 * SMCPAD_FORCE_SAFE_MODE 1 (apply.py --safe-only) never starts the drivers: the "minimal
 * first image" used to prove the OTA round trip on real hardware.
 */
#include "app_config.h"

#if defined(CONFIG_BOARD_SMCPAD) && TCFG_SMCPAD_HW_ENABLE

#include "system/includes.h"
#include "syscfg_id.h"
#include "smcpad_logic.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[SMCPAD]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

#ifndef SMCPAD_SAFE_VM_ID
#define SMCPAD_SAFE_VM_ID       45          /* free user id in spp_and_le (globals use 0x2C) */
#endif
#if SMCPAD_SAFE_VM_ID < CFG_USER_DEFINE_BEGIN || SMCPAD_SAFE_VM_ID > CFG_USER_DEFINE_END
#error "SMCPAD_SAFE_VM_ID must be a user VM id"
#endif
#ifndef SMCPAD_FORCE_SAFE_MODE
#define SMCPAD_FORCE_SAFE_MODE  0
#endif

static u8 safe_mode;

u8 smcpad_safe_mode(void)
{
    return safe_mode;
}

#if TCFG_SMCPAD_BLE_ENABLE
#include "sinco_proto.h"
#include "ble_smcpad.h"

#define SAFE_SINCO_PERIOD_MS    20

static struct sinco_rx safe_rx;
static u8 safe_reply[SINCO_MAX_PKT];
static u32 safe_conn_gen;

static int safe_refuse_read(void *ctx, u8 type, u32 addr, u8 *dst, u32 len)
{
    (void)ctx; (void)type; (void)addr; (void)dst; (void)len;
    return -1;
}

static int safe_refuse_write(void *ctx, u8 type, u32 addr, const u8 *src, u32 len)
{
    (void)ctx; (void)type; (void)addr; (void)src; (void)len;
    return -1;
}

static const struct sinco_ops safe_ops = {
    NULL, SMCPAD_NAME_VER, safe_refuse_read, safe_refuse_write, NULL, NULL
};

/* drains the BLE ring like smcpad_app.c's service_ble(), Sinco only (BLE-MIDI is dropped) */
static void safe_sinco_poll(void *priv)
{
    static u8 buf[512];
    u8 type;
    u32 n, now = sys_timer_get_ms();
    (void)priv;
    if (safe_conn_gen != ble_smcpad_conn_gen()) {
        safe_conn_gen = ble_smcpad_conn_gen();
        sinco_rx_reset(&safe_rx);
    }
    while ((n = ble_smcpad_rx_pop(&type, buf, sizeof(buf))) != 0) {
        if (type != BLE_SMCPAD_RX_SINCO) {
            continue;
        }
        sinco_rx_chunk_start(&safe_rx, now);
        for (u32 i = 0; i < n; i++) {
            u32 p = sinco_rx_byte(&safe_rx, buf[i]);
            if (p) {
                u32 r = sinco_handle(safe_rx.buf, p, safe_reply, &safe_ops);
                if (r) {
                    ble_smcpad_sinco_reply(safe_reply, r);
                }
            }
        }
    }
    ble_smcpad_poll(now);                       /* sends reply chunks (and battery level) */
}

static void safe_services_start(void)
{
    sinco_rx_reset(&safe_rx);
    safe_conn_gen = ble_smcpad_conn_gen() + 1;
    sys_timer_add(NULL, safe_sinco_poll, SAFE_SINCO_PERIOD_MS);
}
#else
static void safe_services_start(void)
{
}
#endif /* TCFG_SMCPAD_BLE_ENABLE */

#if !SMCPAD_FORCE_SAFE_MODE
static void safe_clear(void *priv)
{
    u8 zero = 0;
    (void)priv;
    syscfg_write(SMCPAD_SAFE_VM_ID, &zero, 1);  /* this boot survived: reset the counter */
}
#endif

/* Called once from the trans_data start hook, before smcpad_start(). 1 = do not start. */
u8 smcpad_safe_boot(void)
{
#if SMCPAD_FORCE_SAFE_MODE
    safe_mode = 1;
    log_info("safe-only image: SMC-PAD drivers disabled");
    safe_services_start();
    return 1;
#else
    u8 stored = 0, store;
    if (syscfg_read(SMCPAD_SAFE_VM_ID, &stored, 1) != 1) {
        stored = 0;
    }
    safe_mode = smcpad_safe_step(stored, &store);
    syscfg_write(SMCPAD_SAFE_VM_ID, &store, 1);
    if (safe_mode) {
        log_info("SAFE MODE: %d short boots in a row, SMC-PAD drivers not started", SMCPAD_SAFE_BOOTS - 1);
        safe_services_start();
    } else {
        sys_timeout_add(NULL, safe_clear, SMCPAD_SAFE_CLEAR_MS);
    }
    return safe_mode;
#endif
}

#endif /* CONFIG_BOARD_SMCPAD && TCFG_SMCPAD_HW_ENABLE */
