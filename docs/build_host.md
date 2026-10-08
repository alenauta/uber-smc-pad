# Firmware Build Environment & Flash Layout

JieLi's `pi32v2` compiler (`clang` 4.0.1 fork) and Linux post-build tools (`isd_download`, `fw_add`, `ufw_maker`, `remove_tailing_zeros`) are `x86_64` Linux (`glibc`) binaries, and stock-era OTA repackaging (`repack_oldchain.sh`) uses SDK v1.0.0's 32-bit Windows `fw_add.exe` / `ufw_maker.exe` under `wine`. For mask-ROM USB recovery (`jl-uboot-tool`), a Linux or Windows host with raw SCSI `/dev/sg*` access is also required (see [custom_fw_feasibility.md](custom_fw_feasibility.md) and [recovery.md](recovery.md)).

## Containerized & Native `x86_64` Linux Setup

All build dependencies are captured in [firmware/builder/](../firmware/builder/README.md):

- **Base image:** `debian:bookworm-slim` (`linux/amd64`), usable natively on `x86_64` Linux or via `docker --platform linux/amd64` on `arm64` / macOS.
- **Fetched at setup time (`firmware/builder/setup_toolchain.py`):**
  - JieLi Linux toolchain (`sha256`-verified) symlinked at `/opt/jieli`
  - JieLi Linux post-build tools symlinked at `/opt/utils`
  - Upstream `Jieli-Tech/fw-AC63_BT_SDK` (`ac63_bt_sdk_release_v2.4.0`) unpacked into `$SMCPAD_WORK/src/`
- **Host tests & packaging tools:** `gcc` + `libc6-dev` (for ASan/UBSan host unit tests in `firmware/sdk_overlay/test/`), `wine` + `wine32:i386` (for stock-era `.ufw` packaging), headless Qt/X11 libraries (`QT_QPA_PLATFORM=offscreen` for `isd_download`), and `sg3-utils` / `usbutils` for mask-ROM recovery checks.

### Baseline SDK Verification
- Unmodified SDK target `ac636n_hid` compiles cleanly to `sdk.elf`, `app.bin` (`223,516 B`), `jl_isd.fw`, and `update.ufw` (`521,760 B`), producing reproducible `app.bin` and `jl_isd.fw` binaries across rebuilds.

## Custom Overlay Image vs. Stock `SMC-PAD_005`

An unmodified SDK demo build differs dangerously from stock — in particular, the default demo layout places `BTIF` at `0x7E000`, right on top of the factory pad calibration (`USRTRIM`). The SMC-PAD board configuration in [firmware/sdk_overlay/](../firmware/sdk_overlay/README.md) pins every reserved region to match stock.

Comparison of `make ac636n_spp_and_le` + `build_ufw.sh` with `KEYFILE` (generated locally from your stock `.fwsc` via `prepare_stock.py`):

| Region / Setting | Stock `SMC-PAD_005` | SMC-PAD Overlay Build | Notes |
|---|---|---|---|
| Base app / PID | `AC636N_TRANS` (`spp_and_le` trans demo) | `spp_and_le`, PID `AC636N_TRANS`, VID `0.01` | ✅ Matches stock |
| Keyfile | Target device key (`jlfw.yaml`) | Same (`prepare_stock.py` → `isd_download -key`) | ✅ Matches stock |
| `isd_config` | `SPI 2_3_0_0`, `UTRX PB05`, `RESET PB01_00_0`, `UPDATE_JUMP 0`, `VLVD 4` | Same + `EOFFSET 0` (SDK v2.4.0; `0` = no offset) | ✅ Compatible |
| `PRCT` (code) | `0x00000`, `0x26000` | `0x00000`, `0x30000` (with BLE stack + sequencer; EDR off, `NO_TONE=1`) | Fits cleanly below `0x30000` |
| `VM` (settings journal) | `0x26000`, `0x4F000` | `0x50000`, `0x25000` (`repack_oldchain.sh`; `0x30000–0x4FFFF` reserved) | Global settings (`VM 0x2C`) initialize from stock defaults if journal resets |
| `BTIF` | `0x75000`, `0x1000` | `0x75000`, `0x1000` | ✅ Preserved |
| `EXIF` | `0x76000`, `0x1000` | `0x76000`, `0x1000` | ✅ Preserved |
| `USRFLASH` (presets) | `0x77000`, `0x7000` | `0x77000`, `0x7000`, `OPT 1` | ✅ Preserved |
| `USRTRIM` (calibration) | `0x7E000`, `0x1000` | `0x7E000`, `0x1000`, `OPT 1` | ✅ Preserved |
| `key_mac` | `0x7F000`, `0x1000` | `0x7F000`, `0x1000` | ✅ Preserved |
| Resources | `config.dat`, `md5.bin` | Empty `tone.cfg` (`NO_TONE=1`) | Audio prompt tones unused on MIDI controller |

Updating with our image leaves user presets (`USRFLASH`), factory pad calibration (`USRTRIM`), Bluetooth trim (`BTIF`), and `key_mac` untouched at their factory addresses.

The overlay ([firmware/sdk_overlay/](../firmware/sdk_overlay/README.md)) adds:
- Class-compliant **USB-MIDI 1.0** device support (`usb/usb_midi.[ch]`, 3 virtual cables, unit-tested in `test/test_usb_midi.c`);
- Full **SMC-PAD hardware drivers** using the reverse-engineered pin map ([pinmap.md](pinmap.md));
- **BLE-MIDI & Sinco `00 59` configuration** over BLE (`AE40`) and USB SysEx, plus the stock-compatible **preset interpreter** ([ble_config.md](ble_config.md));
- Standalone **16-track × 64-step polyphonic/drum step sequencer** (`board/smcpad_seq*.h`).

Post-build verification (`firmware/sdk_overlay/test/check_ram_isr.sh`) enforces that no RAM-resident ISR references XIP flash (`0x1Exxxxx`), that sufficient heap headroom remains (`≥ 40 KB`), and that `app.bin` stays within its flash partition budget.
