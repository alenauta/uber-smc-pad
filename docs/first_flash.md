# Flashing & Round-Trip Recovery Guide (`SincoOTA`)

> [!CAUTION]
> **Unofficial firmware — flash entirely at your own risk.**
> This custom firmware is an independent, experimental hobby project provided **"AS IS" without warranty of any kind**. Flashing modified firmware voids your warranty and carries the risk of bricking your controller or losing presets/calibration. **The author assumes zero responsibility or liability for any bricked devices, lost data, or damages.**
>
> Before flashing anything:
> - Charge the SMC-PAD battery first.
> - Keep the official **`SMC-PAD_005.fwsc`** firmware file handy so you can revert to stock at any time.
> - Take a verified 512 KB flash backup of your unit ([firmware/backup/](../firmware/backup/README.md)) if you want an offline copy of your presets (`0x77000`) and factory pad calibration (`0x7E000`).
> - Close **Midi Suite** / **CubeSuite** and unplug the USB cable while flashing over Bluetooth.

---

## Files

| File | Description | Reported Version (`0x11`) |
|---|---|---|
| `SMC-PAD_005.fwsc` | Official M-VAVE stock firmware (`517,940` bytes, SHA-256 `e185d8e5…dca3`) | `SMC-PAD_005` |
| `SMC-PAD_925.fwsc` | Custom firmware + 16-track × 64-step sequencer (built in-browser via [docs/index.html](index.html) or via Docker) | `SMC-PAD_000` |
| `SMC-PAD_900.fwsc` | Minimal safe-mode test image (`apply.py --safe-only`: BLE + OTA + USB-MIDI only) | `SMC-PAD_000` |

Why custom builds report version `000`:
- `SincoOTA` compares the device's reported version against the 3-digit number in the `.fwsc` filename and refuses files with a lower number (*"The device version is higher than the firmware"*).
- Reporting `000` ensures the official **`SMC-PAD_005.fwsc`** is **always accepted** when you want to return to stock, while custom builds wrapped as `SMC-PAD_925.fwsc` are always accepted over stock `005`.

---

## How to Flash with `SincoOTA` (Always Takes 2 Passes)

Just like official M-VAVE firmware updates on the SMC-PAD, flashing over Bluetooth via **`SincoOTA`** (iOS or Android) **always takes two passes** because the first pass reboots the controller from normal application mode into JieLi's BLE recovery/updater loader (`ota.bin`).

### Preparation (Important!)
1. **Unplug USB** from the SMC-PAD, switch the unit **ON**, and make sure Bluetooth is active (close **Midi Suite** / **CubeSuite** and any DAW).
2. **Remove / "Forget" `SMC-PAD` from paired Bluetooth devices** in your phone's OS **Settings → Bluetooth** menu. *(If the OS holds an active BLE-MIDI connection in the background, `SincoOTA` will stall during handoff.)*
3. Copy `SMC-PAD_925.fwsc` (and keep `SMC-PAD_005.fwsc` as your stock backup) onto your phone.

### Pass 1 — Enter Recovery / Updater Mode
1. Open **`SincoOTA`**.
2. Scan and **select `SMC-PAD`**.
3. Select **`SMC-PAD_925.fwsc`** and tap **Start / Upgrade**.
4. `SincoOTA` transfers the small updater stub (`ota.bin`) and **reboots the SMC-PAD into recovery/updater mode** (pad RGBs turn off or freeze, Bluetooth LED turns off, and the first pass stops/times out waiting to reconnect around ~0–3%). **This is completely normal — nothing in your code flash has been overwritten yet.**

### Pass 2 — Re-Select the Device & Flash to 100%
1. **Crucial step:** Inside `SincoOTA`, **unselect / disconnect the current device**, then **search / scan again and select the newly discovered device** (now running in recovery/updater mode).
2. Select **`SMC-PAD_925.fwsc`** again and tap **Start / Upgrade**.
3. The second pass writes the firmware smoothly to **100%** and automatically reboots the controller into the new firmware.

> [!TIP]
> **Updating from one custom build to a newer custom build:** If starting Pass 1 directly with a custom `.fwsc` reports *"OTA failed"*, run Pass 1 selecting the official `SMC-PAD_005.fwsc` (which cleanly enters recovery/updater mode), then unselect, scan & re-select the device, and select `SMC-PAD_925.fwsc` for Pass 2.

---

## Returning to Official Stock Firmware (`SMC-PAD_005.fwsc`)

Because custom images preserve the exact stock SDK v1.0.0 bootloader (`uboot.boot`), stock RCSP updater (`ota.bin`), factory pad calibration (`USRTRIM` at `0x7E000`), and preset partition (`USRFLASH` at `0x77000`), reverting to stock uses the exact same 2-pass `SincoOTA` procedure with **`SMC-PAD_005.fwsc`**:

1. Forget `SMC-PAD` in your phone's Bluetooth settings if paired.
2. Run **Pass 1** + **Pass 2** (unselect → scan → re-select) in `SincoOTA` using **`SMC-PAD_005.fwsc`**.
3. On reboot, USB enumerates as stock `4353:3243` again and `SincoOTA` **Get Version** reports `SMC-PAD_005`. *(Note: the official `SMC-PAD_005.fwsc` carries factory presets in `USRFLASH`, so flashing stock resets presets to factory defaults while keeping pad calibration untouched.)*

### Safe Mode & Hardware Recovery
- **Boot-loop Safe Mode:** If the controller ever hangs or reboots within 5 seconds twice in a row (or if you manually switch power **ON → OFF within 5 s → ON → OFF within 5 s → ON**), the third boot starts in **Safe Mode** with custom drivers bypassed while keeping Bluetooth `SincoOTA` and USB alive so you can flash `SMC-PAD_005.fwsc`.
- **Mask-ROM USB Recovery:** The AC636N mask-ROM USB bootloader (`4c4a:2542`) cannot be overwritten by software; hardware recovery details are in [recovery.md](recovery.md).

---

## Technical Notes on Image Packaging

A JieLi single-bank BLE OTA update runs in two stages:
1. `SincoOTA` pushes `ota.bin` from the `.ufw` container; the on-flash `uboot.boot` boots it into RAM/flash updater mode.
2. `SincoOTA` reconnects to `ota.bin` and streams `flash2.bin` (4 KB JLFS header/bootloader + scrambled `app.bin` code image).

Stock `SMC-PAD_005` uses the **AC63 SDK v1.0.0** bootloader (`uboot.boot` SHA-1 `1453beb0…`) and updater (`ota.bin` SHA-1 `02192e4c…`), which expect the classic 8-section UFW layout (`flash.bin`, `info.log`, `isd_config.ini`, `ota.bin`, `script.ver`, `flash2.bin`, `blimit.bin`, `tail.bin`) rather than SDK v2.4.0's `wp_flash.bin` layout:
- Both the Docker packaging script ([repack_oldchain.sh](../firmware/builder/repack_oldchain.sh)) and the client-side browser patcher ([index.html](index.html) / `patcher.js`) keep the first 4 KB of `flash2.bin`, `uboot.boot`, `ota.bin`, and `tail.bin` from your own `SMC-PAD_005.fwsc`, replacing only the `0x1000+` application code area while leaving `USRFLASH` (`0x77000`) and `USRTRIM` (`0x7E000`) untouched.
