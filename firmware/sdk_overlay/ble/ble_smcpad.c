/* SMC-PAD BLE server (sdk_overlay). Adapted from the AC63 SDK v2.4.0
 * apps/spp_and_le/examples/trans_data/ble_trans.c: same exported functions (trans_server_init,
 * trans_disconnect, bt_ble_before_start_init, bt_ble_init, bt_ble_exit, ble_module_enable,
 * ble_trans_flow_enable, ble_server_send_test_key_num) and the same RCSP glue, with the stock
 * SMC-PAD GATT table instead of the demo one (ble_smcpad_profile.h, generated from
 * ble_smcpad_profile.json by tools/gatt/gen_jl_profile.py).
 *
 *   advertising   flags + BLE-MIDI 128-bit service UUID + name (as stock); scan response:
 *                 the SDK's RCSP tag
 *   pairing       "Just Works" (CONFIG_BT_SM_SUPPORT_ENABLE, IO capability NoInputNoOutput)
 *   connection    asks for 11.25-15 ms first (MIDI latency), then the SDK's defaults
 */
#include "system/app_core.h"
#include "system/includes.h"

#include "app_config.h"
#include "app_action.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "user_cfg.h"
#include "vm.h"
#include "btcontroller_modules.h"
#include "bt_common.h"
#include "3th_profile_api.h"
#include "le_common.h"
#include "rcsp_bluetooth.h"
#include "JL_rcsp_api.h"
#include "custom_cfg.h"
#include "btstack/btstack_event.h"
#include "gatt_common/le_gatt_common.h"
#include "app_power_manage.h"
#include "ble_smcpad_profile.h"
#include "ble_smcpad.h"
#include "sinco_proto.h"

#if CONFIG_APP_SPP_LE

#if LE_DEBUG_PRINT_EN
#define log_info(x, ...)  printf("[BLE_SMCPAD]" x " ", ## __VA_ARGS__)
#else
#define log_info(...)
#endif

#define MIDI_VALUE_HANDLE   ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_VALUE_HANDLE
#define MIDI_CCC_HANDLE     ATT_CHARACTERISTIC_7772E5DB_3868_4112_A1A9_F2669D106BF3_01_CLIENT_CONFIGURATION_HANDLE
#define SINCO_RX_HANDLE     ATT_CHARACTERISTIC_ae41_01_VALUE_HANDLE
#define SINCO_TX_HANDLE     ATT_CHARACTERISTIC_ae42_01_VALUE_HANDLE
#define SINCO_CCC_HANDLE    ATT_CHARACTERISTIC_ae42_01_CLIENT_CONFIGURATION_HANDLE
#define BATT_VALUE_HANDLE   ATT_CHARACTERISTIC_2a19_01_VALUE_HANDLE
#define BATT_CCC_HANDLE     ATT_CHARACTERISTIC_2a19_01_CLIENT_CONFIGURATION_HANDLE

#define ATT_LOCAL_MTU_SIZE        (512)
#define ATT_PACKET_NUMS_MAX       (2)
#define ATT_SEND_CBUF_SIZE        (ATT_PACKET_NUMS_MAX * (ATT_PACKET_HEAD_SIZE + ATT_LOCAL_MTU_SIZE))
#define ADV_INTERVAL_MIN          (160)         /* 100 ms (unit 0.625 ms); demo: 500 ms */
#define BLE_RX_RING               2048          /* power of two, divides 65536 */
#define BATT_PERIOD_MS            60000

static uint8_t conn_update_enable = 1;
/* queued requests; the first one the central accepts is used. {min, max, latency, timeout} */
static const struct conn_update_param_t conn_param_table[] = {
    {9,  12, 0,  400},          /* 11.25-15 ms, BLE-MIDI */
    {12, 24, 0,  400},          /* 15-30 ms (iOS minimum for non-MIDI) */
    {16, 24, 10, 600},          /* SDK defaults */
    {12, 28, 10, 600},
};
#define CONN_PARAM_TABLE_CNT      (sizeof(conn_param_table) / sizeof(conn_param_table[0]))

#define EIR_TAG_STRING   0xd6, 0x05, 0x08, 0x00, 'J', 'L', 'A', 'I', 'S', 'D', 'K'
static const char user_tag_string[] = {EIR_TAG_STRING};
/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, little endian */
static const u8 midi_service_uuid128[16] = {
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
};

static u8  adv_data[ADV_RSP_PACKET_MAX];
static u8  scan_rsp_data[ADV_RSP_PACKET_MAX];
static u16 con_handle;
static u16 att_mtu = 23;
static volatile u32 conn_gen;
static adv_cfg_t server_adv_config;
static void (*app_notify)(void);

/* BT task -> smcpad task: [len lo, len hi, type, data...] records */
static struct {
    volatile u16 head;          /* written by the BT task */
    volatile u16 tail;          /* written by the smcpad task */
    u32 dropped;
    u8  buf[BLE_RX_RING];
} rx;

/* Sinco reply going out on AE42 */
static struct {
    u32 len, off;
    u8  buf[SINCO_MAX_PKT];
} stx;

static u8  batt_last = 0xFF;
static u32 batt_ms;
static volatile u8 ble_on = 1;          /* BT button state; every boot starts with BLE on */

static uint16_t smcpad_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int smcpad_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int smcpad_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param);

/* "Just Works": no MITM (NoInputNoOutput), bonding so the host can reconnect encrypted. 917+:
 * the pad asks for security right after connecting (like the stock firmware), so iOS bonds (one
 * pairing pop-up), keeps the pad in its list and reconnects to it. 0 = the host decides (<= 916).
 * Not waiting for it: MIDI flows during pairing, and a host that declines still works. */
static const sm_cfg_t smcpad_sm_init_config = {
    .slave_security_auto_req = 1,
    .slave_set_wait_security = 0,
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
    .authentication_req_flags = SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

const gatt_server_cfg_t smcpad_server_init_cfg = {
    .att_read_cb = &smcpad_att_read_callback,
    .att_write_cb = &smcpad_att_write_callback,
    .event_packet_handler = &smcpad_event_packet_handler,
};

static gatt_ctrl_t smcpad_gatt_control_block = {
    .mtu_size = ATT_LOCAL_MTU_SIZE,
    .cbuffer_size = ATT_SEND_CBUF_SIZE,
    .multi_dev_flag = 0,
#if CONFIG_BT_GATT_SERVER_NUM
    .server_config = &smcpad_server_init_cfg,
#else
    .server_config = NULL,
#endif
    .client_config = NULL,      /* CONFIG_BT_GATT_CLIENT_NUM is 0 for the SMC-PAD */
#if CONFIG_BT_SM_SUPPORT_ENABLE
    .sm_config = &smcpad_sm_init_config,
#else
    .sm_config = NULL,
#endif
    .hci_cb_packet_handler = NULL,
};

static void notify_app(void)
{
    if (app_notify) {
        app_notify();
    }
}

/* ---- RX ring (producer: BT task) ------------------------------------------------------- */
static void rx_push(u8 type, const u8 *d, u16 n)
{
    u16 head = rx.head;
    u32 used = (u16)(head - rx.tail);
    if (n == 0 || used + 3u + n > BLE_RX_RING) {
        rx.dropped++;
        return;
    }
    rx.buf[head++ & (BLE_RX_RING - 1)] = (u8)n;
    rx.buf[head++ & (BLE_RX_RING - 1)] = (u8)(n >> 8);
    rx.buf[head++ & (BLE_RX_RING - 1)] = type;
    for (u16 i = 0; i < n; i++) {
        rx.buf[head++ & (BLE_RX_RING - 1)] = d[i];
    }
    rx.head = head;             /* publish after the data */
    notify_app();
}

u32 ble_smcpad_rx_pop(u8 *type, u8 *buf, u32 max)
{
    u16 tail = rx.tail;
    if (tail == rx.head) {
        return 0;
    }
    u32 n = rx.buf[tail & (BLE_RX_RING - 1)] | ((u32)rx.buf[(u16)(tail + 1) & (BLE_RX_RING - 1)] << 8);
    *type = rx.buf[(u16)(tail + 2) & (BLE_RX_RING - 1)];
    tail += 3;
    for (u32 i = 0; i < n; i++, tail++) {
        if (i < max) {
            buf[i] = rx.buf[tail & (BLE_RX_RING - 1)];
        }
    }
    rx.tail = tail;
    return n < max ? n : max;
}

u32 ble_smcpad_rx_dropped(void)
{
    return rx.dropped;
}

void ble_smcpad_set_notify(void (*fn)(void))
{
    app_notify = fn;
}

/* ---- TX (smcpad task) -------------------------------------------------------------------- */
u32 ble_smcpad_connected(void)
{
    return con_handle != 0;
}

/* runs in app_core (Q_CALLBACK), like the SDK's own key handlers calling ble_module_enable() */
static void enable_in_app_core(int en)
{
    ble_module_enable((u8)en);  /* 0: adv off, disconnect, idle; 1: advertise again */
    notify_app();
}

int ble_smcpad_set_enabled(u8 en)
{
    int msg[3];
    en = en ? 1 : 0;
    if (en == ble_on) {
        return 0;
    }
    msg[0] = (int)enable_in_app_core;
    msg[1] = 1;
    msg[2] = en;
    if (os_taskq_post_type("app_core", Q_CALLBACK, 3, msg) != 0) {
        return -1;
    }
    ble_on = en;
    return 0;
}

u8 ble_smcpad_enabled(void)
{
    return ble_on;
}

u32 ble_smcpad_conn_gen(void)
{
    return conn_gen;
}

u16 ble_smcpad_payload_max(void)
{
    return att_mtu > 3 ? att_mtu - 3 : 20;
}

u32 ble_smcpad_midi_ready(void)
{
    return con_handle && ble_gatt_server_characteristic_ccc_get(con_handle, MIDI_CCC_HANDLE);
}

static int notify(u16 value_handle, u16 ccc_handle, const u8 *d, u16 n)
{
    u16 h = con_handle;
    if (!h || !ble_gatt_server_characteristic_ccc_get(h, ccc_handle)) {
        return -2;
    }
    if (!ble_comm_att_check_send(h, n)) {
        return -1;
    }
    return ble_comm_att_send_data(h, value_handle, (u8 *)d, n, ATT_OP_AUTO_READ_CCC) ? -1 : 0;
}

int ble_smcpad_midi_send(const u8 *pkt, u16 len)
{
    return notify(MIDI_VALUE_HANDLE, MIDI_CCC_HANDLE, pkt, len);
}

int ble_smcpad_sinco_reply(const u8 *pkt, u32 len)
{
    if (stx.off < stx.len) {
        return -1;
    }
    if (len > sizeof(stx.buf)) {
        return -2;
    }
    memcpy(stx.buf, pkt, len);
    stx.off = 0;
    stx.len = len;
    ble_smcpad_poll(0);
    return 0;
}

u32 ble_smcpad_sinco_busy(void)
{
    return stx.off < stx.len;
}

void ble_smcpad_poll(u32 now_ms)
{
    if (!con_handle) {
        stx.off = stx.len = 0;
        batt_last = 0xFF;
        return;
    }
    while (stx.off < stx.len) {
        u32 n = stx.len - stx.off;
        u16 max = ble_smcpad_payload_max();
        if (n > max) {
            n = max;
        }
        int r = notify(SINCO_TX_HANDLE, SINCO_CCC_HANDLE, stx.buf + stx.off, (u16)n);
        if (r == -2) {              /* not subscribed: nobody listens, drop the reply */
            stx.off = stx.len = 0;
            break;
        }
        if (r) {
            break;                  /* retried on GATT_COMM_EVENT_CAN_SEND_NOW */
        }
        stx.off += n;
    }
    if (now_ms && (batt_last == 0xFF || now_ms - batt_ms >= BATT_PERIOD_MS)) {
        u8 level = get_vbat_percent();
        batt_ms = now_ms;
        if (level != batt_last && notify(BATT_VALUE_HANDLE, BATT_CCC_HANDLE, &level, 1) != -1) {
            batt_last = level;
        }
    }
}

/* ---- GATT server ------------------------------------------------------------------------- */
static void send_connection_update(u16 conn_handle)
{
    if (conn_update_enable) {
        if (0 == ble_gatt_server_connetion_update_request(conn_handle, conn_param_table, CONN_PARAM_TABLE_CNT)) {
            conn_update_enable = 0;
        }
    }
}

/* bonded central reconnected encrypted: it does not rewrite the CCCs, restore them */
static void resume_all_ccc_enable(u16 conn_handle, u8 update_request)
{
    log_info("resume_all_ccc_enable\n");
#if RCSP_BTMATE_EN
    ble_gatt_server_characteristic_ccc_set(conn_handle, ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
#endif
    ble_gatt_server_characteristic_ccc_set(conn_handle, SINCO_CCC_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(conn_handle, MIDI_CCC_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(conn_handle, BATT_CCC_HANDLE, ATT_OP_NOTIFY);
    if (update_request) {
        send_connection_update(conn_handle);
    }
}

static int smcpad_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param)
{
    switch (event) {
    case GATT_COMM_EVENT_CAN_SEND_NOW:
        notify_app();
        break;

    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        con_handle = little_endian_read_16(packet, 0);
        att_mtu = 23;
        conn_update_enable = 1;
        conn_gen++;
        log_info("connection_handle:%04x, interval %d\n", con_handle, little_endian_read_16(ext_param, 14 + 0));
        notify_app();
        break;

    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        log_info("disconnect_handle:%04x,reason= %02x\n", little_endian_read_16(packet, 0), packet[2]);
        if (con_handle == little_endian_read_16(packet, 0)) {
            con_handle = 0;
            conn_gen++;
            notify_app();
        }
        break;

    case GATT_COMM_EVENT_ENCRYPTION_CHANGE:
        log_info("ENCRYPTION_CHANGE:handle=%04x,state=%d,process =%d", little_endian_read_16(packet, 0), packet[2], packet[3]);
        if (packet[3] == LINK_ENCRYPTION_RECONNECT) {
            resume_all_ccc_enable(little_endian_read_16(packet, 0), 1);
        }
        break;

    case GATT_COMM_EVENT_CONNECTION_UPDATE_COMPLETE:
        log_info("conn_param update interval = %d\n", little_endian_read_16(ext_param, 6 + 0));
        break;

    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE:
        att_mtu = little_endian_read_16(packet, 2);
        if (att_mtu > ATT_LOCAL_MTU_SIZE) {
            att_mtu = ATT_LOCAL_MTU_SIZE;
        }
        log_info("ATT MTU = %u\n", att_mtu);
        notify_app();
        break;

    default:
        break;
    }
    return 0;
}

/* BTstack read semantics: buffer == NULL -> total length; else copy from offset */
static uint16_t read_blob(const u8 *src, u16 len, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    if (!buffer) {
        return len;
    }
    if (offset >= len) {
        return 0;
    }
    u16 n = len - offset;
    if (n > buffer_size) {
        n = buffer_size;
    }
    memcpy(buffer, src + offset, n);
    return n;
}

static uint16_t smcpad_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    switch (att_handle) {
    case ATT_CHARACTERISTIC_2a00_01_VALUE_HANDLE: {
        const char *gap_name = ble_comm_get_gap_name();
        return read_blob((const u8 *)gap_name, (u16)strlen(gap_name), offset, buffer, buffer_size);
    }
    case BATT_VALUE_HANDLE: {
        u8 level = get_vbat_percent();
        return read_blob(&level, 1, offset, buffer, buffer_size);
    }
    case MIDI_VALUE_HANDLE:
        return 0;               /* BLE-MIDI spec: a read returns no payload */

    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
    case BATT_CCC_HANDLE:
    case SINCO_CCC_HANDLE:
    case MIDI_CCC_HANDLE:
#if RCSP_BTMATE_EN
    case ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE:
#endif
        if (buffer) {
            buffer[0] = ble_gatt_server_characteristic_ccc_get(connection_handle, att_handle);
            buffer[1] = 0;
        }
        return 2;

    default:
        return 0;
    }
}

static int smcpad_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    (void)transaction_mode;
    (void)offset;

    switch (att_handle) {
    case MIDI_VALUE_HANDLE:
        rx_push(BLE_SMCPAD_RX_MIDI, buffer, buffer_size);
        break;

    case SINCO_RX_HANDLE:
        rx_push(BLE_SMCPAD_RX_SINCO, buffer, buffer_size);
        break;

    case MIDI_CCC_HANDLE:
    case SINCO_CCC_HANDLE:
    case BATT_CCC_HANDLE:
    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
        send_connection_update(connection_handle);
        log_info("write ccc:%04x,%02x\n", att_handle, buffer[0]);
        ble_gatt_server_characteristic_ccc_set(connection_handle, att_handle, buffer[0]);
        notify_app();
        break;

#if RCSP_BTMATE_EN
    case ATT_CHARACTERISTIC_ae02_01_CLIENT_CONFIGURATION_HANDLE:
        ble_op_latency_skip(connection_handle, 0xffff);
        ble_gatt_server_set_update_send(connection_handle, ATT_CHARACTERISTIC_ae02_01_VALUE_HANDLE, ATT_OP_AUTO_READ_CCC);
        log_info("write ccc:%04x,%02x\n", att_handle, buffer[0]);
        ble_gatt_server_characteristic_ccc_set(connection_handle, att_handle, buffer[0]);
        break;

    case ATT_CHARACTERISTIC_ae01_01_VALUE_HANDLE:
        ble_gatt_server_receive_update_data(NULL, buffer, buffer_size);
        break;
#endif

    default:
        break;
    }
    return 0;
}

/* ---- advertising --------------------------------------------------------------------------- */
static int make_set_adv_data(void)
{
    u8 offset = 0;
    u8 *buf = adv_data;

    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_EDR_NOT_SUPPORTED, 1);
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_128BIT_SERVICE_UUIDS, (void *)midi_service_uuid128, 16);

    const char *gap_name = ble_comm_get_gap_name();
    u8 name_len = strlen(gap_name);
    u8 vaild_len = ADV_RSP_PACKET_MAX - (offset + 2);
    if (name_len > vaild_len) {
        name_len = vaild_len;   /* stock fits: 3 + 18 + 2 + 7 = 30 bytes */
    }
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_name, name_len);

    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***smcpad_adv_data overflow!!!!!!\n");
        return -1;
    }
    server_adv_config.adv_data_len = offset;
    server_adv_config.adv_data = adv_data;
    return 0;
}

static int make_set_rsp_data(void)
{
    u8 offset = 0;
    u8 *buf = scan_rsp_data;

#if RCSP_BTMATE_EN
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, (void *)user_tag_string, sizeof(user_tag_string));
#endif
    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***rsp_data overflow!!!!!!\n");
        return -1;
    }
    server_adv_config.rsp_data_len = offset;
    server_adv_config.rsp_data = scan_rsp_data;
    return 0;
}

static void adv_config_set(void)
{
    int ret = 0;
    ret |= make_set_adv_data();
    ret |= make_set_rsp_data();

    server_adv_config.adv_interval = ADV_INTERVAL_MIN;
    server_adv_config.adv_auto_do = 1;
    server_adv_config.adv_type = ADV_IND;
    server_adv_config.adv_channel = ADV_CHANNEL_ALL;
    memset(server_adv_config.direct_address_info, 0, 7);

    if (ret) {
        log_info("adv_setup_init fail!!!\n");
        return;
    }
    ble_gatt_server_set_adv_config(&server_adv_config);
}

/* ---- exported (same names as ble_trans.c) ----------------------------------------------- */
void trans_server_init(void)
{
    ble_gatt_server_set_profile(smcpad_profile_data, sizeof(smcpad_profile_data));
    adv_config_set();
}

void trans_disconnect(void)
{
    if (con_handle) {
        ble_comm_disconnect(con_handle);
    }
}

void bt_ble_before_start_init(void)
{
    ble_comm_init(&smcpad_gatt_control_block);
}

int ble_trans_flow_enable(u8 en)
{
    (void)en;
    return -1;                  /* no ATT flow control (as ble_trans.c's default) */
}

void bt_ble_init(void)
{
    log_info("%s\n", __FUNCTION__);
    ble_comm_set_config_name(SMCPAD_DEVICE_NAME, 0);
    con_handle = 0;
    ble_on = 1;
    trans_server_init();
    ble_module_enable(1);
}

void bt_ble_exit(void)
{
    ble_module_enable(0);
    ble_comm_exit();
}

void ble_module_enable(u8 en)
{
    ble_comm_module_enable(en);
}

void ble_server_send_test_key_num(u8 key_num)
{
    if (con_handle) {
        if (get_remote_test_flag()) {
            ble_op_test_key_num(con_handle, key_num);
        }
    }
}

#endif /* CONFIG_APP_SPP_LE */
