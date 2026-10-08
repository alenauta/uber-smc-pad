# M-VAVE SMC-PAD Clean-Room Custom Firmware & Step Sequencer

Clean-room custom firmware, hardware interoperability documentation, offline preset tools, and a full standalone **16-track × 64-step polyphonic/drum step sequencer** for the **M-VAVE SMC-PAD** wireless MIDI pad controller (**JieLi AC636N / BR25**, `pi32v2` CPU), built from scratch on JieLi's public [fw-AC63_BT_SDK](https://github.com/Jieli-Tech/fw-AC63_BT_SDK).

> [!CAUTION]
> **Unofficial, experimental, and independent.** This is a personal, non-commercial hobby and interoperability project. It is **not affiliated with, endorsed by, sponsored by, or supported by M-VAVE (Sinco Audio / Cuvave), Zhuhai Jieli Technology (JieLi), or any employer of the author.**
>
> **Use at your own risk:** Flashing modified firmware voids your manufacturer warranty and carries the risk of soft- or hard-bricking your controller or losing per-device pad calibration (`USRTRIM` at `0x7E000`) and presets (`USRFLASH` at `0x77000`). This software is provided **"AS IS" without warranty of any kind** (see [LICENSE](LICENSE) and [NOTICE](NOTICE)). **Never flash anything without first taking a verified 512 KB flash backup of your own unit ([firmware/backup/](firmware/backup/README.md)) and reading [docs/first_flash.md](docs/first_flash.md) and [docs/recovery.md](docs/recovery.md).**

Start with **[docs/index.html](docs/index.html) for the zero-install browser firmware patcher & SincoOTA flashing guide** (drop your own official `SMC-PAD_005.fwsc` in the browser to build `SMC-PAD_925.fwsc` locally — nothing is uploaded anywhere), [firmware/sdk_overlay/README.md](firmware/sdk_overlay/README.md) for the custom firmware architecture, [firmware/builder/README.md](firmware/builder/README.md) for the Docker / Linux build setup, [docs/overlay/smcpad_seq_overlay.html](docs/overlay/smcpad_seq_overlay.html) for the printable 1:1 sequencer overlay & cheat sheet, and [docs/references.md](docs/references.md) for upstream SDK and documentation links.

## Highlights
- **Full controller parity:** 16 velocity- & pressure-sensitive RGB pads, 8 endless encoders, 10 buttons, class-compliant USB-MIDI 1.0 (3 virtual cables), 3.5 mm TRS MIDI out, and low-latency BLE-MIDI (1 ms hardware-timer timestamps and automatic pairing on MIDI subscribe).
- **Compatible with stock presets & configuration tools:** Reads and plays the 8 on-flash presets in place (`0x77000`), preserves factory pad calibration (`0x7E000`), and implements the `00 59` configuration protocol over both BLE (`AE40`) and USB SysEx ([docs/midisuite_protocol.md](docs/midisuite_protocol.md)) while blocking raw-flash write/erase commands that could corrupt the bootloader or calibration.
- **Standalone 16-track × 64-step Sequencer (`Shift + Note Repeat`):**
  - **Step & Play views (`PAD BANK`):** Step sequencing with top-to-bottom 4×4 grid, Push-style two-pad note length entry (up to 32 steps), chords (up to 4 notes per step), per-step velocity, gate, micro-timing, probability, ratchets (1–4 hits, auto or fixed subdivisions), ties, and Elektron-style trig conditions (`A:B`, `1st`, `FILL`, `!FILL`).
  - **Live recording (`Rec + Play`):** Captures velocity, note length, chords, and raw micro-timing with non-destructive, retroactive per-track quantize (`K4`).
  - **Scales, roots & colours:** Per-track and global root/scale locking (chromatic/drum kit, major, minor, modes, harmonic minor, pentatonics, blues, or follow-preset) and customizable per-track RGB palette (`KNOB BANK K6`).
  - **Non-destructive rate/grid (`K8`):** Changing a track's rate switches the editing grid while keeping existing notes at their musical time (or `Shift + K8` to stretch/compress playback speed).
  - **16 on-device pattern slots (`Shift + Rec` / `Shift + Play`):** Stored in flash, auto-saved on Stop, with quantized bar-line pattern switching while playing and multi-level per-track undo/redo (`Shift + < / >`).
  - **Tight MIDI clock sync:** 960 PPQN internal engine; master clock out (`F8`/`FA`/`FC` + `F2` Song Position Pointer on double-Stop) or external USB/BLE-MIDI clock follower with 24-pulse beat averaging and signed `-120…+120 ms` sync offset (`KNOB BANK K8`).
- **Safety & reversible OTA updates:** Built-in boot-loop safe mode (two <5 s boots bypass custom drivers while keeping BLE OTA & USB alive), CPU exception capture to NOLOAD RAM, and verified round-trip OTA flashing (`stock → custom → stock`) via `SincoOTA`.
- **Printable 1:1 hardware overlays:** A4/Letter and Silhouette Cameo Print & Cut overlays (including interactive browser theme/color customizer) in [docs/overlay/](docs/overlay/) (`tools/overlay/`).

## What this repository contains, and what it doesn't
- **No M-VAVE / Sinco firmware, keys, or vendor binaries are included.** Users building firmware locally supply their own downloaded official `.fwsc` update file (`prepare_stock.py` extracts the bootloader, OTA loader, and image keyfile locally on your machine).
- **No JieLi SDK source code or compiler binaries are included.** `firmware/builder/setup_toolchain.py` fetches the official `Jieli-Tech/fw-AC63_BT_SDK` release tarball and Linux toolchain directly from upstream at build time, and `firmware/sdk_overlay/apply.py` overlays our clean-room C drivers, MIDI stack, and sequencer onto a local working copy.
- See [NOTICE](NOTICE) for third-party acknowledgements, USB VID:PID notes (`1209:0001` test ID), and trademark disclaimers.

## Key hardware facts
| | |
|---|---|
| MCU | JieLi AC636N (`BR25`, `pi32v2` 32-bit core), 512 KB internal SPI NOR flash |
| Stock FW | JieLi UFW container built on AC63 BT SDK (`SMC-PAD_005`) |
| App entry | `0x1E00120` (XIP flash mapped at `0x1E00000`) |
| Stock USB | `4353:3243`, class-compliant USB-MIDI 1.0, 3 virtual cables |
| BLE GATT | Standard Apple BLE-MIDI + JieLi RCSP OTA (`AE00`) + configuration service (`AE40`) |
| Build environment | Linux `x86_64` or Docker (`linux/amd64`; see [firmware/builder/](firmware/builder/README.md) and [docs/build_host.md](docs/build_host.md)) |
| Hardware recovery | Mask-ROM USB bootloader (`4c4a:2542`) via `USB_KEY` dongle + `jl-uboot-tool` ([docs/recovery.md](docs/recovery.md)) |

## Repository layout
```
firmware/
  backup/                        flash layout & BLE backup verification notes (dumps gitignored)
  build/                         local SDK build outputs (gitignored)
  builder/                       reproducible x86_64 Linux / Docker build environment & scripts
  sdk_overlay/                   clean-room C drivers, USB/BLE MIDI stack, preset engine & sequencer
hardware/
  usbkey_dongle/                 RP2040 MicroPython PIO USB_KEY mask-ROM recovery dongle + cycle model
tools/
  config/decode_presets.py       offline preset / globals decoder
  config/preset_json.py          lossless offline preset binary <-> JSON editor
  fw/make_fwsc.py                SincoOTA .ufw <-> .fwsc container wrapper
  fw/make_web_payload.py         generates zero-key client-side web patcher bundle (docs/payload/)
  gatt/gen_jl_profile.py         JieLi ATT GATT table generator (JSON -> C header), golden-tested
  midi/midimon.py                receive-only CoreMIDI monitor (Python stdlib ctypes)
  midi/smcdiag.py                live diagnostic SysEx client for custom firmware builds
  ble/sinco_probe.swift          read-only BLE version query helper
  ble/sinco_read.swift           read-only BLE flash backup helper (no write path)
  overlay/                       generators for 1:1 printable & Silhouette Cameo vinyl overlays
docs/
  index.html, patcher.js         zero-install client-side browser firmware patcher & flashing guide
  payload/smcpad_925.bin         clean-room custom firmware bundle for the browser patcher
  pinmap.md                      hardware GPIO map: pad mux+SARADC, SPI2 key matrix, WS2812, TRS MIDI
  preset_format.md               on-flash preset, global settings, and calibration record format
  midisuite_protocol.md          00 59 configuration packet & 7-bit USB SysEx framing specification
  ble_config.md                  custom firmware GATT table, command matrix, preset engine & safe mode
  build_host.md                  build environment & flash partition layout comparison vs. stock
  first_flash.md                 safe first-flash & round-trip stock recovery checklist
  recovery.md                    mask-ROM USB bootloader (UBOOT) recovery protocol & host setup
  stock_checks.md                read-only hardware behaviour verification checks
  stock_midi_map.md              observed stock USB-MIDI port & message behaviour
  custom_fw_feasibility.md       architecture & resource budget notes
  references.md                  upstream SDK, documentation, and specification links
  captures/                      raw MIDI test captures
  overlay/                       ready-to-print A4/Letter/Cameo PDFs, SVGs, DXFs, and HTML customizer
```

## Quick usage
```bash
# Watch what the pad sends over USB-MIDI (macOS CoreMIDI)
python3 tools/midi/midimon.py monitor SINCO 60

# Query firmware name/version over BLE (pad in BT mode, USB unplugged; macOS Swift)
swift tools/ble/sinco_probe.swift

# Decode presets / globals from your own flash backup (or SPB.bin)
python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin [--preset N] [--globals]

# Offline preset editor: lossless binary <-> JSON (nothing is sent to the device)
python3 tools/config/preset_json.py export firmware/backup/flash_dump_1.bin presets.json
python3 tools/config/preset_json.py build presets.json presets.bin --ref firmware/backup/flash_dump_1.bin

# Run host C & Python unit tests
python3 tools/gatt/test_gen_jl_profile.py
python3 hardware/usbkey_dongle/host/pio_model.py

# Build custom firmware in Docker (linux/amd64; see firmware/builder/README.md)
docker build --platform linux/amd64 -t smcpad-builder firmware/builder
docker run --rm -it --platform linux/amd64 -v "$PWD":/repo -v smcpad-work:/workspace smcpad-builder \
  bash -lc 'python3 /repo/firmware/builder/setup_toolchain.py && \
            python3 /repo/firmware/builder/prepare_stock.py /repo/firmware/original/SMC-PAD_005.fwsc && \
            python3 /repo/firmware/sdk_overlay/apply.py && \
            KEYFILE=/workspace/keys/smcpad.key NO_TONE=1 bash /repo/firmware/builder/build_ufw.sh'
```
All Python preset, GATT, recovery-model, and overlay tools use `python3` stdlib only.

## Legal, Disclaimers & License
- Released under the **MIT License** ([LICENSE](LICENSE)).
- Upstream community acknowledgements, firmware/SDK redistribution notes, USB test VID:PID notice (`1209:0001`), and trademark disclaimers are in [NOTICE](NOTICE).
- **Trademarks:** M-VAVE, Sinco, Cuvave, SMC-PAD, Midi Suite, CubeSuite, and SincoOTA are trademarks or product names of Guangzhou Sinco Precision Electric Co., Ltd. JieLi, AC636N, and BR25 are trademarks or product names of Zhuhai Jieli Technology Co., Ltd. This project is independent and unaffiliated with any vendor.
