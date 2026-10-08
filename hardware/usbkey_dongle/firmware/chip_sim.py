"""BR25 mask-ROM USB_KEY receiver simulator - MicroPython, for a SECOND RP2040/RP2350 board.

Bench test for the dongle without risking the SMC-PAD. It mimics what the BR25 ROM
does (cpu/br25/tools/rom.lst: main -> usb_key_isr / set_cap_timeout / check_usb_sof):

  * 'power-on' = the dongle's switched pad VBUS rises (SIM_VBUS input, via divider)
  * after POR_DELAY_MS the ROM listens for the key for WINDOW_MS (~10 ms, from
    set_cap_timeout: PRD = sys_clk/64000*10 at /64 prescale)
  * every rising edge on D+ shifts D- into a 16-bit register (init 0xFFFF, MSB first)
  * register == 0x16EF -> drive D+ and D- low for 2 ms (the ACK), then release
  * otherwise the 'firmware boots': D+ pull-up on (like a full-speed device)
  * while unpowered, the lines are clamped low (like ESD diodes of an unpowered chip)

After an ACK it also logs falling-edge intervals on D+ for 1 s, so the optional
SOF emulation (expected 1000 us apart) can be checked.

Wiring (sim board -> dongle 'pad' connector, e.g. through a USB-A plug breakout):
  GP2 -> D+   GP3 -> D-   GND -> GND
  15k from D+ to GND and 15k from D- to GND (the ROM's pull-downs; or set USE_INTERNAL_PD)
  pad VBUS -> 22k -> GP4 -> 10k -> GND  (5 V -> ~1.6 V)    (optional, else SIM_VBUS=None)

Usage:  import chip_sim; chip_sim.loop()      # Ctrl-C to stop
"""
import time
import rp2
from machine import Pin

SIM_DP = 2
SIM_DM = 3
SIM_VBUS = 4          # None: treat 'now' as power-on each time loop() starts a cycle
KEY = 0x16EF
POR_DELAY_MS = 2
WINDOW_MS = 10
ACK_MS = 2
UNPOWERED_CLAMP = True
USE_INTERNAL_PD = False


@rp2.asm_pio(in_shiftdir=rp2.PIO.SHIFT_LEFT, autopush=True, push_thresh=16)
def rx():
    wrap_target()
    wait(0, gpio, SIM_DP)
    wait(1, gpio, SIM_DP)       # rising edge on D+
    nop() [1]                   # ~2 us 'ISR latency' before sampling D-
    in_(pins, 1)                # in_base = D-
    wrap()


def _lines_input():
    pull = Pin.PULL_DOWN if USE_INTERNAL_PD else None
    Pin(SIM_DP, Pin.IN, pull)
    Pin(SIM_DM, Pin.IN, pull)


def _lines_low():
    Pin(SIM_DP, Pin.OUT, value=0)
    Pin(SIM_DM, Pin.OUT, value=0)


def _vbus():
    return True if SIM_VBUS is None else Pin(SIM_VBUS, Pin.IN).value() == 1


def _sof_intervals(ms=1000):
    edges = []
    dp = Pin(SIM_DP, Pin.IN, Pin.PULL_UP)        # the ROM enables a D+ pull-up here
    dp.irq(lambda p: edges.append(time.ticks_us()), Pin.IRQ_FALLING)
    time.sleep_ms(ms)
    dp.irq(None)
    d = [time.ticks_diff(edges[i + 1], edges[i]) for i in range(len(edges) - 1)]
    return d


def cycle():
    """One power cycle. Returns 'ACK', 'MISSED' or None (no power-on seen)."""
    if UNPOWERED_CLAMP:
        _lines_low()
    while not _vbus():
        time.sleep_us(100)
    t_on = time.ticks_us()
    time.sleep_ms(POR_DELAY_MS)
    _lines_input()
    sm = rp2.StateMachine(0, rx, freq=10_000_000, in_base=Pin(SIM_DM))
    sm.active(1)
    reg, bits, result = 0xFFFF, 0, "MISSED"
    t_listen = time.ticks_us()
    while time.ticks_diff(time.ticks_us(), t_listen) < WINDOW_MS * 1000:
        if sm.rx_fifo():
            w = sm.get() & 0xFFFF
            for i in range(15, -1, -1):
                reg = ((reg << 1) | ((w >> i) & 1)) & 0xFFFF
                bits += 1
                if reg == KEY:
                    result = "ACK"
                    break
            if result == "ACK":
                break
    sm.active(0)
    t_ack = time.ticks_diff(time.ticks_us(), t_on)
    if result == "ACK":
        _lines_low()
        time.sleep_ms(ACK_MS)
        Pin(SIM_DP, Pin.IN, None)               # ROM: inputs, all pulls off
        Pin(SIM_DM, Pin.IN, None)
        print("ACK: key after %d bits, %d us after power-on" % (bits, t_ack))
        d = _sof_intervals()
        if d:
            print("D+ falling-edge intervals after ACK (us):", d[:20])
    else:
        Pin(SIM_DP, Pin.IN, Pin.PULL_UP)        # 'firmware' connects as FS device
        print("MISSED: %d bits seen in the %d ms window -> normal boot" % (bits, WINDOW_MS))
    while _vbus() and SIM_VBUS is not None:
        time.sleep_ms(5)
    return result


def loop():
    stats = {"ACK": 0, "MISSED": 0}
    while True:
        r = cycle()
        stats[r] += 1
        print("stats:", stats)
        if SIM_VBUS is None:
            time.sleep_ms(3000)
