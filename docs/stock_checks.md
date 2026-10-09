# Read-only checks on the stock firmware

Four quick checks, about 15 minutes in total, that pin down the facts the custom firmware still guesses: pad order, knob direction, which physical button is which, and the Mackie-fader release time.

**Nothing is sent to the pad.** `midimon.py` only listens, and only buttons on the pad itself are pressed. It works over USB or Bluetooth.

> [!CAUTION]
> During these checks, keep Midi Suite, SincoOTA and any DAW **closed**: they send MIDI/SysEx to the pad. Don't run anything else from `tools/` that talks to the device.

## Setup
1. Connect the pad to the Mac, either way (stock firmware sends everything to USB **and** BLE):
   - **USB:** plug it in and switch it on.
   - **Bluetooth:** USB cable **unplugged**, pad switched on. Open **Audio MIDI Setup** → *Window* → *Show MIDI Studio* → the **Bluetooth** icon ("Configure Bluetooth"), click **Connect** next to `SMC-PAD`, and close that window. Connecting only subscribes to the pad's MIDI output; nothing is sent to it.
2. Select the **Performance** preset: **Shift + Pad 1**.
3. Make sure the pad bank is **A** (PAD BANK LED off) and the knob bank is **1**.
4. Open Terminal in the repo folder:
   ```sh
   cd path/to/smc-pad
   python3 tools/midi/midimon.py list        # USB: "SINCO SMC-PAD-Master" (+ -Private, Port 3); BLE: a source named "SMC-PAD"
   ```

Each check below writes a log to `docs/captures/`. Stop each recording with **Ctrl-C**.
- **USB:** use the filter `SMC-PAD-Master`, as written below. It shows each message once (stock duplicates everything on `-Private`).
- **Bluetooth:** macOS names the port `SMC-PAD Bluetooth`. Replace `SMC-PAD-Master` with `Bluetooth` in every command, e.g. `python3 tools/midi/midimon.py monitor Bluetooth | tee docs/captures/2026-10-09_A_pads.log`.
- Bluetooth adds a few ms of jitter to the timestamps. That is fine for all four checks.

## Check A: pad order (2 min)
```sh
python3 tools/midi/midimon.py monitor SMC-PAD-Master | tee docs/captures/2026-10-09_A_pads.log
```
1. Tap each pad **once**, slowly (about 1 per second), **bottom row left → right, then the next row up**, ending top-right (16 taps).
2. Press **PAD BANK** (bank B), tap the **bottom-left** pad once, then press PAD BANK again (back to A).
3. Ctrl-C.

Expected: notes 36, 37 … 51 in tap order, then 52 for bank B. Any other order is exactly what we need to know.

## Check B: knob direction and bank (2 min)
```sh
python3 tools/midi/midimon.py monitor SMC-PAD-Master | tee docs/captures/2026-10-09_B_knobs.log
```
1. Turn **K1** (the knob labelled 1: **bottom-left**; the panel numbers them from the bottom, `7 8 / 5 6 / 3 4 / 1 2`) **clockwise 5 clicks**, slowly.
2. Turn it **counter-clockwise 5 clicks**.
3. Turn **K8** clockwise 3 clicks.
4. Press the **knob-bank** button, turn K1 clockwise 3 clicks, then press the knob-bank button again.
5. Ctrl-C.

Expected: CC30 values going up, then down; CC37 for K8; CC38 after the bank switch.

## Check C: which button is which (3 min)
```sh
python3 tools/midi/midimon.py monitor SMC-PAD-Master | tee docs/captures/2026-10-09_C_buttons.log
```
1. Press each of the **10 buttons on its own**, once, about 2 seconds apart, in a fixed order you choose. Don't combine any with Shift.
2. **Write down the order** you pressed them in, using the printed labels (e.g. "SHIFT, PAD BANK, KNOB BANK, PLAY, …").
3. Note anything that happens on the pad itself (an LED changing, a mode switching).
4. Ctrl-C. If a button switched a mode on (an LED stays lit), press it again to switch it off.

Expected: 5 buttons send CC 25–29 (press 127 / release 0). The other 5 send nothing (system functions). Your list tells us which label is which hardware id.

## Check D: Mackie fader timing (2 min)
```sh
python3 tools/midi/midimon.py monitor SMC-PAD-Master | tee docs/captures/2026-10-09_D_fader.log
```
1. Select the **DAW** preset: **Shift + Pad 2**.
2. Turn **K1 one click** clockwise, then **take your hand off and wait 5 seconds**.
3. Do it once more (one click, hands off, 5 s).
4. Back to **Performance**: **Shift + Pad 1**.
5. Ctrl-C.

Expected: `90 68 7f` (touch), a pitch bend `e0 …`, then after a pause `90 68 00` (release). The timestamps give the release delay.

## Send back
- The four log files in `docs/captures/` (or just say "done": I can read them from the repo).
- The button order from check C, plus anything you noticed on the pad.

Each answer becomes a constant or table change in the firmware ([smcpad_logic.c](../firmware/sdk_overlay/board/smcpad_logic.c), [smcpad_preset.h](../firmware/sdk_overlay/board/smcpad_preset.h)).

## Results (2026-10-09, stock 005, over Bluetooth)
| Check | Result | Consequence |
|---|---|---|
| A pads | Bottom-left → top-right = notes **36…51** in order; bank B bottom-left = **52** ✅. Note Off `8n kk 40`, AT 0 before it, AT every ~7–15 ms ✅ | Pad index = record order, as implemented |
| B knobs | Clockwise = **value up** ✅. Top-left knob (label **K7**) sent **CC36** = index 6; bottom-right (label **K2**) sent **CC31** = index 1; after the knob-bank switch K7 sent **CC44** (6 + 8) ✅ | Panel label **Kn = stock index n−1** (CC 29+n in bank 1, +8 in bank 2). Panel layout from the bottom: `1 2 / 3 4 / 5 6 / 7 8` |
| C buttons | Pressed bottom-up (NOTE REPEAT … PAD BANK, BT not pressed): the 5 transport buttons, bottom-up **Record, Pause, Play, Right, Left**, sent CC **29, 28, 27, 26, 25** (127/0); the function buttons sent nothing ✅ | Records 1–5 = **Left, Right, Play, Pause, Record**. Matches the factory DAW preset (Mackie notes 46 Bank←, 47 Bank→, 94 Play, 93 Stop, 95 Rec) |
| D fader | DAW preset knob: touch / pitch bend / release arrive as **`F0 35 59 ss d1 d2 F7`** over BLE, e.g. `F0 35 59 10 6E 7F F7` = `90 6E 7F`. Release **~200 ms** after the last move | See below |

**Check D explained** (from the stock code):
- MCP pads, Note buttons and the Mackie fader go through `0x1E0FEA2`, not the normal `midi_send`. It wraps each message as `F0 35 59 <status & 7F> d1 d2 F7` (template in RAM `0x11296`).
- The USB TX path recognises that SysEx and sends the plain message on **cable 2** (`SMC-PAD-MCP`, macOS "Port 3"). BLE passes the wrapper through unchanged, which no DAW understands.
- The knob turned was stock index 6, so the DAW preset uses channel 7: touch note `0x68 + 6 = 0x6E`, pitch bend `E6`.

**Firmware changes:**
- Mackie-class messages now go out on **USB cable 2**, as on stock, and as **plain MIDI over BLE**.
- `SMCPAD_FADER_RELEASE_MS` is now **200**.
