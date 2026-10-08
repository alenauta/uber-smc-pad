#!/usr/bin/env python3
"""Receive-only CoreMIDI monitor for macOS (stdlib only, via ctypes).

Never sends anything to any device.

Usage:
  midimon.py list                       # list MIDI sources/destinations
  midimon.py monitor [FILTER] [SECONDS] # print incoming messages
      FILTER  : substring of the source name (default "SMC"), "" = all
      SECONDS : stop after N seconds (default: run until Ctrl-C)
"""
import ctypes
import ctypes.util
import platform
import sys
import time

cm = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreMIDI"))
cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))

OSStatus = ctypes.c_int32
MIDIRef = ctypes.c_uint32
kCFStringEncodingUTF8 = 0x08000100

cm.MIDIGetNumberOfSources.restype = ctypes.c_ulong
cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
cm.MIDIGetSource.restype = MIDIRef
cm.MIDIGetSource.argtypes = [ctypes.c_ulong]
cm.MIDIGetDestination.restype = MIDIRef
cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
cm.MIDIObjectGetStringProperty.restype = OSStatus
cm.MIDIObjectGetStringProperty.argtypes = [MIDIRef, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cm.MIDIObjectGetIntegerProperty.restype = OSStatus
cm.MIDIObjectGetIntegerProperty.argtypes = [MIDIRef, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32)]
cm.MIDIClientCreate.restype = OSStatus
cm.MIDIClientCreate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(MIDIRef)]
cm.MIDIPortConnectSource.restype = OSStatus
cm.MIDIPortConnectSource.argtypes = [MIDIRef, MIDIRef, ctypes.c_void_p]

cf.CFStringCreateWithCString.restype = ctypes.c_void_p
cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
cf.CFStringGetCString.restype = ctypes.c_bool
cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
cf.CFRelease.argtypes = [ctypes.c_void_p]


def _const(name):
    return ctypes.c_void_p.in_dll(cm, name)


kDisplayName = _const("kMIDIPropertyDisplayName")
kUniqueID = _const("kMIDIPropertyUniqueID")


def cfstr(s):
    return cf.CFStringCreateWithCString(None, s.encode(), kCFStringEncodingUTF8)


def get_str(obj, prop):
    ref = ctypes.c_void_p()
    if cm.MIDIObjectGetStringProperty(obj, prop, ctypes.byref(ref)) != 0 or not ref.value:
        return "?"
    buf = ctypes.create_string_buffer(512)
    cf.CFStringGetCString(ref, buf, 512, kCFStringEncodingUTF8)
    cf.CFRelease(ref)
    return buf.value.decode(errors="replace")


def get_int(obj, prop):
    v = ctypes.c_int32()
    cm.MIDIObjectGetIntegerProperty(obj, prop, ctypes.byref(v))
    return v.value


def endpoints():
    srcs = [cm.MIDIGetSource(i) for i in range(cm.MIDIGetNumberOfSources())]
    dsts = [cm.MIDIGetDestination(i) for i in range(cm.MIDIGetNumberOfDestinations())]
    return srcs, dsts


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def describe(msg):
    st = msg[0]
    if st == 0xF0:
        return f"SysEx ({len(msg)} bytes)"
    if st >= 0xF8:
        return {0xF8: "Clock", 0xFA: "Start", 0xFB: "Continue", 0xFC: "Stop",
                0xFE: "ActiveSense", 0xFF: "Reset"}.get(st, "Realtime")
    hi, ch = st & 0xF0, (st & 0x0F) + 1
    d = list(msg[1:])
    if hi == 0x90 and len(d) >= 2:
        return f"ch{ch:<2} NoteOn  {d[0]:3} ({note_name(d[0])}) vel {d[1]}" + ("  (=off)" if d[1] == 0 else "")
    if hi == 0x80 and len(d) >= 2:
        return f"ch{ch:<2} NoteOff {d[0]:3} ({note_name(d[0])}) vel {d[1]}"
    if hi == 0xB0 and len(d) >= 2:
        return f"ch{ch:<2} CC {d[0]:3} = {d[1]}"
    if hi == 0xA0 and len(d) >= 2:
        return f"ch{ch:<2} PolyAT {d[0]:3} ({note_name(d[0])}) = {d[1]}"
    if hi == 0xD0 and d:
        return f"ch{ch:<2} ChanAT = {d[0]}"
    if hi == 0xC0 and d:
        return f"ch{ch:<2} ProgChange {d[0]}"
    if hi == 0xE0 and len(d) >= 2:
        return f"ch{ch:<2} PitchBend {(d[1] << 7 | d[0]) - 8192}"
    return "?"


def split_messages(data):
    """Split a raw MIDI byte stream (one packet) into messages."""
    out, i = [], 0
    while i < len(data):
        b = data[i]
        if b == 0xF0:
            j = data.find(b"\xF7", i)
            j = len(data) - 1 if j < 0 else j
            out.append(data[i:j + 1]); i = j + 1
        elif b >= 0xF8:
            out.append(data[i:i + 1]); i += 1
        elif b >= 0x80:
            hi = b & 0xF0
            n = 2 if hi in (0xC0, 0xD0) else (3 if hi < 0xF0 else {0xF1: 2, 0xF2: 3, 0xF3: 2}.get(b, 1))
            out.append(data[i:i + n]); i += n
        else:  # running status / stray data byte
            out.append(data[i:i + 1]); i += 1
    return out


# MIDIPacketList layout (#pragma pack(4)): u32 numPackets; then packets of
# { u64 timeStamp; u16 length; u8 data[length] }. On ARM, MIDIPacketNext
# rounds the next packet address up to a 4-byte boundary.
ALIGN4 = platform.machine().startswith("arm")

ReadProc = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
cm.MIDIInputPortCreate.restype = OSStatus
cm.MIDIInputPortCreate.argtypes = [MIDIRef, ctypes.c_void_p, ReadProc, ctypes.c_void_p, ctypes.POINTER(MIDIRef)]

_names = {}
_t0 = time.monotonic()


def _on_packets(pktlist, _ref, src_ref):
    base = pktlist
    n = ctypes.c_uint32.from_address(base).value
    p = base + 4
    src = _names.get(src_ref or 0, "?")
    for _ in range(n):
        length = ctypes.c_uint16.from_address(p + 8).value
        data = ctypes.string_at(p + 10, length)
        for msg in split_messages(data):
            print(f"{time.monotonic() - _t0:8.3f}  {src:<18} {msg.hex(' '):<12} {describe(msg)}", flush=True)
        p = p + 10 + length
        if ALIGN4:
            p = (p + 3) & ~3


_cb = ReadProc(_on_packets)


def cmd_list():
    srcs, dsts = endpoints()
    print("Sources (device -> Mac):")
    for s in srcs:
        print(f"  {get_str(s, kDisplayName):<40} uid={get_int(s, kUniqueID)}")
    print("Destinations (Mac -> device):")
    for d in dsts:
        print(f"  {get_str(d, kDisplayName):<40} uid={get_int(d, kUniqueID)}")


def cmd_monitor(flt, seconds):
    client, port = MIDIRef(), MIDIRef()
    assert cm.MIDIClientCreate(cfstr("midimon"), None, None, ctypes.byref(client)) == 0
    assert cm.MIDIInputPortCreate(client, cfstr("in"), _cb, None, ctypes.byref(port)) == 0
    srcs, _ = endpoints()
    connected = 0
    for idx, s in enumerate(srcs, start=1):
        name = get_str(s, kDisplayName)
        if flt.lower() in name.lower():
            _names[idx] = name
            cm.MIDIPortConnectSource(port, s, ctypes.c_void_p(idx))
            print(f"listening: {name}", flush=True)
            connected += 1
    if not connected:
        print(f"no sources matching {flt!r}", file=sys.stderr)
        return 1
    print("--- press pads / turn knobs (Ctrl-C to stop) ---", flush=True)
    try:
        end = time.monotonic() + seconds if seconds else None
        while end is None or time.monotonic() < end:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    print("--- done ---")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] == "list":
        cmd_list()
    elif args[0] == "monitor":
        flt = args[1] if len(args) > 1 else "SMC"
        secs = float(args[2]) if len(args) > 2 else 0
        sys.exit(cmd_monitor(flt, secs))
    else:
        print(__doc__)
