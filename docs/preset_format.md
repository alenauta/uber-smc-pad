# Preset & global-settings format (stock firmware `SMC-PAD_005`)

Tools (stdlib only, offline):
- [decode_presets.py](../tools/config/decode_presets.py): human-readable dump, diff, globals.
- [preset_json.py](../tools/config/preset_json.py): lossless binary ⇄ JSON editor.

```sh
python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin            # all presets
python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin --preset 2 --all-pads
python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin --diff "<SPB.bin>"
python3 tools/config/decode_presets.py firmware/backup/flash_dump_1.bin --globals
```

Sources:
- on-device preset/globals reads and live MIDI captures ([stock_checks.md](stock_checks.md), [stock_midi_map.md](stock_midi_map.md));
- the `00 59` configuration protocol specification in [midisuite_protocol.md](midisuite_protocol.md).

Confidence markers: ✅ confirmed (code + data/capture) · 🟡 strong inference · ❓ unknown.

## Where presets live
| Source | Location |
|---|---|
| SPI flash | `0x77000`–`0x7DE97`: 8 × 3539 B, packed, inside the `USRFLASH` area (`0x77000`, 0x7000 B) ✅ |
| Sinco read/write type 5 (BLE **and** USB SysEx) | `addr` = byte offset into the 8 × 3539 B block ✅. Midi Suite reads `(0, 28312)` in one go. |
| Factory defaults | Midi Suite `assets/bin/SPB.bin` (28 312 B, same layout). Factory reset writes this file. ✅ |
| Export/import | `.spc` file = one raw 3539 B preset ✅ |

### Preset slots
The manual says: *"Shift + Pads 1-8: Switch between different preset configurations. Pad 1 is Performance preset, Pad 2 is DAW preset, the rest are user presets."*

| Slot | Kind | Factory content (`SPB.bin`) |
|---|---|---|
| 1 | Predefined: **Performance** | Notes on ch10, CC buttons 25–29, CC knobs 30–45 |
| 2 | Predefined: **DAW** (Mackie) | Note buttons (46/47 bank ←/→, 94 Play, 93 Stop, 95 Rec), MCP pads, knobs K1–K8 type Note on ch1–8 |
| 3–8 | **User** | Byte-identical copies of Performance |

All 8 slots share one storage format. Whether slots 1–2 can be edited is untested. On this unit only user slots 4 and 5 have been edited.

## Preset layout (3539 B = 5·23 + 16·6 + 128·26) ✅
| Offset | Size | Content |
|---|---|---|
| `0x000` | 5 × 23 | Buttons |
| `0x073` | 16 × 6 | Knobs: bank 1 = K1–K8, then bank 2 |
| `0x0D3` | 128 × 26 | Pad records |

### Button record (23 B)
| Byte | Field |
|---|---|
| 0 | type: `0` CC Single · `1` CC Toggle · `2` CC Push (momentary) · `3` Program · `4` Custom SysEx · `5` Note ✅ |
| 1 | channel, 0-based (ignored by type 5 Note) |
| 2, 3, 4 | number / value 1 / value 2. Single: v1 (byte 3) on press. Toggle: v2 on the first press, then v1, alternately. Push: v2 (byte 4) on press, v1 on release, so factory 0/127 sends 127 / 0 ✅. Program: see the pad Program note below. |
| 5 | SysEx length |
| 6–21 | SysEx bytes, sent raw for type 4. Every factory button stores MMC `F0 7F 7F 06 0x F7`. |
| 22 | "Led": incoming note number that drives this button's LED (`FF` = none). Factory: DAW transport = its own note (94/93/95) ✅ |

Type 5 **Note** always sends `90 kk 7F` on press and `90 kk 00` on release, on **channel 1** ✅.

### Knob record (6 B)
| Byte | Field |
|---|---|
| 0 | type: `0` CC · `1` Aftertouch · `2` Note (= Mackie fader) · `3` CW (relative CC; min/max become Left/Right values) ✅ |
| 1 | speed: `0` lowest · `1` low · `2` normal · `3` fast · `4` fastest ✅ |
| 2 | channel, 0-based |
| 3, 4, 5 | number / min / max |

What each type sends ✅ (firmware): the value moves **one step per tick** towards the target, so every intermediate value is sent.
- `0` CC: `Bn cc val`. `1` Aftertouch: `Dn val`. `3` CW: `Bn cc Left|Right`, repeated |delta| times.
- `2` **Note = Mackie fader**: touch note `90 (0x68+channel) 7F` on channel 1, then pitch bend `En 00 val` on the knob's channel (7-bit), and `90 (0x68+channel) 00` about 100 ticks after the last move. Byte 3 is ignored. The DAW preset (K1–K8 = ch1–8) therefore gives Mackie faders 1–8.
- Incoming `Bn cc vv` matching a knob's channel/number sets its current value (no jump on the next move).

### Pad record (26 B)
| Byte | Field |
|---|---|
| 0 | type: `0` Note · `1` CC Toggle · `2` CC Momentary · `3` Program · `4` MCP (Mackie) · `5` Custom SysEx · `6` Comb-MCP ✅ |
| 1 | channel, 0-based (factory: 9 → ch10); ignored by MCP |
| 2, 3, 4 | number / value 1 / value 2. Momentary: CC / Up (byte 3, release) / Down (byte 4, press). Note: note / **velocity min / velocity max** ✅. Program: see below. |
| 5, 6, 7 | LED colour R, G, B ✅ |
| 8 | "Led": incoming note number that drives this pad's LED (`FF` = none). Factory: MCP pads = their own note ✅ |
| 9 | SysEx length ✅ |
| 10–25 | SysEx bytes (16) ✅ |

What each pad type sends ✅ (firmware, 005):
- `0` Note: velocity → curve → scaled into [byte 3, byte 4]; `9n kk vv`. Release is always `8n kk 40`. With aftertouch on, held pads send `Dn pp`.
- `1` CC Toggle: `Bn cc v2` on the first press, then `Bn cc v1`, alternately. `2` CC Momentary: v2 (Down) on press, v1 (Up) on release.
- `3` Program: `Bn 00 b3 · Bn 20 b2 · Cn b4` (byte 2 goes to CC32 bank LSB and byte 3 to CC0 bank MSB, matching Midi Suite's "LSB / MSB" labels). Same for buttons.
- `4` MCP: `90 kk 7F` / `90 kk 00`, always **channel 1**, velocity 127.
- `5` Custom SysEx: bytes 10… sent raw (length = byte 9).
- `6` Comb-MCP: the 16-byte field is parsed as a **list of short MIDI messages**, all sent on press 🟡.

**LED feedback** ✅: a host `90 kk vv` sets the LED state of every pad/button record in the active preset whose "Led" byte = kk; `9F 7F 7F` clears all.

### Which pad record is used ✅ (Midi Suite editor + firmware)
- **Bank A:** `record = (octave·16 + pad + transpose) & 0xFF`, with `octave` 0–6 (default 2 → notes 36–51) and `transpose` −16…+16 from the globals. The firmware doesn't clamp; instead, setting octave to 0 or 6 forces transpose to 0 (Midi Suite's editor clamps to 0–127). The 128 records are one linear table rather than fixed groups: the octave and transpose values slide a 16-pad window over it.
- **Bank B:** `record = 112 + pad`. This is fixed: octave and transpose don't apply.
- `pad` 0–15 runs bottom-left to top-right. Midi Suite draws it top row first: `[12..15, 8..11, 4..7, 0..3]`.

## The device's current presets vs factory
`--diff SPB.bin`: slots 1–3 and 6–8 are factory. The modified slots only change bank B:
- **Preset 4**: pad 5 → CC Toggle #50, colour `#7E51F4`.
- **Preset 5**:
  - pad 1 → CC Momentary #46;
  - pads 2 and 3 → CC Momentary #47;
  - pad 5 → CC Momentary #56;
  - pad 13 → CC Momentary #58;
  - pad 16 → CC Toggle #4.

  This matches the USB capture exactly, including the CC4 press with no release.

## Global settings (Sinco type 4, 12 B) ✅
Midi Suite reads it on load and polls it once a second. The firmware stores it as JieLi **VM item `0x2C`** (magic `A3 3A` + 12 B); in flash that shows up as 18-byte journal entries `00 A3 3A | payload[12] | 3 B` starting at **`0x27000`** (VM area `0x26000`–`0x74FFF`). There were 19 entries at backup time. Missing item ⇒ boot defaults 120/50/4/0/0/2/0/1/1/0/0/0 ✅.

| Byte | Field | Range | Factory reset | This unit |
|---|---|---|---|---|
| 0–1 | Tempo, u16 LE BPM | 30–300 | 120 | 74 |
| 2 | Swing % | 50–75 | 50 | 50 |
| 3 | Note-repeat rate | 0 1/4 · 1 1/4T · 2 1/8 · 3 1/8T · 4 1/16 · 5 1/16T · 6 1/32 · 7 1/32T | 4 (1/16) | 1 (1/4T) |
| 4 | Sync (to DAW clock) | 0/1 | 0 | 0 |
| 5 | Latch | 0/1 | 0 | 0 |
| 6 | Octave | 0–6 | 2 | 2 |
| 7 | Transpose, signed | −16…+16 | 0 | 0 |
| 8 | Velocity curve | 0–3 (Shift + Pads 9–12) | 1 | 2 |
| 9 | Aftertouch | 0/1 | 1 | 1 |
| 10 | Active preset, 0-based | 0–7 | 0 | 0 (it was 4 = preset 5 during the capture) |
| 11 | Pad bank | 0 = A, 1 = B | 0 | 0 |

## Writing (documented here, not used by us)
Over the same Sinco packet (BLE, or USB SysEx; see [midisuite_protocol.md](midisuite_protocol.md)):
- `0x22` WRITE type/addr/len + data. Midi Suite sends every UI edit as a live single-field write.
- A **zero-length `0x22` write = commit to flash** ("Save"). Type 5 for presets, type 3 for calibration.

Calibration (type 3, flash `0x7E000` USRTRIM) = 16 pads × 5 × u16 LE ✅: `[0]` on-threshold = `level·100 + 50` (level 0–7), `[1..4]` four increasing velocity/pressure breakpoints. Defaults **50, 550, 720, 850, 950**; the flash record ends with `"trimdat\0"`. A zero-length type-3 write commits it. Firmware 005 rejects pads whose threshold is outside 50…750 or whose breakpoints aren't increasing.

Velocity curves (global byte 8) 🟡: `0` = v>>3 · `1` = 3v/32+32 · `2` = v/16+64 · `3` = fixed 127.

## Output routing ✅
- Every controller message goes to **USB, BLE-MIDI and TRS at the same time**.
- Over USB every event is sent twice, on **cable 0 and cable 1** (Private + Master). The third cable (`SMC-PAD-MCP`, USB PID `4353:3243`) carries Sinco SysEx replies and Mackie/DAW messages ✅.

> [!CAUTION]
> Our preset and diagnostic tools stay read-only unless explicitly directed. Always maintain a verified flash backup and tested recovery path ([recovery.md](recovery.md)). Avoid sending unknown short USB SysEx frames ending in `7D F7`, which can trigger JieLi's USB bootloader mode.

## Remaining unknowns: cheap read-only experiments
Run `midimon.py` over USB and do these on the device; nothing gets written by us. They now serve to **confirm** the firmware analysis:
1. DAW preset (Shift + Pad 2): turn K1–K8; expect `90 68+n 7F`, pitch bend on ch n, then `90 68+n 00`.
2. Press an MCP pad; expect `90 kk 7F` / `90 kk 00` on channel 1.
3. LED feedback: send `90 kk vv` from the host to a pad whose Led = kk. **This sends MIDI to the device, so it needs approval first.**
