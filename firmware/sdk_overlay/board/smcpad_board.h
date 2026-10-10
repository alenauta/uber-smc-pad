/* SMC-PAD board: pins and driver API (SDK side).
 *
 * Pins come from the stock firmware, see docs/pinmap.md. Nothing here has run on real
 * hardware yet: do NOT flash builds with this until the recovery path is bench-tested.
 */
#ifndef SMCPAD_BOARD_H
#define SMCPAD_BOARD_H

#include "smcpad_logic.h"

/* ---- pins (IO_PORTx_yy numbers = 16 * port + pin) ------------------------------- */
#define SMCPAD_PAD_ADC_A_PIN    IO_PORTA_04     /* mux A out, AD_CH_PA4 */
#define SMCPAD_PAD_ADC_B_PIN    IO_PORTA_06     /* mux B out, AD_CH_PA6 */
#define SMCPAD_MUX_S0_PIN       IO_PORTB_02
#define SMCPAD_MUX_S1_PIN       IO_PORTB_03
#define SMCPAD_MUX_S2_PIN       IO_PORTC_04
#define SMCPAD_KM_CLK_PIN       IO_PORTB_06     /* SPI2 CLK, group A */
#define SMCPAD_KM_DAT_PIN       IO_PORTB_07     /* SPI2 DO, group A */
#define SMCPAD_KM_LATCH_PIN     IO_PORTB_04
#define SMCPAD_KM_ROW0_PIN      IO_PORTB_00     /* encoders */
#define SMCPAD_KM_ROW1_PIN      IO_PORTC_01     /* buttons */
#define SMCPAD_RGB_PIN          IO_PORTB_01     /* SPI1 DO, group A, WS2812 chain */
#define SMCPAD_BLED_COM0_PIN    IO_PORTA_03
#define SMCPAD_BLED_COM1_PIN    IO_PORTC_02
#define SMCPAD_BLED_COM2_PIN    IO_PORTC_03
#define SMCPAD_BLED_SEG0_PIN    IO_PORTA_05
#define SMCPAD_BLED_SEG1_PIN    IO_PORTA_02
#define SMCPAD_BLED_SEG2_PIN    IO_PORTA_00
#define SMCPAD_BLED_SEG3_PIN    IO_PORTA_01
#define SMCPAD_PERIPH_EN_PIN    IO_PORTB_05     /* driven low at init, like stock */
#define SMCPAD_TRS_TX_PIN       IO_PORTC_05     /* UART1 TX via output channel */

/* ---- tuning ----------------------------------------------------------------------- */
#ifndef SMCPAD_ADC_START_DELAY
#define SMCPAD_ADC_START_DELAY  1       /* x8 ADC clocks of mux settling (stock: 1) */
#endif
#ifndef SMCPAD_ADC_INTERNAL_EVERY
#define SMCPAD_ADC_INTERNAL_EVERY 8     /* one VBAT/VBG/DTEMP conversion every N pad frames */
#endif
#ifndef SMCPAD_PAD_CAL_FRAMES
#define SMCPAD_PAD_CAL_FRAMES   64      /* idle frames averaged at start-up */
#endif
#ifndef SMCPAD_KM_SPI_HZ
#define SMCPAD_KM_SPI_HZ        1000000 /* stock: 4 MHz (lsb/12); 1 MHz = ~3 kHz frames */
#endif
#ifndef SMCPAD_BTN_DEBOUNCE_FRAMES
#define SMCPAD_BTN_DEBOUNCE_FRAMES 48   /* ~23 ms: frames run at ~2.1 kHz (measured); stock 3 x 20 ms */
#endif
#ifndef SMCPAD_BLED_EVERY
#define SMCPAD_BLED_EVERY       4       /* button-LED phase every 4 key-matrix steps (power of 2) */
#endif
#ifndef SMCPAD_WS_SPI_HZ
#define SMCPAD_WS_SPI_HZ        6000000 /* 8 SPI bits per LED bit = 750 kHz (stock) */
#endif
#ifndef SMCPAD_TRS_BAUD
#define SMCPAD_TRS_BAUD         31250
#endif

/* ---- driver API (smcpad_hw.c) ----------------------------------------------------- */
struct smcpad_hw_status {
    u32 adc_frames;
    u32 km_frames;
    u16 pad_drops;
    u16 trs_drops;
    s8  polarity;
    u8  pads_running;
};

/* notify() is called from IRQ context when pad events / buttons / knobs changed */
void smcpad_hw_init(void (*notify)(void));
void smcpad_hw_notified(void);          /* task: re-arm notify() before draining events */
u8   smcpad_hw_pad_calibrated(void);    /* idle sampling finished */
void smcpad_hw_pad_begin(void);         /* baselines + polarity from idle, start detection */
u8   smcpad_hw_pad_pop(struct smcpad_pad_ev *e);
u16  smcpad_hw_pad_pressure(u8 pad);
u16  smcpad_hw_pad_raw(u8 pad);
u16  smcpad_hw_pad_baseline(u8 pad);
void smcpad_hw_pad_cfg(struct smcpad_pad_cfg *get, const struct smcpad_pad_cfg *set);  /* live tuning */
s32  smcpad_hw_knob_count(u8 knob);
u16  smcpad_hw_buttons(void);           /* debounced, bit b = button b pressed */
void smcpad_hw_km_frame(u8 *out);       /* diagnostics: last raw key-matrix frame (16 B) */
void smcpad_hw_button_leds(u16 on_mask);
void smcpad_hw_rgb_set(u8 led, u32 rgb);
void smcpad_hw_rgb_brightness(u8 b);
u8   smcpad_hw_rgb_show(void);          /* 0 if the previous frame is still sending */
u32  smcpad_hw_trs_write(const u8 *d, u32 n);
void smcpad_hw_sync_sdk_adc(void);      /* VBAT / VBG / DTEMP -> SDK adc_api table */
void smcpad_hw_status(struct smcpad_hw_status *s);
u32  smcpad_hw_ms(void);                /* 1 ms counter (TIMER2) */
void smcpad_hw_fast_wake(u8 on);        /* 1: notify the task every ms (sequencer running) */

/* Flash layout since 907 (repack_oldchain.sh defaults): code 0x00000-0x2FFFF, sequencer
 * patterns 0x30000-0x4FFFF (outside every JLFS area, untouched by updates), VM 0x50000-0x74FFF,
 * BTIF 0x75000, EXIF 0x76000, presets (USRFLASH) 0x77000, USRTRIM 0x7E000, key_mac 0x7F000. */
#define SMCPAD_PATTERN_FLASH    0x30000
#define SMCPAD_PATTERN_FLASH_LEN 0x20000

/* ---- application (smcpad_app.c) --------------------------------------------------- */
void smcpad_start(void);                /* call once from the app, after usb_start() */

#endif
