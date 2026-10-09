/* SMC-PAD board drivers: pad scanner (SARADC + 2 x 8-ch mux), key matrix (SPI2 +
 * 2 x 74HC595), button-LED multiplex, WS2812 pad LEDs (SPI1) and TRS MIDI out (UART1).
 *
 * Register usage mirrors the stock firmware (docs/pinmap.md):
 *   pad scan ISR 0x1D654, key init 0x1E0F1CA, SPI2 ISR 0x1D8BA, LED mux in SOFT2 0x1D97A,
 *   WS2812 init 0x1E11BA6 / refresh 0x1E1050A, UART1 init 0x1E11D2A.
 * Untested on hardware. Do NOT flash until the recovery path is bench-tested.
 */
#include "app_config.h"

#if defined(CONFIG_BOARD_SMCPAD) && TCFG_SMCPAD_HW_ENABLE

#include "system/includes.h"
#include "asm/adc_api.h"
#include "asm/clock.h"
#include "asm/gpio.h"
#include "asm/power/p33.h"
#include "smcpad_board.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[SMCPAD]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

/* ---- register bits ------------------------------------------------------------------ */
/* JL_ADC->CON: [2:0] clk div (0b110 as the SDK), 3 ?, 4 EN, 5 IE, 6 start/clr pnd,
 * 7 pending, [11:8] channel, [15:12] start delay x8 clocks (cpu/br25/adc_api.c) */
#define ADC_CON(ch)     (0x6u | BIT(3) | BIT(4) | BIT(5) | BIT(6) | \
                         ((u32)((ch) & 0xf) << 8) | ((u32)SMCPAD_ADC_START_DELAY << 12))
/* JL_SPIx->CON (cpu/br25/spi.c): 0 EN, 5 update edge fall, 13 IE, 14 clr pnd, 15 pnd */
#define SPI_CON_WS      (BIT(0) | BIT(5) | BIT(14))             /* 0x4021, = stock / led_spi.c */
#define SPI_CON_KM      (BIT(5) | BIT(13) | BIT(14))            /* 0x6020, = stock; EN later */
#define SPI_PND         BIT(15)
#define SPI_CLR_PND     BIT(14)
/* JL_UART1->CON0 (cpu/br25/uart_dev.c): 0 EN, 2 TX IE, 10/12/13 clr OT/RX/TX pnd, 15 TX pnd */
#define UT_EN           BIT(0)
#define UT_TX_IE        BIT(2)
#define UT_CLR_ALL      (BIT(13) | BIT(12) | BIT(10))
#define UT_CLR_TX       BIT(13)
#define UT_TX_PND       BIT(15)

/* pin bit masks for direct access from the IRQs */
#define B_MUX_S0        BIT(2)          /* PB2 */
#define B_MUX_S1        BIT(3)          /* PB3 */
#define C_MUX_S2        BIT(4)          /* PC4 */
#define B_KM_LATCH      BIT(4)          /* PB4 */
#define B_KM_ROW0       BIT(0)          /* PB0 */
#define C_KM_ROW1       BIT(1)          /* PC1 */
#define A_BLED_COM0     BIT(3)          /* PA3 */
#define C_BLED_COM1     BIT(2)          /* PC2 */
#define C_BLED_COM2     BIT(3)          /* PC3 */
#define A_BLED_SEGS     (BIT(5) | BIT(2) | BIT(0) | BIT(1))

static SMCPAD_TABLE u8 bled_seg_bit[SMCPAD_BLED_SEGS] = {5, 2, 0, 1};   /* PORTA bits */
static SMCPAD_TABLE u32 adc_internal_ch[3] = {AD_CH_VBAT, AD_CH_LDOREF, AD_CH_DTEMP};

static void (*hw_notify)(void);
static volatile u8 hw_notify_pending;

SMCPAD_RAM_CODE
static void notify_task(void)
{
    if (!hw_notify_pending && hw_notify) {
        hw_notify_pending = 1;
        hw_notify();
    }
}

/* ======================================================================================
 * Pads: SARADC, 16 mux positions + one internal channel every SMCPAD_ADC_INTERNAL_EVERY
 * ==================================================================================== */
enum { PADS_CAL = 0, PADS_CAL_DONE, PADS_RUN };
static struct {
    u8  cur;                    /* position in flight: 0..15 mux, 16 = internal */
    u8  phase;                  /* PADS_* */
    u8  internal_idx;
    u8  frame_mod;
    s8  polarity;
    volatile u8 internal_fresh;
    u16 cal_frames;
    volatile u32 frames;
    u32 idle_sum[SMCPAD_NUM_PADS];
    volatile u16 raw[SMCPAD_NUM_PADS];
    u16 internal_val[3];
} pads;
static struct smcpad_pad pad_state[SMCPAD_NUM_PADS];
static struct smcpad_pad_cfg pad_cfg;
static struct smcpad_ev_ring pad_ring;

/* adc_pmu_detect_en() is library code in flash (0x1E13F90 in our build). This is the same
 * sequence, from RAM; P33_CON_SET itself is mask ROM (0x117D60). */
SMCPAD_RAM_CODE
static void pmu_mux_on(u32 ch)
{
    P33_CON_SET(P3_ANA_CON2, 1, 1, 1);
    P33_CON_SET(P3_ANA_CON4, 0, 1, 1);
    P33_CON_SET(P3_ANA_CON4, 1, 3, (u8)(ch >> 20));
}

/* adc_ch_mux_select() turns the PMU mux off before every non-PMU conversion */
SMCPAD_RAM_CODE
static void pmu_mux_off(void)
{
    P33_CON_SET(P3_ANA_CON4, 0, 1, 0);
}

SMCPAD_RAM_CODE
static void adc_start(u8 pos)
{
    u32 ch;
    if (pos < SMCPAD_NUM_PADS) {
        u8 m = pos & 7;
        JL_PORTB->OUT = (JL_PORTB->OUT & ~(B_MUX_S0 | B_MUX_S1)) | ((u32)(m & 3) << 2);
        if (m & 4) {
            JL_PORTC->OUT |= C_MUX_S2;
        } else {
            JL_PORTC->OUT &= ~C_MUX_S2;
        }
        ch = pos < 8 ? AD_CH_PA4 : AD_CH_PA6;
    } else {
        ch = adc_internal_ch[pads.internal_idx];
        pmu_mux_on(ch);                             /* as adc_sample() does for AD_OF_PMU */
    }
    JL_ADC->CON = ADC_CON(ch);                      /* also clears pending + starts */
}

___interrupt
SMCPAD_RAM_CODE
static void smcpad_adc_isr(void)
{
    u16 res = (u16)JL_ADC->RES;
    u8 pos = pads.cur;
    u8 next;

    if (pos < SMCPAD_NUM_PADS) {
        u8 pad = smcpad_pad_of_mux[pos];
        pads.raw[pad] = res;
        if (pads.phase == PADS_RUN) {
            struct smcpad_pad_ev e;
            e.type = smcpad_pad_sample(&pad_state[pad], &pad_cfg, pads.polarity, res, &e.velocity);
            if (e.type != SMCPAD_EV_NONE) {
                e.pad = pad;
                smcpad_ring_push(&pad_ring, &e);
                notify_task();
            }
        } else if (pads.phase == PADS_CAL && pads.cal_frames >= 2) {
            pads.idle_sum[pad] += res;              /* first 2 frames may hold SDK leftovers */
        }
        next = (u8)(pos + 1);
        if (next == SMCPAD_NUM_PADS) {              /* frame complete */
            pads.frames++;
            if (pads.phase == PADS_CAL && ++pads.cal_frames >= SMCPAD_PAD_CAL_FRAMES + 2) {
                pads.phase = PADS_CAL_DONE;
                notify_task();
            }
            next = 0;
            if (++pads.frame_mod >= SMCPAD_ADC_INTERNAL_EVERY) {
                pads.frame_mod = 0;
                next = SMCPAD_NUM_PADS;
            }
        }
    } else {
        u8 i = pads.internal_idx;
        pads.internal_val[i] = res;
        pads.internal_fresh |= (u8)BIT(i);
        pmu_mux_off();                              /* back to the pads (GPIO channels) */
        pads.internal_idx = (u8)(i + 1 >= 3 ? 0 : i + 1);
        next = 0;
    }
    pads.cur = next;
    adc_start(next);
}

static void pads_init(void)
{
    memset(&pads, 0, sizeof(pads));
    memset(&pad_ring, 0, sizeof(pad_ring));
    pad_cfg = smcpad_pad_cfg_default;

    /* stock 0x1E0F31A: mux selects output low, ADC inputs analog */
    gpio_direction_output(SMCPAD_MUX_S0_PIN, 0);
    gpio_direction_output(SMCPAD_MUX_S1_PIN, 0);
    gpio_direction_output(SMCPAD_MUX_S2_PIN, 0);
    u8 ain[2] = {SMCPAD_PAD_ADC_A_PIN, SMCPAD_PAD_ADC_B_PIN};
    for (int i = 0; i < 2; i++) {
        gpio_set_pull_up(ain[i], 0);
        gpio_set_pull_down(ain[i], 0);
        gpio_set_direction(ain[i], 1);
        gpio_set_die(ain[i], 0);
    }

    /* The SDK's adc_scan() (2 ms) skips while CON.EN is set, which is always the case
     * from here on; VBAT/VBG/DTEMP are fed back with smcpad_hw_sync_sdk_adc(). */
    JL_ADC->CON = BIT(6);
    JL_ADC->CON = 0;
    request_irq(IRQ_SARADC_IDX, 5, smcpad_adc_isr, 0);
    pads.cur = 0;
    pmu_mux_off();                  /* the SDK's adc_isr leaves the PMU mux on WVDD */
    adc_start(0);
}

u8 smcpad_hw_pad_calibrated(void)
{
    return pads.phase != PADS_CAL;
}

void smcpad_hw_pad_begin(void)
{
    u16 idle[SMCPAD_NUM_PADS];
    if (pads.phase != PADS_CAL_DONE) {
        return;
    }
    for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
        idle[i] = (u16)(pads.idle_sum[i] / SMCPAD_PAD_CAL_FRAMES);
    }
    pads.polarity = smcpad_pad_polarity(idle, SMCPAD_NUM_PADS);
    for (u8 i = 0; i < SMCPAD_NUM_PADS; i++) {
        smcpad_pad_init(&pad_state[i], idle[i]);
    }
    log_info("pads: polarity %d, idle %d %d %d %d ...", pads.polarity, idle[0], idle[1], idle[2], idle[3]);
    pads.phase = PADS_RUN;                          /* the ISR only reads pad_state after this */
}

u8 smcpad_hw_pad_pop(struct smcpad_pad_ev *e)
{
    return smcpad_ring_pop(&pad_ring, e);
}

u16 smcpad_hw_pad_pressure(u8 pad)
{
    return pad < SMCPAD_NUM_PADS ? pad_state[pad].pressure : 0;
}

u16 smcpad_hw_pad_raw(u8 pad)
{
    return pad < SMCPAD_NUM_PADS ? pads.raw[pad] : 0;
}

u16 smcpad_hw_pad_baseline(u8 pad)
{
    return pad < SMCPAD_NUM_PADS ? (u16)(pad_state[pad].baseline_q4 >> 4) : 0;
}

/* diagnostics / live tuning: read and optionally replace the pad detector settings */
void smcpad_hw_pad_cfg(struct smcpad_pad_cfg *get, const struct smcpad_pad_cfg *set)
{
    if (set) {
        local_irq_disable();
        pad_cfg = *set;
        local_irq_enable();
    }
    if (get) {
        *get = pad_cfg;
    }
}

/* sdk_overlay patch in cpu/br25/adc_api.c */
extern void adc_ext_set_value(u32 ch, u32 value);

void smcpad_hw_sync_sdk_adc(void)
{
    u8 fresh;
    local_irq_disable();
    fresh = pads.internal_fresh;
    pads.internal_fresh = 0;
    local_irq_enable();
    for (u8 i = 0; i < 3; i++) {
        if (fresh & BIT(i)) {
            adc_ext_set_value(adc_internal_ch[i], pads.internal_val[i]);
        }
    }
}

/* ======================================================================================
 * Key matrix (SPI2 -> 2 x 595, latch PB4, rows PB0/PC1) + button-LED multiplex
 * ==================================================================================== */
static struct {
    u8  step;                   /* slot written by the next IRQ */
    u8  wr;                     /* frame buffer being filled */
    u8  bled_phase;
    u8  frame[2][SMCPAD_KM_STEPS];
    u16 word;                   /* SPI2 DMA source, 2 bytes */
    volatile u8 bled_seg_on[SMCPAD_BLED_PHASES];
    volatile u32 frames;
    u16 last_buttons;
    struct smcpad_debounce db;
    struct smcpad_knob knobs[SMCPAD_NUM_KNOBS];
} km __attribute__((aligned(4)));

SMCPAD_RAM_CODE
static void bled_step(void)
{
    u8 p = km.bled_phase;
    u8 on = km.bled_seg_on[p];
    u32 clr = 0;

    JL_PORTA->OUT |= A_BLED_SEGS;                       /* blank */
    JL_PORTA->OUT |= A_BLED_COM0;
    JL_PORTC->OUT |= C_BLED_COM1 | C_BLED_COM2;
    if (p == 0) {
        JL_PORTA->OUT &= ~A_BLED_COM0;
    } else if (p == 1) {
        JL_PORTC->OUT &= ~C_BLED_COM1;
    } else {
        JL_PORTC->OUT &= ~C_BLED_COM2;
    }
    for (u8 s = 0; s < SMCPAD_BLED_SEGS; s++) {
        if (on & BIT(s)) {
            clr |= BIT(bled_seg_bit[s]);
        }
    }
    JL_PORTA->OUT &= ~clr;                              /* lit segments low */
    km.bled_phase = (u8)(p + 1 >= SMCPAD_BLED_PHASES ? 0 : p + 1);
}

SMCPAD_RAM_CODE
static void km_frame_done(const u8 *frame)
{
    u8 moved = smcpad_knobs_frame(km.knobs, frame);
    u16 changed = smcpad_debounce_step(&km.db, smcpad_km_buttons_raw(frame),
                                       SMCPAD_BTN_DEBOUNCE_FRAMES);
    km.frames++;
    if (moved || changed) {
        notify_task();
    }
}

___interrupt
SMCPAD_RAM_CODE
static void smcpad_spi2_isr(void)
{
    /* sample the rows for the column latched one step ago (stock 0x1D8BA) */
    u8 v = (u8)((JL_PORTB->IN & B_KM_ROW0) | (JL_PORTC->IN & C_KM_ROW1));
    u8 *f = km.frame[km.wr];
    f[km.step] = v;
    JL_PORTB->OUT |= B_KM_LATCH;                        /* 595 RCLK rising edge */
    u8 next = (u8)((km.step + 1) & (SMCPAD_KM_STEPS - 1));
    km.step = next;
    km.word = smcpad_km_column_word[next];
    JL_SPI2->CON |= SPI_CLR_PND;
    JL_SPI2->ADR = (u32)&km.word;
    JL_SPI2->CNT = 2;
    JL_PORTB->OUT &= ~B_KM_LATCH;
    if ((next & (SMCPAD_BLED_EVERY - 1)) == 0) {
        bled_step();                                /* button-LED phase: 4x per frame */
    }
    if (next == 0) {
        km.wr ^= 1;
        km_frame_done(f);
    }
}

static u32 spi_baud(u32 hz)
{
    u32 lsb = (u32)clk_get("lsb");
    u32 div = (lsb + hz / 2) / hz;
    return div ? div - 1 : 0;
}

static void km_init(void)
{
    memset(&km, 0, sizeof(km));
    for (u8 k = 0; k < SMCPAD_NUM_KNOBS; k++) {
        smcpad_knob_init(&km.knobs[k], 0);
    }

    /* stock 0x1E0F1CA: LED mux lines output low, rows input WITH pull-up (helper 0x1E008C6 writes
     * PORTx->PU = 1 for PB0/PC1). Without it a closed key keeps the row low for every column
     * (first hardware test: all buttons fired together, knobs dead). */
    static const u8 bled_pins[7] = {
        SMCPAD_BLED_SEG0_PIN, SMCPAD_BLED_SEG1_PIN, SMCPAD_BLED_SEG2_PIN, SMCPAD_BLED_SEG3_PIN,
        SMCPAD_BLED_COM0_PIN, SMCPAD_BLED_COM1_PIN, SMCPAD_BLED_COM2_PIN,
    };
    for (u8 i = 0; i < 7; i++) {
        gpio_set_die(bled_pins[i], 1);
        gpio_direction_output(bled_pins[i], 1);         /* stock: low; high = all off */
        gpio_set_pull_up(bled_pins[i], 0);
        gpio_set_pull_down(bled_pins[i], 0);
    }
    static const u8 rows[2] = {SMCPAD_KM_ROW0_PIN, SMCPAD_KM_ROW1_PIN};
    for (u8 i = 0; i < 2; i++) {
        gpio_set_die(rows[i], 1);
        gpio_set_direction(rows[i], 1);
        gpio_set_pull_up(rows[i], 1);
        gpio_set_pull_down(rows[i], 0);
    }
    gpio_direction_output(SMCPAD_KM_LATCH_PIN, 0);
    static const u8 spi_pins[2] = {SMCPAD_KM_CLK_PIN, SMCPAD_KM_DAT_PIN};
    for (u8 i = 0; i < 2; i++) {
        gpio_set_die(spi_pins[i], 1);
        gpio_set_pull_up(spi_pins[i], 0);
        gpio_set_pull_down(spi_pins[i], 0);
        gpio_set_direction(spi_pins[i], 0);
    }

    JL_IOMAP->CON1 &= ~BIT(16);                         /* SPI2 group A: PB6 CLK, PB7 DO */
    request_irq(IRQ_SPI2_IDX, 5, smcpad_spi2_isr, 0);
    JL_SPI2->CON = SPI_CON_KM;
    JL_SPI2->BAUD = spi_baud(SMCPAD_KM_SPI_HZ);
    km.word = smcpad_km_column_word[0];
    JL_SPI2->ADR = (u32)&km.word;
    JL_SPI2->CNT = 2;
    JL_SPI2->CON |= BIT(0);

    /* stock drives PB5 low at the end of the key init */
    gpio_direction_output(SMCPAD_PERIPH_EN_PIN, 0);
}

s32 smcpad_hw_knob_count(u8 knob)
{
    return knob < SMCPAD_NUM_KNOBS ? km.knobs[knob].count : 0;
}

u16 smcpad_hw_buttons(void)
{
    return km.db.state;
}

/* diagnostics: the last complete key-matrix frame, byte[step] = PB0 | PC1 << 1 (active low) */
void smcpad_hw_km_frame(u8 *out)
{
    local_irq_disable();
    memcpy(out, km.frame[km.wr ^ 1], SMCPAD_KM_STEPS);
    local_irq_enable();
}

void smcpad_hw_button_leds(u16 on_mask)
{
    u8 seg[SMCPAD_BLED_PHASES];
    smcpad_bled_phases(on_mask, seg);
    for (u8 p = 0; p < SMCPAD_BLED_PHASES; p++) {
        km.bled_seg_on[p] = seg[p];
    }
}

/* ======================================================================================
 * WS2812 pad LEDs on SPI1 group A (PB1)
 * ==================================================================================== */
static u8 ws_buf[SMCPAD_WS_BUF_LEN] __attribute__((aligned(4)));
static u32 ws_rgb[SMCPAD_NUM_RGB];
static u8 ws_brightness = 64;
static u8 ws_busy;

static void ws_init(void)
{
    JL_IOMAP->CON1 &= ~BIT(4);                          /* SPI1 group A: DO = PB1 */
    gpio_set_die(SMCPAD_RGB_PIN, 1);
    gpio_set_pull_up(SMCPAD_RGB_PIN, 0);
    gpio_set_pull_down(SMCPAD_RGB_PIN, 0);
    gpio_direction_output(SMCPAD_RGB_PIN, 0);
    JL_SPI1->CON = SPI_CON_WS;
    JL_SPI1->BAUD = spi_baud(SMCPAD_WS_SPI_HZ);
    memset(ws_rgb, 0, sizeof(ws_rgb));
    ws_busy = 0;
    smcpad_hw_rgb_show();                               /* all off */
}

void smcpad_hw_rgb_set(u8 led, u32 rgb)
{
    if (led < SMCPAD_NUM_RGB) {
        ws_rgb[led] = rgb & 0xffffff;
    }
}

void smcpad_hw_rgb_brightness(u8 b)
{
    ws_brightness = b;
}

u8 smcpad_hw_rgb_show(void)
{
    if (ws_busy && !(JL_SPI1->CON & SPI_PND)) {
        return 0;
    }
    smcpad_ws_encode(ws_buf, ws_rgb, ws_brightness);
    JL_SPI1->CON |= SPI_CLR_PND;
    JL_SPI1->ADR = (u32)ws_buf;
    JL_SPI1->CNT = SMCPAD_WS_BUF_LEN;
    ws_busy = 1;
    return 1;
}

/* ======================================================================================
 * TRS MIDI out: UART1 TX DMA through output channel 1 on PC5
 * ==================================================================================== */
#define TRS_RING 256
static struct {
    u8 buf[TRS_RING];
    volatile u8 head;           /* task */
    volatile u8 tail;           /* IRQ */
    volatile u8 dma_len;
    volatile u16 drops;
} trs __attribute__((aligned(4)));

SMCPAD_RAM_CODE
static void trs_kick(void)
{
    u8 t = trs.tail, h = trs.head;
    if (h == t) {
        trs.dma_len = 0;
        return;
    }
    u8 n = (u8)(h > t ? h - t : TRS_RING - t);     /* contiguous part */
    trs.dma_len = n;
    JL_UART1->CON0 |= UT_CLR_TX;
    JL_UART1->TXADR = (u32)&trs.buf[t];
    JL_UART1->TXCNT = n;
}

___interrupt
SMCPAD_RAM_CODE
static void smcpad_uart1_isr(void)
{
    if (JL_UART1->CON0 & UT_TX_PND) {
        JL_UART1->CON0 |= UT_CLR_TX;
        trs.tail = (u8)(trs.tail + trs.dma_len);
        trs_kick();
    }
    JL_UART1->CON0 |= BIT(12) | BIT(10);            /* RX/OT pendings: unused */
}

static void trs_set_baud(void)
{
    u32 clk = (u32)clk_get("uart");
    JL_UART1->BAUD = ((clk + SMCPAD_TRS_BAUD / 2) / SMCPAD_TRS_BAUD) / 4 - 1;
}

static void trs_init(void)
{
    memset(&trs, 0, sizeof(trs));
    gpio_set_uart1(-1);                             /* no fixed UART1 pins */
    JL_UART1->CON0 = UT_CLR_ALL;
    trs_set_baud();
    gpio_output_channle(SMCPAD_TRS_TX_PIN, CH1_UT1_TX);
    request_irq(IRQ_UART1_IDX, 1, smcpad_uart1_isr, 0);
    JL_UART1->CON0 |= UT_TX_IE | UT_EN;
}

u32 smcpad_hw_trs_write(const u8 *d, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        u8 h = trs.head;
        if ((u8)(h + 1) == trs.tail) {
            trs.drops++;
            break;
        }
        trs.buf[h] = d[i];
        trs.head = (u8)(h + 1);
    }
    local_irq_disable();
    if (trs.dma_len == 0) {
        trs_kick();
    }
    local_irq_enable();
    return i;
}

/* ======================================================================================
 * clocks, init, status
 * ==================================================================================== */
static u8 hw_up;

static void smcpad_clock_enter(void)
{
}

static void smcpad_clock_exit(void)
{
    if (hw_up) {                                    /* lsb / uart clocks may have changed */
        JL_SPI1->BAUD = spi_baud(SMCPAD_WS_SPI_HZ);
        JL_SPI2->BAUD = spi_baud(SMCPAD_KM_SPI_HZ);
        trs_set_baud();
    }
}
CLOCK_CRITICAL_HANDLE_REG(smcpad, smcpad_clock_enter, smcpad_clock_exit)

void smcpad_hw_init(void (*notify)(void))
{
    hw_notify = notify;
    hw_notify_pending = 0;
    trs_init();         /* stock order: UART1, then LEDs (which call the key init) */
    ws_init();
    km_init();
    pads_init();
    hw_up = 1;
    log_info("hw up: lsb %d Hz, uart %d Hz", clk_get("lsb"), clk_get("uart"));
}

void smcpad_hw_notified(void)
{
    hw_notify_pending = 0;
}

void smcpad_hw_status(struct smcpad_hw_status *s)
{
    s->adc_frames = pads.frames;
    s->km_frames = km.frames;
    s->pad_drops = pad_ring.dropped;
    s->trs_drops = trs.drops;
    s->polarity = pads.polarity;
    s->pads_running = pads.phase == PADS_RUN;
}

#endif /* CONFIG_BOARD_SMCPAD && TCFG_SMCPAD_HW_ENABLE */
