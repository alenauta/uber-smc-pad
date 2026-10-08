#!/usr/bin/env python3
"""Decode SMC-PAD presets (and the persisted global-state journal). Stdlib only, read-only.

Accepted inputs (auto-detected by size):
  * 512 KiB SPI flash dump (firmware/backup/flash_dump_*.bin): presets @0x77000, VM journal @0x27000
  * 28312-byte preset blob: Midi Suite's factory SPB.bin, or a BLE type-5 read of 0..0x6E98
  * 3539-byte single preset

Usage:
  python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin
  python3 tools/config/decode_presets.py DUMP --preset 5 --all-pads
  python3 tools/config/decode_presets.py DUMP --diff "Midi Suite.app/.../assets/bin/SPB.bin"
  python3 tools/config/decode_presets.py DUMP --globals

See docs/preset_format.md for the layout and the confidence level of each field.
"""
import argparse
import sys

PRESET_SIZE = 3539
N_PRESETS = 8
FLASH_PRESETS = 0x77000
FLASH_VM = 0x27000

BTN_OFF, BTN_SIZE, N_BTN = 0x000, 23, 5
ENC_OFF, ENC_SIZE, N_ENC = 0x073, 6, 16
PAD_OFF, PAD_SIZE, N_PADREC = 0x0D3, 26, 128
DEFAULT_OCTAVE_GROUP = 2          # records 32..47 = bank A at the default octave
BANK_B_GROUP = 7                  # records 112..127 = PAD BANK B

PRESET_NAMES = {1: "Performance", 2: "DAW"}
BTN_NAMES = ["btn1", "btn2", "btn3", "btn4", "btn5"]

# Type codes. '?' marks inferences that have not been confirmed by a capture yet.
PAD_TYPES = {0: "Note", 1: "CC-toggle?", 2: "CC", 3: "PgmChg?", 4: "MCU-note"}
BTN_TYPES = {2: "CC", 5: "MCU-note"}
ENC_MODES = {0: "CC-abs", 2: "MCU/PB?"}

NOTE_NAMES = "C C# D D# E F F# G G# A A# B".split()


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 2)   # 36 = C1 (Ableton/MPC convention)


def load(path):
    data = open(path, "rb").read()
    if len(data) == 0x80000:
        return data[FLASH_PRESETS:FLASH_PRESETS + PRESET_SIZE * N_PRESETS], data
    if len(data) >= PRESET_SIZE * N_PRESETS:
        return data[:PRESET_SIZE * N_PRESETS], None
    if len(data) == PRESET_SIZE:
        return data, None
    sys.exit("unrecognised input size %d" % len(data))


def presets_of(blob):
    return [blob[i * PRESET_SIZE:(i + 1) * PRESET_SIZE] for i in range(len(blob) // PRESET_SIZE)]


def hx(b):
    return " ".join("%02x" % x for x in b)


def fb(v):
    return "-" if v == 0xFF else str(v)


def decode_button(r):
    t, ch, num, lo, hi, sxlen = r[0:6]
    sx = r[6:6 + min(sxlen, 16)]
    return dict(type=t, ch=ch + 1, num=num, min=lo, max=hi, sysex=sx, feedback=r[22])


def decode_encoder(r):
    mode, t, ch, num, lo, hi = r
    return dict(mode=mode, type=t, ch=ch + 1, num=num, min=lo, max=hi)


def decode_pad(r):
    t, ch, num, lo, hi, R, G, B, fbk = r[0:9]
    return dict(type=t, ch=ch + 1, num=num, min=lo, max=hi, rgb=(R, G, B), feedback=fbk,
                extra=r[9:])


def fmt_pad(p):
    t = PAD_TYPES.get(p["type"], "type%d" % p["type"])
    what = "%-3d" % p["num"]
    if p["type"] == 0:
        what += " %-4s" % note_name(p["num"])
    else:
        what += "     "
    s = "%-10s ch%-2d %s %3d-%-3d #%02x%02x%02x fb=%s" % (
        t, p["ch"], what, p["min"], p["max"], *p["rgb"], fb(p["feedback"]))
    if any(p["extra"]):
        s += " extra=" + hx(p["extra"])
    return s


def print_preset(idx, p, all_pads=False):
    name = PRESET_NAMES.get(idx, "User")
    print("=== Preset %d (%s)" % (idx, name))
    for i in range(N_BTN):
        b = decode_button(p[BTN_OFF + i * BTN_SIZE:BTN_OFF + (i + 1) * BTN_SIZE])
        t = BTN_TYPES.get(b["type"], "type%d" % b["type"])
        sx = (" sysex=" + hx(b["sysex"])) if b["sysex"] else ""
        print("  %-5s %-8s ch%-2d num %-3d %3d-%-3d fb=%s%s" % (
            BTN_NAMES[i], t, b["ch"], b["num"], b["min"], b["max"], fb(b["feedback"]), sx))
    for bank in range(2):
        cells = []
        for i in range(8):
            e = decode_encoder(p[ENC_OFF + (bank * 8 + i) * ENC_SIZE:][:ENC_SIZE])
            m = ENC_MODES.get(e["mode"], "m%d" % e["mode"])
            cells.append("K%d %s ch%d cc%d" % (i + 1, m, e["ch"], e["num"]))
        print("  knobs bank %d: %s" % (bank + 1, " | ".join(cells)))
    recs = [decode_pad(p[PAD_OFF + i * PAD_SIZE:][:PAD_SIZE]) for i in range(N_PADREC)]
    groups = range(8) if all_pads else (DEFAULT_OCTAVE_GROUP, BANK_B_GROUP)
    for g in groups:
        label = {DEFAULT_OCTAVE_GROUP: "bank A (default octave)", BANK_B_GROUP: "bank B"}.get(
            g, "bank A octave group %d" % g)
        print("  pads %s  [records %d-%d]" % (label, g * 16, g * 16 + 15))
        for i in range(16):
            print("    pad%-2d %s" % (i + 1, fmt_pad(recs[g * 16 + i])))


def print_diff(blob, ref):
    for k, (a, b) in enumerate(zip(presets_of(blob), presets_of(ref)), 1):
        diffs = [i for i in range(PRESET_SIZE) if a[i] != b[i]]
        if not diffs:
            print("preset %d: identical to reference" % k)
            continue
        print("preset %d: %d byte(s) differ" % (k, len(diffs)))
        seen = set()
        for i in diffs:
            if i >= PAD_OFF:
                r = (i - PAD_OFF) // PAD_SIZE
                key = ("pad", r)
                if key in seen:
                    continue
                seen.add(key)
                o = PAD_OFF + r * PAD_SIZE
                g, n = divmod(r, 16)
                print("  pad record %d (group %d, pad%d)" % (r, g, n + 1))
                print("    ref: %s" % fmt_pad(decode_pad(b[o:o + PAD_SIZE])))
                print("    now: %s" % fmt_pad(decode_pad(a[o:o + PAD_SIZE])))
            elif i >= ENC_OFF:
                r = (i - ENC_OFF) // ENC_SIZE
                if ("enc", r) not in seen:
                    seen.add(("enc", r))
                    o = ENC_OFF + r * ENC_SIZE
                    print("  knob %d: ref %s -> now %s" % (r + 1, hx(b[o:o + 6]), hx(a[o:o + 6])))
            else:
                r = i // BTN_SIZE
                if ("btn", r) not in seen:
                    seen.add(("btn", r))
                    o = r * BTN_SIZE
                    print("  button %d: ref %s\n            now %s" % (
                        r + 1, hx(b[o:o + 23]), hx(a[o:o + 23])))


# ---- persisted global state (VM journal at 0x27000) -------------------------------------------
# 18-byte entries: 00 a3 3a | 12-byte payload | chk | 2 bytes. The payload equals the first 12
# bytes returned by a BLE type-4 read. Field names below are hypotheses (see preset_format.md).
GLOB_FIELDS = [
    (0, 2, "tempo? (u16 LE, BPM)"),
    (2, 1, "swing? (%)"),
    (3, 1, "b3 (rate/curve?)"),
    (4, 1, "b4 (latch?)"),
    (5, 1, "b5 (sync?)"),
    (6, 1, "b6"),
    (7, 1, "b7 (transpose?)"),
    (8, 1, "octave group? (default 2)"),
    (9, 1, "b9"),
    (10, 1, "active preset? (0-based)"),
    (11, 1, "pad bank? (0=A 1=B)"),
]


def vm_journal(flash):
    out, o = [], FLASH_VM
    while flash[o:o + 3] == b"\x00\xa3\x3a":
        out.append((o, flash[o + 3:o + 15], flash[o + 15:o + 18]))
        o += 18
    return out


def decode_glob(payload):
    parts = []
    for off, n, name in GLOB_FIELDS:
        v = int.from_bytes(payload[off:off + n], "little")
        parts.append("%s=%d" % (name.split(" ")[0], v))
    return " ".join(parts)


def print_globals(flash):
    j = vm_journal(flash)
    print("VM journal @0x%X: %d entries (oldest first)" % (FLASH_VM, len(j)))
    for o, payload, tail in j:
        print("  %06x  %s  | %s" % (o, hx(payload), decode_glob(payload)))
    if j:
        print("current (last entry):")
        for off, n, name in GLOB_FIELDS:
            print("  %-28s %d" % (name, int.from_bytes(j[-1][1][off:off + n], "little")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--preset", type=int, action="append", help="1-8 (repeatable); default all")
    ap.add_argument("--all-pads", action="store_true", help="show all 8 pad record groups")
    ap.add_argument("--diff", metavar="REF", help="compare against a reference (e.g. SPB.bin)")
    ap.add_argument("--globals", action="store_true", help="decode the VM global-state journal")
    a = ap.parse_args()

    blob, flash = load(a.input)
    if a.diff:
        print_diff(blob, load(a.diff)[0])
        return
    if a.globals:
        if flash is None:
            sys.exit("--globals needs a full 512 KiB flash dump")
        print_globals(flash)
        return
    ps = presets_of(blob)
    for k in (a.preset or range(1, len(ps) + 1)):
        print_preset(k, ps[k - 1], a.all_pads)


if __name__ == "__main__":
    main()
