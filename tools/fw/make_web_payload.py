#!/usr/bin/env python3
"""Generate the static client-side web patcher bundle (docs/payload/smcpad_925.bin).

Extracts only clean-room custom firmware sections from a locally built custom .fwsc image
(unscrambling the 0x1000+ application area using the container seed read dynamically from the
user's local stock SMC-PAD_005.fwsc). Zero vendor bootloader/updater bytes and zero seed bytes
are stored in the output bundle.

Usage:
  python3 tools/fw/make_web_payload.py \\
      firmware/original/SMC-PAD_005.fwsc \\
      firmware/build/SMC-PAD_925.fwsc \\
      docs/payload/smcpad_925.bin
Stdlib only.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_fwsc  # noqa: E402

MAGIC = b"SMCP925\x01"


def jl_crc16(data: bytes, init: int = 0) -> int:
    crc = init & 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def jl_lfsr_xor(buf: bytearray, off: int, size: int, seed: int = 0xFFFF) -> int:
    k = seed & 0xFFFF
    for i in range(size):
        buf[off + i] ^= k & 0xFF
        k = ((k << 1) ^ (0x1021 if (k & 0x8000) else 0)) & 0xFFFF
    return k


def jl_sfc_xor(buf: bytearray, off: int, size: int, base: int, seed: int, block: int = 32) -> None:
    for i in range(0, size, block):
        chunk = min(size - i, block)
        jl_lfsr_xor(buf, off + i, chunk, (seed ^ ((off + i - base) >> 2)) & 0xFFFF)


def read_container_seed(table32: bytes) -> int:
    if len(table32) < 32:
        raise ValueError("seed table shorter than 32 bytes")
    s = sum(table32[:16]) & 0xFF
    if s >= 0xE0:
        s = 0xAA
    elif s <= 0x10:
        s = 0x55
    seed = 0
    for i in range(16):
        if (table32[16 + i] ^ table32[15 - i]) < s:
            seed |= 1 << i
    return seed


def parse_ufw(u: bytes):
    hdr = bytearray(u[:0x400])
    jl_lfsr_xor(hdr, 0, 0x40, 0xFFFF)
    hdr_crc, list_crc, imgsize, numents, wa3, wa4, chipname = struct.unpack_from(
        "<HHIHHI48s", hdr, 0
    )
    if hdr_crc != jl_crc16(bytes(hdr[2:0x40])):
        raise ValueError("UFW header CRC mismatch")
    if list_crc != jl_crc16(u[0x40 : 0x40 + numents * 0x50]):
        raise ValueError("UFW entry list CRC mismatch")
    ents = []
    for i in range(numents):
        off = 0x40 + i * 0x50
        jl_lfsr_xor(hdr, off, 0x50, 0xFFFF)
        etype, eidx, edcrc, ewa1, eoff, esz, esz2, ewa2, ename = struct.unpack_from(
            "<HHHHIII44s16s", hdr, off
        )
        nm = ename.split(b"\x00", 1)[0].decode("ascii")
        ents.append(
            {
                "name": nm,
                "type": etype,
                "index": eidx,
                "dcrc": edcrc,
                "wa1": ewa1,
                "offset": eoff,
                "size": esz,
                "size2": esz2,
                "wa2_hex": bytes(ewa2).hex(),
                "raw_name_hex": bytes(ename).hex(),
                "data": bytes(u[eoff : eoff + esz]),
            }
        )
    return (imgsize, wa3, wa4, bytes(chipname)), ents


def build_bundle(stock_fwsc_path: pathlib.Path, custom_fwsc_path: pathlib.Path) -> bytes:
    raw_stock = stock_fwsc_path.read_bytes()
    raw_custom = custom_fwsc_path.read_bytes()

    u_stock, stock_tag = make_fwsc.unwrap(raw_stock)
    u_custom, custom_tag = make_fwsc.unwrap(raw_custom)
    stock_tag_name = make_fwsc.decode_tag(stock_tag)
    custom_tag_name = make_fwsc.decode_tag(custom_tag)
    if stock_tag_name != "SMC-PAD_005":
        raise ValueError(f"Expected stock tag SMC-PAD_005, got {stock_tag_name!r}")

    _, ents_stock = parse_ufw(u_stock)
    meta_custom, ents_custom = parse_ufw(u_custom)
    map_stock = {e["name"]: e for e in ents_stock}
    map_custom = {e["name"]: e for e in ents_custom}

    f_stock = map_stock["flash.bin"]["data"]
    f2_stock = map_stock["flash2.bin"]["data"]
    f_custom = map_custom["flash.bin"]["data"]
    f2_custom = map_custom["flash2.bin"]["data"]

    # Sanity check: custom build must share stock's 4 KB flash2 header, ota.bin, and tail.bin
    if f2_custom[:0x1000] != f2_stock[:0x1000]:
        raise ValueError("Custom flash2.bin[0:0x1000] does not match stock SMC-PAD_005")
    if map_custom["ota.bin"]["data"] != map_stock["ota.bin"]["data"]:
        raise ValueError("Custom ota.bin does not match stock SMC-PAD_005")
    if map_custom["tail.bin"]["data"] != map_stock["tail.bin"]["data"]:
        raise ValueError("Custom tail.bin does not match stock SMC-PAD_005")

    seed = read_container_seed(f_stock[0xEEC:0xF0C])
    plain_app = bytearray(f2_custom[0x1000:])
    jl_sfc_xor(plain_app, 0, len(plain_app), 0x1000, seed)

    named_blobs = [
        ("plain_app", bytes(plain_app)),
        ("jlfs_top", f_custom[:0xA0]),
        ("ini_tail", f_custom[0xF0E:0x1000]),
        ("isd_config.ini", map_custom["isd_config.ini"]["data"]),
        ("script.ver", map_custom["script.ver"]["data"]),
        ("blimit.bin", map_custom["blimit.bin"]["data"]),
    ]

    blob_buf = bytearray()
    chunk_meta = {}
    for k, b in named_blobs:
        chunk_meta[k] = {"offset": len(blob_buf), "size": len(b)}
        blob_buf.extend(b)

    last_end = ents_custom[-1]["offset"] + ents_custom[-1]["size"]
    imgsize, wa3, wa4, chipname = meta_custom

    manifest = {
        "format": 1,
        "build_name": custom_tag_name,
        "stock_name": "SMC-PAD_005.fwsc",
        "stock_size": len(raw_stock),
        "stock_sha256": hashlib.sha256(raw_stock).hexdigest(),
        "expected_flash2_sha256": hashlib.sha256(f2_custom).hexdigest(),
        "expected_ota_sha256": hashlib.sha256(map_stock["ota.bin"]["data"]).hexdigest(),
        "ufw_size": len(u_custom),
        "ufw_header": {
            "imgsize": imgsize,
            "wa3": wa3,
            "wa4": wa4,
            "chipname_hex": chipname.hex(),
        },
        "ufw_trailer": {
            "offset": last_end,
            "hex": u_custom[last_end:].hex(),
        },
        "flash1_size": len(f_custom),
        "flash2_size": len(f2_custom),
        "entries": [
            {
                "name": e["name"],
                "type": e["type"],
                "index": e["index"],
                "dcrc": e["dcrc"],
                "wa1": e["wa1"],
                "offset": e["offset"],
                "size": e["size"],
                "size2": e["size2"],
                "wa2_hex": e["wa2_hex"],
                "raw_name_hex": e["raw_name_hex"],
            }
            for e in ents_custom
        ],
        "chunks": chunk_meta,
    }

    manifest_bytes = json.dumps(manifest, separators=(",", ":"), sort_keys=True).encode("utf-8")
    out = bytearray()
    out.extend(MAGIC)
    out.extend(struct.pack("<I", len(manifest_bytes)))
    out.extend(manifest_bytes)
    out.extend(blob_buf)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stock_fwsc", type=pathlib.Path, help="Official SMC-PAD_005.fwsc")
    ap.add_argument("custom_fwsc", type=pathlib.Path, help="Locally built custom SMC-PAD_9xx.fwsc")
    ap.add_argument("output", type=pathlib.Path, help="Output bundle path (e.g. docs/payload/smcpad_925.bin)")
    args = ap.parse_args()

    bundle = build_bundle(args.stock_fwsc, args.custom_fwsc)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    print(
        f"Wrote {args.output} ({len(bundle):,} bytes, SHA-256 {hashlib.sha256(bundle).hexdigest()[:16]}…)"
    )


if __name__ == "__main__":
    main()
