/* SMC-PAD BLE server: stock-compatible GATT (Device Information, Battery, Sinco AE40,
 * BLE-MIDI, JieLi RCSP AE00), replacing trans_data's ble_trans.c. See ble_smcpad.c.
 *
 * Context rules: the GATT callbacks run in the BT stack task and only copy into a ring;
 * everything below is for the smcpad task.
 */
#ifndef BLE_SMCPAD_H
#define BLE_SMCPAD_H

#include "typedef.h"

/* Identity. First tests keep the stock name, so SincoOTA (which filters by name) can still
 * see the pad and flash stock firmware back over BLE. To rename it, change it here. */
#ifndef SMCPAD_DEVICE_NAME
#define SMCPAD_DEVICE_NAME      "SMC-PAD"
#endif
/* 0x11 reply = "<name>_<version>". SincoOTA compares this version with the one in the .fwsc
 * file name and refuses older files ("The device version is higher than the firmware"), so we
 * report 000: the official SMC-PAD_005.fwsc can always be flashed back. Our own images are
 * wrapped as SMC-PAD_9xx.fwsc (tools/fw/make_fwsc.py; the name is not tied to the payload). */
#ifndef SMCPAD_FW_VERSION_STR
#define SMCPAD_FW_VERSION_STR   "000"
#endif
#define SMCPAD_NAME_VER         SMCPAD_DEVICE_NAME "_" SMCPAD_FW_VERSION_STR

enum {
    BLE_SMCPAD_RX_MIDI = 1,     /* a BLE-MIDI packet (write to 7772E5DB) */
    BLE_SMCPAD_RX_SINCO = 2,    /* a chunk of a Sinco packet (write to AE41) */
};

/* fn is called from the BT task when data arrived, a send slot freed up or the link changed */
void ble_smcpad_set_notify(void (*fn)(void));
/* Pops one received write: returns its length (0 = ring empty), *type = BLE_SMCPAD_RX_*. */
u32  ble_smcpad_rx_pop(u8 *type, u8 *buf, u32 max);
u32  ble_smcpad_rx_dropped(void);

u32  ble_smcpad_connected(void);
u32  ble_smcpad_conn_gen(void);         /* changes on every connect / disconnect */
/* BT button: 0 = stop advertising and drop the link, 1 = advertise again. Applied in app_core
 * shortly after; <0 if it could not be queued. Every boot starts enabled (OTA stays reachable). */
int  ble_smcpad_set_enabled(u8 en);
u8   ble_smcpad_enabled(void);
u16  ble_smcpad_payload_max(void);      /* ATT MTU - 3 */
u32  ble_smcpad_midi_ready(void);       /* connected and BLE-MIDI notifications enabled */
/* One BLE-MIDI notification. 0 = sent, <0 = no room / not subscribed (caller keeps it). */
int  ble_smcpad_midi_send(const u8 *pkt, u16 len);
/* Queues a Sinco reply (copied) for AE42; sent in MTU-sized chunks by ble_smcpad_poll().
 * <0 if the previous reply is still going out. */
int  ble_smcpad_sinco_reply(const u8 *pkt, u32 len);
u32  ble_smcpad_sinco_busy(void);
/* Task: pushes queued Sinco chunks, battery level notifications. */
void ble_smcpad_poll(u32 now_ms);

#endif
