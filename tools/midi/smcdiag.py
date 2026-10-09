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
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
