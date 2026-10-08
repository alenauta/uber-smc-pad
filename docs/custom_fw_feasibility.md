# Custom firmware feasibility: toolchain, path A vs B, sequencer

Status: research only (2026-10-08). Nothing was installed or executed, and no device was touched.
Confidence markers: ✅ verified this session (file, URL or binary inspected) · 🟡 strong inference · ❓ uncertain / to verify.

## TL;DR
- **The toolchain is Linux x86_64 or Windows only.** JieLi's pi32v2 compiler is a closed **clang/LLVM 4.0.1 fork**. Nothing runs natively on macOS or on arm64 Linux. ✅
- **Recommended route:** build in an **`x86_64` Linux container or host** ([firmware/builder/](../firmware/builder/README.md)), or via **GitHub Actions (`ubuntu-latest`)** for reproducible CI builds.
- **Flashing/recovery cannot happen on macOS.** `jl-uboot-tool` supports Linux and Windows only, and USB mass-storage restrictions on macOS block raw UBOOT SCSI access. Plan a separate Linux or Windows box with physical USB access (an RPi or small x86-64 box works).
- **Path B (SDK rebuild) is the only realistic home for a sequencer.** Path A (binary patching) suits small fixes only.
- **A sequencer is feasible on the hardware.** CPU, RAM and flash are not the constraint. The real cost is bringing path B up to parity with the stock firmware (drivers for pads, encoders and LEDs).

---

## 1. Toolchain options & recommendation

### 1.1 What the SDK needs (verified from the repo)
| Item | Finding | Source |
|---|---|---|
| Build systems | Code::Blocks `.cbp` (Windows) **or** `make`. The Makefile has explicit Windows and Linux branches. ✅ | [README](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/README.md), [apps/hid/board/br25/Makefile](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/apps/hid/board/br25/Makefile) |
| BR25 targets | `make ac636n_hid`, `make ac636n_spp_and_le`, `make ac636n_mesh` ✅ | [top Makefile](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/Makefile) |
| Linux install | Unpack the toolchain to `/opt/jieli` so that `/opt/jieli/common/bin/clang` exists. Set `ulimit -n` > 8096 (the LTO link opens many files). ✅ | Makefile header comment |
| Compiler flags | `clang -target pi32v2 -mcpu=r3 -flto -Oz`. Links with `lto-wrapper` (a Python wrapper around `ld` plus `LLVMgold.so`) ✅ | Makefile; `common/bin/lto-wrapper` in the tarball |
| Linux toolchain | `jieli-linux-toolchains-20250805.1.tar.xz` (26 MB, sha256 `f686586b…8225958`, which matches ElectronicCats). No auth: `https://pkgman.jieliapp.com/s/linux-toolchain` redirects to Aliyun OSS. Binaries are **ELF x86-64, dynamically linked (glibc)**. The version string reads "clang -cc1 version 4.0.1 / LLVM 4.0.1". ✅ | [JieLi tool index](https://doc.zh-jieli.com/Tools/zh-cn/other_info/index.html); tarball inspected without running anything |
| Windows toolchain | `2.5.2.exe` (53 MB), installs to `C:\JL\pi32\bin` ✅ | same index (`/s/win-toolchain`) |
| Post-build (Linux) | `jieli-linux-post-build-tools-20260923.1.tar.xz`: `isd_download`, `fw_add`, `ufw_maker`, `remove_tailing_zeros`, `packres`, `json_to_res`, `fat_comm`. All x86-64 ELF ✅. The SDK also ships `cpu/br25/tools/isd_download` and `lz4_packet` as Linux ELFs ✅ | `/s/linux-postbuild`; [cpu/br25/tools](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/tree/master/cpu/br25/tools) |
| Post-build script | `cpu/br25/tools/download.c` is preprocessed into `download.sh`/`.bat`. It does objcopy of `.text/.data/.data_code/overlays/banks` → `lz4_packet` → concatenate into `app.bin`. Then `isd_download -tonorflash -dev br25 -boot 0x12000 -div8 -wait 300 -uboot uboot.boot -app app.bin cfg_tool.bin -res tone.cfg -uboot_compress -flash-params flash_params.bin`, then `fw_add` (ota.bin, script.ver), then `ufw_maker -fw_to_ufw` → `update.ufw` ✅ | [download.c](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/cpu/br25/tools/download.c), [data_trans/download.bat](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/cpu/br25/tools/download/data_trans/download.bat) |
| Linux quirk | The `__SHELL__` branch ends with `host-client -project …`, a JieLi helper we don't have, and calls `/opt/utils/strip-ini`. ❓ We'll replace this tail with a direct `isd_download` call, as ElectronicCats did for AC707N. | download.c lines 67–71 |
| Config tool | `cfg_tool.bin` (BT name, etc.) is produced by the Windows-only Lua config tool (`AC696X_config_tool/*.jlxproj`). We can reuse the SDK default `cpu/br25/tools/cfg_tool.bin` or the stock 864 B one 🟡 | tree |

**Image keyfile:** `isd_download` accepts a `-key <file>.key` argument. Upstream [kagaimiq/jl-misctools](https://github.com/kagaimiq/jl-misctools) (`keyfile/keyfgen.py`, invoked automatically by `firmware/builder/prepare_stock.py`) generates that `.key` locally from the user's own stock `.fwsc` metadata.

### 1.2 Options on macOS
Environment (read-only checks): macOS arm64. No local Linux container runtime installed. Present: Rosetta, Python 3, `swift`.

| Option | Works? | Needs | Verdict |
|---|---|---|---|
| Native macOS | ❌ The compiler is a closed LLVM 4.0.1 fork, and upstream LLVM has no pi32v2 target. Rosetta runs x86_64 *Mach-O*, not Linux ELF. | n/a | Not possible |
| Local Linux VM/containers (UTM, Lima, Colima, OrbStack, Docker Desktop, Apple `container`) | 🟡 Technically yes: an arm64 Linux guest + Rosetta-for-Linux, or qemu x86_64 emulation | Local VM / container setup | Possible, or use a dedicated Linux box |
| **Remote Linux x86_64 box or cloud VM** (e.g. Debian/Ubuntu x86_64) | ✅ The toolchain runs natively | SSH/SCP access from the Mac | **Recommended for the dev loop** |
| **GitHub Actions** `ubuntu-latest` (x86_64) | ✅ | A repo (private is fine). Download the toolchain at CI time; don't commit it, because JieLi's redistribution terms are unclear ❓ | **Recommended for reproducible builds/artifacts** |
| Cloud Windows VM + Windows App (RDP) | ✅ Code::Blocks + `isd_download.exe` + config tool | A Windows VM licence/cost | Optional: only for the config tool / cross-checking official tools |

**Recommendation:** use a dedicated Linux x86_64 box or VM for interactive work (≈1 min per LTO build, as on AC707N per ElectronicCats). Mirror the same script in GitHub Actions so every commit produces `sdk.elf`, `app.bin`, `jl_isd.fw` and `update.ufw` artifacts. Reference flow: [ElectronicCats jieli-sdk-build.md](https://github.com/ElectronicCats/jieli-ble-badge-research/blob/main/docs/jieli-sdk-build.md) (they used wine for `isd_download.exe`; BR25 has native Linux binaries, so we shouldn't need wine 🟡).

### 1.3 Flashing is a separate problem (important)
- [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) talks to the mask-ROM "UBOOT" device via SCSI (`scsiio`), with **Linux and Windows code paths only** ✅.
- → **You need a Linux or Windows flashing host**: a Linux laptop/mini-PC or a Raspberry Pi. The Python tools run on arm64; only the *compiler* needs x86_64.
- A cloud VM cannot flash (no USB).
- Alternative: **BLE OTA (RCSP)** from macOS with a Swift port of [JieLi's OTA protocol](https://github.com/Jieli-Tech/iOS-JL_OTA). That avoids USB, but a broken image may also break BLE, so USB recovery must exist first ❓.

---

## 2. Path A vs Path B

| | **A: patch stock `app.bin`** | **B: rebuild on fw-AC63_BT_SDK** |
|---|---|---|
| Bring-up risk | Low: all drivers and USB/BLE MIDI keep working | High: pin map, pad ADC scan, encoder decode, LED driver, charger/power must all be reimplemented |
| Toolchain | JieLi `clang` (`pi32v2` target) + post-build tools | §1 toolchain + post-build |
| Free space | ~6 KB in the app area. More requires moving or shrinking VM (`0x26000–0x74FFF`, 316 KB) and rewriting the JLFS/area table ❓ | Unlimited for practical purposes (see budget below) |
| Midi Suite / SincoOTA compatibility | Kept (`AE40`, RCSP OTA, preset format) | Reimplemented cleanly from the wire protocol ([midisuite_protocol.md](midisuite_protocol.md)) |
| USB-MIDI / BLE-MIDI | Present | **Not in the SDK** ✅. `uac_audio.h` only defines the MIDIStreaming subclass, jack and endpoint constants ([uac_audio.h](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/include_lib/driver/device/usb/device/uac_audio.h)). The USB device stack (HID/CDC/MSD/UAC1) and BLE GATT are generic, so we write both classes from scratch. Usable SDK references: [standard_keyboard/usb_hid_devices.c](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/tree/master/apps/hid/examples/standard_keyboard) and [spp_and_le/examples/multi_conn](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/tree/master/apps/spp_and_le/examples/multi_conn) |
| SDK version | Stock = 2021 (`BTCTRLER-@20210611`) | Latest tag `ac63_bt_sdk_release_v2.4.0` + `ac63_patch_release_20250224` ✅ |
| Good for | Small table tweaks | Full custom OS, layers, sequencer, arpeggiator, new modes |
| Effort (focused eng-time, rough) | 1–2 wk mapping, then 2–5 days per small tweak | ~6–10 wk to full stock parity + sequencer (see §3.6) |

**Verdict:** commit to **Path B (clean-room SDK build)** so all drivers, USB/BLE MIDI, and the sequencer are clean, maintainable C source code.

---

## 3. Sequencer feasibility & design sketch

### 3.1 Resources
| Resource | Value | Source |
|---|---|---|
| CPU | pi32v2 with FPU, **up to 160 MHz**, 64 vectored IRQs, 4 priority levels ✅ | AC6366C datasheet V1.3 (`doc/datasheet/AC636N/` in the SDK) |
| Timers | **6 × 32-bit timers** (capture/PWM) ✅ | datasheet |
| RAM | BR25: 128 K RAM0 + 8 K RAM1 + ~1.5 K, plus 16 K icache ✅. The SDK maps app RAM at `0x10000`–~`0x32000` (≈136 K minus the mask-ROM export area) ✅ | [kagaimiq/jielie BR25](https://github.com/kagaimiq/jielie), [sdk_ld.c](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/cpu/br25/sdk_ld.c) |
| Flash | 512 KB SPI, code executed in place through cache. Stock: app area ≈148 KB, VM 316 KB (AUTO-sized = whatever is left over; the SDK minimum is 8 K) ✅ | [isd_config_rule.c](https://github.com/Jieli-Tech/fw-AC63_BT_SDK/blob/master/cpu/br25/tools/isd_config_rule.c) |
| Available heap | Measured from our SDK build map (`≥ 40 KB` enforced by `check_ram_isr.sh`) | `sdk.map` |
| USB | 1 × full-speed USB 2.0 OTG ✅ | datasheet |
| Power | VBAT 2.0–4.5 V, charger 20–300 mA, 3 µA soft-off ✅ | datasheet |

The stock firmware already has the timing pieces: note-repeat tempo, swing, sync to DAW clock, tap tempo and latch (see globals in `preset_format.md`), and an `usr_systimer` module string ✅. That proves the chip handles clocked MIDI output alongside BLE and USB.

### 3.2 Flash budget (path B)
| Block | Size |
|---|---|
| uboot + headers + ota loader | ~8 KB (stock 4 KB + `ota.bin` slot ❓) |
| App (SDK `hid`/`spp_and_le` + MIDI + drivers + sequencer) | 150–220 KB ❓ |
| VM (settings journal) | 32 KB (was 316 KB) |
| Presets (stock format, 8 × 3539 B) | 28 KB |
| **Patterns** (see below) | 64 KB |
| BTIF/EXIF/USRTRIM/key_mac (must keep `key_mac` @ `0x7F000` and trim data) | ~12 KB |
| **Headroom** | **≈170 KB** |

Pattern format (drum sequencer, 16 tracks = 16 pads, 64 steps = 4 pages of 16):
- Per step 3 B: `vel` (0 = off) · `gate4|prob4` · `micro4|ratchet2|flags2`.
- Track header 8 B: note, channel, length, clock divisor, swing, mute, colour.
- Per pattern: 16 × (64 × 3 + 8) + 32 = **3 232 B**, so **16 patterns ≈ 51 KB** (fits the 64 KB slot). A 16-step-only variant takes 816 B per pattern.
- RAM: playing + editing copies ≈ 6.5 KB, plus a small event queue. This is negligible next to BT buffers (the SDK comments mention ~18 KB RX).

### 3.3 Timing
- **Internal clock:** a dedicated hardware timer IRQ at 1 kHz (or 2 kHz) with a fractional phase accumulator → **96 PPQN** internal resolution. At 300 BPM a tick is 2.08 ms; at 120 BPM it is 5.2 ms. Swing and micro-timing are offsets in PPQN units. MIDI Clock out = every 4th tick (24 PPQN, `F8`), plus `FA/FB/FC` and SPP `F2`.
- Don't use the SDK soft timers (`sys_timer`/`usr_timer`) for the clock. Their resolution and jitter are ❓ (see `doc/architure/系统定时器接口设计说明文档.pdf`). Keep the scheduler ISR and the MIDI TX enqueue in RAM (`.volatile_ram_code`-style sections) so flash cache misses can't stretch them 🟡.
- **Clock in (USB):** USB-MIDI arrives in 1 ms frames, so jitter is about ±1 ms. Use a PLL or moving-average filter on `F8` intervals to derive tempo, and interpolate 96 PPQN between pulses.
- **BLE-MIDI:** connection interval 7.5–15 ms (Apple recommends 11.25 ms for MIDI accessories) ❓ (not re-verified this session: [Apple Accessory Design Guidelines](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf), [MIDI Association BLE-MIDI](https://midi.org/midi-over-bluetooth-low-energy-ble-midi)). Packets carry 13-bit ms timestamps.
  - For **clock out**, set accurate timestamps; receivers that honour them (CoreMIDI) cancel most of the interval jitter.
  - For **clock in**, de-jitter using the sender timestamps, not arrival time.
  - Expect BLE latency of ~10–20 ms. Tell users USB is the "tight" sync option.
- **Flash writes stall execute-in-place.** A 4 KB sector erase takes tens to hundreds of ms 🟡, so only save patterns while stopped, or queue the save until Stop. Confirm whether the SDK's VM/flash driver runs from RAM with IRQs on ❓.

### 3.4 Power
While playing, low-power sleep must be inhibited (keep the CPU at a modest clock such as 48–96 MHz ❓). LED animation and BLE dominate the power draw, not the sequencer logic. Add "auto-stop on idle when not synced", and dim the LEDs on battery.

### 3.5 UI sketch (Shift layers)
| Control | Seq mode |
|---|---|
| Enter/exit | Shift + Note Repeat (or long-press Note Repeat) |
| Pads | 16 steps of the selected track. Colour = track colour, brightness = velocity, white = playhead |
| Pad Bank button (hold) + pad | Select track (pad = instrument) |
| Left/Right | Page 1–4 (steps 1–16 … 49–64). Shift = pattern length |
| Play / Stop / Rec | Transport. Rec + Play = live record (quantized into steps). Shift + Play = send/follow clock toggle |
| Hold step + encoders bank 1 | Velocity, gate, probability, micro-timing, ratchet, note offset |
| Encoders bank 2 | Track: length, clock divisor, swing, MIDI ch/note, mute, tempo |
| Shift + pads | Pattern select (chain with Shift + 2 pads). Shift + Rec + pad = clear, copy/paste via hold |
| Tap tempo | Reuse the existing note-repeat tap gesture |

### 3.6 Effort (focused engineering time, rough)
| Step | Estimate |
|---|---|
| Containerized build of vanilla `ac636n_hid` → `update.ufw` | 2–4 days |
| Flashing/recovery host validated (RPi/Linux + jl-uboot-tool, full dump, restore stock) | 2–5 days |
| Hardware mapping: pad sensing (ADC/mux), encoders, buttons, RGB LED driver, charger/battery | 1–3 weeks (the largest unknown) |
| USB-MIDI class (3 cables, descriptors, IN/OUT) | 3–5 days |
| BLE-MIDI GATT (+ timestamps, coexistence with USB) | 3–5 days |
| Pads (velocity/pressure), encoders, LEDs, presets, power ⇒ **stock parity** | 2–3 weeks |
| **Prototype 1:** MIDI clock follower/sender (USB first) + 16-step 16-track drum seq | 1–2 weeks |
| Full sequencer (pages, per-step params, patterns in flash, polish) | +3–4 weeks |

**Verdict:** a sequencer is **realistic on path B** and the hardware has ample headroom. As a path-A patch it is **not realistic**: it needs 10–20 KB of new code, hooks into undocumented pad, LED and MIDI routines, and relocating VM. At most, a tiny path-A experiment (e.g., MIDI clock passthrough or LED playhead) could prove timing hooks.

---

## 4. Risks
| Risk | Impact | Mitigation |
|---|---|---|
| No flashing path on macOS (no macOS `jl-uboot-tool` SCSI backend) | Blocks USB UBOOT recovery on macOS | Dedicated Linux/RPi flashing host, or BLE OTA |
| Bricking / losing `key_mac`, trim, BT data | Device dead or unpaired | Verified BLE dump exists. Make a mask-ROM USB dump before the first flash. Never `-format all`. Keep the `0x7F000` and USRTRIM areas |
| Pin map / pad sensing harder than expected | Path B stalls | Map the hardware pins systematically and verify on a logic analyser |
| Toolchain licence/redistribution unclear | Can't publish binaries/toolchain | Download at build time. Publish sources and patches only ❓ |
| `host-client`/`strip-ini` missing in the Linux post-build | Packaging fails | Call `isd_download` directly and verify the resulting flash area table |
| SincoOTA might reject custom images (signature?) | Can't update via the official app | Not observed so far ❓. Use USB/RCSP tools we control |
| BLE-MIDI jitter | Loose sync over BT | Timestamps, and recommend USB for clock |
| Flash erase stalls XIP during playback | Timing glitches | Save only when stopped. Put the ISR in RAM |

## 5. Next steps
1. **Flashing host:** prepare a Linux box or RPi. Make a read-only mask-ROM dump with `jl-uboot-tool` and diff it against `firmware/backup/`.
2. **Build pipeline:** script `setup_toolchain.py` and `build_ufw.sh` (`Makefile` build + direct `isd_download -key smcpad.key …`) inside a reproducible `x86_64` Linux container ([firmware/builder/](../firmware/builder/README.md)).
3. **Packaging check:** verify that the `update.ufw` area table (`PRCT`, `VM`, `BTIF`, `EXIF`, `USRFLASH`, `USRTRIM`) matches stock.
4. **Hardware pin map:** document pad scan (SARADC + mux), encoder matrix, LED output, and TRS MIDI TX in [docs/pinmap.md](pinmap.md).
5. **Path B "hello":** vanilla `ac636n_hid` with an LED blink plus a USB-MIDI descriptor. Then add BLE-MIDI.
6. **Prototype 1:** USB clock follower + 16-step drum sequencer.

### Open questions
- What is the stock RAM headroom, and the size of our path-B app with the BT stack (≈150–220 KB assumed)?
- Is the SDK's soft-timer jitter good enough, or do we need a dedicated HW timer (assumed: HW timer)?
- Does the BR25 single-backup OTA loader stay bootable after a bad app image (would allow BLE recovery)?
- Can `isd_download` drive the BR25 USB boot mode directly without JieLi's USB dongle, or must we use jl-uboot-tool?
