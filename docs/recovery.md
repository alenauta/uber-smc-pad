# SMC-PAD recovery path: mask-ROM USB bootloader (UBOOT) via USB_KEY

This doc covers how to get a bricked or experimental SMC-PAD (JieLi **AC636N / BR25**) back
to a known flash image: the `USB_KEY` entry signal, the tooling, and the full procedure on
a Linux USB host (`x86_64` PC/laptop or Raspberry Pi).

> [!IMPORTANT]
> Status: **on paper only.** The dongle firmware passed a cycle-accurate PIO + electrical model
> (`hardware/usbkey_dongle/host/pio_model.py`), and the dump comparison was verified against the
> vendor update image. Run the dongle bench tests
> ([hardware/usbkey_dongle/README.md](../hardware/usbkey_dongle/README.md#bench-test-plan-logic-analyzer-before-connecting-the-smc-pad))
> first, then take **a verified read-only dump before writing anything**.

**Why it's worth it.** The BR25's UBOOT lives in mask ROM, so no flash content can destroy
it. With working `USB_KEY` entry, or with an erased/invalid flash (the ROM then falls into
UBOOT by itself), the pad stays recoverable as long as the chip has power and its D+/D- are reachable.
- Sources: [how-to-enter-uboot.md](https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/how-to-enter-uboot.md), [usb-key.md](https://github.com/kagaimiq/jielie/blob/main/isp/usb/usb-key.md)

## Contents
1. [Protocol: USB_KEY → UBOOT](#1-protocol-usb_key--uboot)
2. [Dongle (summary)](#2-dongle-summary)
3. [Linux USB host setup](#3-linux-usb-host-setup)
4. [End-to-end procedure](#4-end-to-end-procedure)
5. [Comparing a UBOOT dump with the BLE dump](#5-comparing-a-uboot-dump-with-the-ble-dump)
6. [Restore](#6-restore)
7. [Risks](#7-risks)
8. [Open questions](#8-open-questions)
9. [Sources](#9-sources)

---

## 1. Protocol: USB_KEY → UBOOT

Confidence levels:
- **H** = documented by two independent sources, or documented and visible in the public BR25 mask-ROM listing
- **M** = one source, or inferred from the public BR25 mask-ROM listing
- **L / ?** = assumption or unknown

| # | Claim | Conf. | Source |
|---|---|---|---|
| P1 | The key is the 16-bit value **0x16EF**, sent **MSB first** | H | [usb-key.md][ukey]. ROM `check_key_type` @0x11649C compares with 0x16EF → boot type 1 (USB). Other keys: 0x68AF → 4 (UART), 0x6B7C → 5. `usb_key_isr` @0x11721A shifts the new bit in at bit 0 (`r1 <= insert(r3, p:1, l:31)`), i.e. MSB first, and compares with 0x16EF ([rom.lst][romlst], `usb_key.c:35–47`) |
| P2 | **D+ = clock** (the chip samples on the **rising edge**), **D- = data** | H | [usb-key.md][ukey]. ROM installs `usb_key_isr` as the timer3 capture handler on DP (`set_timer3_captrue_dp`). The Arduino sketch in [christian-kramer][ck] (CLOCK→D+, DATA→D-) entered "BR21 UBOOT1.00". ⚠ The ASCII art in [how-to-enter-uboot.md][howto] labels the lines the other way round; we follow the ROM and usb-key.md. |
| P3 | Bit rate about **50 kHz**, no strict requirement (the chip receives in software) | H | [usb-key.md][ukey]. The Arduino sketch runs at ~19.3 µs/bit |
| P4 | Send the key **repeatedly until the ACK**. The ACK is both lines driven **low for ~1–2 ms** | H | [usb-key.md][ukey] ("about 1-2 ms"). The ROM clears the four DP/DM port bits, then `mdelay(2)` (`usb_key.c:43–47`) |
| P5 | The chip has **15 kΩ pull-downs** while listening (7.5 kΩ with a host attached). ACK sensing needs pull-ups **< 4.7 kΩ**; MCU-internal pull-ups give false ACKs | H | [usb-key.md][ukey] "Response considerations". The model gives 0.76 V with a 50k pull-up |
| P6 | The key is only accepted **shortly after a cold power-up**, so it must already be on the wire when the chip powers up | M | ROM `main`: the key register is initialised to 0xFFFF and the capture is armed only if `nvram_boot_state == 0` |
| P7 | Listening window ≈ **10 ms** | **L** | Inferred from `set_cap_timeout` (PRD = osc/0xFA00·10). The design doesn't depend on it: the key repeats every 471 µs from before power-up |
| P8 | After the ACK the chip pulls **D+ up** and measures **1 ms SOF intervals** to set up its PLL. Other FS/LS traffic on a single-TT hub breaks this (fix: separate hub/controller, MTT hub, or dongle-generated SOFs) | H | [usb-key.md][ukey] "USB bus considerations". ROM `check_usb_sof(3)`: 3 attempts × ~1 s + 200 ms; `tmr3_isr` wants 4 consecutive periods within ±0x104 ticks (M) |
| P9 | In UBOOT the chip enumerates as **USB mass storage, `4c4a:2542`, SCSI vendor "BR25", product "UBOOT1.00"**, using vendor SCSI opcodes 0xFB/0xFC/0xFD | H | [usb-ids.md][usbids], [usb-protocol.md][uproto], [jl-uboot-tool][jlut] `jldevfind.py` |
| P10 | jl-uboot-tool supports BR25 ("Working"). It uploads `br25loader.bin` to 0x12000 (MengLi-encrypted, 512 B blocks), then speaks Loader V2: `chip_key`, `online_device`, read, write and erase | H | [jlut README][jlut], `data/usb-loaders.yaml`, `jltech/uboot.py` |
| P11 | Signal levels: a 3.3 V push-pull through 1 kΩ is fine (USB FS signalling is 3.3 V; the vendor dongle is itself a 3.3 V JL chip) | M | USB 2.0 FS levels. The vendor dongle is described in [how-to-enter-uboot.md][howto] |
| P12 | The pad does **not** have to be powered from the dongle, but the dongle must **control or observe the chip's power-up** (VBUS switch and/or the user flipping the pad's switch) | M | Follows from P6. The pad's battery makes VBUS-only cycling uncertain (§8) |
| P13 | A blank or invalid flash makes the ROM enter UBOOT **without** a key | H | [how-to-enter-uboot.md][howto], [usb-key.md][ukey] |

Existing implementations we found (none for the Pico):
- the vendor "USB updater" dongle (JL chip + power switch + USB switch) [howto];
- the [christian-kramer][ck] Arduino sketch (bit-bang only, no ACK detection, no hand-off);
- [12voltman/jieliDFUtrigger](https://github.com/12voltman/jieliDFUtrigger): ATtiny412 + switch, USB-C, "how to use: TBD";
- [KunYi/jl-uboot-rs](https://github.com/KunYi/jl-uboot-rs): an experimental Rust host tool, Linux/Windows.

kagaimiq generated the *UART* key with RP2040 PIO ([uart-key.md](https://github.com/kagaimiq/jielie/blob/main/isp/uart/uart-key.md)).

## 2. Dongle (summary)
The full design, BOM, wiring and bench tests are in
[hardware/usbkey_dongle/README.md](../hardware/usbkey_dongle/README.md).
- **Raspberry Pi Pico + MicroPython + PIO**: the key at 50 kHz is generated cycle-exactly by PIO, and the ACK is sensed through switched 2k2 pull-ups.
- **K1**, a DPDT signal relay, switches the pad's D+/D- between the host (at rest, i.e. a plain cable) and the Pico.
- **K2**, a relay on VBUS, lets the dongle power-cycle the pad.
- If VBUS cycling doesn't reset the battery-powered pad, the LED fast-blinks and **you flip the pad's power switch OFF→ON** while the key keeps running.
- The pad plugs in with **its own USB-A→C cable** into the dongle's USB-A socket. The dongle's USB-A plug goes into the Linux host's USB port (or MTT USB hub).
- Recommended: Pico powered from a **separate 5 V charger** and started with its **button**, so no extra full-speed device sits on the pad's USB bus (P8).
- On-paper checks:
  - `python3 hardware/usbkey_dongle/host/pio_model.py` → ALL PASSED (waveform, 15 scenarios incl. battery pad / no false ACK)
  - `python3 hardware/usbkey_dongle/host/check_capture.py --selftest`

## 3. Linux USB host setup

### 3.1 Why Linux (or Windows) rather than macOS
- `jl-uboot-tool`'s `scsiio` only has Linux (`SG_IO`) and Windows (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) back ends. On macOS it raises `ImportError("Platform 'darwin' is not supported")` ([scsiio/__init__.py][jlut]).
- UBOOT enumerates as a raw SCSI mass-storage device (P9), which requires Linux `/dev/sg*` or Windows raw SCSI access ([custom_fw_feasibility.md](custom_fw_feasibility.md)).
- Any standard Linux box (`x86_64` laptop/mini-PC or `arm64` Raspberry Pi — `jl-uboot-tool` is pure Python) works directly, or you can pass `/dev/sgN` into the [firmware/builder/](../firmware/builder/README.md) Docker container (`docker run --device=/dev/sg0 ...`).

### 3.2 What `jl-uboot-tool` needs
| Need | Detail |
|---|---|
| Python ≥ 3.7 | Standard on Debian/Ubuntu/Raspberry Pi OS ✅ |
| Python packages | `requirements.txt`: `crcmod`, `pyyaml`, `pycryptodomex`, `tqdm`. On Debian/Ubuntu: `apt-get install python3-yaml python3-crcmod python3-tqdm python3-pycryptodome usbutils sg3-utils` (all pre-installed in [firmware/builder/Dockerfile](../firmware/builder/Dockerfile)) |
| Source | `git clone https://github.com/kagaimiq/jl-uboot-tool` (tested against commit `adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a`, 2025-03-16) |
| Device node | **`/dev/sgN`** (SCSI generic, char major 21), opened **`O_RDWR`**, `SG_IO` ioctl ([scsiio/linuxport.py][jlut]). Auto-discovery (`jldevfind.py`) scans `/dev/sg*`, sends `INQUIRY`, and skips nodes that raise `PermissionError` (run with `sudo` or grant `disk`/`cdrom` group permissions). `--device /dev/sgN` skips the scan. |
| Why **not** `/dev/sdX` | On a block device, `SG_IO` goes through `scsi_cmd_allowed()`, which allows only standard read/write opcodes unless the caller holds **`CAP_SYS_RAWIO`**. The UBOOT vendor opcodes `0xFB`/`0xFC`/`0xFD` aren't whitelisted, returning `EPERM` ([scsi_ioctl.c][sioctl] L267–362). On `/dev/sg*`, `sg.c` applies that filter only if the node was opened **read-only** (`read_only && sg_allow_access()`, [sg.c][sgc] L762/L923). `jl-uboot-tool` opens `/dev/sgN` `O_RDWR`, so **no extra capability is needed with `/dev/sg*`** ✅ |
| Kernel module | Standard kernels build `CONFIG_CHR_DEV_SG=y` or `sg.ko` (`sudo modprobe sg` if `/dev/sg*` does not appear automatically when `4c4a:2542` enumerates, [x86_64_defconfig][x86def]) |

### 3.3 Desktop automounters & Docker device passthrough
- **Desktop automounters (`udisks2`):** When `4c4a:2542` enumerates, the Linux kernel attaches `sd` and `udev` runs `blkid`/`scsi_id`. UBOOT answers standard `READ(10)`/`READ CAPACITY` probes with dummy errors; this log noise is harmless as long as no desktop file manager tries to format or mount `/dev/sdX`. Do not open `/dev/sdX` directly.
- **Running inside Docker (optional):** `--privileged` is not needed; pass the specific SCSI generic node once the pad is in UBOOT (`docker run --rm -it --device=/dev/sgN ...`). `SG_IO` on `/dev/sgN` opened `O_RDWR` is permitted under Docker's default seccomp/AppArmor profile.

### 3.4 Physical setup and the SOF caveat
```
Linux USB host / MTT hub ──┬── USB-A ── dongle J2 (HOST) ═ K1/K2 ═ J1 (PAD) ── pad's A→C cable ── SMC-PAD
                           └── (no other full/low-speed USB devices on a shared single-TT hub)
5 V USB charger ── Pico (dongle) ── button SW1, LED
```
- Per P8, anything full/low-speed on the same single-TT hub that the host polls can derail the ROM's clock calibration (serial adapters, Bluetooth dongles, keyboards/mice, or the Pico's own CDC port while a terminal has `/dev/ttyACM0` open). The chip then resets into the app after a few seconds.
  - High-speed devices (USB-Ethernet, USB 3 SSD) are isolated by the hub's transaction translator and do not interfere.
  - Connect the dongle directly to a dedicated host port or an **MTT** hub, or unplug other FS/LS devices from a shared hub during the few minutes needed for recovery.

## 4. End-to-end procedure

### Phase 0: one-time preparation
1. Build the dongle and pass bench tests T0–T6 and T8 ([dongle README](../hardware/usbkey_dongle/README.md#bench-test-plan-logic-analyzer-before-connecting-the-smc-pad)).
2. On your Linux host, install the dependencies (§3.2), clone `kagaimiq/jl-uboot-tool`, and create an output directory (`mkdir -p dumps`).
3. Have this repo's `hardware/usbkey_dongle/host/` scripts and your verified BLE flash backup (`firmware/backup/flash_dump_1.bin`) ready for comparison.
4. Charge the pad beforehand, then switch it **OFF** before starting Phase 1.

### Phase 1: enter UBOOT
1. Switch the pad **OFF**. Wire it up as in §3.4.
2. Press the dongle button (or `usbkey.run()`).
   - The dongle cuts VBUS for 1.5 s, connects the lines to the Pico, starts the key and restores VBUS. Slow blink.
   - If the pad's chip runs from VBUS, the ACK comes within milliseconds.
3. If the LED **fast-blinks**, switch the pad **ON** (OFF→ON) within 20 s. The key keeps running.
4. **LED solid** = ACK and hand-off done. **LED off** = failed. See the [troubleshooting table](../hardware/usbkey_dongle/README.md#troubleshooting) and retry. Retrying is harmless.

### Phase 2: verify the chip
On the Linux USB host:
```sh
lsusb -d 4c4a:2542                       # expect: ID 4c4a:2542 ... (BR25 UBOOT 1.00)
sudo modprobe sg                         # ensure /dev/sg* nodes are present
ls -l /dev/sg*; for d in /sys/class/scsi_generic/sg*; do echo "$d: $(cat $d/device/vendor) $(cat $d/device/model)"; done
                                         # expect vendor "BR25", model "UBOOT1.00" -> note N
sudo sg_inq /dev/sgN
cd path/to/jl-uboot-tool
sudo python3 jldevfind.py                # expect: "BR25 UBOOT1.00 (...) at /dev/sgN"
sudo python3 jluboottool.py --device /dev/sgN exit
```
Expected "Quick info" (`jl-uboot-tool` uploads `br25loader.bin` to RAM; nothing is written to flash):
- `Chip: BR25 - AC636N/AC696N/AC608N series`
- `Running loader with argument 0x0001.` → `The Loader has been successfully installed.`
- `>> Chip key: 0x.... <<`. This is the unit's 16-bit SFC key read directly from the chip. Record it and pass it via `--chipkey` in §5.
- `Online device: ID 0x……13, Type 0x03 (SPI NOR flash on SPI0)`. The last ID byte is `log2(size)`; `0x13` = 512 KiB matches the `0x80000`-byte BLE dump.
- The stock firmware's product ID string `AC636N_TRANS` belongs to the application image, not the mask ROM; UBOOT itself will not report it.

Hard rules:
- **Never** run `burnchipkey` (it writes eFuses irreversibly).
- Don't run `erasechip`, `erase` or `write` in this phase.

### Phase 3: full read (twice)
```sh
sudo python3 jluboottool.py --device /dev/sgN "read 0 0x80000 dumps/uboot_A.bin" exit
sudo python3 jluboottool.py --device /dev/sgN "read 0 0x80000 dumps/uboot_B.bin" exit
sha256sum dumps/uboot_[AB].bin           # must be identical
```
- The loader stays resident between runs. If the second run fails to re-upload, power-cycle and re-enter UBOOT (Phase 1).
- Save both files safely alongside `firmware/backup/` and record their SHA-256 checksums. **`uboot_A.bin` is now the primary physical restore image.**

### Phase 4: compare with the BLE dump
```sh
python3 hardware/usbkey_dongle/host/compare_dump.py --selftest
python3 hardware/usbkey_dongle/host/compare_dump.py dumps/uboot_A.bin \
    --chipkey <0xXXXX_from_phase2> \
    --ble firmware/backup/flash_dump_1.bin --out-ble-equiv dumps/uboot_A_ble_equiv.bin
```
See §5 for what each result means.

### Phase 5: leave UBOOT
- `sudo python3 jluboottool.py --device /dev/sgN reset`, or switch the pad OFF and unplug it.
- Switch it ON normally; it should boot into the existing firmware unchanged.

## 5. Comparing a UBOOT dump with the BLE dump
A BLE backup (`firmware/backup/flash_dump_1.bin`) is read **through the running firmware** over BLE (`00 59` read type `0`), whereas `jl-uboot-tool` reads the **physical** SPI-flash bytes via the RAM loader, bypassing on-the-fly SFC descrambling.

The two dumps differ by design, region by region:

| Region | BLE dump has | Physical (UBOOT) has | Transform (`compare_dump.py`) | Conf. |
|---|---|---|---|---|
| `0x00000–0x00FFF` header/JLFS | see right | **H1**: BLE XOR ENC(`0xFFFF`) LFSR keystream restarted every `0x200 B` (512-byte blocks). **H2**: identical to BLE | default `--header-mode enc512`. The tool prints the byte count for both and hints if the other mode fits | H1: **M** (matches the UFW image, 4096/4096 bytes). H2: [backup README](../firmware/backup/README.md) calls the BLE header "raw on-flash form". |
| `0x01000–0x25FFF` app area | unscrambled XIP view | **SFC-scrambled**: 16-bit LFSR per 32-byte block, seed = `chipkey ^ ((addr − 0x1000) >> 2)` | `sfc_cipher(buf, 0x1000, len, base=0x1000, key=chipkey)` | M–H |
| `0x26000–0x7FFFF`: VM `0x26000–0x74FFF` (header `55AAAA55` @0x26000, journal @0x27000), BTIF `0x75000`, EXIF `0x76000`, USRFLASH `0x77000–0x7DFFF` (presets), USRTRIM `0x7E000`, key_mac `0x7F000` | plain | plain (identical) | none | M |

All transforms are symmetric XOR streams, so the same function maps physical → BLE-equivalent and back (`compare_dump.py --selftest` verifies involution and wrong-key sensitivity).

Interpreting a real UBOOT dump:
| Result | Meaning / action |
|---|---|
| `IDENTICAL` or `MATCH except the known header/CRC bytes` | ✅ Recovery path proven: the pad's flash is fully understood, and `uboot_A.bin` is a trustworthy restore image |
| `HINT: … --header-mode plain` and then a match | ✅ Same, and H2 was right for the header |
| Mismatch in `0x26000+` only (VM etc.) | Runtime data changed since the BLE dump (settings, pairing). Fine; the UBOOT dump is newer |
| Mismatch in the app area | Check `--chipkey` (from Phase 2) and `--sfc-end`. **Don't restore from a BLE-derived image until understood** |
| UBOOT dump byte-identical to the BLE dump | `jl-uboot-tool` returned descrambled data (unexpected). Restoring means writing back what was read |

## 6. Restore
**Restore source, in order of preference:**
1. **`uboot_A.bin`**, your unit's own physical UBOOT dump. Exact, including all per-device regions:
   - `0x75000` BTIF (Bluetooth trim), `0x76000` EXIF
   - `0x77000–0x7DFFF` USRFLASH (presets, 28 KB)
   - `0x7E000` **USRTRIM** (per-unit factory pad calibration)
   - `0x7F000` `key_mac`
   - `0x26000–0x74FFF` VM settings
2. Only if no UBOOT dump exists: `compare_dump.py --to-physical flash_dump_1.bin predicted.bin`, splicing the 41 header/CRC bytes from your stock `.fwsc`'s `flash.bin`, and verifying the result with `compare_dump.py` before writing.

**Full restore** (stock or known-good dump):
```sh
sudo python3 jluboottool.py --device /dev/sgN "write 0 dumps/uboot_A.bin" \
        "read 0 0x80000 dumps/verify.bin" exit
cmp dumps/uboot_A.bin dumps/verify.bin && echo RESTORE-VERIFIED
```
- `write` erases the covered range itself (64 KiB blocks where aligned, else 4 KiB sectors), then writes with CRC-checked blocks ([jluboottool.py][jlut] `do_write`, `flash_erase`).
- **Partial restore** (only the app area after a bad custom image):
  - Write a 4 KiB-aligned slice taken from `uboot_A.bin`: the stock image area (`PRCT`) is `0x0–0x25FFF` → `write 0 head.bin` (`head.bin` = first `0x26000` bytes). That leaves `VM`, `BTIF`, `EXIF`, `USRFLASH`, `USRTRIM`, and `key_mac` untouched.
  - `erase` doesn't preserve partial sectors, so keep slices 4 KiB-aligned.
- If power drops mid-write, the flash is blank or invalid → the ROM enters UBOOT automatically on next boot (P13) → repeat the write. The mask ROM itself cannot be damaged by flash writes.

## 7. Risks
| Risk | Mitigation |
|---|---|
| **Battery**: the chip doesn't reset on VBUS cycling, so the key is missed | Use the dongle's "flip the power switch" prompt (fast blink). If the switch ever fails to reset the MCU, disconnect the internal battery JST connector during recovery (§8) |
| False ACK → hand-off at the wrong time | Strong 2k2 pull-ups. ACK accepted only as a HIGH→LOW transition right after a packet, with a 0.3–4 ms low duration |
| SOF calibration disturbed (shared single-TT hub) | §3.4: power the Pico from a separate 5 V supply, avoid other FS/LS devices on the same hub, or use a dedicated root port / MTT hub |
| Back-feeding a powered-off pad through D+/D- | 1k series resistors limit current to ≤ 2.5 mA per line (matching the vendor dongle) |
| Wrong wiring (VBUS onto a data line) | Run T0 continuity test first. Relays at rest = plain pass-through cable. Never connect the pad before T0–T6 and T8 pass |
| Irreversible eFuse write | **Never** run `burnchipkey`. No step in this repository uses it |
| Erasing/losing per-device data (`USRTRIM` calibration, BT data) | Take two identical UBOOT dumps before any write, keep your BLE dump, prefer partial restores, and avoid `erasechip` |

## 8. Open questions
1. **Pad power architecture:** does VBUS alone power or reset the AC636N with the power switch OFF, or does the power switch cut MCU power completely while USB-C only feeds the charger IC? Plan on **"flip the power switch"** mode (switch OFF, plug in, arm dongle, switch ON).
2. **Header form:** H1 (`ENC-512`, matching the UFW image) or H2 (BLE header = raw)? Answered by the first UBOOT dump (§5).
3. **Meaning of the 41 differing header/CRC bytes** between UFW `flash.bin` and runtime BLE reads.
4. **Optional future improvements:** a `libusb` Bulk-Only-Transport back end for `scsiio`, or a compact RP2040 PCB using an analog USB mux (`TS3USB221` / `FSUSB42`) instead of signal relays.

## 9. Sources
- [ukey]: kagaimiq/jielie, *USB_KEY*: https://github.com/kagaimiq/jielie/blob/main/isp/usb/usb-key.md
- [howto]: kagaimiq/jl-uboot-tool, *How to enter USB download (UBOOT) mode*: https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/how-to-enter-uboot.md
- [jlut]: kagaimiq/jl-uboot-tool (`jluboottool.py`, `jldevfind.py`, `scsiio/`, `jltech/uboot.py`, `data/usb-loaders.yaml`): https://github.com/kagaimiq/jl-uboot-tool
- [uproto]: jl-uboot-tool *USB protocol* / *USB loader v2*: https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/usb-protocol.md, https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/usb-loader-v2.md
- [usbids]: kagaimiq/jielie *USB IDs*: https://github.com/kagaimiq/jielie/blob/main/misc/usb-ids.md
- [romlst]: BR25 mask ROM listing (`main`, `check_key_type`, `usb_key_isr`, `set_cap_timeout`, `check_usb_sof`, `tmr3_isr`): https://github.com/jiang20082233/AC6966B-JBD/blob/696X/SDK/cpu/br25/tools/rom.lst
- [ck]: christian-kramer, *JieLi AC690X Familiarization*: https://github.com/christian-kramer/JieLi-AC690X-Familiarization
- 12voltman/jieliDFUtrigger: https://github.com/12voltman/jieliDFUtrigger · KunYi/jl-uboot-rs: https://github.com/KunYi/jl-uboot-rs
- [sgc]: Linux `drivers/scsi/sg.c`: https://github.com/torvalds/linux/blob/v6.18/drivers/scsi/sg.c
- [sioctl]: Linux `drivers/scsi/scsi_ioctl.c`: https://github.com/torvalds/linux/blob/v6.18/drivers/scsi/scsi_ioctl.c
- [x86def]: Linux `arch/x86/configs/x86_64_defconfig`: https://github.com/torvalds/linux/blob/master/arch/x86/configs/x86_64_defconfig

[ukey]: https://github.com/kagaimiq/jielie/blob/main/isp/usb/usb-key.md
[howto]: https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/how-to-enter-uboot.md
[jlut]: https://github.com/kagaimiq/jl-uboot-tool
[uproto]: https://github.com/kagaimiq/jl-uboot-tool/blob/main/docs/usb-protocol.md
[usbids]: https://github.com/kagaimiq/jielie/blob/main/misc/usb-ids.md
[romlst]: https://github.com/jiang20082233/AC6966B-JBD/blob/696X/SDK/cpu/br25/tools/rom.lst
[ck]: https://github.com/christian-kramer/JieLi-AC690X-Familiarization
[sgc]: https://github.com/torvalds/linux/blob/v6.18/drivers/scsi/sg.c
[sioctl]: https://github.com/torvalds/linux/blob/v6.18/drivers/scsi/scsi_ioctl.c
[x86def]: https://github.com/torvalds/linux/blob/master/arch/x86/configs/x86_64_defconfig
