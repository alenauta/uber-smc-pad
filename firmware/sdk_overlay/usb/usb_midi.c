/*
 * USB-MIDI 1.0 device class for the JieLi AC63 SDK (BR25 / AC636N), SMC-PAD project.
 * See usb_midi.h for configuration and context rules. Modelled on the SDK's cdc.c/hid.c.
 *
 * References: USB Device Class Definition for MIDI Devices 1.0 (midi10.pdf),
 * USB Audio 1.0 (AudioControl interface header).
 */
#include "system/includes.h"
#include "usb/device/usb_stack.h"
#include "usb/usb_config.h"
#include "app_config.h"
#include "os/os_api.h"
#include "usb_midi.h"

#define LOG_TAG_CONST       USB
#define LOG_TAG             "[MIDI]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

#if TCFG_USB_SLAVE_MIDI_ENABLE

#if TCFG_USB_SLAVE_MSD_ENABLE && ((USB_MIDI_EP_IN == MSD_BULK_EP_IN) || (USB_MIDI_EP_OUT == MSD_BULK_EP_OUT))
#error "USB-MIDI endpoint collides with MSD; move USB_MIDI_EP_IN/OUT"
#endif
#if TCFG_USB_SLAVE_CDC_ENABLE && ((USB_MIDI_EP_IN == CDC_DATA_EP_IN) || (USB_MIDI_EP_OUT == CDC_DATA_EP_OUT))
#error "USB-MIDI endpoint collides with CDC; move USB_MIDI_EP_IN/OUT"
#endif
#if TCFG_USB_SLAVE_HID_ENABLE && ((USB_MIDI_EP_IN == HID_EP_IN) || (USB_MIDI_EP_OUT == HID_EP_OUT))
#error "USB-MIDI endpoint collides with HID; move USB_MIDI_EP_IN/OUT"
#endif
#if (USB_MIDI_EP_IN < 1) || (USB_MIDI_EP_IN >= USB_MAX_HW_EPNUM) || (USB_MIDI_EP_OUT < 1) || (USB_MIDI_EP_OUT >= USB_MAX_HW_EPNUM)
#error "USB-MIDI endpoint number out of range"
#endif

/* descriptor constants */
#define DT_CS_INTERFACE     0x24
#define DT_CS_ENDPOINT      0x25
#define AC_HEADER           0x01
#define MS_HEADER           0x01
#define MS_MIDI_IN_JACK     0x02
#define MS_MIDI_OUT_JACK    0x03
#define MS_GENERAL          0x01
#define JACK_EMBEDDED       0x01
#define JACK_EXTERNAL       0x02
/* jack ids for cable c: 4c+1 emb IN, 4c+2 ext IN, 4c+3 emb OUT, 4c+4 ext OUT */
#define JACK_EMB_IN(c)      (u8)(4 * (c) + 1)
#define JACK_EXT_IN(c)      (u8)(4 * (c) + 2)
#define JACK_EMB_OUT(c)     (u8)(4 * (c) + 3)
#define JACK_EXT_OUT(c)     (u8)(4 * (c) + 4)

#define MS_CS_TOTAL_LEN     (7 + USB_MIDI_NUM_CABLES * (6 + 6 + 9 + 9) + 2 * (9 + 4 + USB_MIDI_NUM_CABLES))

static const char *const jack_names[] = {
    USB_MIDI_JACK_NAME_0,
#if USB_MIDI_NUM_CABLES > 1
    USB_MIDI_JACK_NAME_1,
#endif
#if USB_MIDI_NUM_CABLES > 2
    USB_MIDI_JACK_NAME_2,
#endif
};
#define NUM_JACK_NAMES  (sizeof(jack_names) / sizeof(jack_names[0]))

struct midi_rx_parser {
    u8 in_sysex;
    u8 overflow;
    u16 len;
    u8 buf[USB_MIDI_SYSEX_MAX];
};

struct usb_midi_gadget {
    u8 *ep_in_dma;
    u8 *ep_out_dma;
    usb_dev usb_id;
    u8 ms_itf;
    void (*rx_notify)(void);
    OS_MUTEX tx_mutex;
    volatile u32 rx_head;       /* written by the IRQ */
    volatile u32 rx_tail;       /* written by the task */
    u32 rx_ring[USB_MIDI_RX_RING];
    u8 rx_tmp[USB_MIDI_MAXP] __attribute__((aligned(4)));
    struct usb_midi_stats stats;
    struct midi_rx_parser parser[USB_MIDI_NUM_CABLES];
};

static struct usb_midi_gadget _midi = {0};
static struct usb_midi_gadget *midi = NULL;
/* outside the gadget struct: survives usb_midi_register() / re-enumeration */
static usb_midi_sysex_byte_cb sysex_hook;

static u32 usb_midi_itf_handler(struct usb_device_t *usb_device, struct usb_ctrlrequest *req);
static void usb_midi_reset(struct usb_device_t *usb_device, u32 itf);

/* ---------------------------------------------------------------- helpers --- */
static u32 ascii_to_usb_string(u8 *dst, const char *s, u32 max_len)
{
    u32 n = 2;
    while (*s && (n + 2) <= max_len && n < 254) {
        dst[n++] = (u8)*s++;
        dst[n++] = 0;
    }
    dst[0] = (u8)n;
    dst[1] = USB_DT_STRING;
    return n;
}

static u8 *put_ep(u8 *p, u8 addr)
{
    /* Audio-class standard endpoint descriptor: 9 bytes (bRefresh, bSynchAddress) */
    *p++ = 9;
    *p++ = USB_DT_ENDPOINT;
    *p++ = addr;
    *p++ = USB_ENDPOINT_XFER_BULK;
    *p++ = LOBYTE(USB_MIDI_MAXP);
    *p++ = HIBYTE(USB_MIDI_MAXP);
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    return p;
}

/* ------------------------------------------------------------ descriptors --- */
u32 usb_midi_desc_config(const usb_dev usb_id, u8 *ptr, u32 *cur_itf_num)
{
    u8 *p = ptr;
    const u8 ac_itf = (u8) * cur_itf_num;
    const u8 ms_itf = ac_itf + 1;
    u32 c;

    /* AudioControl interface (no endpoints) */
    *p++ = 9;  *p++ = USB_DT_INTERFACE; *p++ = ac_itf; *p++ = 0; *p++ = 0;
    *p++ = USB_CLASS_AUDIO; *p++ = 0x01 /* AUDIOCONTROL */; *p++ = 0; *p++ = 0;
    /* CS AC header: bcdADC 1.00, wTotalLength 9, 1 streaming interface */
    *p++ = 9;  *p++ = DT_CS_INTERFACE; *p++ = AC_HEADER; *p++ = 0x00; *p++ = 0x01;
    *p++ = 9;  *p++ = 0; *p++ = 1; *p++ = ms_itf;

    /* MIDIStreaming interface, 2 endpoints */
    *p++ = 9;  *p++ = USB_DT_INTERFACE; *p++ = ms_itf; *p++ = 0; *p++ = 2;
    *p++ = USB_CLASS_AUDIO; *p++ = 0x03 /* MIDISTREAMING */; *p++ = 0; *p++ = 0;
    /* CS MS header: bcdMSC 1.00 */
    *p++ = 7;  *p++ = DT_CS_INTERFACE; *p++ = MS_HEADER; *p++ = 0x00; *p++ = 0x01;
    *p++ = LOBYTE(MS_CS_TOTAL_LEN); *p++ = HIBYTE(MS_CS_TOTAL_LEN);

    for (c = 0; c < USB_MIDI_NUM_CABLES; c++) {
        u8 istr = (c < NUM_JACK_NAMES) ? (u8)(USB_MIDI_STR_INDEX_BASE + c) : 0;
        /* IN jack, embedded (host -> device data enters here) */
        *p++ = 6; *p++ = DT_CS_INTERFACE; *p++ = MS_MIDI_IN_JACK; *p++ = JACK_EMBEDDED;
        *p++ = JACK_EMB_IN(c); *p++ = istr;
        /* IN jack, external */
        *p++ = 6; *p++ = DT_CS_INTERFACE; *p++ = MS_MIDI_IN_JACK; *p++ = JACK_EXTERNAL;
        *p++ = JACK_EXT_IN(c); *p++ = 0;
        /* OUT jack, embedded (device -> host), source = external IN jack */
        *p++ = 9; *p++ = DT_CS_INTERFACE; *p++ = MS_MIDI_OUT_JACK; *p++ = JACK_EMBEDDED;
        *p++ = JACK_EMB_OUT(c); *p++ = 1; *p++ = JACK_EXT_IN(c); *p++ = 1; *p++ = istr;
        /* OUT jack, external, source = embedded IN jack */
        *p++ = 9; *p++ = DT_CS_INTERFACE; *p++ = MS_MIDI_OUT_JACK; *p++ = JACK_EXTERNAL;
        *p++ = JACK_EXT_OUT(c); *p++ = 1; *p++ = JACK_EMB_IN(c); *p++ = 1; *p++ = 0;
    }

    /* bulk OUT + CS endpoint (associated embedded IN jacks) */
    p = put_ep(p, USB_DIR_OUT | USB_MIDI_EP_OUT);
    *p++ = 4 + USB_MIDI_NUM_CABLES; *p++ = DT_CS_ENDPOINT; *p++ = MS_GENERAL; *p++ = USB_MIDI_NUM_CABLES;
    for (c = 0; c < USB_MIDI_NUM_CABLES; c++) {
        *p++ = JACK_EMB_IN(c);
    }
    /* bulk IN + CS endpoint (associated embedded OUT jacks) */
    p = put_ep(p, USB_DIR_IN | USB_MIDI_EP_IN);
    *p++ = 4 + USB_MIDI_NUM_CABLES; *p++ = DT_CS_ENDPOINT; *p++ = MS_GENERAL; *p++ = USB_MIDI_NUM_CABLES;
    for (c = 0; c < USB_MIDI_NUM_CABLES; c++) {
        *p++ = JACK_EMB_OUT(c);
    }

    if (usb_set_interface_hander(usb_id, ac_itf, usb_midi_itf_handler) != ac_itf) {
        ASSERT(0, "midi ac interface_hander fail");
    }
    if (usb_set_interface_hander(usb_id, ms_itf, usb_midi_itf_handler) != ms_itf) {
        ASSERT(0, "midi ms interface_hander fail");
    }
    if (usb_set_reset_hander(usb_id, ms_itf, usb_midi_reset) != ms_itf) {
        ASSERT(0, "midi reset_hander fail");
    }
    if (midi) {
        midi->ms_itf = ms_itf;
    }
    *cur_itf_num = ms_itf + 1;
    return (u32)(p - ptr);
}

void usb_midi_patch_device_descriptor(u8 *ptr)
{
    ptr[8]  = LOBYTE(SMCPAD_USB_VID);
    ptr[9]  = HIBYTE(SMCPAD_USB_VID);
    ptr[10] = LOBYTE(SMCPAD_USB_PID);
    ptr[11] = HIBYTE(SMCPAD_USB_PID);
    ptr[12] = LOBYTE(SMCPAD_USB_BCD_DEVICE);
    ptr[13] = HIBYTE(SMCPAD_USB_BCD_DEVICE);
}

void usb_midi_get_manufacturer_str(u8 *ptr)
{
    ascii_to_usb_string(ptr, SMCPAD_USB_MANUFACTURER, 64);
}

void usb_midi_get_product_str(u8 *ptr)
{
    ascii_to_usb_string(ptr, SMCPAD_USB_PRODUCT, 64);
}

u32 usb_midi_string_request(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    u32 idx = LOBYTE(req->wValue);
    if (idx < USB_MIDI_STR_INDEX_BASE || idx >= USB_MIDI_STR_INDEX_BASE + NUM_JACK_NAMES) {
        return 0;
    }
    u8 *buf = usb_get_setup_buffer(usb_device);
    u32 n = ascii_to_usb_string(buf, jack_names[idx - USB_MIDI_STR_INDEX_BASE], 64);
    usb_set_data_payload(usb_device, req, buf, n);
    return 1;
}

/* -------------------------------------------------------------- endpoints --- */
static void midi_rx_isr(struct usb_device_t *usb_device, u32 ep)
{
    (void)ep;
    if (midi == NULL) {
        return;
    }
    const usb_dev usb_id = usb_device2id(usb_device);
    /* bulk endpoints are double-buffered: always read into our own buffer (see cdc.c) */
    u32 len = usb_g_bulk_read(usb_id, USB_MIDI_EP_OUT, midi->rx_tmp, USB_MIDI_MAXP, 0);
    u32 pushed = 0;
    for (u32 i = 0; i + 4 <= len; i += 4) {
        const u8 *b = &midi->rx_tmp[i];
        if ((b[0] & 0x0f) == 0) {           /* CIN 0: reserved / padding */
            continue;
        }
        u32 head = midi->rx_head;
        if (head - midi->rx_tail >= USB_MIDI_RX_RING) {
            midi->stats.rx_overflow++;
            continue;
        }
        midi->rx_ring[head & (USB_MIDI_RX_RING - 1)] =
            (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
        midi->rx_head = head + 1;
        midi->stats.rx_packets++;
        pushed++;
    }
    if (pushed && midi->rx_notify) {
        midi->rx_notify();
    }
}

static void midi_endpoint_init(struct usb_device_t *usb_device, u32 itf)
{
    (void)itf;
    if (midi == NULL) {
        return;
    }
    const usb_dev usb_id = usb_device2id(usb_device);
    usb_g_ep_config(usb_id, USB_MIDI_EP_IN | USB_DIR_IN, USB_ENDPOINT_XFER_BULK,
                    0, midi->ep_in_dma, USB_MIDI_MAXP);
    usb_g_ep_config(usb_id, USB_MIDI_EP_OUT | USB_DIR_OUT, USB_ENDPOINT_XFER_BULK,
                    1, midi->ep_out_dma, USB_MIDI_MAXP);
    usb_g_set_intr_hander(usb_id, USB_MIDI_EP_OUT | USB_DIR_OUT, midi_rx_isr);
    usb_enable_ep(usb_id, USB_MIDI_EP_IN);
}

static void usb_midi_reset(struct usb_device_t *usb_device, u32 itf)
{
    log_info("%s()", __func__);
#if USB_ROOT2
    usb_disable_ep(usb_device2id(usb_device), USB_MIDI_EP_IN);
#else
    midi_endpoint_init(usb_device, itf);
#endif
}

static u32 usb_midi_itf_handler(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    if (req == (struct usb_ctrlrequest *)-1) {
        return 0;
    }
    u8 *tx_payload = usb_get_setup_buffer(usb_device);
    const u32 itf = LOBYTE(req->wIndex);

    if ((req->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
        /* MIDI 1.0 defines no mandatory class requests (element requests are optional) */
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        return 0;
    }
    switch (req->bRequest) {
    case USB_REQ_SET_INTERFACE:
        if (usb_device->bDeviceStates != USB_CONFIGURED || req->wValue != 0) {
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        } else {
            if (midi && itf == midi->ms_itf) {
                midi_endpoint_init(usb_device, itf);
            }
            usb_set_setup_phase(usb_device, USB_EP0_STAGE_SETUP);
        }
        break;
    case USB_REQ_GET_INTERFACE:
        if (req->wValue || req->wLength != 1 || usb_device->bDeviceStates != USB_CONFIGURED) {
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        } else {
            tx_payload[0] = 0x00;
            usb_set_data_payload(usb_device, req, tx_payload, 1);
        }
        break;
    case USB_REQ_GET_STATUS:
        if (usb_device->bDeviceStates != USB_CONFIGURED) {
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        } else {
            tx_payload[0] = 0;
            tx_payload[1] = 0;
            usb_set_data_payload(usb_device, req, tx_payload, 2);
        }
        break;
    default:
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        break;
    }
    return 0;
}

#if USB_MIDI_ECHO_TEST
static void midi_echo_cb(u8 cable, const u8 *msg, u32 len)
{
    usb_midi_send_msg((u16)BIT(cable), msg, len);
}
static void midi_echo_timer(void *priv)
{
    usb_midi_poll(midi_echo_cb);
}
#endif

void usb_midi_register(const usb_dev usb_id)
{
    if (midi) {
        return;
    }
    memset(&_midi, 0, sizeof(_midi));
    midi = &_midi;
    midi->usb_id = usb_id;
    os_mutex_create(&midi->tx_mutex);
    midi->ep_in_dma = usb_alloc_ep_dmabuffer(usb_id, USB_MIDI_EP_IN | USB_DIR_IN, USB_MIDI_MAXP * 2);
    midi->ep_out_dma = midi->ep_in_dma + USB_MIDI_MAXP;
#if USB_MIDI_ECHO_TEST
    static u16 echo_timer;
    if (!echo_timer) {
        echo_timer = sys_timer_add(NULL, midi_echo_timer, 5);
    }
#endif
    log_info("registered: %d cables, EP%d IN / EP%d OUT", USB_MIDI_NUM_CABLES, USB_MIDI_EP_IN, USB_MIDI_EP_OUT);
}

void usb_midi_release(const usb_dev usb_id)
{
    (void)usb_id;
    if (midi) {
        /* the DMA pool is reset by usb_memory_init() on the next usb_device_mode() */
        os_mutex_del(&midi->tx_mutex, 0);
        midi = NULL;
    }
}

/* ---------------------------------------------------------- application API --- */
u32 usb_midi_is_online(void)
{
    if (midi == NULL) {
        return 0;
    }
    return usb_id2device(midi->usb_id)->bDeviceStates == USB_CONFIGURED;
}

void usb_midi_set_rx_notify(void (*notify)(void))
{
    if (midi) {
        midi->rx_notify = notify;
    }
}

void usb_midi_get_stats(struct usb_midi_stats *out)
{
    if (midi) {
        memcpy(out, &midi->stats, sizeof(*out));
    } else {
        memset(out, 0, sizeof(*out));
    }
}

static u32 midi_write_locked(const u8 *buf, u32 len)
{
    u32 off = 0;
    while (off < len) {
        u32 n = len - off > USB_MIDI_MAXP ? USB_MIDI_MAXP : len - off;
        u32 w = usb_g_bulk_write(midi->usb_id, USB_MIDI_EP_IN, buf + off, n);
        if (w == 0) {
            midi->stats.tx_short_writes++;
            break;
        }
        off += w;
    }
    midi->stats.tx_packets += off / 4;
    return off;
}

u32 usb_midi_send_packets(const u8 *pkts, u32 npkts)
{
    if (!usb_midi_is_online() || npkts == 0) {
        return 0;
    }
    os_mutex_pend(&midi->tx_mutex, 0);
    u32 w = midi_write_locked(pkts, npkts * 4);
    os_mutex_post(&midi->tx_mutex);
    return w / 4;
}

/* length of a non-SysEx message from its status byte, 0 = invalid */
static u32 midi_msg_len(u8 status)
{
    if (status < 0x80) {
        return 0;
    }
    if (status < 0xF0) {
        u8 hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
    switch (status) {
    case 0xF1:
    case 0xF3:
        return 2;
    case 0xF2:
        return 3;
    case 0xF6:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        return 1;
    default:            /* F0/F7 handled by the caller, F4/F5 undefined */
        return 0;
    }
}

/* Encode the next USB-MIDI event packet of msg for one cable, starting at *pos.
 * Advances *pos; the message is complete when *pos == len. Always writes 4 bytes. */
static void midi_encode_next(u8 cable, const u8 *msg, u32 len, u32 *pos, u8 *o)
{
    const u8 cn = (u8)(cable << 4);
    const u32 i = *pos;
    const u32 left = len - i;
    u8 cin;
    u32 n;

    if (msg[0] == 0xF0) {
        if (left > 3) {
            cin = 0x4;                      /* SysEx start / continue */
            n = 3;
        } else {
            cin = (u8)(0x4 + left);         /* CIN 5/6/7: SysEx ends with 1/2/3 bytes */
            n = left;
        }
    } else {
        n = len;
        if (msg[0] < 0xF0) {
            cin = msg[0] >> 4;              /* channel voice: CIN = status high nibble */
        } else if (len == 1) {
            cin = (msg[0] == 0xF6) ? 0x5 : 0xF;
        } else {
            cin = (len == 2) ? 0x2 : 0x3;   /* 2/3-byte system common */
        }
    }
    o[0] = cn | cin;
    o[1] = msg[i];
    o[2] = n > 1 ? msg[i + 1] : 0;
    o[3] = n > 2 ? msg[i + 2] : 0;
    *pos = i + n;
}

u32 usb_midi_send_msg(u16 cable_mask, const u8 *msg, u32 len)
{
    if (!msg || len == 0 || !usb_midi_is_online()) {
        return 0;
    }
    if (msg[0] == 0xF0) {
        if (len < 2 || msg[len - 1] != 0xF7) {
            return 0;
        }
    } else if (midi_msg_len(msg[0]) != len) {
        return 0;
    }
    cable_mask &= (u16)((1u << USB_MIDI_NUM_CABLES) - 1);
    if (!cable_mask) {
        return 0;
    }

    u8 buf[USB_MIDI_MAXP] __attribute__((aligned(4)));
    u32 fill = 0, ok = 1;
    os_mutex_pend(&midi->tx_mutex, 0);
    for (u8 c = 0; c < USB_MIDI_NUM_CABLES && ok; c++) {
        if (!(cable_mask & BIT(c))) {
            continue;
        }
        u32 pos = 0;
        while (pos < len) {
            if (fill + 4 > sizeof(buf)) {
                if (midi_write_locked(buf, fill) != fill) {
                    ok = 0;
                    break;
                }
                fill = 0;
            }
            midi_encode_next(c, msg, len, &pos, buf + fill);
            fill += 4;
        }
    }
    if (ok && fill && midi_write_locked(buf, fill) != fill) {
        ok = 0;
    }
    os_mutex_post(&midi->tx_mutex);
    return ok ? len : 0;
}

u32 usb_midi_read_packet(u8 pkt[4])
{
    if (midi == NULL) {
        return 0;
    }
    u32 tail = midi->rx_tail;
    if (tail == midi->rx_head) {
        return 0;
    }
    u32 v = midi->rx_ring[tail & (USB_MIDI_RX_RING - 1)];
    midi->rx_tail = tail + 1;
    pkt[0] = (u8)v;
    pkt[1] = (u8)(v >> 8);
    pkt[2] = (u8)(v >> 16);
    pkt[3] = (u8)(v >> 24);
    return 1;
}

static void sysex_append(struct midi_rx_parser *ps, u8 cable, const u8 *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (sysex_hook) {
            sysex_hook(cable, b[i]);
        }
        if (ps->len < USB_MIDI_SYSEX_MAX) {
            ps->buf[ps->len++] = b[i];
        } else {
            ps->overflow = 1;
        }
    }
}

void usb_midi_set_sysex_hook(usb_midi_sysex_byte_cb hook)
{
    sysex_hook = hook;
}

void usb_midi_poll(usb_midi_msg_cb cb)
{
    u8 p[4];
    while (usb_midi_read_packet(p)) {
        const u8 cable = p[0] >> 4;
        const u8 cin = p[0] & 0x0f;
        if (cable >= USB_MIDI_NUM_CABLES) {
            continue;
        }
        struct midi_rx_parser *ps = &midi->parser[cable];
        u32 end_n = 0;          /* >0: SysEx ends with this many bytes */
        switch (cin) {
        case 0x4:               /* SysEx start or continue */
            if (p[1] == 0xF0) {
                ps->in_sysex = 1;
                ps->overflow = 0;
                ps->len = 0;
            }
            if (ps->in_sysex) {
                sysex_append(ps, cable, &p[1], 3);
            }
            break;
        case 0x5:               /* 1-byte system common, or SysEx end with 1 byte */
            if (p[1] == 0xF7) {
                end_n = 1;
            } else if (cb) {
                cb(cable, &p[1], 1);
            }
            break;
        case 0x6:
            end_n = 2;
            break;
        case 0x7:
            end_n = 3;
            break;
        case 0x2:               /* 2-byte system common */
        case 0xC:
        case 0xD:
            if (cb) {
                cb(cable, &p[1], 2);
            }
            break;
        case 0x3:               /* 3-byte system common */
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xE:
            if (cb) {
                cb(cable, &p[1], 3);
            }
            break;
        case 0xF:               /* single byte (real-time), may interleave SysEx */
            if (cb) {
                cb(cable, &p[1], 1);
            }
            break;
        default:                /* 0x0/0x1 reserved */
            break;
        }
        if (end_n) {
            if (p[1] == 0xF0) {             /* tiny SysEx entirely in one packet */
                ps->in_sysex = 1;
                ps->overflow = 0;
                ps->len = 0;
            }
            if (ps->in_sysex) {
                sysex_append(ps, cable, &p[1], end_n);
                if (ps->overflow) {
                    midi->stats.rx_sysex_dropped++;
                } else if (cb) {
                    cb(cable, ps->buf, ps->len);
                }
            }
            ps->in_sysex = 0;
            ps->len = 0;
        }
    }
}

#endif /* TCFG_USB_SLAVE_MIDI_ENABLE */
