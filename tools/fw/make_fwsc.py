#!/usr/bin/env python3
"""Wrap a JieLi .ufw update into a Sinco .fwsc (the file SincoOTA flashes), or unwrap one.

Format (derived from SMC-PAD_005.fwsc, see docs/ble_config.md "Flashing"):
  - the first 20 blocks of 0x30 bytes each carry 0x2F UFW bytes + 1 tag byte, the rest is the
    plain UFW (so .fwsc size = .ufw size + 20);
  - tag[i] = ord(name[i]) + i + 1 for the file name "<DEVICE>_<VVV>", padded with 0x40 '@'.
    The tag is not tied to the payload.

Usage:
  make_fwsc.py --selftest                        unwrap + re-wrap the stock file byte-identically
  make_fwsc.py in.ufw SMC-PAD_901.fwsc           wrap (name = output file stem, or --name)
  make_fwsc.py --unwrap in.fwsc out.ufw          unwrap
  make_fwsc.py --info file.fwsc                  show the decoded tag name
Stdlib only.
"""
import argparse
import pathlib
import re
import sys

BLK, DATA, N = 0x30, 0x2F, 20
ROOT = pathlib.Path(__file__).resolve().parents[2]


def encode_tag(name):
    b = name.encode("ascii")
    if len(b) > N:
        raise ValueError("name longer than 20 characters")
    return bytes((c + i + 1) & 0xFF for i, c in enumerate(b)) + b"\x40" * (N - len(b))


def decode_tag(tag):
    """'@' padding decodes to printable characters too, so cut after "<NAME>_<3 digits>"."""
    s = bytes((c - i - 1) & 0xFF for i, c in enumerate(tag)).decode("latin-1")
    m = re.match(r"(.+?_\d{3})", s)
    return m.group(1) if m else s


def wrap(ufw, name):
    if len(ufw) < N * DATA:
        raise ValueError("ufw too short")
    tag = encode_tag(name)
    out = bytearray()
    for i in range(N):
        out += ufw[i * DATA:(i + 1) * DATA] + tag[i:i + 1]
    return bytes(out + ufw[N * DATA:])


def unwrap(fwsc):
    ufw, tag = bytearray(), bytearray()
    for i in range(N):
        blk = fwsc[i * BLK:(i + 1) * BLK]
        ufw += blk[:DATA]
        tag.append(blk[DATA])
    return bytes(ufw + fwsc[N * BLK:]), bytes(tag)


def selftest():
    ok = True
    for v in ("005",):
        p = ROOT / "firmware" / "original" / f"SMC-PAD_{v}.fwsc"
        if not p.exists():
            print(f"[SKIP] {p} not found")
            continue
        d = p.read_bytes()
        ufw, tag = unwrap(d)
        good = tag == encode_tag(p.stem) and wrap(ufw, p.stem) == d and len(ufw) + N == len(d)
        print(f"{p.name}: tag {tag!r} -> {p.stem!r}, ufw {len(ufw)} B, re-wrap "
              f"{'byte-identical OK' if good else 'FAIL'}")
        ok &= good
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--unwrap", action="store_true", help="input .fwsc -> output .ufw")
    ap.add_argument("--info", action="store_true", help="print the tag of an input .fwsc")
    ap.add_argument("--name", help="tag name, default = output file stem (e.g. SMC-PAD_901)")
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.input:
        ap.error("input missing")
    src = pathlib.Path(a.input)
    if a.info:
        ufw, tag = unwrap(src.read_bytes())
        print(f"{src.name}: tag name {decode_tag(tag)!r} (raw {tag.hex(' ')}), ufw {len(ufw)} B")
        return
    if not a.output:
        ap.error("output missing")
    dst = pathlib.Path(a.output)
    if a.unwrap:
        dst.write_bytes(unwrap(src.read_bytes())[0])
    else:
        name = a.name or dst.stem
        dst.write_bytes(wrap(src.read_bytes(), name))
        print(f"wrote {dst} ({name})")


if __name__ == "__main__":
    main()
