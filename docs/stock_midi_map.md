# Stock firmware – observed USB/MIDI behaviour

> [!NOTE]
> The device under test runs official stock firmware **`SMC-PAD_005`** (Sinco `0x11` BLE query).

Captured 2026-10-08 with [`tools/midi/midimon.py`](../tools/midi/midimon.py) (receive-only). Raw log: [captures/2026-10-08_stock_run1.log](captures/2026-10-08_stock_run1.log) (120 s, ~4.4k messages).

## USB identity
| Field | Value |
|---|---|
| VID:PID | `0x4353:0x3243` |
| Manufacturer / Product | `Jieli Technology` / `SINCO` |
| Serial | 8-character ASCII string |
| bcdDevice / bcdUSB | `0x0100` / `0x0110`, full-speed, EP0 64 B |
| Interfaces | #0 Audio Control (1/1, 0 EPs), #1 MIDI Streaming (1/3, 2 EPs) |
| MIDI ports (in + out) | `SMC-PAD-Private`, `SMC-PAD-Master`, `SMC-PAD-MCP` |

> [!NOTE]
> Stock firmware `SMC-PAD_005` exposes 3 USB-MIDI virtual cables (`SMC-PAD-Private`, `SMC-PAD-Master`, and `SMC-PAD-MCP`).

## Port behaviour
- Every message is sent **twice**: identically on `Private` **and** `Master`, in the same millisecond.
- `SMC-PAD-MCP` stays silent in Performance mode and carries Mackie/DAW messages plus USB `00 59` SysEx replies.
- Nothing was received on the host-to-device direction (we didn't send anything).

## Pads (16, velocity + channel pressure)
- **Channel 10**, Note On `0x99` with velocity; Note Off `0x89` with a fixed release velocity of **64**.
- Bank A layout (MPC style, notes rise from bottom-left):
  ```
  48 49 50 51   <- top row
  44 45 46 47
  40 41 42 43
  36 37 38 39   <- bottom row
  ```
- Velocity seen: 72..127 (soft hits around 70–80; the curve may be compressed at the low end).
- While a pad is held it streams **Channel Aftertouch** (`0xD9`, mono, not polyphonic) about every **10 ms** (≈100 Hz scan), ending with `AT 0` and then the Note Off.
- **Bank B** (PAD BANK) is pad records 112–127 (see [preset_format.md](preset_format.md)). The device was on **user preset 5**, where bank B pads 1/2/3/5/13/16 had been reprogrammed to CC46/47/47/56/58/4 on ch1. That explains the missing notes 52–54, 56, 64 and 67 in the sweep. ✅ Decoded from the flash dump.

## Knobs (endless encoders emulating absolute CC)
- **Channel 1.** Bank 1: **CC30–37**. Bank 2: **CC38–45** (CC42 not seen, CC40 only once).
- Every value change is exactly ±1 (1887/1887 steps), and each knob starts from 0 → these are **rotary encoders**, and the firmware keeps a 0..127 counter per knob. They are not potentiometers.

## Buttons (momentary, ch1, 127 on press / 0 on release)
| CC | Notes |
|---|---|
| 25, 26, 27, 28, 29 | the 5 assignable buttons (preset button records 1–5) |

> [!NOTE]
> CC46, 47, 56, 58 and 4 in the capture came from **bank-B pads of user preset 5**, not from buttons. CC4 (pad 16) is pad type 1 = **CC Toggle** (confirmed from Midi Suite code), hence no release. Pad 3 is also CC47, which explains the "stray" `0`.

## Implications for custom firmware
- Encoders mean we can offer relative/endless modes, acceleration, and per-layer value memory at no hardware cost.
- The firmware already measures continuous pad pressure. **Poly aftertouch per pad** should be a pure firmware change.
- Duplicate output on two ports and a silent third port are easy cleanups.
