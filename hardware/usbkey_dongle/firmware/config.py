# USB_KEY dongle configuration (MicroPython, RP2040 Pico / Pico W / RP2350 Pico 2).
# See ../README.md for the wiring. Every value here is referenced by usbkey.py.

# --- Pins -------------------------------------------------------------------
# GP2..GP5 MUST stay consecutive: one PIO state machine drives/senses all four.
PIN_BASE = 2          # GP2 = pad D+ (via R1 1k), GP3 = pad D- (via R2 1k)
#                       GP4 = 2k2 pull-up for D+ (R3), GP5 = 2k2 pull-up for D- (R4)

# Which of the two line pins carries the CLOCK. Per the BR25 mask ROM
# (set_timer3_captrue_dp + usb_key_isr reading DM) and kagaimiq/jielie usb-key.md,
# D+ is the clock and D- the data. Set True only for experiments.
SWAP_CLOCK_DATA = False

PIN_K1 = 6            # data relay driver. 1 = pad D+/D- -> Pico, 0 = pad D+/D- -> host.
#                       None = no relay fitted ("passive tap", see README option B).
PIN_K2 = 7            # pad VBUS switch. 1 = pad powered. None = not fitted (manual power).
PIN_BUTTON = 14       # optional start button to GND (internal pull-up). None = disabled.
PIN_LED = "LED"       # on-board LED ("LED" works on Pico, Pico W and Pico 2)

# --- Key protocol -------------------------------------------------------------
KEY = 0x16EF          # USB_KEY (ROM check_key_type: 0x16EF -> USB boot)
BIT_US = 20           # bit period in us (50 kHz, like the vendor dongle: ~19.3 us).
#                       The PIO program uses 20 cycles per bit, so the SM clock
#                       is 20 / BIT_US MHz. Keep BIT_US >= 10 (ROM receives in software).

# ACK = both lines driven low by the chip for ~2 ms (ROM usb_key_isr: mdelay(2)).
REQUIRE_DM_LOW = True     # require D- low too (needs R4 fitted)
ACK_MIN_US = 300          # measured remaining low time must be within these bounds
ACK_MAX_US = 4000         # (lines go high again when the ROM releases them)

# --- Sequencing -------------------------------------------------------------------
VBUS_OFF_MS = 1500        # pad unpowered this long before each attempt (discharge)
RELAY_SETTLE_MS = 20      # relay operate/release + bounce
AUTO_ACK_TIMEOUT_MS = 1500    # with K2: wait this long after VBUS-on for an ACK
MANUAL_WINDOW_S = 20      # then (or without K2): keep sending, ask user to power the pad
RETRIES = 3               # automatic power-cycle attempts (only with K2)
HANDOFF_QUIET_MS = 3000   # no serial output this long after handing D+/D- to the host

# --- Optional SOF emulation -------------------------------------------------------
# After the ACK the ROM calibrates its clock from 1 ms SOF intervals on D+
# (check_usb_sof: 3 attempts x 1 s; tmr3_isr needs 4 matching periods).
# If True, the dongle sends SOF_PULSES clean low pulses 1.000 ms apart before
# handing the bus to the host, so host/hub traffic cannot disturb the calibration
# (kagaimiq/jielie usb-key.md, "USB bus considerations"). Default off = vendor behaviour.
#
# Electrical note (host/pio_model.py): the ROM pulls D+ up (~1.5k, USB full-speed
# attach) during this phase. A low pulse through R1 (1k) only reaches ~1.3 V, which
# is NOT a valid logic low. So SOF emulation needs its own low-impedance path:
# fit R5 = 330R from PIN_SOF to the Pico side of K1's D+ contact, set PIN_SOF = 8.
SOF_EMULATION = False
PIN_SOF = None            # 8 with R5 fitted; None = pulse on the clock pin via R1 (marginal)
SOF_PULSES = 50           # 50 ms of 1 ms pulses (ROM needs 4 good periods; see README)
SOF_START_DELAY_MS = 4    # ACK (2 ms) + ROM switches lines to input + enables D+ pull-up
