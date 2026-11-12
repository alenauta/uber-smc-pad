#!/usr/bin/env python3
"""Prepare stock bootloader, OTA loader, and keyfile from the user's own official .fwsc file.

  python3 firmware/builder/prepare_stock.py path/to/SMC-PAD_005.fwsc [--work-dir /workspace]

Uses upstream kagaimiq/jl-misctools (fetched into $SMCPAD_WORK/jl-misctools by setup_toolchain.py)
to unpack the user-supplied official update file locally, reads the image's chipkey metadata from
the unpacked jlfw.yaml, generates $SMCPAD_WORK/keys/smcpad.key via keyfgen.py, and places the
stock bootloader (uboot.boot) and RCSP OTA loader (ota.bin) into $SMCPAD_WORK/loaders/.
"""
import argparse
import os
import shutil
import subprocess
import sys
import yaml


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fwsc", help="Path to your downloaded official SMC-PAD_005.fwsc")
    ap.add_argument("--work-dir", default=os.environ.get("SMCPAD_WORK", "/workspace"))
    a = ap.parse_args()

    if not os.path.isfile(a.fwsc):
        sys.exit(f"Stock firmware file not found: {a.fwsc}")

    misctools = os.path.join(a.work_dir, "jl-misctools")
    fwunpack = os.path.join(misctools, "firmware", "fwunpack_newfw.py")
    keyfgen = os.path.join(misctools, "keyfile", "keyfgen.py")
    if not os.path.isfile(fwunpack) or not os.path.isfile(keyfgen):
        sys.exit(f"jl-misctools missing under {misctools}; run firmware/builder/setup_toolchain.py first")

    stock_out = os.path.join(a.work_dir, "stock_unpacked")
    shutil.rmtree(stock_out, ignore_errors=True)
    subprocess.check_call([sys.executable, fwunpack, "--dirname", stock_out, a.fwsc])

    meta_path = os.path.join(stock_out, "jlfw.yaml")
    if not os.path.isfile(meta_path):
        sys.exit(f"Expected metadata file missing: {meta_path}")
    with open(meta_path, encoding="utf-8") as f:
        meta = yaml.safe_load(f)

    chipkey = meta.get("chip-key", meta.get("chipkey"))
    if chipkey is None:
        sys.exit(f"No chip-key field found in {meta_path}")
    chipkey_int = int(chipkey, 0) if isinstance(chipkey, str) else int(chipkey)

    keys_dir = os.path.join(a.work_dir, "keys")
    os.makedirs(keys_dir, exist_ok=True)
    keyfile = os.path.join(keys_dir, "smcpad.key")
    subprocess.check_call([sys.executable, keyfgen, f"0x{chipkey_int:04X}", keyfile])

    loaders_dir = os.path.join(a.work_dir, "loaders")
    os.makedirs(loaders_dir, exist_ok=True)
    uboot_src = os.path.join(stock_out, "top", "uboot.boot")
    if os.path.isfile(uboot_src):
        shutil.copyfile(uboot_src, os.path.join(loaders_dir, "uboot.boot"))

    # Locate ota.bin unpacked from the outer UFW container
    for candidate in (
        os.path.join(stock_out, "ufw", "ota.bin"),
        os.path.join(stock_out, "ota.bin"),
    ):
        if os.path.isfile(candidate):
            shutil.copyfile(candidate, os.path.join(loaders_dir, "ota_sdk_v1.0.0.bin"))
            break

    print(f"[prepare] Keyfile written to {keyfile}")
    print(f"[prepare] Unpacked stock components in {stock_out} and {loaders_dir}")


if __name__ == "__main__":
    main()
