#!/usr/bin/env python3
"""Generate a JieLi (BTstack-style) binary GATT profile header from a JSON spec.

The vendor tool (gatt_inc_generator.exe) is Windows-only; this reproduces its output format:

    entry = size u16 LE, flags u16 LE, handle u16 LE, uuid (2 or 16 bytes LE), value...
    end   = 00 00

* primary service   : flags READ,            uuid 0x2800, value = service uuid
* characteristic    : flags READ,            uuid 0x2803, value = props u8, value handle u16, uuid
* value             : flags = props | DYNAMIC(0x100) | UUID128(0x200), uuid = characteristic uuid
* CCC (notify/ind.) : flags READ|WRITE|DYNAMIC (0x010a), uuid 0x2902, value 00 00

Handle defines follow the vendor naming: ATT_CHARACTERISTIC_<uuid>_<nn>_VALUE_HANDLE and
..._CLIENT_CONFIGURATION_HANDLE, nn = occurrence of that uuid (01, 02 ...); 16-bit uuids in
lower case, 128-bit uuids upper case with '_' instead of '-'.

Spec (JSON):
    {"name": "smcpad_profile_data",
     "services": [
        {"uuid": "1800", "chars": [{"uuid": "2a00", "props": ["READ", "DYNAMIC"]}]},
        {"uuid": "ae00", "if": "RCSP_BTMATE_EN", "start_handle": 128, "chars": [...]},
        ...]}
    props: READ WRITE_WITHOUT_RESPONSE WRITE NOTIFY INDICATE DYNAMIC AUTHENTICATION_REQUIRED
    AUTHORIZATION_REQUIRED; optional "value": "text" (static value, only without DYNAMIC).

Usage: gen_jl_profile.py spec.json out.h        (Python stdlib only)
"""
import json
import sys

PROPS = {"BROADCAST": 0x01, "READ": 0x02, "WRITE_WITHOUT_RESPONSE": 0x04, "WRITE": 0x08,
         "NOTIFY": 0x10, "INDICATE": 0x20}
FLAG_DYNAMIC = 0x100
FLAG_UUID128 = 0x200
# value-entry-only flags (not in the declaration's props byte)
VALUE_FLAGS = {"AUTHENTICATION_REQUIRED": 0x400, "AUTHORIZATION_REQUIRED": 0x800}


def uuid_bytes(u):
    """'2a00' -> 2 bytes LE; '03B80E5A-EDE8-4B33-A751-6CE34EC4C700' -> 16 bytes LE."""
    h = u.replace("-", "")
    if len(h) == 4:
        v = int(h, 16)
        return bytes([v & 0xFF, v >> 8])
    if len(h) == 32:
        return bytes.fromhex(h)[::-1]
    raise ValueError("bad uuid %r" % u)


def uuid_key(u):
    h = u.replace("-", "")
    return h.lower() if len(h) == 4 else u.upper().replace("-", "_")


def u16(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def entry(flags, handle, uuid_b, value):
    body = u16(flags) + u16(handle) + uuid_b + value
    return u16(len(body) + 2) + body


def build(spec):
    """Returns (blocks, defines). blocks = [(cond or None, [(comment, bytes)])]."""
    blocks, defines, seen = [], [], {}
    handle = 1
    for svc in spec["services"]:
        if "start_handle" in svc:
            if svc["start_handle"] < handle and not svc.get("if"):
                raise ValueError("start_handle goes backwards")
            handle = svc["start_handle"]
        items = []
        items.append(("0x%04x PRIMARY_SERVICE  %s" % (handle, svc["uuid"]),
                      entry(PROPS["READ"], handle, uuid_bytes("2800"), uuid_bytes(svc["uuid"]))))
        handle += 1
        for ch in svc.get("chars", []):
            props = 0
            vflags = 0
            dynamic = False
            for p in ch["props"]:
                if p == "DYNAMIC":
                    dynamic = True
                elif p in VALUE_FLAGS:
                    vflags |= VALUE_FLAGS[p]
                else:
                    props |= PROPS[p]
            ub = uuid_bytes(ch["uuid"])
            key = uuid_key(ch["uuid"])
            seen[key] = seen.get(key, 0) + 1
            n = seen[key]
            pstr = " | ".join(ch["props"])
            decl_h, val_h = handle, handle + 1
            items.append(("0x%04x CHARACTERISTIC %s %s" % (decl_h, ch["uuid"], pstr),
                          entry(PROPS["READ"], decl_h, uuid_bytes("2803"),
                                bytes([props]) + u16(val_h) + ub)))
            flags = props | vflags | (FLAG_DYNAMIC if dynamic else 0) | (FLAG_UUID128 if len(ub) == 16 else 0)
            value = b""
            if "value" in ch:
                if dynamic:
                    raise ValueError("static value with DYNAMIC: %s" % ch["uuid"])
                value = ch["value"].encode("utf-8")
            items.append(("0x%04x VALUE %s %s" % (val_h, ch["uuid"], pstr),
                          entry(flags, val_h, ub, value)))
            defines.append((svc.get("if"), "ATT_CHARACTERISTIC_%s_%02d_VALUE_HANDLE" % (key, n), val_h))
            handle += 2
            if props & (PROPS["NOTIFY"] | PROPS["INDICATE"]):
                items.append(("0x%04x CLIENT_CHARACTERISTIC_CONFIGURATION" % handle,
                              entry(0x010A, handle, uuid_bytes("2902"), b"\x00\x00")))
                defines.append((svc.get("if"),
                                "ATT_CHARACTERISTIC_%s_%02d_CLIENT_CONFIGURATION_HANDLE" % (key, n), handle))
                handle += 1
        blocks.append((svc.get("if"), items))
    return blocks, defines


def flat_bytes(blocks, enabled=()):
    """All profile bytes (with END), including conditional blocks whose condition is enabled."""
    out = b""
    for cond, items in blocks:
        if cond is None or cond in enabled:
            out += b"".join(b for _, b in items)
    return out + b"\x00\x00"


def render(spec, src_name="spec"):
    blocks, defines = build(spec)
    name = spec.get("name", "profile_data")
    guard = "_%s_H" % name.upper()
    L = ["// GENERATED by tools/gatt/gen_jl_profile.py from %s - do not edit." % src_name,
         "// binary representation",
         "// attribute size in bytes (16), flags(16), handle (16), uuid (16/128), value(...)",
         "#ifndef %s" % guard, "#define %s" % guard, "", "#include <stdint.h>", '#include "app_config.h"', "",
         "static const uint8_t %s[] = {" % name]
    for cond, items in blocks:
        if cond:
            L.append("#if %s" % cond)
        for comment, b in items:
            L.append("    // " + comment)
            L.append("    " + ", ".join("0x%02x" % x for x in b) + ",")
        if cond:
            L.append("#endif")
        L.append("")
    L += ["    // END", "    0x00, 0x00,", "};", "", "// characteristics <--> handles"]
    cur = None
    for cond, dname, h in defines:
        if cond != cur:
            if cur:
                L.append("#endif")
            if cond:
                L.append("#if %s" % cond)
            cur = cond
        L.append("#define %s 0x%04x" % (dname, h))
    if cur:
        L.append("#endif")
    L += ["", "#endif", ""]
    return "\n".join(L)


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    with open(argv[1], encoding="utf-8") as f:
        spec = json.load(f)
    text = render(spec, argv[1].replace("\\", "/").split("/")[-1])
    with open(argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote", argv[2])


if __name__ == "__main__":
    main(sys.argv)
