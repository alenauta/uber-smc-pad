# Flash Backup Procedure – SMC-PAD

Before flashing any custom firmware, always take two independent 512 KB flash backups of your own unit over **BLE** (`AE40` service, read type `0`) using [`tools/ble/sinco_read.swift --dump`](../../tools/ble/sinco_read.swift), and verify that their SHA-256 checksums match.

| File (gitignored) | Size | Notes |
|---|---|---|
| `flash_dump_1.bin` | `524,288 B` (`0x00000–0x7FFFF`) | First full pass (~95 s: `1024 × 512 B` reads) |
| `flash_dump_2.bin` | `524,288 B` (`0x00000–0x7FFFF`) | Second pass — verify `sha256sum` matches `flash_dump_1.bin`, then mark `chmod 444` |

## Flash Map (`512 KB`)
Because BLE read type `0` reads through the running firmware's XIP flash mapping, the `0x01000–0x25FFF` code region is returned in its unscrambled XIP form rather than raw physical SPI NOR bytes:

| Region | Contents |
|---|---|
| `0x00000–0x00FFF` | Flash header & JLFS directory table |
| `0x01000–0x25FFF` | Stock application code area (`PRCT`, returned in XIP form over BLE) |
| `0x26000–0x74FFF` | JieLi `VM` settings journal (header `55 AA AA 55` at `0x26000`, journal at `0x27000`) |
| `0x75000–0x75FFF` | `BTIF` (Bluetooth controller trim / pairing info) |
| `0x76000–0x76FFF` | `EXIF` (OTA scratch sector) |
| `0x77000–0x7DFFF` | **`USRFLASH`** — the 8 user presets (`8 × 3,539 B`, plain binary) |
| `0x7E000–0x7EFFF` | **`USRTRIM`** — per-unit factory pad calibration (`16 × 10 B + "trimdat\0"`, plain binary) |
| `0x7F000–0x7FFFF` | `key_mac` — reserved factory sector |

For physical SPI flash dumps via the mask-ROM USB bootloader (`jl-uboot-tool`) and conversion between physical and BLE dumps, see [docs/recovery.md](../../docs/recovery.md) and [`hardware/usbkey_dongle/host/compare_dump.py`](../../hardware/usbkey_dongle/host/compare_dump.py).

> [!CAUTION]
> Do not issue type-`0` BLE reads past `0x80000` (`512 KB`): out-of-range reads (such as `0xFFFFE0`) cause the controller to drop the BLE link and reset. `sinco_read.swift` enforces the `0x80000` upper bound.
