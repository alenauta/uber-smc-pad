/* Minimal stand-ins for the JieLi SDK headers so usb_midi.c compiles and runs on a PC.
 * Only what usb_midi.c touches; behaviour is recorded in globals for test_usb_midi.c. */
#ifndef SDK_STUBS_H
#define SDK_STUBS_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t usb_dev;

#define BIT(n)          (1u << (n))
#define LOBYTE(x)       ((u8)((x) & 0xff))
#define HIBYTE(x)       ((u8)(((x) >> 8) & 0xff))
#define ASSERT(c, ...)  do { if (!(c)) { fprintf(stderr, "ASSERT: " __VA_ARGS__); fputc('\n', stderr); abort(); } } while (0)
#define log_info(...)   do { } while (0)
#define log_error(...)  do { } while (0)

#define TCFG_USB_SLAVE_MIDI_ENABLE  1
#define TCFG_USB_SLAVE_MSD_ENABLE   0
#define TCFG_USB_SLAVE_CDC_ENABLE   0
#define TCFG_USB_SLAVE_HID_ENABLE   0
#define USB_ROOT2                   0
#define USB_MAX_HW_EPNUM            5

#define USB_DIR_OUT                 0
#define USB_DIR_IN                  0x80
#define USB_DT_STRING               3
#define USB_DT_INTERFACE            4
#define USB_DT_ENDPOINT             5
#define USB_CLASS_AUDIO             1
#define USB_ENDPOINT_XFER_BULK      2
#define USB_TYPE_MASK               (0x03 << 5)
#define USB_TYPE_STANDARD           (0x00 << 5)
#define USB_TYPE_CLASS              (0x01 << 5)
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_EP0_STAGE_SETUP         0
#define USB_EP0_SET_STALL           3
enum { USB_ATTACHED, USB_POWERED, USB_DEFAULT, USB_ADDRESS, USB_CONFIGURED, USB_SUSPENDED };

struct usb_device_t { u8 bDeviceStates; };
struct usb_ctrlrequest { u8 bRequestType, bRequest; u16 wValue, wIndex, wLength; };
typedef u32(*itf_hander)(struct usb_device_t *, struct usb_ctrlrequest *);
typedef void(*itf_reset_hander)(struct usb_device_t *, u32 itf);
typedef void(*usb_interrupt)(struct usb_device_t *, u32 ep);
typedef int OS_MUTEX;

/* recorded state */
extern struct usb_device_t stub_dev;
extern itf_hander stub_itf[8];
extern itf_reset_hander stub_reset[8];
extern usb_interrupt stub_rx_isr;
extern u8 stub_setup_buf[512];
extern u8 stub_payload[512];
extern u32 stub_payload_len;
extern int stub_phase;
extern u8 stub_tx[8192];
extern u32 stub_tx_len, stub_tx_writes, stub_tx_max_write;
extern const u8 *stub_rx_data;
extern u32 stub_rx_len;
extern u32 stub_ep_cfg_calls;

usb_dev usb_device2id(const struct usb_device_t *d);
struct usb_device_t *usb_id2device(const usb_dev id);
u32 usb_set_interface_hander(const usb_dev id, u32 itf, itf_hander h);
u32 usb_set_reset_hander(const usb_dev id, u32 itf, itf_reset_hander h);
void *usb_get_setup_buffer(const struct usb_device_t *d);
u8 *usb_set_data_payload(struct usb_device_t *d, struct usb_ctrlrequest *r, const void *data, u32 len);
void usb_set_setup_phase(struct usb_device_t *d, u8 phase);
u32 usb_g_ep_config(const usb_dev id, u32 ep, u32 type, u32 ie, u8 *ptr, u32 size);
u32 usb_g_set_intr_hander(const usb_dev id, u32 ep, usb_interrupt h);
void usb_enable_ep(const usb_dev id, u32 ep);
void usb_disable_ep(const usb_dev id, u32 ep);
u32 usb_g_bulk_read(const usb_dev id, u32 ep, u8 *ptr, u32 len, u32 block);
u32 usb_g_bulk_write(const usb_dev id, u32 ep, const u8 *ptr, u32 len);
void *usb_alloc_ep_dmabuffer(const usb_dev id, u32 ep, u32 size);
static inline void os_mutex_create(OS_MUTEX *m) { *m = 0; }
static inline void os_mutex_del(OS_MUTEX *m, int f) { (void)m; (void)f; }
static inline void os_mutex_pend(OS_MUTEX *m, int t) { ASSERT(*m == 0, "mutex re-entered"); *m = 1; (void)t; }
static inline void os_mutex_post(OS_MUTEX *m) { *m = 0; }
static inline u16 sys_timer_add(void *p, void (*f)(void *), u32 ms) { (void)p; (void)f; (void)ms; return 1; }

#endif
