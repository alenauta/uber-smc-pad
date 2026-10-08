# BLE and configuration (custom firmware)

What the custom firmware ([firmware/sdk_overlay](../firmware/sdk_overlay/README.md)) offers over Bluetooth LE and USB for MIDI and configuration, and where it deliberately differs from stock `SMC-PAD_005`.

> [!CAUTION]
> Always keep a verified flash backup ([recovery.md](recovery.md)) and the official `SMC-PAD_005.fwsc` handy before flashing.

## Identity
| | Custom | Stock |
|---|---|---|
| BLE name (advertising, GAP `2a00`) | `SMC-PAD` (`SMCPAD_DEVICE_NAME` in [ble_smcpad.h](../firmware/sdk_overlay/ble/ble_smcpad.h)) | `SMC-PAD` |
| Sinco `0x11` name/version | `SMC-PAD_000` | `SMC-PAD_005` |
| Device info `2a29` / `2a24` | `SMC-PAD Project` / `SMC-PAD` | vendor strings |
| USB VID:PID | `1209:0001` "SMC-PAD Project" (test ID) | `4353:3243` |
| Classic Bluetooth (EDR) | off | off |
| Pairing | "Just Works" bonding (SM on, `NoInputNoOutput`). 919+: the pad asks to pair when an app subscribes to BLE-MIDI notes (`sm_api_request_pairing`, once per unencrypted connection), so synths / DAWs bond (one iOS pop-up), stay listed and reconnect, while SincoOTA / Midi Suite (no MIDI subscription) connect without a pop-up. 917–918 asked on every connection (`slave_security_auto_req = 1`), which disturbed SincoOTA updates. ≤ 916: host-initiated only. | bonds (iOS pairing pop-up), auto-reconnects |
| Image PID (OTA) | `AC636N_TRANS` (unchanged, RCSP OTA still matches) | `AC636N_TRANS` |

Version `000`: SincoOTA compares the device version with the version in the `.fwsc` file name and refuses older files ("The device version is higher than the firmware"). Reporting 000 guarantees the official stock files can always be flashed back. Our images are wrapped as `SMC-PAD_9xx.fwsc` ([make_fwsc.py](../tools/fw/make_fwsc.py)). Midi Suite may show its "update with SincoOTA" hint; that is harmless. Flashing order and recovery: [first_flash.md](first_flash.md).

**Safe mode** ([smcpad_safe.c](../firmware/sdk_overlay/board/smcpad_safe.c)):
- Trigger: two boots in a row that end within 5 s, either a crash/watchdog loop or by hand (on, off within 5 s, twice).
- The third boot skips all SMC-PAD drivers. Only BLE, OTA, USB and a minimal Sinco responder run (`0x11` / `0x12` answered, storage refused), so SincoOTA can always reach the pad.
- `apply.py --safe-only` builds an image that always boots that way.

## GATT table
Generated from [ble_smcpad_profile.json](../firmware/sdk_overlay/ble/ble_smcpad_profile.json) by [gen_jl_profile.py](../tools/gatt/gen_jl_profile.py). The generator's output is golden-tested against the SDK's own table (`python3 tools/gatt/test_gen_jl_profile.py`). Layout matches stock BLE services.

| Service | Characteristic | Props | Value handle / CCC | Use |
|---|---|---|---|---|
| `1800` GAP | `2a00` name | R (dyn) | `0x03` | `SMC-PAD` |
| `1801` GATT | `2a05` | I | | service changed |
| `180a` Device info | `2a29`, `2a24` | R | `0x0a`, `0x0c` | strings above |
| `180f` Battery | `2a19` | R, N (dyn) | `0x0f` / `0x10` | `get_vbat_percent()`, notified every 60 s |
| `ae40` Sinco | `ae41` | WwR (dyn) | `0x13` | Sinco packets in, MTU-sized chunks |
| | `ae42` | N | `0x15` / `0x16` | Sinco replies, chunked to MTU − 3 |
| `03B80E5A-…` BLE-MIDI | `7772E5DB-…` | R, WwR, N (dyn) | `0x19` / `0x1a` | MIDI in / out |
| `ae00` RCSP (`RCSP_BTMATE_EN`) | `ae01` / `ae02` | WwR / N | `0x82`, `0x84` / `0x85` | JieLi RCSP OTA (SincoOTA) |

Advertising: flags + the 128-bit BLE-MIDI service UUID + name, every 100 ms. The scan response carries the RCSP tag, as in the SDK. After connecting, the firmware asks for a 11.25–15 ms connection interval (`{9, 12, 0, 400}` first in the table) for MIDI latency.

## MIDI routing
- **Out:** every controller message goes to USB cable 0, TRS and BLE-MIDI (once notifications are on). Stock also sends a copy on USB cable 1; we don't.
- **In:** USB short messages, BLE-MIDI and Sinco `0x15` payloads all feed the preset interpreter's host-in handler (LED and knob-value feedback, see below).
- BLE-MIDI packets carry 13-bit timestamps. Messages are packed into one notification per task pass. If the link is busy, the oldest pending packet is dropped (counted, never blocks).

## Sinco protocol support
Packet `00 59 | cmd | len u24 | payload | ~Σ`. Over BLE it goes to `ae41`/`ae42`. Over USB it is SysEx, `F0` + an LSB-first 7-bit stream + `F7`, and replies go back **on the cable the request came in on**. See [midisuite_protocol.md](midisuite_protocol.md) for the format.

| Cmd | Stock | Custom |
|---|---|---|
| `0x11` name/version | 27 B, `SMC-PAD_005` | ✅ 27 B, `SMC-PAD_000` |
| `0x12` status | 13 B, layout unknown | ✅ 13 B of zeros |
| `0x15` MIDI inject | into the MIDI-in ring, then ack | ✅ to the host-in handler, ack `00 59 00 01 00 00 00 FF` |
| `0x23` read | type/addr/len + data | ✅ types 0, 3, 4, 5 |
| `0x22` write | type/addr/len + data → ack `00 59 00 01 00 00 00 FF` (cmd **0**) | ✅ types 3, 4, 5, same ack (since 905; 904 echoed cmd `0x22`, which Cube Suite rejects with "failed to send data") |
| `0x21` erase | storage-object erase | ❌ refused (error reply) |
| `0x24` aux | u16 (CRC?) | ❌ refused (meaning unknown) |
| anything else | error | error `00 59 00 01 00 00 01 FE` |

| Type | Area | Read | Write |
|---|---|---|---|
| 0 raw flash | absolute address | ✅ if `addr + len ≤ 0x80000` (full backup still works) | ❌ refused |
| 3 calibration | 16 × 10 B (RAM copy of USRTRIM `0x7E000`) | ✅ | ✅ RAM. **Zero-length write = commit**: erase 4 KB, write 160 B + `trimdat\0`. addr 1/2 len 0 = calibration mode on/off (stored, no effect yet) |
| 4 globals | 12 B | ✅ | ✅ live, stock clamping rules (octave 0/6 ⇒ transpose 0). Saved to VM `0x2C` (magic `0x3AA3`, stock format) 2 s after the last change |
| 5 presets | 8 × 3539 B (RAM copy of USRFLASH `0x77000`) | ✅ | ✅ live in RAM (takes effect immediately). **Zero-length write = commit**: erase 7 × 4 KB, write 28 312 B. `addr 0x80000000` (stock mode flag, ≤ 4 B): write accepted and ignored, read returns 0 |

**Why the refusals.** These are plan decision 3: nothing a host sends can brick the device or erase the recovery anchors.
- Raw writes and `0x21` could overwrite the bootloader, the BT MAC or the code.
- `0x24` and the type-5 flag are not understood.

Flash erase/write is only allowed when the address looked up from the image's `USRFLASH` / `USRTRIM` files equals the stock `0x77000` / `0x7E000` ([smcpad_store_port.c](../firmware/sdk_overlay/board/smcpad_store_port.c)).

Blank or unreadable `USRFLASH` at boot: RAM gets the factory Performance preset in all 8 slots. It is byte-identical to Midi Suite's `SPB.bin` slot 1, but slot 2 is *not* the stock DAW preset. Nothing is written to flash until a commit.

## Preset interpreter
[smcpad_preset.c](../firmware/sdk_overlay/board/smcpad_preset.c) plays the active preset (globals byte 10) from the stock record layout ([preset_format.md](preset_format.md)).

| Input | Supported | Deferred |
|---|---|---|
| Pads | Note (curve → [min, max], release `8n kk 40`, channel AT from the strongest held pad), CC Toggle, CC Momentary, Program, MCP (`90 kk 7F/00` ch1), Custom SysEx (only well-formed `F0 … F7`) | Comb-MCP |
| Pad index | bank A `octave·16 + pad + transpose`, bank B `112 + pad`. Out of range ⇒ the pad is silent (stock reads past the table) | |
| Buttons | hw buttons 3–7 = records 1–5: CC Single, Toggle, Push, Program, SysEx, Note (ch1) | |
| System buttons | hw 1 = pad bank (toggle; momentary in presets 5–8, as stock), hw 2 = knob bank | hw 0, 8, 9 (stock Shift / note-repeat functions, not traced) |
| Knobs | CC, Aftertouch, "Note" = Mackie fader (touch `90 68+ch 7F` ch1, `En 00 v`, release after 1 s), CW (Right/Left value per step). Stock speed table, deltas collected over 50 ms, every intermediate value sent (≤ 8 per 10 ms tick) | |
| Host → device | `9n kk vv` sets the LED of every pad/button record with Led = kk; `9F 7F 7F` clears all; `Bn cc vv` sets the matching CC knob's value | MCP LED special cases |
| LEDs | pad = record colour, ⅛ brightness idle, full while held, host Led state scales it; button LED = held, toggle state, host Led state, bank state | |

Value semantics (`x = 0x7F` → byte 4, `x = 0` → byte 3):
- Push/Momentary send byte 4 on press and byte 3 on release;
- Toggle's first press sends byte 4;
- Single sends byte 3.

This matches observed stock USB-MIDI captures (factory buttons 0/127 → 127 on press).

Program uses the 005 order (byte 2 = bank LSB → CC32, byte 3 = MSB → CC0), which matches Midi Suite's labels.

Not implemented: note repeat / arpeggiator, tempo / clock sync, latch, Shift functions.
