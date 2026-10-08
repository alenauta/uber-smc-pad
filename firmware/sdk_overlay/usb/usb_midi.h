/*
 * USB-MIDI 1.0 device class for the JieLi AC63 SDK (BR25 / AC636N), SMC-PAD project.
 * Copied into apps/common/device/usb/device/ by firmware/sdk_overlay/apply.py.
 *
 * Enumerates like the stock SMC-PAD: one AudioControl + one MIDIStreaming interface,
 * USB_MIDI_NUM_CABLES virtual cables (embedded+external jack pair per direction and cable),
 * bulk IN/OUT endpoints of 64 bytes.
 *
 * Context rules:
 *  - usb_midi_send_*() and usb_midi_poll()/usb_midi_read_packet(): TASK context only.
 *  - The rx_notify callback runs in the USB IRQ: keep it tiny (e.g. post a message).
 */
#ifndef __USB_MIDI_H__
#define __USB_MIDI_H__

#include "typedef.h"
#include "usb/device/usb_stack.h"

/* ---- configuration (override in the board cfg) ---------------------------------- */
#ifndef USB_MIDI_EP_IN
#define USB_MIDI_EP_IN              1           /* EP1 is free when MSD is disabled */
#endif
#ifndef USB_MIDI_EP_OUT
#define USB_MIDI_EP_OUT             1
#endif
#ifndef USB_MIDI_MAXP
#define USB_MIDI_MAXP               64
#endif
#ifndef USB_MIDI_NUM_CABLES
#define USB_MIDI_NUM_CABLES         3           /* stock: Master, Private, MCP */
#endif
#ifndef USB_MIDI_RX_RING
#define USB_MIDI_RX_RING            64          /* 4-byte event packets, power of two */
#endif
#ifndef USB_MIDI_SYSEX_MAX
#define USB_MIDI_SYSEX_MAX          256         /* per cable, including F0/F7 */
#endif
#ifndef USB_MIDI_STR_INDEX_BASE
#define USB_MIDI_STR_INDEX_BASE     0x10        /* jack name string ids: BASE..BASE+cables-1 */
#endif
#ifndef USB_MIDI_JACK_NAME_0
#define USB_MIDI_JACK_NAME_0        "SMC-PAD-Master"
#endif
#ifndef USB_MIDI_JACK_NAME_1
#define USB_MIDI_JACK_NAME_1        "SMC-PAD-Private"
#endif
#ifndef USB_MIDI_JACK_NAME_2
#define USB_MIDI_JACK_NAME_2        "SMC-PAD-MCP"
#endif
/* Device identity. Default: pid.codes test VID/PID (1209:0001), deliberately NOT the stock
 * 4353:3243, so vendor tools (Midi Suite / SincoOTA) never mistake us for a stock pad. */
#ifndef SMCPAD_USB_VID
#define SMCPAD_USB_VID              0x1209
#endif
#ifndef SMCPAD_USB_PID
#define SMCPAD_USB_PID              0x0001
#endif
#ifndef SMCPAD_USB_BCD_DEVICE
#define SMCPAD_USB_BCD_DEVICE       0x0001
#endif
#ifndef SMCPAD_USB_MANUFACTURER
#define SMCPAD_USB_MANUFACTURER     "SMC-PAD Project"
#endif
#ifndef SMCPAD_USB_PRODUCT
#define SMCPAD_USB_PRODUCT          "SMC-PAD"
#endif
#ifndef USB_MIDI_ECHO_TEST
#define USB_MIDI_ECHO_TEST          0           /* 1: echo every received message (bring-up) */
#endif

#if USB_MIDI_NUM_CABLES < 1 || USB_MIDI_NUM_CABLES > 16
#error "USB_MIDI_NUM_CABLES must be 1..16"
#endif
#if (USB_MIDI_RX_RING & (USB_MIDI_RX_RING - 1)) != 0
#error "USB_MIDI_RX_RING must be a power of two"
#endif

/* ---- SDK glue (called from usb_device.c / descriptor.c / user_setup.c) ----------- */
u32  usb_midi_desc_config(const usb_dev usb_id, u8 *ptr, u32 *cur_itf_num);
void usb_midi_register(const usb_dev usb_id);
void usb_midi_release(const usb_dev usb_id);
void usb_midi_patch_device_descriptor(u8 *ptr);     /* VID/PID/bcdDevice */
void usb_midi_get_manufacturer_str(u8 *ptr);
void usb_midi_get_product_str(u8 *ptr);
/* returns 1 if the GET_DESCRIPTOR(STRING) request was a jack name and has been answered */
u32  usb_midi_string_request(struct usb_device_t *usb_device, struct usb_ctrlrequest *req);

/* ---- application API ------------------------------------------------------------ */
typedef void (*usb_midi_msg_cb)(u8 cable, const u8 *msg, u32 len);

u32  usb_midi_is_online(void);                       /* host has configured the device */
void usb_midi_set_rx_notify(void (*notify)(void));   /* IRQ context! */

/* Encode one complete MIDI message (channel, system common/real-time, or a whole
 * F0..F7 SysEx) and send it on every cable in cable_mask (bit n = cable n), packets
 * interleaved in as few bulk transfers as possible. Returns bytes of msg accepted
 * (len on success, 0 if offline/invalid/short write). */
u32  usb_midi_send_msg(u16 cable_mask, const u8 *msg, u32 len);
/* Raw USB-MIDI event packets (4 bytes each). Returns packets written. */
u32  usb_midi_send_packets(const u8 *pkts, u32 npkts);

/* Pop one raw received packet. Returns 1 if a packet was copied to pkt[4]. */
u32  usb_midi_read_packet(u8 pkt[4]);
/* Drain the RX ring, reassemble messages (incl. SysEx) and call cb for each one. */
void usb_midi_poll(usb_midi_msg_cb cb);
/* Optional: called from usb_midi_poll() (task context) with every SysEx byte, F0..F7, as it
 * arrives, so frames longer than USB_MIDI_SYSEX_MAX can be stream-decoded. Whole frames that
 * fit are still delivered to the poll callback as well. */
typedef void (*usb_midi_sysex_byte_cb)(u8 cable, u8 b);
void usb_midi_set_sysex_hook(usb_midi_sysex_byte_cb hook);

struct usb_midi_stats {
    u32 rx_packets, rx_overflow, rx_sysex_dropped;
    u32 tx_packets, tx_short_writes;
};
void usb_midi_get_stats(struct usb_midi_stats *out);

#endif /* __USB_MIDI_H__ */
