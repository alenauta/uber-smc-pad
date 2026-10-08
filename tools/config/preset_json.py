#!/usr/bin/env python3
"""Lossless JSON <-> binary converter for SMC-PAD presets (offline; never talks to the device).

  export: binary (flash dump / SPB.bin / type-5 read / single preset) -> editable JSON
  build:  JSON -> 28312-byte preset block (8 x 3539 B), same layout as SPB.bin / BLE type 5

  python3 tools/config/preset_json.py export firmware/backup/flash_dump_1.bin presets.json
  # edit presets.json ...
  python3 tools/config/preset_json.py build presets.json presets.bin [--ref firmware/backup/flash_dump_1.bin]
  python3 tools/config/preset_json.py selftest firmware/backup/flash_dump_1.bin

Unknown bytes are preserved ("extra"/"sysex_pad"), so export->build is byte-identical.
Field meanings and confidence: docs/preset_format.md. "*_name" fields are informational only and
are ignored by build; edit the numeric fields.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode_presets as dp  # noqa: E402  (shared constants + loader)

GROUP_LABELS = ["bankA_oct%d" % g for g in range(7)] + ["bankB"]


def hexs(b):
    return " ".join("%02x" % x for x in b)


def unhex(s):
    return bytes.fromhex(s.replace(" ", "")) if s else b""


def fbv(v):
    return None if v == 0xFF else v


def fbb(v):
    return 0xFF if v is None else v


# ---------------------------------------------------------------- export
def export_preset(slot, p):
    out = {"slot": slot, "slot_kind": dp.PRESET_NAMES.get(slot, "User"),
           "buttons": [], "knobs": [], "pads": {}}
    for i in range(dp.N_BTN):
        r = p[dp.BTN_OFF + i * dp.BTN_SIZE:][:dp.BTN_SIZE]
        sxlen = r[5]
        n = min(sxlen, 16)
        btn = {"type": r[0], "type_name": dp.BTN_TYPES.get(r[0], "?"), "ch": r[1] + 1,
               "num": r[2], "min": r[3], "max": r[4], "sysex": hexs(r[6:6 + n]),
               "feedback": fbv(r[22])}
        if sxlen > 16:
            btn["sysex_len"] = sxlen
        if any(r[6 + n:22]):
            btn["sysex_pad"] = hexs(r[6 + n:22])
        out["buttons"].append(btn)
    for i in range(dp.N_ENC):
        r = p[dp.ENC_OFF + i * dp.ENC_SIZE:][:dp.ENC_SIZE]
        out["knobs"].append({"bank": i // 8 + 1, "knob": i % 8 + 1, "mode": r[0],
                             "mode_name": dp.ENC_MODES.get(r[0], "?"), "type": r[1],
                             "ch": r[2] + 1, "cc": r[3], "min": r[4], "max": r[5]})
    for g, label in enumerate(GROUP_LABELS):
        pads = []
        for i in range(16):
            r = p[dp.PAD_OFF + (g * 16 + i) * dp.PAD_SIZE:][:dp.PAD_SIZE]
            pad = {"pad": i + 1, "type": r[0], "type_name": dp.PAD_TYPES.get(r[0], "?"),
                   "ch": r[1] + 1, "num": r[2], "min": r[3], "max": r[4],
                   "rgb": "#%02x%02x%02x" % (r[5], r[6], r[7]), "feedback": fbv(r[8])}
            if any(r[9:]):
                pad["extra"] = hexs(r[9:])
            pads.append(pad)
        out["pads"][label] = pads
    return out


# ---------------------------------------------------------------- build
class BuildError(Exception):
    pass


def u7(v, what):
    if not (isinstance(v, int) and 0 <= v <= 127):
        raise BuildError("%s must be 0..127, got %r" % (what, v))
    return v


def u8(v, what):
    if not (isinstance(v, int) and 0 <= v <= 255):
        raise BuildError("%s must be 0..255, got %r" % (what, v))
    return v


def chan(v, what):
    if not (isinstance(v, int) and 1 <= v <= 16):
        raise BuildError("%s must be 1..16, got %r" % (what, v))
    return v - 1


def build_preset(obj):
    p = bytearray(dp.PRESET_SIZE)
    s = "slot %s" % obj.get("slot")
    if len(obj["buttons"]) != dp.N_BTN or len(obj["knobs"]) != dp.N_ENC:
        raise BuildError("%s: need %d buttons and %d knobs" % (s, dp.N_BTN, dp.N_ENC))
    for i, b in enumerate(obj["buttons"]):
        w = "%s button %d" % (s, i + 1)
        sx = unhex(b.get("sysex", ""))
        pad = unhex(b.get("sysex_pad", ""))
        if len(sx) > 16:
            raise BuildError("%s: sysex longer than 16 bytes" % w)
        body = (sx + pad).ljust(16, b"\0")
        if len(body) != 16:
            raise BuildError("%s: sysex + sysex_pad exceed 16 bytes" % w)
        rec = bytes([u8(b["type"], w + " type"), chan(b["ch"], w + " ch"), u7(b["num"], w + " num"),
                     u7(b["min"], w + " min"), u7(b["max"], w + " max"),
                     u8(b.get("sysex_len", len(sx)), w + " sysex_len")]) + body + \
            bytes([u8(fbb(b.get("feedback")), w + " feedback")])
        p[dp.BTN_OFF + i * dp.BTN_SIZE:dp.BTN_OFF + (i + 1) * dp.BTN_SIZE] = rec
    for i, k in enumerate(obj["knobs"]):
        w = "%s knob %d" % (s, i + 1)
        rec = bytes([u8(k["mode"], w + " mode"), u8(k["type"], w + " type"), chan(k["ch"], w + " ch"),
                     u7(k["cc"], w + " cc"), u7(k["min"], w + " min"), u7(k["max"], w + " max")])
        p[dp.ENC_OFF + i * dp.ENC_SIZE:dp.ENC_OFF + (i + 1) * dp.ENC_SIZE] = rec
    for g, label in enumerate(GROUP_LABELS):
        pads = obj["pads"][label]
        if len(pads) != 16:
            raise BuildError("%s %s: need 16 pads" % (s, label))
        for i, q in enumerate(pads):
            w = "%s %s pad %d" % (s, label, i + 1)
            rgb = q["rgb"].lstrip("#")
            if len(rgb) != 6:
                raise BuildError("%s: rgb must be #rrggbb" % w)
            extra = unhex(q.get("extra", "")).ljust(17, b"\0")
            if len(extra) != 17:
                raise BuildError("%s: extra must be <= 17 bytes" % w)
            rec = bytes([u8(q["type"], w + " type"), chan(q["ch"], w + " ch"), u7(q["num"], w + " num"),
                         u7(q["min"], w + " min"), u7(q["max"], w + " max")]) + bytes.fromhex(rgb) + \
                bytes([u8(fbb(q.get("feedback")), w + " feedback")]) + extra
            o = dp.PAD_OFF + (g * 16 + i) * dp.PAD_SIZE
            p[o:o + dp.PAD_SIZE] = rec
    return bytes(p)


def build_all(doc):
    presets = sorted(doc["presets"], key=lambda x: x["slot"])
    if [x["slot"] for x in presets] != list(range(1, dp.N_PRESETS + 1)):
        raise BuildError("JSON must contain slots 1..8 exactly once")
    return b"".join(build_preset(x) for x in presets)


# ---------------------------------------------------------------- CLI
def cmd_export(a):
    blob, _ = dp.load(a.input)
    doc = {"format": "smc-pad-presets/1", "source": os.path.basename(a.input),
           "presets": [export_preset(k + 1, p) for k, p in enumerate(dp.presets_of(blob))]}
    with open(a.output, "w") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    print("wrote %s (%d presets)" % (a.output, len(doc["presets"])))


def cmd_build(a):
    doc = json.load(open(a.input))
    try:
        blob = build_all(doc)
    except (BuildError, KeyError) as e:
        sys.exit("build error: %s" % e)
    if os.path.exists(a.output) and not a.force:
        sys.exit("%s exists (use --force)" % a.output)
    open(a.output, "wb").write(blob)
    print("wrote %s (%d bytes)" % (a.output, len(blob)))
    if a.ref:
        dp.print_diff(blob, dp.load(a.ref)[0])


def cmd_selftest(a):
    blob, _ = dp.load(a.input)
    doc = {"presets": [export_preset(k + 1, p) for k, p in enumerate(dp.presets_of(blob))]}
    doc = json.loads(json.dumps(doc))
    rebuilt = build_all(doc)
    ok = rebuilt == blob
    print("round-trip %s: %s" % (a.input, "OK (byte-identical)" if ok else "MISMATCH"))
    if not ok:
        bad = [i for i in range(len(blob)) if blob[i] != rebuilt[i]]
        print("  first differing offsets:", [hex(i) for i in bad[:10]])
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("input")
    e.add_argument("output")
    e.set_defaults(fn=cmd_export)
    b = sub.add_parser("build")
    b.add_argument("input")
    b.add_argument("output")
    b.add_argument("--ref", help="print a diff against this reference")
    b.add_argument("--force", action="store_true")
    b.set_defaults(fn=cmd_build)
    t = sub.add_parser("selftest")
    t.add_argument("input")
    t.set_defaults(fn=cmd_selftest)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
