# References

## Official JieLi SDK & Documentation
- **fw-AC63_BT_SDK** – <https://github.com/Jieli-Tech/fw-AC63_BT_SDK>
  - Official SDK supporting AC631N / AC632N / AC635N / **AC636N** / AC637N (`ac63_bt_sdk_release_v2.4.0`).
  - Starting points used by our clean-room overlay: `apps/spp_and_le/board/br25` and `cpu/br25/`.
- **JieLi developer documentation & Linux toolchain portal** – <https://doc.zh-jieli.com/>
- **Official JieLi RCSP BLE OTA reference implementations** –
  [Android-JL_OTA](https://github.com/Jieli-Tech/Android-JL_OTA),
  [iOS-JL_OTA](https://github.com/Jieli-Tech/iOS-JL_OTA),
  [JL_OTA_Flutter](https://github.com/Jieli-Tech/JL_OTA_Flutter)

## Community JieLi Documentation & Tools
| Project | Purpose |
|---|---|
| [kagaimiq/jielie](https://github.com/kagaimiq/jielie) | Community hardware documentation for JieLi BR25 memory/IO/IRQ maps, UFW/JLFS container formats, and mask-ROM ISP protocols |
| [kagaimiq/jl-misctools](https://github.com/kagaimiq/jl-misctools) | JieLi container & keyfile utilities (`fwunpack_newfw.py`, `keyfgen.py`), fetched automatically at build time by `firmware/builder/setup_toolchain.py` |
| [kagaimiq/jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) | Open-source mask-ROM USB (`UBOOT1.00`) flash backup and recovery utility supporting BR17–BR25 |
| [ElectronicCats/jieli-ble-badge-research](https://github.com/ElectronicCats/jieli-ble-badge-research) | Community reference workflow for building AC63/AC707N SDK targets on Linux |

## Standard MIDI Specifications
- **BLE-MIDI:** Apple / MIDI Association *MIDI over Bluetooth Low Energy* specification (service `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3`).
- **USB-MIDI:** *Universal Serial Bus Device Class Definition for MIDI Devices* v1.0.
- **Mackie Control Universal (MCU):** MIDI note/pitch-bend surface control conventions used on the MCP virtual port.
