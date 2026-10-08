/*
 * Host unit tests for firmware/sdk_overlay/usb/usb_midi.c.
 *
 * Build & run with gcc:
 *   cd firmware/sdk_overlay && gcc -Wall -Wextra -Werror -std=c11 \
 *      -I test/stubs -I usb test/test_usb_midi.c -o /tmp/test_usb_midi && /tmp/test_usb_midi
 */
#include "sdk_stubs.h"
#include "usb_midi.h"
#ifndef USB_MIDI_NO_INCLUDE_C
#include "../usb/usb_midi.c"
#endif

/* ---- stub state & implementations --------------------------------------- */
struct usb_device_t stub_dev = {0};
itf_hander stub_itf[8] = {0};
itf_reset_hander stub_reset[8] = {0};
usb_interrupt stub_rx_isr = NULL;
u8 stub_setup_buf[512] = {0};
u8 stub_payload[512] = {0};
u32 stub_payload_len = 0;
int stub_phase = 0;
u8 stub_tx[8192] = {0};
u32 stub_tx_len = 0, stub_tx_writes = 0, stub_tx_max_write = 0;
const u8 *stub_rx_data = NULL;
u32 stub_rx_len = 0;
u32 stub_ep_cfg_calls = 0;
static u8 stub_dma_pool[256] = {0};
static u32 notify_count = 0;

usb_dev usb_device2id(const struct usb_device_t *d) { (void)d; return 0; }
struct usb_device_t *usb_id2device(const usb_dev id) { (void)id; return &stub_dev; }

u32 usb_set_interface_hander(const usb_dev id, u32 itf, itf_hander h)
{
    (void)id;
    ASSERT(itf < 8, "itf out of range");
    stub_itf[itf] = h;
    return itf;
}

u32 usb_set_reset_hander(const usb_dev id, u32 itf, itf_reset_hander h)
{
    (void)id;
    ASSERT(itf < 8, "itf out of range");
    stub_reset[itf] = h;
    return itf;
}

void *usb_get_setup_buffer(const struct usb_device_t *d)
{
    (void)d;
    return stub_setup_buf;
}

u8 *usb_set_data_payload(struct usb_device_t *d, struct usb_ctrlrequest *r, const void *data, u32 len)
{
    (void)d;
    (void)r;
    ASSERT(len <= sizeof(stub_payload), "payload too big");
    memcpy(stub_payload, data, len);
    stub_payload_len = len;
    return stub_payload;
}

void usb_set_setup_phase(struct usb_device_t *d, u8 phase)
{
    (void)d;
    stub_phase = phase;
}

u32 usb_g_ep_config(const usb_dev id, u32 ep, u32 type, u32 ie, u8 *ptr, u32 size)
{
    (void)id; (void)ep; (void)type; (void)ie; (void)ptr; (void)size;
    stub_ep_cfg_calls++;
    return 0;
}

u32 usb_g_set_intr_hander(const usb_dev id, u32 ep, usb_interrupt h)
{
    (void)id; (void)ep;
    stub_rx_isr = h;
    return 0;
}

void usb_enable_ep(const usb_dev id, u32 ep) { (void)id; (void)ep; }
void usb_disable_ep(const usb_dev id, u32 ep) { (void)id; (void)ep; }

u32 usb_g_bulk_read(const usb_dev id, u32 ep, u8 *ptr, u32 len, u32 block)
{
    (void)id; (void)ep; (void)block;
    u32 n = stub_rx_len < len ? stub_rx_len : len;
    if (n && stub_rx_data) {
        memcpy(ptr, stub_rx_data, n);
    }
    return n;
}

u32 usb_g_bulk_write(const usb_dev id, u32 ep, const u8 *ptr, u32 len)
{
    (void)id; (void)ep;
    if (stub_tx_max_write == 0) {
        return 0;
    }
    u32 n = len <= stub_tx_max_write ? len : stub_tx_max_write;
    ASSERT(stub_tx_len + n <= sizeof(stub_tx), "stub_tx overflow");
    memcpy(stub_tx + stub_tx_len, ptr, n);
    stub_tx_len += n;
    stub_tx_writes++;
    return n;
}

void *usb_alloc_ep_dmabuffer(const usb_dev id, u32 ep, u32 size)
{
    (void)id; (void)ep;
    ASSERT(size <= sizeof(stub_dma_pool), "dma pool too small");
    return stub_dma_pool;
}

static void on_rx_notify(void) { notify_count++; }

static void reset_tx(void)
{
    stub_tx_len = 0;
    stub_tx_writes = 0;
    stub_tx_max_write = 64;
}

/* ---- helper: decode UTF-16LE USB string descriptor to ASCII ------------- */
static void decode_usb_str(const u8 *desc, char *out, size_t cap)
{
    u32 len = desc[0];
    ASSERT(len >= 2 && (len & 1) == 0, "bad string desc length %u", len);
    ASSERT(desc[1] == USB_DT_STRING, "bad string desc type 0x%02x", desc[1]);
    size_t k = 0;
    for (u32 i = 2; i < len && k + 1 < cap; i += 2) {
        ASSERT(desc[i + 1] == 0, "non-ASCII high byte");
        out[k++] = (char)desc[i];
    }
    out[k] = '\0';
}

/* ---- 1. Descriptor & String tests --------------------------------------- */
static void test_descriptors_and_strings(void)
{
    memset(&stub_dev, 0, sizeof(stub_dev));
    memset(stub_itf, 0, sizeof(stub_itf));
    memset(stub_reset, 0, sizeof(stub_reset));
    stub_rx_isr = NULL;
    stub_ep_cfg_calls = 0;

    usb_midi_register(0);

    u8 desc[256];
    u32 cur_itf = 2;    /* test non-zero base interface number */
    u32 n = usb_midi_desc_config(0, desc, &cur_itf);
    ASSERT(cur_itf == 4, "expected cur_itf=4, got %u", cur_itf);
    ASSERT(n == 156, "expected 156 descriptor bytes for 3 cables, got %u", n);

    /* Walk TLV descriptors and verify structure */
    u32 off = 0, jacks = 0, eps = 0, cs_eps = 0;
    while (off < n) {
        u8 blen = desc[off];
        u8 btype = desc[off + 1];
        ASSERT(blen >= 2 && off + blen <= n, "bad TLV at %u (len %u)", off, blen);
        if (btype == USB_DT_INTERFACE) {
            ASSERT(blen == 9, "itf len");
        } else if (btype == 0x24 /* DT_CS_INTERFACE */) {
            u8 sub = desc[off + 2];
            if (sub == 0x02 || sub == 0x03) {
                jacks++;
            }
        } else if (btype == USB_DT_ENDPOINT) {
            ASSERT(blen == 9, "audio bulk ep must be 9 bytes");
            u16 maxp = desc[off + 4] | ((u16)desc[off + 5] << 8);
            ASSERT(maxp == 64, "expected wMaxPacketSize 64, got %u", maxp);
            eps++;
        } else if (btype == 0x25 /* DT_CS_ENDPOINT */) {
            ASSERT(blen == 4 + USB_MIDI_NUM_CABLES, "cs ep len");
            ASSERT(desc[off + 3] == USB_MIDI_NUM_CABLES, "num assoc jacks");
            cs_eps++;
        } else {
            ASSERT(0, "unexpected descriptor type 0x%02x", btype);
        }
        off += blen;
    }
    ASSERT(off == n, "descriptor walk mismatch");
    ASSERT(jacks == 4 * USB_MIDI_NUM_CABLES, "expected 12 jacks, got %u", jacks);
    ASSERT(eps == 2 && cs_eps == 2, "expected 2 EPs and 2 CS_EPs");

    /* Device descriptor patch */
    u8 dev_desc[18] = {18, 1};
    usb_midi_patch_device_descriptor(dev_desc);
    u16 vid = dev_desc[8] | ((u16)dev_desc[9] << 8);
    u16 pid = dev_desc[10] | ((u16)dev_desc[11] << 8);
    ASSERT(vid == SMCPAD_USB_VID && pid == SMCPAD_USB_PID, "vid/pid mismatch: %04x:%04x", vid, pid);

    /* Manufacturer, product, and jack strings */
    u8 sbuf[64];
    char ascii[64];
    usb_midi_get_manufacturer_str(sbuf);
    decode_usb_str(sbuf, ascii, sizeof(ascii));
    ASSERT(strcmp(ascii, SMCPAD_USB_MANUFACTURER) == 0, "mfg str: %s", ascii);

    usb_midi_get_product_str(sbuf);
    decode_usb_str(sbuf, ascii, sizeof(ascii));
    ASSERT(strcmp(ascii, SMCPAD_USB_PRODUCT) == 0, "prod str: %s", ascii);

    const char *expected_jacks[3] = {
        USB_MIDI_JACK_NAME_0, USB_MIDI_JACK_NAME_1, USB_MIDI_JACK_NAME_2
    };
    for (u32 c = 0; c < 3; c++) {
        struct usb_ctrlrequest req = { .wValue = (u16)(USB_MIDI_STR_INDEX_BASE + c) };
        stub_payload_len = 0;
        u32 handled = usb_midi_string_request(&stub_dev, &req);
        ASSERT(handled == 1 && stub_payload_len > 2, "jack string %u not handled", c);
        decode_usb_str(stub_payload, ascii, sizeof(ascii));
        ASSERT(strcmp(ascii, expected_jacks[c]) == 0, "jack %u: %s", c, ascii);
    }
    struct usb_ctrlrequest bad_req = { .wValue = USB_MIDI_STR_INDEX_BASE + 3 };
    ASSERT(usb_midi_string_request(&stub_dev, &bad_req) == 0, "out-of-range jack string should return 0");
}

/* ---- 2. Control requests & endpoint init -------------------------------- */
static void test_control_requests(void)
{
    ASSERT(stub_itf[2] != NULL && stub_itf[3] != NULL, "itf handlers not registered");
    ASSERT(stub_reset[3] != NULL, "reset handler not registered");

    /* Reset initializes endpoints */
    stub_ep_cfg_calls = 0;
    stub_reset[3](&stub_dev, 3);
    ASSERT(stub_ep_cfg_calls == 2 && stub_rx_isr != NULL, "reset should configure IN+OUT EPs");

    /* Not configured yet -> usb_midi_is_online() is 0, SET_INTERFACE stalls */
    stub_dev.bDeviceStates = USB_ADDRESS;
    ASSERT(usb_midi_is_online() == 0, "should be offline");
    struct usb_ctrlrequest req = { .bRequestType = USB_TYPE_STANDARD, .bRequest = USB_REQ_SET_INTERFACE, .wValue = 0, .wIndex = 3 };
    stub_phase = -1;
    stub_itf[3](&stub_dev, &req);
    ASSERT(stub_phase == USB_EP0_SET_STALL, "SET_INTERFACE before configured must stall");

    /* Configured -> SET_INTERFACE(0) succeeds and re-inits EPs */
    stub_dev.bDeviceStates = USB_CONFIGURED;
    ASSERT(usb_midi_is_online() == 1, "should be online");
    stub_ep_cfg_calls = 0;
    stub_itf[3](&stub_dev, &req);
    ASSERT(stub_phase == USB_EP0_STAGE_SETUP && stub_ep_cfg_calls == 2, "SET_INTERFACE(0) failed");

    /* Non-zero alt setting stalls */
    req.wValue = 1;
    stub_itf[3](&stub_dev, &req);
    ASSERT(stub_phase == USB_EP0_SET_STALL, "SET_INTERFACE(1) must stall");

    /* Class request stalls */
    struct usb_ctrlrequest creq = { .bRequestType = USB_TYPE_CLASS, .bRequest = 0x01, .wIndex = 3 };
    stub_itf[3](&stub_dev, &creq);
    ASSERT(stub_phase == USB_EP0_SET_STALL, "class request must stall");

    /* GET_INTERFACE and GET_STATUS */
    struct usb_ctrlrequest greq = { .bRequestType = USB_TYPE_STANDARD, .bRequest = USB_REQ_GET_INTERFACE, .wValue = 0, .wIndex = 3, .wLength = 1 };
    stub_payload_len = 0;
    stub_itf[3](&stub_dev, &greq);
    ASSERT(stub_payload_len == 1 && stub_payload[0] == 0x00, "GET_INTERFACE failed");

    struct usb_ctrlrequest sreq = { .bRequestType = USB_TYPE_STANDARD, .bRequest = USB_REQ_GET_STATUS, .wIndex = 3 };
    stub_payload_len = 0;
    stub_itf[3](&stub_dev, &sreq);
    ASSERT(stub_payload_len == 2 && stub_payload[0] == 0 && stub_payload[1] == 0, "GET_STATUS failed");
}

/* ---- 3. TX encoding & multi-cable sends --------------------------------- */
static void test_tx_encoding(void)
{
    stub_dev.bDeviceStates = USB_CONFIGURED;
    reset_tx();

    /* Invalid messages rejected */
    const u8 bad_data[] = { 0x40, 0x00 };
    const u8 bad_sysex[] = { 0xF0, 0x01, 0x02 };    /* missing F7 */
    ASSERT(usb_midi_send_msg(0x01, bad_data, 2) == 0, "status < 0x80 must fail");
    ASSERT(usb_midi_send_msg(0x01, bad_sysex, 3) == 0, "unterminated sysex must fail");
    ASSERT(usb_midi_send_msg(0x00, (const u8[]){0x90, 60, 127}, 3) == 0, "empty cable_mask must fail");
    const u8 note_on[] = { 0x99, 36, 100 };
    ASSERT(usb_midi_send_msg(0x01, note_on, 2) == 0, "wrong length must fail");
    ASSERT(stub_tx_len == 0, "no bytes should have been written");

    /* Stock duplicate send: cables 0 and 1 in one bulk write */
    ASSERT(usb_midi_send_msg(0x03, note_on, 3) == 3, "send_msg 0x03 failed");
    ASSERT(stub_tx_len == 8 && stub_tx_writes == 1, "expected 1 bulk write of 8 bytes");
    const u8 exp_dup[8] = { 0x09, 0x99, 36, 100, 0x19, 0x99, 36, 100 };
    ASSERT(memcmp(stub_tx, exp_dup, 8) == 0, "duplicate packet bytes mismatch");

    /* 2-byte Program Change & Channel Pressure, 1-byte F6 and F8, 2-byte F1, 3-byte F2 */
    reset_tx();
    ASSERT(usb_midi_send_msg(0x04, (const u8[]){0xC0, 12}, 2) == 2, "PC failed");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xD9, 80}, 2) == 2, "AT failed");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF6}, 1) == 1, "Tune Request failed");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF8}, 1) == 1, "Timing Clock failed");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF1, 0x23}, 2) == 2, "MTC QF failed");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF2, 0x01, 0x02}, 3) == 3, "SPP failed");
    const u8 exp_misc[24] = {
        0x2C, 0xC0, 12, 0,
        0x0D, 0xD9, 80, 0,
        0x05, 0xF6, 0,  0,
        0x0F, 0xF8, 0,  0,
        0x02, 0xF1, 0x23, 0,
        0x03, 0xF2, 0x01, 0x02,
    };
    ASSERT(stub_tx_len == 24 && memcmp(stub_tx, exp_misc, 24) == 0, "misc packet encoding mismatch");

    /* SysEx tail lengths: mod 3 == 1 (CIN 5), mod 3 == 2 (CIN 6), mod 3 == 0 (CIN 7) */
    reset_tx();
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF0, 0xF7}, 2) == 2, "2B sysex");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF0, 0x7E, 0xF7}, 3) == 3, "3B sysex");
    ASSERT(usb_midi_send_msg(0x01, (const u8[]){0xF0, 1, 2, 0xF7}, 4) == 4, "4B sysex");
    const u8 exp_sx[16] = {
        0x06, 0xF0, 0xF7, 0x00,
        0x07, 0xF0, 0x7E, 0xF7,
        0x04, 0xF0, 0x01, 0x02,
        0x05, 0xF7, 0x00, 0x00,
    };
    ASSERT(stub_tx_len == 16 && memcmp(stub_tx, exp_sx, 16) == 0, "sysex tail encoding mismatch");

    /* Large SysEx (>64 B bulk packet) splits across bulk writes */
    reset_tx();
    u8 big_sx[60];
    big_sx[0] = 0xF0;
    for (u32 i = 1; i < 59; i++) {
        big_sx[i] = (u8)(i & 0x7F);
    }
    big_sx[59] = 0xF7;     /* 60 bytes -> 20 USB-MIDI packets = 80 bytes -> 2 bulk writes (64 + 16) */
    ASSERT(usb_midi_send_msg(0x01, big_sx, sizeof(big_sx)) == sizeof(big_sx), "big sysex send");
    ASSERT(stub_tx_len == 80 && stub_tx_writes == 2, "expected 80 B in 2 writes, got %u B in %u", stub_tx_len, stub_tx_writes);

    /* Short write failure */
    reset_tx();
    stub_tx_max_write = 0;
    struct usb_midi_stats st0, st1;
    usb_midi_get_stats(&st0);
    ASSERT(usb_midi_send_msg(0x01, note_on, 3) == 0, "short write should return 0");
    usb_midi_get_stats(&st1);
    ASSERT(st1.tx_short_writes == st0.tx_short_writes + 1, "tx_short_writes not incremented");
}

/* ---- 4. RX ISR & poll reassembly ---------------------------------------- */
struct rx_event {
    u8 cable;
    u32 len;
    u8 data[USB_MIDI_SYSEX_MAX];
};
static struct rx_event rx_log[128] = {0};
static u32 rx_log_n = 0;

static void record_cb(u8 cable, const u8 *msg, u32 len)
{
    ASSERT(rx_log_n < 128, "rx_log full");
    struct rx_event *e = &rx_log[rx_log_n++];
    e->cable = cable;
    e->len = len;
    ASSERT(len <= sizeof(e->data), "msg too long");
    memcpy(e->data, msg, len);
}

static void feed_usb_out(const u8 *pkts, u32 nbytes)
{
    stub_rx_data = pkts;
    stub_rx_len = nbytes;
    stub_rx_isr(&stub_dev, USB_MIDI_EP_OUT);
}

static u16 hook_log[1024];      /* (cable << 8) | byte */
static u32 hook_n;
static const u8 clk_rt[4] = { 0x0F, 0xF8, 0x00, 0x00 };
static void hook_cb(u8 cable, u8 b)
{
    if (hook_n < 1024) {
        hook_log[hook_n] = (u16)((cable << 8) | b);
    }
    hook_n++;
}

static void test_rx_and_poll(void)
{
    usb_midi_set_rx_notify(on_rx_notify);
    notify_count = 0;
    rx_log_n = 0;

    /* Feed:
     *  - CIN 0 padding (ignored)
     *  - cable 5 out of range (dropped by poll)
     *  - SysEx on cable 2 with an interleaved 0xF8 timing clock on cable 0 in the middle
     *  - 2-byte PC on cable 1
     *  - 1-packet tiny SysEx F0 42 F7 on cable 0
     */
    const u8 batch[] = {
        0x00, 0x00, 0x00, 0x00,     /* padding */
        0x59, 0x90, 0x3C, 0x7F,     /* cable 5 (> NUM_CABLES) */
        0x24, 0xF0, 0x00, 0x32,     /* cable 2 SysEx start */
        0x0F, 0xF8, 0x00, 0x00,     /* cable 0 real-time clock interleaved! */
        0x24, 0x45, 0x00, 0x00,     /* cable 2 SysEx continue */
        0x26, 0x7F, 0xF7, 0x00,     /* cable 2 SysEx end (2 bytes) */
        0x1C, 0xC1, 0x07, 0x00,     /* cable 1 Program Change */
        0x07, 0xF0, 0x42, 0xF7,     /* cable 0 3-byte SysEx in one packet */
    };
    feed_usb_out(batch, sizeof(batch));
    ASSERT(notify_count == 1, "rx_notify should have fired once");

    usb_midi_poll(record_cb);
    ASSERT(rx_log_n == 4, "expected 4 decoded messages, got %u", rx_log_n);

    /* 1: real-time F8 on cable 0 arrived before SysEx finished */
    ASSERT(rx_log[0].cable == 0 && rx_log[0].len == 1 && rx_log[0].data[0] == 0xF8, "event 0 mismatch");
    /* 2: complete 8-byte SysEx on cable 2 */
    const u8 exp_sx2[] = { 0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x7F, 0xF7 };
    ASSERT(rx_log[1].cable == 2 && rx_log[1].len == sizeof(exp_sx2) &&
           memcmp(rx_log[1].data, exp_sx2, sizeof(exp_sx2)) == 0, "event 1 sysex mismatch");
    /* 3: Program Change on cable 1 */
    ASSERT(rx_log[2].cable == 1 && rx_log[2].len == 2 &&
           rx_log[2].data[0] == 0xC1 && rx_log[2].data[1] == 0x07, "event 2 PC mismatch");
    /* 4: 3-byte SysEx on cable 0 */
    ASSERT(rx_log[3].cable == 0 && rx_log[3].len == 3 &&
           rx_log[3].data[0] == 0xF0 && rx_log[3].data[1] == 0x42 && rx_log[3].data[2] == 0xF7,
           "event 3 tiny sysex mismatch");

    /* SysEx overflow (> USB_MIDI_SYSEX_MAX) is dropped cleanly */
    rx_log_n = 0;
    struct usb_midi_stats st0, st1;
    usb_midi_get_stats(&st0);
    const u8 sx_start[4] = { 0x04, 0xF0, 0x01, 0x02 };
    const u8 sx_cont[4]  = { 0x04, 0x03, 0x04, 0x05 };
    const u8 sx_end[4]   = { 0x05, 0xF7, 0x00, 0x00 };
    feed_usb_out(sx_start, 4);
    for (u32 i = 0; i < (USB_MIDI_SYSEX_MAX / 3) + 4; i++) {
        feed_usb_out(sx_cont, 4);
        usb_midi_poll(record_cb);
    }
    feed_usb_out(sx_end, 4);
    usb_midi_poll(record_cb);
    usb_midi_get_stats(&st1);
    ASSERT(rx_log_n == 0, "overflowed sysex must not be delivered");
    ASSERT(st1.rx_sysex_dropped == st0.rx_sysex_dropped + 1, "rx_sysex_dropped not incremented");

    /* The SysEx byte hook streams every byte, even of frames longer than USB_MIDI_SYSEX_MAX */
    hook_n = 0;
    usb_midi_set_sysex_hook(hook_cb);
    const u8 sx_start2[4] = { 0x24, 0xF0, 0x01, 0x02 };     /* cable 2 */
    const u8 sx_cont2[4]  = { 0x24, 0x03, 0x04, 0x05 };
    const u8 sx_end2[4]   = { 0x26, 0x06, 0xF7, 0x00 };
    const u32 nconts = (USB_MIDI_SYSEX_MAX / 3) + 4;
    feed_usb_out(sx_start2, 4);
    for (u32 i = 0; i < nconts; i++) {
        feed_usb_out(sx_cont2, 4);
        usb_midi_poll(record_cb);
    }
    feed_usb_out(clk_rt, 4);                                /* real-time: not a SysEx byte */
    feed_usb_out(sx_end2, 4);
    usb_midi_poll(record_cb);
    ASSERT(hook_n == 3 + 3 * nconts + 2, "hook saw %u bytes", hook_n);
    ASSERT(hook_log[0] == (0x200 | 0xF0) && hook_log[3] == (0x200 | 0x03) &&
           hook_log[hook_n - 2] == (0x200 | 0x06) && hook_log[hook_n - 1] == (0x200 | 0xF7),
           "hook byte order / cable mismatch");
    ASSERT(rx_log_n == 1 && rx_log[0].data[0] == 0xF8, "only the real-time byte is delivered");
    rx_log_n = 0;
    /* short frames still reach the poll callback too */
    hook_n = 0;
    const u8 tiny[4] = { 0x07, 0xF0, 0x42, 0xF7 };
    feed_usb_out(tiny, 4);
    usb_midi_poll(record_cb);
    ASSERT(hook_n == 3 && rx_log_n == 1 && rx_log[0].len == 3, "tiny sysex: hook + callback");
    usb_midi_set_sysex_hook(NULL);
    rx_log_n = 0;

    /* RX ring overflow when poll is not called for > USB_MIDI_RX_RING packets */
    usb_midi_get_stats(&st0);
    const u8 clk[4] = { 0x0F, 0xF8, 0x00, 0x00 };
    for (u32 i = 0; i < USB_MIDI_RX_RING + 5; i++) {
        feed_usb_out(clk, 4);
    }
    usb_midi_get_stats(&st1);
    ASSERT(st1.rx_overflow == st0.rx_overflow + 5, "expected 5 ring overflows, got %u",
           st1.rx_overflow - st0.rx_overflow);
    usb_midi_poll(record_cb);
    ASSERT(rx_log_n == USB_MIDI_RX_RING, "expected %u messages drained", USB_MIDI_RX_RING);

    usb_midi_release(0);
    ASSERT(usb_midi_is_online() == 0, "after release, device must be offline");
}

int usb_midi_run_tests(void)
{
    test_descriptors_and_strings();
    test_control_requests();
    test_tx_encoding();
    test_rx_and_poll();
    printf("test_usb_midi: ALL PASSED\n");
    fflush(stdout);
    return 0;
}

int main(void)
{
    return usb_midi_run_tests();
}
