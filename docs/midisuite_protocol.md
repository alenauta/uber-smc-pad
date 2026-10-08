# `00 59` Configuration Protocol & Preset Wire Specification

Wire-level interoperability specification for the `00 59` configuration protocol used over **BLE (`AE40`)** and **USB-MIDI SysEx** to read and update SMC-PAD presets, global settings, and pad calibration.

Implemented cleanly in [firmware/sdk_overlay/sinco/](../firmware/sdk_overlay/sinco/) (`sinco_proto.[ch]`, `sinco_store.[ch]`) and tested in `test_sinco_proto.c` and `test_sinco_store.c`.

---

## 1. Packet Structure

Both BLE and USB-MIDI carry the same inner binary packet:

```text
00 59 | cmd (u8) | len (u24 LE) | payload[len] | checksum (u8)
checksum = (~sum(payload)) & 0xFF
```

- **BLE transport (GATT service `AE40`):**
  - Requests are written to characteristic `AE41` (Write Without Response) in MTU-sized chunks.
  - Replies arrive as notifications on characteristic `AE42` (also chunked to `MTU - 3` bytes).
- **USB-MIDI transport (7-bit LSB-first SysEx framing):**
  - Inner packets are wrapped in `F0 <7-bit stream> F7` with no MIDI Manufacturer ID:
    ```python
    def to_midi_sysex(pkt: bytes) -> bytes:
        out = [0xF0]
        acc = n = 0
        for b in pkt:
            acc |= b << n
            n += 8
            while n >= 7:
                out.append(acc & 0x7F)
                acc >>= 7
                n -= 7
        if n:
            out.append(acc & 0x7F)
        out.append(0xF7)
        return bytes(out)
    ```
  - Decoding skips `F0`, stops at `F7`, shifts 7 bits in LSB-first per byte, and emits an 8-bit byte whenever `n >= 8`.
  - Every USB SysEx frame begins with `F0 00 32 …` simply because the inner header begins with `00 59`.

---

## 2. Commands

| `cmd` | Direction | Payload | Reply / Semantics |
|---|---|---|---|
| `0x11` | Host → Pad | *(empty, len 0)* | Reply `cmd = 0x11`: null-padded ASCII `<NAME>_<VVV>` (e.g. `SMC-PAD_005`; custom firmware reports `SMC-PAD_000` so stock updaters remain willing to flash back) |
| `0x12` | Host → Pad | *(empty, len 0)* | Reply `cmd = 0x12`: 13-byte status block |
| `0x15` | Host → Pad | Short MIDI bytes | Injects MIDI into host-in feedback handler; replies with ACK (`00 59 00 01 00 00 00 FF`) |
| `0x23` | Host → Pad | `type (u8), addr (u32 LE), len (u24 LE)` | **Read** storage region (chunks ≤ 512 B over BLE, ≤ 1009 B over USB). Reply echoes `type, addr, len` followed by `data[len]` |
| `0x22` | Host → Pad | `type (u8), addr (u32 LE), len (u24 LE), data[len]` | **Write** storage region in RAM (chunks ≤ 512 B). Replies with status packet `00 59 00 01 00 00 00 FF` (`cmd = 0x00`, `code = 0x00`) |
| `0x22` (len 0) | Host → Pad | `type (u8), 0 (u32 LE), 0 (u24 LE)` | **Commit ("Save")** RAM buffer to flash (`type = 5` for presets at `0x77000`, `type = 3` for calibration at `0x7E000`) |
| `0x00` | Pad → Host | `code (u8)` | Status / error reply (`code = 0x00` OK → `00 59 00 01 00 00 00 FF`; `code = 0x01` error → `00 59 00 01 00 00 01 FE`) |

### Worked Wire Examples
```text
Query name/version:
  Inner : 00 59 11 00 00 00 FF
  SysEx : F0 00 32 45 00 00 00 40 7F F7

Read globals (type 4, addr 0, len 12):
  Inner : 00 59 23 08 00 00 04 00 00 00 00 0C 00 00 EF
  SysEx : F0 00 32 0D 41 00 00 00 02 00 00 00 00 40 01 00 00 6F 01 F7

Save presets to flash (type 5, addr 0, len 0):
  Inner : 00 59 22 08 00 00 05 00 00 00 00 00 00 00 FA
  SysEx : F0 00 32 09 41 00 00 40 02 00 00 00 00 00 00 00 00 7A 01 F7
```

---

## 3. Storage Types & Record Layouts

| `type` | Region | Size | Description |
|---|---|---|---|
| `0` | Raw SPI flash | `512 KB` (`0x00000–0x7FFFF`) | Read-only in custom firmware (used for full unit backup; raw writes are refused for safety) |
| `3` | Pad calibration | `160 B` (`16 × 10 B`) | Backed by `USRTRIM` (`0x7E000`, followed by `"trimdat\0"`). Bytes `0..1` per pad = `u16 LE` sensitivity threshold (`level * 100 + 50`, `level = 0..7`); bytes `2..9` = four `u16 LE` velocity/pressure breakpoints |
| `4` | Global settings | `12 B` | Persisted in JieLi VM item `0x2C` (magic `0x3AA3`) |
| `5` | User presets | `28,312 B` (`8 × 3,539 B`) | Backed by `USRFLASH` (`0x77000–0x7DE97`); exported single-preset `.spc` files are raw `3,539 B` slices |

### Global Settings (`type = 4`, 12 Bytes)
| Byte(s) | Type | Field | Range | Factory Default |
|---|---|---|---|---|
| `0–1` | `u16 LE` | Tempo (BPM) | `30–300` | `120` |
| `2` | `u8` | Swing (%) | `50–75` | `50` |
| `3` | `u8` | Note-Repeat Rate | `0` `1/4`, `1` `1/4T`, `2` `1/8`, `3` `1/8T`, `4` `1/16`, `5` `1/16T`, `6` `1/32`, `7` `1/32T` | `4` (`1/16`) |
| `4` | `u8` | External Clock Sync | `0` / `1` | `0` |
| `5` | `u8` | Note-Repeat Latch | `0` / `1` | `0` |
| `6` | `u8` | Octave Group | `0–6` (displayed `1–7`) | `2` |
| `7` | `s8` | Transpose | `-16…+16` (forced `0` at octave `0` or `6`) | `0` |
| `8` | `u8` | Pad Velocity Curve | `0–3` (Linear, Soft, Hard, Fixed) | `1` |
| `9` | `u8` | Aftertouch Enable | `0` / `1` | `1` |
| `10` | `u8` | Active Preset | `0–7` (`Preset 1–8`) | `0` |
| `11` | `u8` | Active Pad Bank | `0` = Bank A, `1` = Bank B | `0` |

### Preset Record (`3,539 B = 5×23 + 16×6 + 128×26`)
- **`+0x000` — 5 Assignable Transport Buttons (`5 × 23 B`):**
  - `byte 0`: `0` CC Single, `1` CC Toggle, `2` CC Push (momentary), `3` Program Change, `4` Custom SysEx, `5` Note (ch 1).
  - `byte 1`: MIDI channel (`0..15`).
  - `bytes 2–4`: `[number, value1, value2]` (`LSB, MSB, Program` for Program Change; `value2` sent on press and `value1` on release for Push/Momentary).
  - `byte 5`: Custom SysEx length (`0..16`), `bytes 6–21`: SysEx payload (`16 B`), `byte 22`: incoming LED feedback note (`0xFF` = none).
- **`+0x073` — 16 Encoder Records (`2 banks × 8 knobs × 6 B`):**
  - `byte 0`: `0` CC, `1` Channel Aftertouch, `2` Mackie Fader (pitch bend + touch note `0x68 + ch`), `3` Relative CW (`Left / Right`).
  - `byte 1`: Speed (`0` Lowest … `4` Fastest), `byte 2`: channel (`0..15`), `bytes 3–5`: `[cc, min, max]`.
- **`+0x0D3` — 128 Pad Records (`128 × 26 B`):**
  - Bank A indexes `clamp(octave * 16 + pad + transpose, 0, 127)`; Bank B indexes `112 + pad` (`pad = 0..15` bottom-left to top-right).
  - `byte 0`: `0` Note, `1` CC Toggle, `2` CC Momentary, `3` Program Change, `4` MCP (Mackie note on ch 1), `5` Custom SysEx, `6` Comb-MCP.
  - `byte 1`: channel (`0..15`), `bytes 2–4`: `[note/cc/lsb, min/up/msb, max/down/prog]`, `bytes 5–7`: RGB colour (`R, G, B`), `byte 8`: LED feedback note, `byte 9`: SysEx length (`0..16`), `bytes 10–25`: SysEx payload (`16 B`).
