# SMC-PAD Hardware Pin & Peripheral Map (JieLi AC636N / BR25)

Hardware peripheral routing for the **M-VAVE SMC-PAD** on the **JieLi AC636N (`BR25`)** MCU, verified on real hardware across custom firmware builds `901`–`925`.

SFR names follow JieLi's `br25.h` in `fw-AC63_BT_SDK`. For general BR25 alternate-function crossbars, see [kagaimiq/jielie `br25/iomap`](https://github.com/kagaimiq/jielie/blob/main/chips/br25/iomap.md).

## 1. GPIO & Peripheral Summary

| Pin | Dir | Function | BR25 Peripheral / Notes |
|---|---|---|---|
| **PA4** | analog | Pad ADC input A (`AD_CH_PA4` = ch 3), output of **8:1 analog mux A** | `SARADC` channel 3 (no digital input / no internal pull) |
| **PA6** | analog | Pad ADC input B (`AD_CH_PA6` = ch 4), output of **8:1 analog mux B** | `SARADC` channel 4 (no digital input / no internal pull) |
| **PB2** | out | Pad analog mux select **S0** (`idx[0]`) | `PORTB.OUT[2]` |
| **PB3** | out | Pad analog mux select **S1** (`idx[1]`) | `PORTB.OUT[3]` |
| **PC4** | out | Pad analog mux select **S2** (`idx[2]`) | `PORTC.OUT[4]` |
| **PB6** | out | Key-matrix shift-register **clock** (`74HC595` SRCLK) | `SPI2` CLK, group A (`IOMAP_CON1.16 = 0`) |
| **PB7** | out | Key-matrix shift-register **serial data** (`74HC595` SER) | `SPI2` DO, group A |
| **PB4** | out | Key-matrix shift-register **latch** (`74HC595` RCLK) | Pulsed once per `SPI2` column transfer |
| **PB0** | in (pull-up) | Key-matrix return **row 0**: 8 endless encoders (A/B quadrature phases) | `PORTB.IN[0]`, internal pull-up enabled (active low) |
| **PC1** | in (pull-up) | Key-matrix return **row 1**: 10 tactile panel buttons | `PORTC.IN[1]`, internal pull-up enabled (active low) |
| **PB1** | out | **Pad RGB LED chain** (16 × WS2812B-compatible LEDs) | `SPI1` DO, group A (~6 MHz, 1 SPI byte per WS2812 bit) |
| **PA3** | out | Button-LED 3×4 matrix **common 0** (active low) | Multiplexed in scan ISR |
| **PC2** | out | Button-LED 3×4 matrix **common 1** (active low) | Multiplexed in scan ISR |
| **PC3** | out | Button-LED 3×4 matrix **common 2** (active low) | Multiplexed in scan ISR |
| **PA5** | out | Button-LED 3×4 matrix **segment 0** (active low) | Multiplexed in scan ISR |
| **PA2** | out | Button-LED 3×4 matrix **segment 1** (active low) | Multiplexed in scan ISR |
| **PA0** | out | Button-LED 3×4 matrix **segment 2** (active low) | Multiplexed in scan ISR |
| **PA1** | out | Button-LED 3×4 matrix **segment 3** (active low) | Multiplexed in scan ISR |
| **PB5** | out | Active-low analog/LED rail enable (driven **low** when active, **high** in sleep) | Also `UTRX=PB05` in bootloader `isd_config.ini` |
| **PC5** | out | **3.5 mm TRS MIDI OUT** (31,250 Bd TX) | `UART1` TX routed via output channel 1 (`IOMAP_CON3[23:20] = 1`) |
| **DP / DM** | I/O | Full-speed USB 2.0 device (USB-MIDI 1.0) | `USB_CTRL` |
| **PD0–PD4** | — | Internal 512 KB SPI NOR flash (`PD4` = flash power) | Managed by SDK / XIP cache |

> [!NOTE]
> **Battery & Charging:** There is no external battery sense GPIO. `VBAT` is sampled internally via `AD_CH_MUX` (`AD_CH_VBAT = VBAT/4` referenced against internal `AD_CH_PMU_VBG`), and Li-Po charging uses the BR25's integrated PMU (`P33` registers).

## 2. Subsystem Details

### 2.1 Velocity & Pressure Pads (2 × 8-Channel Analog Mux → SARADC)
- `PB2`/`PB3`/`PC4` select mux channel `idx & 7` (`0..7`), while `idx >> 3` selects `PA4` (`0..7`, Mux A) vs. `PA6` (`8..15`, Mux B).
- Because of the single-step conversion pipeline, conversion step `n` reads the sample settled during step `n - 1`, followed by a 17th internal PMU (`VBAT` / `VBG`) conversion every scan cycle.
- **Mux channel → logical pad index (`0..15`, bottom-left `PAD 1` to top-right `PAD 16`):**

  | Mux channel | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|---|
  | **Mux A (`PA4`)** | 2 | 0 | 1 | 3 | 15 | 14 | 12 | 13 |
  | **Mux B (`PA6`)** | 6 | 4 | 5 | 7 | 8 | 9 | 11 | 10 |

- Per-pad factory calibration (`USRTRIM` at `0x7E000`, 16 × 10 B followed by `"trimdat\0"`) stores five 16-bit LE threshold/breakpoint words per pad (defaults `50, 550, 720, 850, 950`).

### 2.2 Encoders & Buttons (16 Columns via 2 × `74HC595`, 2 Return Lines)
- `SPI2` shifts 16-bit walking-zero column words (`steps 0..7 = ~(1 << 8..15)`, `steps 8..15 = ~(1 << 0..7)`), pulsing `PB4` (`RCLK`) each step and sampling active-low return lines `PB0` (encoders) and `PC1` (buttons) with internal pull-ups enabled.
- **Endless encoders (`K1–K8`, row 0 = `PB0`):**

  | Encoder | K1 (0) | K2 (1) | K3 (2) | K4 (3) | K5 (4) | K6 (5) | K7 (6) | K8 (7) |
  |---|---|---|---|---|---|---|---|---|
  | `(A, B)` scan steps | `15, 0` | `11, 12` | `1, 2` | `9, 10` | `7, 8` | `13, 14` | `5, 6` | `3, 4` |

- **Buttons (`0..9`, row 1 = `PC1`):**
  - Scan steps for logical buttons `0..9`: **`3, 4, 5, 6, 7, 8, 10, 9, 1, 2`** (steps `0` and `11..15` on row 1 are unused).
  - Logical button assignment: `0` = BT, `1` = PAD BANK, `2` = KNOB BANK, `3` = `<` (Left), `4` = `>` (Right), `5` = Play, `6` = Stop, `7` = Record, `8` = Shift, `9` = Note Repeat.

### 2.3 Pad RGB LEDs (`WS2812` Chain on `SPI1` Group A, `PB1`)
- `SPI1` runs at `lsb/8` (~6 MHz) on `PB1` with a 548-byte DMA buffer: 24 GRB bits per pad encoded as `0xFC` (`1`) and `0xC0` (`0`), followed by zero bytes for the `>50 µs` reset latch.
- **Logical pad (`0..15`, bottom-left to top-right) → physical WS2812 chain index:**
  `11, 4, 2, 7, 10, 14, 6, 9, 0, 5, 15, 1, 13, 12, 8, 3`.

### 2.4 Button Monochrome LEDs (3 × 4 Active-Low Matrix)
- Three active-low commons (`phase 0..2` = `PA3`, `PC2`, `PC3`) × four active-low segments (`bit 0..3` = `PA5`, `PA2`, `PA0`, `PA1`).
- **LED index (`phase << 4 | segment`):** `0x10, 0x03, 0x12, 0x01, 0x11, 0x22, 0x21, 0x20, 0x02, 0x00`.
- Note on button-to-LED numbering (verified on hardware in Build `903`): LED `0..4` = Left, Right, Play, Stop, Record (hardware buttons `3..7`), LED `5` = BT (button `0`), LED `6` = PAD BANK (button `1`), LED `7` = KNOB BANK (button `2`), LED `8` = Shift (button `8`), LED `9` = Note Repeat (button `9`).

### 2.5 3.5 mm TRS MIDI Out (`UART1` TX on `PC5`)
- Standard 31,250 Bd 8N1 UART TX routed to `PC5` via BR25 output channel 1 (`CHx_UT1_TX`). No MIDI IN hardware is present on the board.

## 3. SDK Board Configuration Notes (`firmware/sdk_overlay/`)
- Disable clashing AC636N demo-board peripherals in `board_smcpad_cfg.h`: `UART0` debug TX on `PC4` (pad mux `S2`), `ADKEY` on `PB1` (`WS2812`), SDK `spi.c` `SPI1`/`SPI2` demo pins, and `AT_UART` on `PC1`/`PC2`/`PA5`/`PA6`.
- Keep `SARADC`, `SPI2`, and `UART1` scan ISRs plus all lookup tables in `.data_code` / `.data` RAM (`check_ram_isr.sh`) so flash sector erases during VM/pattern saves never stall pad scanning or MIDI timing.
