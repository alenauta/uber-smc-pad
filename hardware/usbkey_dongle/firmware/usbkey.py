"""JieLi BR25 (AC636N) USB_KEY dongle - MicroPython for RP2040/RP2350 (Raspberry Pi Pico).

Sends the mask-ROM "USB_KEY" (0x16EF, MSB first, D+ = clock, D- = data, latched
on the rising clock edge) to the SMC-PAD at power-up, detects the chip's
acknowledge (both lines driven low for ~2 ms) and hands the pad's D+/D- over to
the USB host. On the host the chip then shows up as USB 4c4a:2542
"BR25 UBOOT1.00" and kagaimiq/jl-uboot-tool can read/write the SPI flash.

Protocol sources (see ../README.md and docs/recovery.md for details):
  - kagaimiq/jielie isp/usb/usb-key.md
  - kagaimiq/jl-uboot-tool docs/how-to-enter-uboot.md
  - BR25 mask ROM listing (cpu/br25/tools/rom.lst): usb_key_isr, check_key_type,
    set_cap_timeout, check_usb_sof, tmr3_isr

Usage on the Pico REPL:
  import usbkey
  usbkey.run()        # full procedure, prints "RESULT: ACK" or "RESULT: FAIL"
  usbkey.waveform(5)  # bench: send the key for 5 s, never accept an ACK
  usbkey.levels()     # bench: line levels with/without the 2k2 pull-ups
  usbkey.relays()     # bench: click K1 / K2
  usbkey.sof_test()   # bench: emit the optional 1 ms SOF pulses on D+
"""

import gc
import time
from machine import Pin, mem32
import rp2

import config as C

HIGH_WORD = 0xFFFFFFFF      # pushed by the PIO when the clock line sensed high
SIO_GPIO_IN = 0xD0000004    # SIO GPIO_IN register (RP2040 and RP2350)

# ---------------------------------------------------------------------------
# PIO program 1: key transmitter + ACK sensor (one packet per loop)
#
#   set pins  : GP2..GP5 (bit0 = GP2 D+, bit1 = GP3 D-, bit2 = GP4 PU+, bit3 = GP5 PU-)
#   side-set  : clock pin value (D+ normally)
#   out pins  : data pin value  (D- normally)
#   in pins   : GP2, GP3        (bit0 = GP2, bit1 = GP3)
#   jmp pin   : clock pin
#   20 cycles per bit -> SM clock = 20 / BIT_US MHz (1 MHz for 20 us bits)
#
# Per packet: drive clock+data, 16 bits MSB first (data changes with the falling
# edge, is held for the whole high half after the rising edge), keep the clock
# high, release both lines and switch on the 2k2 pull-ups, then sample the clock
# line 8 times over ~140 us:
#   all high -> push 0xFFFFFFFF (noblock) and send the next packet
#   any low  -> push the 2-bit line sample (block) and wait for a verdict word
#               from Python: 0 = stop (ACK accepted), anything else = continue.
# ---------------------------------------------------------------------------
@rp2.asm_pio(
    set_init=(rp2.PIO.IN_LOW, rp2.PIO.IN_LOW, rp2.PIO.IN_LOW, rp2.PIO.IN_LOW),
    out_init=rp2.PIO.IN_LOW,
    sideset_init=rp2.PIO.IN_HIGH,
    out_shiftdir=rp2.PIO.SHIFT_LEFT,
    in_shiftdir=rp2.PIO.SHIFT_LEFT,
)
def key_tx():
    pull(block)                 .side(1)        # word 0: KEY << 16
    mov(y, osr)                 .side(1)
    set(pins, 0b1100)           .side(1)        # pull-up pin latches = 1 (dirs still input)
    label("packet")
    mov(osr, y)                 .side(1)
    set(pindirs, 0b0011)        .side(1)        # drive clock + data, pull-ups off
    set(x, 15)                  .side(1)
    label("bit")
    out(pins, 1)                .side(0) [9]    # clock low, next data bit
    jmp(x_dec, "bit")           .side(1) [9]    # clock high: chip latches data here
    set(pindirs, 0b1100)        .side(1) [7]    # release lines, 2k2 pull-ups on, settle
    set(x, 7)                   .side(1)
    label("sense")
    jmp(pin, "high")            .side(1)
    jmp("low")                  .side(1)
    label("high")
    jmp(x_dec, "sense")         .side(1) [15]
    mov(isr, invert(null))      .side(1)
    push(noblock)               .side(1)        # "lines high"
    jmp("packet")               .side(1)
    label("low")
    mov(isr, null)              .side(1)
    in_(pins, 2)                .side(1)
    push(block)                 .side(1)        # 2-bit line sample
    pull(block)                 .side(1)        # verdict
    mov(x, osr)                 .side(1)
    jmp(not_x, "stop")          .side(1)
    jmp("packet")               .side(1)
    label("stop")
    set(pindirs, 0)             .side(1)        # release everything
    label("park")
    jmp("park")                 .side(1)


# ---------------------------------------------------------------------------
# PIO program 2 (optional): SOF emulation. N open-drain low pulses of 2 us on D+,
# exactly 1000 cycles (= 1.000 ms at 1 MHz) apart, then push a "done" word.
# ---------------------------------------------------------------------------
@rp2.asm_pio(set_init=rp2.PIO.IN_LOW)
def sof_pulses():
    pull(block)                                 # N - 1
    mov(y, osr)
    label("pulse")
    set(pindirs, 1) [1]                         # 2 cycles low (latch is 0)
    set(pindirs, 0)                             # 1
    set(x, 30)                                  # 1
    label("dly")
    jmp(x_dec, "dly") [31]                      # 31 * 32 = 992
    nop() [2]                                   # 3
    jmp(y_dec, "pulse")                         # 1  -> 1000 cycles per pulse
    push(block)
    label("park")
    jmp("park")


# ---------------------------------------------------------------------------
# Pure decision logic (also used by host/pio_model.py for the on-paper test)
# ---------------------------------------------------------------------------
class AckLogic:
    """Decides whether a 'line low' event from the PIO is the chip's ACK.

    The ROM drives both lines low right after the 16th rising clock edge of a
    matching packet, so the ACK is seen as a HIGH->LOW change between the sense
    phase of one packet and the next. A low is therefore only an ACK candidate if
    the sense phase of the PREVIOUS packet read HIGH (chip powered: its 15k
    pull-downs lose against our 2k2 pull-ups). This rejects
      - an unpowered chip, whose ESD diodes clamp the lines low all the time, and
      - the tail of a power-off clamp when the pad is switched back on (a low that
        started long before; found by host/pio_model.py, scenario 'battery pad').
    The candidate is then confirmed by the remaining low time (~2 ms in the ROM).
    """

    def __init__(self, clk_bit, data_bit, require_data_low=True,
                 min_us=300, max_us=4000):
        self.clk_bit = clk_bit
        self.data_bit = data_bit
        self.require_data_low = require_data_low
        self.min_us = min_us
        self.max_us = max_us
        self.highs = 0
        self.lows = 0
        self.rejected = 0
        self.prev_high = False

    def high(self):
        self.highs += 1
        self.prev_high = True

    def is_candidate(self, sample):
        self.lows += 1
        clk_low = ((sample >> self.clk_bit) & 1) == 0
        data_low = ((sample >> self.data_bit) & 1) == 0
        cand = self.prev_high and clk_low and (data_low or not self.require_data_low)
        self.prev_high = False
        return cand

    def accept(self, low_us):
        ok = self.min_us <= low_us <= self.max_us
        if not ok:
            self.rejected += 1
        return ok


def _log(*a):
    print("[usbkey %8d ms]" % time.ticks_ms(), *a)


# ---------------------------------------------------------------------------
# Hardware helpers
# ---------------------------------------------------------------------------
class Board:
    """Relays and LED. Both relays are wired so that 'not energized' = transparent
    cable: K1 NC contacts connect pad D+/D- to the host, K2 NC contact passes VBUS."""

    def __init__(self):
        self.k1 = Pin(C.PIN_K1, Pin.OUT, value=0) if C.PIN_K1 is not None else None
        self.k2 = Pin(C.PIN_K2, Pin.OUT, value=0) if C.PIN_K2 is not None else None
        try:
            self.led = Pin(C.PIN_LED, Pin.OUT, value=0)
        except Exception:
            self.led = None

    @property
    def has_vbus_switch(self):
        return self.k2 is not None

    def lines_to_pico(self, on):
        if self.k1 is not None:
            self.k1.value(1 if on else 0)

    def vbus(self, on):
        # K2 energized = VBUS cut (NC contact), so invert.
        if self.k2 is not None:
            self.k2.value(0 if on else 1)

    def led_set(self, v):
        if self.led is not None:
            self.led.value(1 if v else 0)

    def led_blink(self, period_ms):
        self.led_set((time.ticks_ms() // (period_ms // 2)) & 1)


def _release_lines():
    for n in range(C.PIN_BASE, C.PIN_BASE + 4):
        Pin(n, Pin.IN, None)
    if getattr(C, "PIN_SOF", None) is not None:
        Pin(C.PIN_SOF, Pin.IN, None)


def _sof_pin():
    p = getattr(C, "PIN_SOF", None)
    return _clk_pin() if p is None else p


def _clk_pin():
    return C.PIN_BASE + (1 if C.SWAP_CLOCK_DATA else 0)


def _data_pin():
    return C.PIN_BASE + (0 if C.SWAP_CLOCK_DATA else 1)


def _measure_low_us(pin, limit_us):
    """Time until `pin` reads high (pull-ups are still on while the PIO waits)."""
    mask = 1 << pin
    t0 = time.ticks_us()
    while not (mem32[SIO_GPIO_IN] & mask):
        dt = time.ticks_diff(time.ticks_us(), t0)
        if dt > limit_us:
            return dt
    return time.ticks_diff(time.ticks_us(), t0)


class KeyTx:
    def __init__(self, accept_ack=True):
        clk, dat = _clk_pin(), _data_pin()
        self.clk = clk
        self.accept_ack = accept_ack
        self.logic = AckLogic(clk - C.PIN_BASE, dat - C.PIN_BASE,
                              C.REQUIRE_DM_LOW, C.ACK_MIN_US, C.ACK_MAX_US)
        self.ack_low_us = None
        self.sm = rp2.StateMachine(
            0, key_tx, freq=int(20_000_000 // C.BIT_US),
            set_base=Pin(C.PIN_BASE), out_base=Pin(dat), sideset_base=Pin(clk),
            in_base=Pin(C.PIN_BASE), jmp_pin=Pin(clk))

    def start(self):
        self.sm.active(1)
        self.sm.put(C.KEY << 16)

    def poll(self):
        """Drain the RX FIFO. Returns True once an ACK has been accepted."""
        sm = self.sm
        while sm.rx_fifo():
            v = sm.get()
            if v == HIGH_WORD:
                self.logic.high()
                continue
            if self.accept_ack and self.logic.is_candidate(v):
                low = _measure_low_us(self.clk, C.ACK_MAX_US + 1000)
                if self.logic.accept(low):
                    self.ack_low_us = low
                    sm.put(0)               # PIO releases all four pins and parks
                    return True
            sm.put(1)                       # keep sending
        return False

    def stop(self):
        self.sm.active(0)
        _release_lines()

    def stats(self):
        L = self.logic
        return "highs=%d lows=%d rejected=%d" % (L.highs, L.lows, L.rejected)


def sof_emulate(n=None):
    n = n or C.SOF_PULSES
    sm = rp2.StateMachine(4, sof_pulses, freq=1_000_000, set_base=Pin(_sof_pin()))
    sm.active(1)
    sm.put(n - 1)
    t0 = time.ticks_ms()
    while not sm.rx_fifo() and time.ticks_diff(time.ticks_ms(), t0) < n + 50:
        pass
    sm.active(0)
    _release_lines()


# ---------------------------------------------------------------------------
# Main procedure
# ---------------------------------------------------------------------------
def _attempt(b, n, allow_manual):
    _log("attempt %d: pad VBUS off, lines -> Pico" % (n + 1))
    b.vbus(False)
    b.lines_to_pico(True)
    time.sleep_ms(max(C.VBUS_OFF_MS if b.has_vbus_switch else 0, C.RELAY_SETTLE_MS))

    tx = KeyTx()
    gc.collect()
    gc.disable()                            # no GC pause while the 2 ms ACK is measured
    try:
        tx.start()                          # key on the wire BEFORE the chip powers up
        t0 = time.ticks_ms()
        if b.has_vbus_switch:
            b.vbus(True)
            _log("VBUS on, sending key (ROM window is only ~10 ms after reset)")
            deadline, manual = C.AUTO_ACK_TIMEOUT_MS, False
        else:
            _log(">>> POWER THE PAD NOW (plug it in / power switch ON). Sending key for %d s"
                 % C.MANUAL_WINDOW_S)
            deadline, manual = C.MANUAL_WINDOW_S * 1000, True

        while True:
            if tx.poll():
                break
            el = time.ticks_diff(time.ticks_ms(), t0)
            b.led_blink(100 if manual else 500)
            if el > deadline:
                if not manual and allow_manual:
                    _log("no ACK after VBUS-on (%s)." % tx.stats())
                    _log(">>> Pad power switch OFF->ON now (key still running, %d s)"
                         % C.MANUAL_WINDOW_S)
                    deadline += C.MANUAL_WINDOW_S * 1000
                    manual = True
                else:
                    _log("no ACK (%s)" % tx.stats())
                    tx.stop()
                    b.lines_to_pico(False)
                    b.led_set(0)
                    return False

        el = time.ticks_diff(time.ticks_ms(), t0)
        tx.stop()
        _log("ACK %d ms after start, remaining low %d us (%s)" % (el, tx.ack_low_us, tx.stats()))
        if C.SOF_EMULATION:
            time.sleep_ms(C.SOF_START_DELAY_MS)
            sof_emulate()
            _log("sent %d SOF pulses" % C.SOF_PULSES)
        b.lines_to_pico(False)              # pad D+/D- -> host
        b.led_set(1)
        # Stay quiet on our own USB-CDC while the ROM calibrates on SOFs (see README,
        # "shared hub"): print nothing for HANDOFF_QUIET_MS.
        time.sleep_ms(getattr(C, "HANDOFF_QUIET_MS", 0))
        _log("pad handed to host: expect USB 4c4a:2542 'BR25 UBOOT1.00'")
        return True
    except BaseException:
        tx.stop()
        b.lines_to_pico(False)
        raise
    finally:
        gc.enable()


def run(retries=None):
    b = Board()
    attempts = (retries or C.RETRIES) if b.has_vbus_switch else 1
    for n in range(attempts):
        if _attempt(b, n, allow_manual=(n == 0)):
            print("RESULT: ACK")
            return True
    print("RESULT: FAIL")
    return False


# ---------------------------------------------------------------------------
# Bench helpers (no SMC-PAD needed)
# ---------------------------------------------------------------------------
def waveform(seconds=5):
    """Send the key continuously (never accept an ACK) for a logic-analyzer capture."""
    b = Board()
    b.lines_to_pico(True)
    time.sleep_ms(C.RELAY_SETTLE_MS)
    tx = KeyTx(accept_ack=False)
    tx.start()
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            tx.poll()
            b.led_blink(200)
    finally:
        tx.stop()
        b.lines_to_pico(False)
        b.led_set(0)
    _log("waveform done (%s)" % tx.stats())


def levels():
    """Print D+/D- levels with pull-ups off and on (lines not driven)."""
    _release_lines()
    clk, dat = _clk_pin(), _data_pin()
    def rd():
        g = mem32[SIO_GPIO_IN]
        return "D+(clk)=%d D-(data)=%d" % ((g >> clk) & 1, (g >> dat) & 1)
    print("pull-ups off:", rd())
    pu = [Pin(C.PIN_BASE + 2, Pin.OUT, value=1), Pin(C.PIN_BASE + 3, Pin.OUT, value=1)]
    time.sleep_ms(1)
    print("pull-ups on :", rd(), " (expect 1/1 unless something drives the lines low)")
    _release_lines()


def relays():
    b = Board()
    for name, f in (("K1 lines->Pico", b.lines_to_pico), ("K2 VBUS cut", lambda v: b.vbus(not v))):
        print(name, "ON");  f(True);  time.sleep_ms(1000)
        print(name, "OFF"); f(False); time.sleep_ms(1000)


def sof_test(n=12):
    sof_emulate(n)
    print("sent %d pulses on GP%d" % (n, _sof_pin()))


def button_loop():
    btn = Pin(C.PIN_BUTTON, Pin.IN, Pin.PULL_UP)
    _log("press the button on GP%d to start (Ctrl-C for the REPL)" % C.PIN_BUTTON)
    while True:
        if btn.value() == 0:
            time.sleep_ms(30)
            if btn.value() == 0:
                run()
                while btn.value() == 0:
                    time.sleep_ms(10)
        time.sleep_ms(10)
