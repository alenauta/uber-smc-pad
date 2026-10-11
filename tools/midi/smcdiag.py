#!/usr/bin/env python3
"""SMC-PAD custom-firmware diagnostics over USB-MIDI (stdlib only, CoreMIDI via ctypes).

Talks ONLY to our own firmware (USB 1209:0001, ports "SMC-PAD -Master"), with the diagnostic
SysEx of smcpad_app.c (non-commercial ID 0x7D, "SP"). Never use it with the stock firmware.
Every request ends with "00 F7", so no frame ever ends in "7D F7".

Usage:
  smcdiag.py status                 counters, debounced buttons, knob counts      (cmd 0x02)
  smcdiag.py frame [N]              raw key-matrix frame, N times                 (cmd 0x03)
  smcdiag.py pads                   raw / baseline / pressure per pad             (cmd 0x01)
  smcdiag.py peek ADDR [N]          read N 32-bit words (RAM, SFR)                (cmd 0x10)
  smcdiag.py poke ADDR VALUE        write one SFR word (0x1E0000-0x1FFFFF)        (cmd 0x11)
  smcdiag.py regs                   key-matrix related registers (SPI2, IOMAP, PORTA/B/C)
  smcdiag.py flicker                guided LED-flicker test: pauses parts for 10 s each, then restores
  smcdiag.py watch [SECONDS]        print knob counts / buttons / raw rows whenever they change
  smcdiag.py padcfg [KEY=VAL ...]   show / change the pad detector live (904+, RAM only, lost on reboot)
                                    keys: on off vel_full peak shift hold release  (cmd 0x20 / 0x21)
  smcdiag.py boots                  reset history of the last 8 boots + last exception (909+, cmd 0x04)
  smcdiag.py reboot                 soft reset (909+, cmd 0x30)
  smcdiag.py crashtest              deliberate CPU exception, to check the crash capture (909+, 0x31)
(BAUD / ADR / CNT registers are write-only and read back 0.)
"""
import ctypes
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import midimon as mm  # noqa: E402  (CoreMIDI bindings, endpoints, packet parsing)

cm = mm.cm
MIDIRef = mm.MIDIRef
HDR = bytes([0xF0, 0x7D, 0x53, 0x50])

REGS = {
    "SPI2.CON": 0x1E1E00, "SPI2.BAUD": 0x1E1E04, "SPI2.ADR": 0x1E1E0C, "SPI2.CNT": 0x1E1E10,
    "SPI1.CON": 0x1E1D00, "SPI1.BAUD": 0x1E1D04,
    "IOMAP.CON0": 0x1E5118, "IOMAP.CON1": 0x1E511C, "IOMAP.CON2": 0x1E5120, "IOMAP.CON3": 0x1E5124,
}
for _port, _base in (("PA", 0x1E5000), ("PB", 0x1E5040), ("PC", 0x1E5080)):
    for _i, _r in enumerate(("OUT", "IN", "DIR", "DIE", "PU", "PD", "HD0", "HD", "DIEH")):
        REGS[f"{_port}.{_r}"] = _base + 4 * _i

cm.MIDIOutputPortCreate.restype = mm.OSStatus
cm.MIDIOutputPortCreate.argtypes = [MIDIRef, ctypes.c_void_p, ctypes.POINTER(MIDIRef)]
cm.MIDIPacketListInit.restype = ctypes.c_void_p
cm.MIDIPacketListInit.argtypes = [ctypes.c_void_p]
cm.MIDIPacketListAdd.restype = ctypes.c_void_p
cm.MIDIPacketListAdd.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_uint64,
                                 ctypes.c_ulong, ctypes.c_char_p]
cm.MIDISend.restype = mm.OSStatus
cm.MIDISend.argtypes = [MIDIRef, MIDIRef, ctypes.c_void_p]

_rx = bytearray()
_replies = []
_ev = threading.Event()


def _on_packets(pktlist, _ref, _src):
    n = ctypes.c_uint32.from_address(pktlist).value
    p = pktlist + 4
    for _ in range(n):
        length = ctypes.c_uint16.from_address(p + 8).value
        for b in ctypes.string_at(p + 10, length):
            if b == 0xF0:
                _rx.clear()
            if _rx or b == 0xF0:
                _rx.append(b)
            if b == 0xF7 and _rx:
                if bytes(_rx[:4]) == HDR:
                    _replies.append(bytes(_rx))
                    _ev.set()
                _rx.clear()
        p = p + 10 + length
        if mm.ALIGN4:
            p = (p + 3) & ~3


_cb = mm.ReadProc(_on_packets)


class Pad:
    def __init__(self):
        self.client, self.inp, self.out = MIDIRef(), MIDIRef(), MIDIRef()
        assert cm.MIDIClientCreate(mm.cfstr("smcdiag"), None, None, ctypes.byref(self.client)) == 0
        assert cm.MIDIInputPortCreate(self.client, mm.cfstr("in"), _cb, None, ctypes.byref(self.inp)) == 0
        assert cm.MIDIOutputPortCreate(self.client, mm.cfstr("out"), ctypes.byref(self.out)) == 0
        srcs, dsts = mm.endpoints()
        self.dst = None
        for s in srcs:
            if "smc-pad" in mm.get_str(s, mm.kDisplayName).lower():
                cm.MIDIPortConnectSource(self.inp, s, None)
        for d in dsts:
            name = mm.get_str(d, mm.kDisplayName).lower()
            if "smc-pad" in name and "master" in name:
                self.dst = d
        if self.dst is None:
            sys.exit("no 'SMC-PAD -Master' destination: is the pad on USB with our firmware?")

    def request(self, payload, timeout=1.0):
        msg = HDR + bytes(payload) + b"\xF7"
        assert not msg.endswith(b"\x7D\xF7")
        buf = ctypes.create_string_buffer(1024)
        pkt = cm.MIDIPacketListInit(buf)
        pkt = cm.MIDIPacketListAdd(buf, 1024, pkt, 0, len(msg), msg)
        _replies.clear()
        _ev.clear()
        assert cm.MIDISend(self.out, self.dst, buf) == 0
        if not _ev.wait(timeout):
            raise TimeoutError("no reply")
        time.sleep(0.02)
        for r in _replies:
            if r[4] == payload[0]:
                return r[5:-1]
        raise TimeoutError("no matching reply")


def u14(b, i):
    return b[i] | (b[i + 1] << 7)


def enc32(v):
    return bytes((v >> (7 * i)) & 0x7F for i in range(5))


def dec32(b, i):
    return sum((b[i + k] & 0x7F) << (7 * k) for k in range(5))


def cmd_status(pad):
    r = pad.request([0x02])
    print(f"adc_frames {u14(r,0) | u14(r,2) << 14}  km_frames {u14(r,4) | u14(r,6) << 14}  "
          f"pad_drops {u14(r,8)}  trs_drops {u14(r,10)}  buttons {u14(r,12):010b}  pads_running {r[14]}")
    knobs = [u14(r, 15 + 2 * k) for k in range(8)]
    print("knob counts", [k - 0x4000 if k & 0x2000 else k for k in knobs])
    if len(r) >= 40:                                       # 907+: sequencer
        f = r[31]
        state = [n for b, n in ((1, "running"), (2, "following host clock"), (4, "seq mode"), (8, "sync on"))
                 if f & b] or ["stopped"]
        ext = u14(r, 38)
        print(f"sequencer: {', '.join(state)}  tempo {u14(r, 36) / 10:.1f} BPM"
              f"{f'  host {ext / 10:.1f} BPM' if ext else ''}  late events {u14(r, 32)}  queue drops {u14(r, 34)}")
    if len(r) >= 47:                                       # 908+: play early, clock phase, undo
        err, emax = u14(r, 41) - 8192, u14(r, 43)
        print(f"play early {r[40]} ms  host F8 phase error last {err:+d} max {emax} (1/960 beat, 40 = 1 clock)"
              f"  undo levels {u14(r, 45)}")
    if len(r) >= 51:                                       # 911+: battery
        print(f"battery {u14(r, 47) / 100:.2f} V  (lowest since boot {u14(r, 49) / 100:.2f} V)")
    if len(r) >= 55:                                       # 913+: main loop load
        print(f"main loop busy {r[51]} % (highest since boot {r[52]} %)  longest pass {u14(r, 53)} ms")


def cmd_frame(pad, n=1):
    for _ in range(n):
        r = pad.request([0x03])
        f = r[:16]
        row0 = "".join("0" if not (b & 1) else "1" for b in f)
        row1 = "".join("0" if not (b & 2) else "1" for b in f)
        print(f"step   0123456789abcdef   buttons {u14(r,16):010b}  km_frames {dec32(r,18)}")
        print(f"row0   {row0}   (PB0: encoders, 0 = closed)")
        print(f"row1   {row1}   (PC1: buttons,  0 = closed)")
        if n > 1:
            time.sleep(0.3)


def cmd_pads(pad):
    r = pad.request([0x01])
    print("polarity", r[0] - 1)
    for i in range(16):
        o = 1 + 6 * i
        print(f"pad {i:2}: raw {u14(r,o):5}  base {u14(r,o+2):5}  pressure {u14(r,o+4):5}")


def cmd_peek(pad, addr, n=1):
    r = pad.request([0x10, *enc32(addr), n, 0x00])
    a = dec32(r, 0)
    for i in range(n):
        print(f"0x{a + 4*i:08X}: 0x{dec32(r, 5 + 5*i):08X}")


def cmd_poke(pad, addr, val):
    r = pad.request([0x11, *enc32(addr), *enc32(val), 0x00])
    print(f"0x{dec32(r,0):08X} <- 0x{val:08X}, readback 0x{dec32(r,5):08X}")


def cmd_regs(pad):
    for name, a in REGS.items():
        r = pad.request([0x10, *enc32(a), 1, 0x00])
        print(f"{name:11} 0x{a:06X} = 0x{dec32(r,5):08X}")


def peek1(pad, a):
    return dec32(pad.request([0x10, *enc32(a), 1, 0x00]), 5)


def poke1(pad, a, v):
    return dec32(pad.request([0x11, *enc32(a), *enc32(v), 0x00]), 5)


SPI1_CON, ADC_CON = 0x1E1D00, 0x1E3100


def cmd_flicker(pad, secs=10):
    def phase(title):
        print(f"\n>>> {title}: watch a lit button LED for {secs} s", flush=True)
        for i in range(secs, 0, -1):
            print(f"   {i:2}", end="\r", flush=True)
            time.sleep(1)
        print("      ")
    input("Light one button LED (e.g. press PAD BANK once), then press Enter here... ")
    phase("A  normal")
    v = peek1(pad, SPI1_CON)
    poke1(pad, SPI1_CON, v & ~1)                    # pad-LED (WS2812) SPI off
    try:
        phase("B  pad-LED updates paused (pad colours freeze)")
    finally:
        poke1(pad, SPI1_CON, v)
    v = peek1(pad, ADC_CON)
    poke1(pad, ADC_CON, 0)                          # pad scanning off (pads dead meanwhile)
    try:
        phase("C  pad scanning paused (pads do nothing)")
    finally:
        poke1(pad, ADC_CON, v | (1 << 6))           # restart the conversion chain
    phase("D  normal again")
    print("Done. Which of A / B / C / D flickered? (pads restarted:", "ok" if peek1(pad, ADC_CON) else "CHECK", ")")


def cmd_watch(pad, secs=60):
    last = None
    end = time.monotonic() + secs
    print(f"watching for {secs} s: turn knobs / press buttons (Ctrl-C to stop)")
    try:
        while time.monotonic() < end:
            r = pad.request([0x02])
            knobs = [u14(r, 15 + 2 * k) for k in range(8)]
            knobs = [k - 0x4000 if k & 0x2000 else k for k in knobs]
            cur = (u14(r, 12), tuple(knobs))
            if cur != last:
                print(f"{time.strftime('%H:%M:%S')}  buttons {cur[0]:010b}  knobs {list(cur[1])}", flush=True)
                last = cur
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass


PADCFG_KEYS = ("on", "off", "vel_full", "peak", "shift", "hold", "release")


def _padcfg_decode(r):
    return dict(zip(PADCFG_KEYS, (u14(r, 0), u14(r, 2), u14(r, 4), r[6], r[7], r[8], r[9])))


def cmd_padcfg(pad, sets):
    c = _padcfg_decode(pad.request([0x20, 0x00]))
    if sets:
        for s in sets:
            k, _, v = s.partition("=")
            if k not in PADCFG_KEYS or not v:
                sys.exit(f"bad setting {s!r}; keys: {' '.join(PADCFG_KEYS)}")
            c[k] = int(v, 0)
        for k in ("on", "off", "vel_full"):
            if not 0 <= c[k] < 0x4000:
                sys.exit(f"{k} must be 0..16383")
        for k in ("peak", "shift", "hold", "release"):
            if not 0 <= c[k] < 0x80:
                sys.exit(f"{k} must be 0..127")
        if c["off"] >= c["on"]:
            sys.exit("off must be below on")
        body = [c["on"] & 0x7F, c["on"] >> 7, c["off"] & 0x7F, c["off"] >> 7,
                c["vel_full"] & 0x7F, c["vel_full"] >> 7, c["peak"], c["shift"], c["hold"], c["release"]]
        c = _padcfg_decode(pad.request([0x21, *body, 0x00]))
    print("  ".join(f"{k} {v}" for k, v in c.items()))
    print("(frames: one pad frame is ~1.7 ms; hold = minimum note length, release = samples below 'off')")


PWR_SRC = {0x01: "power-on", 0x40: "soft reset / crash", 0x04: "0x04 (watchdog or low voltage?)"}
STAGES = ("idle", "pads", "knobs", "buttons", "bt", "usb", "ble", "seq", "leds", "rgb", "adc", "globals")
BUTTONS = ("BT", "PAD BANK", "KNOB BANK", "LEFT", "RIGHT", "PLAY", "STOP", "REC", "SHIFT", "NOTE REPEAT")


def cmd_boots(pad):
    r = pad.request([0x04])
    if r[0] != 2:                                          # 909 / 910 format
        boots, n = u14(r, 0), r[2]
        print(f"boots recorded: {boots}  (newest first; rst = JL_RST->SRC, pwr = P33 reset source)")
        for k in range(min(n, 8)):
            i = 3 + 10 * k
            print(f"  #{k}: rst 0x{u14(r, i):02X}  pwr 0x{u14(r, i + 2):02X}")
        return
    boots, n = u14(r, 1), r[3]
    print(f"boots recorded: {boots}  (newest first)")
    for k in range(min(n, 8)):
        i = 4 + 17 * k
        rst, pwr, prev, stage = u14(r, i), u14(r, i + 2), r[i + 4], r[i + 5]
        ms, evk, evi, vb2, age = dec32(r, i + 6), r[i + 11], r[i + 12], u14(r, i + 13), u14(r, i + 15)
        line = f"  #{k}: {PWR_SRC.get(pwr, f'pwr 0x{pwr:02X}')}  (rst 0x{rst:02X} pwr 0x{pwr:02X})"
        if prev:
            ev = {1: f"knob K{evi + 1}", 2: f"button {BUTTONS[evi] if evi < len(BUTTONS) else evi}",
                  3: f"pad {evi + 1}"}.get(evk, "none")
            agestr = ">25 s" if age >= 255 else f"{age / 10:.1f} s"
            line += (f"\n        previous run: up {ms / 1000:.1f} s, ended in '{STAGES[stage] if stage < len(STAGES) else stage}',"
                     f" last event {ev} {agestr} before, battery {vb2 * 0.02:.2f} V")
        else:
            line += "\n        previous run: no breadcrumb (power was off, or older firmware)"
        print(line)
    try:
        c = pad.request([0x05])                            # 916+: latest CPU exception
    except TimeoutError:
        return
    if not c or not c[0]:
        print("latest exception: none captured")
        return
    names = ("EMU_MSG", "DEBUG_MSG", "PRP_MMU", "PRP_WRL", "RETE", "RETI", "RETS", "RETX", "PSR", "SP",
             "ETM0", "ETM1", "ETM2", "ETM3")
    w = [dec32(c, 1 + 5 * i) for i in range(15)]
    stage = c[76]
    task = bytes(c[77:89]).split(b"\0")[0].decode(errors="replace")
    print(f"latest exception: at uptime {w[14] / 1000:.1f} s, stage "
          f"'{STAGES[stage] if stage < len(STAGES) else stage}', task '{task}'")
    for i in range(0, 14, 4):
        print("   " + "  ".join(f"{names[j]} 0x{w[j]:08X}" for j in range(i, min(i + 4, 14))))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    pad = Pad()
    num = lambda s: int(s, 0)  # noqa: E731
    if a[0] == "status":
        cmd_status(pad)
    elif a[0] == "frame":
        cmd_frame(pad, int(a[1]) if len(a) > 1 else 1)
    elif a[0] == "pads":
        cmd_pads(pad)
    elif a[0] == "peek":
        cmd_peek(pad, num(a[1]), int(a[2]) if len(a) > 2 else 1)
    elif a[0] == "poke":
        cmd_poke(pad, num(a[1]), num(a[2]))
    elif a[0] == "regs":
        cmd_regs(pad)
    elif a[0] == "flicker":
        cmd_flicker(pad)
    elif a[0] == "watch":
        cmd_watch(pad, float(a[1]) if len(a) > 1 else 60)
    elif a[0] == "padcfg":
        cmd_padcfg(pad, a[1:])
    elif a[0] == "boots":
        cmd_boots(pad)
    elif a[0] in ("reboot", "crashtest"):
        pad.request([0x30 if a[0] == "reboot" else 0x31])
        print("sent; the pad restarts in ~0.1 s (USB re-enumerates)")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
