#!/usr/bin/env python3
"""Create/refresh the SMC-PAD working copy of the JieLi AC63 SDK .

    python3 firmware/sdk_overlay/apply.py [--fresh]

* The pristine SDK in /workspace/src/<tag> is never modified.
* A working copy is made once at /workspace/work/sdk-smcpad (cp -a; --fresh recreates it).
* Every run regenerates our files from the pristine sources, so it is idempotent:
  - apps/spp_and_le/board/br25/board_smcpad.c / board_smcpad_cfg.h
      = the AC636N demo board with CONFIG_BOARD_AC636N_DEMO -> CONFIG_BOARD_SMCPAD
  - SMC-PAD hardware (default on; --no-smcpad-hw to skip): copies board/smcpad_*.[ch] into the
    board dir, frees the demo peripherals that clash with docs/pinmap.md (UART0 on PC4, ADKEY on
    PB1, SDK SPI1/SPI2 drivers, AT UART pins), adds adc_ext_set_value() to cpu/br25/adc_api.c
    and calls smcpad_start() from trans_data after usb_start().
    Pins are the real ones but the drivers are UNTESTED on hardware: still NOT for flashing.
  - board_smcpad_global_build_cfg.h  = ours (stock-compatible flash layout), from this directory
  - board_config.h selects CONFIG_BOARD_SMCPAD; the board Makefile compiles board_smcpad.c
  - cpu/br25/tools/isd_config_rule.c: CONFIG_NO_UBOOT_DEBUG_PIN suppresses the PA05 UTTX default
  - USB-MIDI device class (default on; --no-usb-midi to skip): copies usb/usb_midi.[ch] into
    apps/common/device/usb/device/ and patches usb_common_def.h, usb_device.c, usb_config.c,
    descriptor.c, user_setup.c, the board Makefile, board_smcpad_cfg.h and trans_data's
    app_spp_and_le.c (see README.md, "USB-MIDI")
  - SMC-PAD BLE (default on with the SMC-PAD hardware; --no-ble-smcpad to skip): copies
    ble/, sinco/, midi/ and board/smcpad_store_port.[ch] into the board dir, compiles
    ble_smcpad.c instead of trans_data/ble_trans.c (stock GATT: BLE-MIDI, Sinco AE40, RCSP
    AE00, battery, device info), turns Classic Bluetooth off (TCFG_USER_EDR_ENABLE 0) and
    pairing on ("Just Works": CONFIG_BT_SM_SUPPORT_ENABLE 1 in app_config.h)
Then build with:  bash firmware/builder/build_ufw.sh ac636n_spp_and_le /workspace/work/sdk-smcpad
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRISTINE = "/workspace/src/ac63_bt_sdk_release_v2.4.0"
WORK = "/workspace/work/sdk-smcpad"
BOARD = "apps/spp_and_le/board/br25"


def rd(path):
    with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
        return f.read()


def wr(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(text)
    print("  wrote", os.path.relpath(path, WORK))


def replace_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        sys.exit(f"apply.py: expected exactly one '{old!r}' in {what}, found {n}; SDK changed?")
    return text.replace(old, new)


def patch_file(rel, edits):
    """Regenerate WORK/rel from PRISTINE/rel with exact-match (old, new) edits."""
    t = rd(os.path.join(PRISTINE, rel))
    for old, new in edits:
        t = replace_once(t, old, new, rel)
    wr(os.path.join(WORK, rel), t)


def replace_after(text, anchor, old, new, what):
    """Replace the first `old` after the unique `anchor` (for lines repeated per app)."""
    if text.count(anchor) != 1:
        sys.exit(f"apply.py: expected exactly one anchor {anchor!r} in {what}; SDK changed?")
    a = text.index(anchor) + len(anchor)
    i = text.find(old, a)
    if i < 0:
        sys.exit(f"apply.py: {old!r} not found after {anchor!r} in {what}; SDK changed?")
    return text[:i] + new + text[i + len(old):]


USB = "apps/common/device/usb"
USB_MIDI_BOARD_EDITS = [
    ("#define TCFG_PC_ENABLE\t\t\t\t\t\tDISABLE_THIS_MOUDLE//PC模块使能\n",
     "#define TCFG_PC_ENABLE\t\t\t\t\t\tENABLE_THIS_MOUDLE//PC模块使能 (sdk_overlay: USB-MIDI)\n"
     "#define USB_MEM_NO_USE_OVERLAY_EN\t       1   // USB buffers in .bss, not in overlay_pc\n"),
    ("#define     USB_DEVICE_CLASS_CONFIG (SPEAKER_CLASS|MIC_CLASS|HID_CLASS)\n",
     "#define     USB_DEVICE_CLASS_CONFIG (MIDI_CLASS)   // sdk_overlay: USB-MIDI only\n"),
    # no powerdown while USB runs; the real power plan (charger/VBUS detect) comes with the pin map
    ("#define TCFG_LOWPOWER_LOWPOWER_SEL\t\t\tSLEEP_EN",
     "#define TCFG_LOWPOWER_LOWPOWER_SEL\t\t\t0 /* sdk_overlay: was SLEEP_EN, see README */ //"),
]


SMCPAD_FILES = ("smcpad_logic.h", "smcpad_logic.c", "smcpad_board.h", "smcpad_hw.c", "smcpad_app.c")
SMCPAD_BOARD_EDITS = [
    # PC4 is free on the SMC-PAD but nothing listens there; PC5 (TRS) is UART1, see smcpad_hw.c
    ("#define TCFG_UART0_ENABLE\t\t\t\t\tENABLE_THIS_MOUDLE ",
     "#define TCFG_UART0_ENABLE\t\t\t\t\tDISABLE_THIS_MOUDLE /* sdk_overlay: no debug UART on the SMC-PAD */ "),
    # AT-command UART pins (PC1 = key-matrix row 1, PA6 = mux B ADC): not used by trans_data
    ("#define UART_DB_TX_PIN                      IO_PORTC_01 ",
     "#define UART_DB_TX_PIN                      NO_CONFIG_PORT /* sdk_overlay: was IO_PORTC_01 */ "),
    ("#define UART_DB_RX_PIN                      IO_PORTC_02\n",
     "#define UART_DB_RX_PIN                      NO_CONFIG_PORT /* sdk_overlay: was IO_PORTC_02 */\n"),
    ("#define UART_DB_RTS_PIN                     IO_PORTA_06\n",
     "#define UART_DB_RTS_PIN                     NO_CONFIG_PORT /* sdk_overlay: was IO_PORTA_06 */\n"),
    ("#define UART_DB_CTS_PIN                     IO_PORTA_05\n",
     "#define UART_DB_CTS_PIN                     NO_CONFIG_PORT /* sdk_overlay: was IO_PORTA_05 */\n"),
    # SPI1 (WS2812) and SPI2 (key matrix) are driven directly by smcpad_hw.c
    ("#define\tTCFG_HW_SPI1_ENABLE\t\tENABLE_THIS_MOUDLE",
     "#define\tTCFG_HW_SPI1_ENABLE\t\tDISABLE_THIS_MOUDLE /* sdk_overlay: smcpad_hw.c */"),
    ("#define\tTCFG_HW_SPI2_ENABLE\t\tENABLE_THIS_MOUDLE",
     "#define\tTCFG_HW_SPI2_ENABLE\t\tDISABLE_THIS_MOUDLE /* sdk_overlay: smcpad_hw.c */"),
    # PB1 is the WS2812 data line, not an AD key (also drops the PB1 wakeup port in board_smcpad.c)
    ("#define TCFG_ADKEY_ENABLE                   ENABLE_THIS_MOUDLE ",
     "#define TCFG_ADKEY_ENABLE                   DISABLE_THIS_MOUDLE /* sdk_overlay: PB1 = WS2812 */ "),
    # (TCFG_SYS_LVD_EN is forced to 0 by app_config.h for spp_and_le anyway)
    ("#define TCFG_SYS_LVD_EN\t\t\t\t\t\t      1   //电量检测使能\n",
     "#define TCFG_SYS_LVD_EN\t\t\t\t\t\t      1   //电量检测使能\n"
     "#define TCFG_SMCPAD_HW_ENABLE                     1   // sdk_overlay: pads/keys/LEDs/TRS, smcpad_hw.c\n"),
]
# board_smcpad.c (on top of the demo -> smcpad renames)
SMCPAD_BOARD_C_EDITS = [
    ("    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);\n",
     "    /* sdk_overlay: PB1 is the WS2812 data line; long-press reset stays disabled (time 0) */\n"
     "    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);\n"),
]
ADC_API = "cpu/br25/adc_api.c"
# smcpad_hw.c owns the SARADC; it hands VBAT / LDOREF / DTEMP results back through this
ADC_API_EDITS = [
    ("u32 adc_value_to_voltage(u32 adc_vbg, u32 adc_ch_val)\n",
     "/* sdk_overlay: results of conversions made by a board driver that owns the SARADC */\n"
     "void adc_ext_set_value(u32 ch, u32 value)\n"
     "{\n"
     "    if (ch == AD_CH_LDOREF) {\n"
     "        vbg_adc_value = value;\n"
     "    }\n"
     "    if (ch == AD_CH_VBAT) {\n"
     "        vbat_adc_value = value;\n"
     "    }\n"
     "    for (int i = 0; i < ADC_MAX_CH; i++) {\n"
     "        if (adc_queue[i].ch == ch) {\n"
     "            adc_queue[i].value = value;\n"
     "        }\n"
     "    }\n"
     "}\n\n"
     "u32 adc_value_to_voltage(u32 adc_vbg, u32 adc_ch_val)\n"),
]
APP_TD = "apps/spp_and_le/examples/trans_data/app_spp_and_le.c"
APP_TD_USB_EDIT = ("#if TCFG_USB_SLAVE_CDC_ENABLE\n    extern void usb_start();\n",
                   "#if TCFG_USB_SLAVE_CDC_ENABLE || TCFG_USB_SLAVE_MIDI_ENABLE\n    extern void usb_start();\n")
APP_TD_SMCPAD_EDIT = ("    usb_start();\n#endif\n}\n",
                      "    usb_start();\n#endif\n"
                      "#if TCFG_SMCPAD_HW_ENABLE\n"
                      "    extern void smcpad_start(void);\n"
                      "    smcpad_start();     // sdk_overlay: pads / keys / LEDs / TRS\n"
                      "#endif\n}\n")

# SMC-PAD BLE: (overlay subdir, file) copied flat into the board dir; .c files are compiled
BLE_FILES = [("ble", "ble_smcpad.c"), ("ble", "ble_smcpad.h"), ("ble", "ble_smcpad_profile.h"),
             ("sinco", "sinco_proto.c"), ("sinco", "sinco_proto.h"),
             ("sinco", "sinco_store.c"), ("sinco", "sinco_store.h"),
             ("midi", "ble_midi.c"), ("midi", "ble_midi.h"),
             ("board", "smcpad_store_port.c"), ("board", "smcpad_store_port.h")]
BLE_TRANS_LINE = "\t../../../../apps/spp_and_le/examples/trans_data/ble_trans.c \\\n"
BLE_BOARD_EDITS = [
    # BLE only, like stock: frees ~70 KB of code and the EDR heap (RCSP OTA runs over BLE)
    ("#define TCFG_USER_EDR_ENABLE                      1   //EDR功能使能\n",
     "#define TCFG_USER_EDR_ENABLE                      0   //EDR功能使能 (sdk_overlay: BLE only, as stock)\n"
     "#define TCFG_SMCPAD_BLE_ENABLE                    1   // sdk_overlay: ble_smcpad.c, Sinco, BLE-MIDI\n"),
]
APP_CONFIG = "apps/spp_and_le/include/app_config.h"
APP_CONFIG_SM = ("#if CONFIG_APP_SPP_LE\n",                  # anchor: the CONFIG_APP_SPP_LE block (unique)
                 "#define CONFIG_BT_SM_SUPPORT_ENABLE        0 ",
                 "#define CONFIG_BT_SM_SUPPORT_ENABLE        1 /* sdk_overlay: Just Works pairing */ ")


def apply_usb_midi():
    dst = os.path.join(WORK, USB, "device")
    for f in ("usb_midi.c", "usb_midi.h"):
        shutil.copyfile(os.path.join(HERE, "usb", f), os.path.join(dst, f))
        print("  wrote", os.path.join(USB, "device", f))

    patch_file(USB + "/usb_common_def.h", [
        ("#define     CUSTOM_HID_CLASS    0x00000020\n",
         "#define     CUSTOM_HID_CLASS    0x00000020\n"
         "#define     MIDI_CLASS          0x00000040   // sdk_overlay: USB-MIDI 1.0\n"),
        ("#if (USB_DEVICE_CLASS_CONFIG & CDC_CLASS)\n"
         "#define TCFG_USB_SLAVE_CDC_ENABLE           1\n"
         "#else\n"
         "#define TCFG_USB_SLAVE_CDC_ENABLE           0\n"
         "#endif\n",
         "#if (USB_DEVICE_CLASS_CONFIG & CDC_CLASS)\n"
         "#define TCFG_USB_SLAVE_CDC_ENABLE           1\n"
         "#else\n"
         "#define TCFG_USB_SLAVE_CDC_ENABLE           0\n"
         "#endif\n"
         "\n"
         "#if (USB_DEVICE_CLASS_CONFIG & MIDI_CLASS)\n"
         "#define TCFG_USB_SLAVE_MIDI_ENABLE          1\n"
         "#else\n"
         "#define TCFG_USB_SLAVE_MIDI_ENABLE          0\n"
         "#endif\n"),
        ("#define TCFG_USB_SLAVE_HID_ENABLE           0\n#define TCFG_USB_SLAVE_CDC_ENABLE           0\n",
         "#define TCFG_USB_SLAVE_HID_ENABLE           0\n#define TCFG_USB_SLAVE_CDC_ENABLE           0\n"
         "#define TCFG_USB_SLAVE_MIDI_ENABLE          0\n"),
    ])

    patch_file(USB + "/device/usb_device.c", [
        ('#include "usb/device/cdc.h"\n',
         '#include "usb/device/cdc.h"\n#include "usb_midi.h"\n'),
        ("        cdc_release(usb_id);\n#endif\n",
         "        cdc_release(usb_id);\n#endif\n"
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n        usb_midi_release(usb_id);\n#endif\n"),
        ("        cdc_register(usb_id);\n    }\n#endif\n",
         "        cdc_register(usb_id);\n    }\n#endif\n"
         "\n#if TCFG_USB_SLAVE_MIDI_ENABLE\n"
         "    if ((class & MIDI_CLASS) == MIDI_CLASS) {\n"
         "        log_info(\"add desc midi\");\n"
         "        usb_midi_register(usb_id);\n"
         "        usb_add_desc_config(usb_id, class_index++, usb_midi_desc_config);\n"
         "    }\n#endif\n"),
    ])

    patch_file(USB + "/usb_config.c", [
        ("#define     CDC_DMA_SIZE    0\n#endif\n",
         "#define     CDC_DMA_SIZE    0\n#endif\n\n"
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n#define     MIDI_DMA_SIZE   (64*2)\n#else\n"
         "#define     MIDI_DMA_SIZE   0\n#endif\n"),
        ("+ CUSTOM_HID_DMA_SIZE + USB_DMA_BUF_ALIGN+ 100)",
         "+ CUSTOM_HID_DMA_SIZE + USB_DMA_BUF_ALIGN + MIDI_DMA_SIZE + USB_DMA_BUF_ALIGN + 100)"),
    ])

    patch_file(USB + "/device/descriptor.c", [
        ('#include "usb/device/uac_audio.h"\n',
         '#include "usb/device/uac_audio.h"\n#include "usb_midi.h"\n'),
        ("    memcpy(ptr, sDeviceDescriptor, USB_DT_DEVICE_SIZE);\n",
         "    memcpy(ptr, sDeviceDescriptor, USB_DT_DEVICE_SIZE);\n"
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n    usb_midi_patch_device_descriptor(ptr);\n#endif\n"),
        ("    memcpy(ptr, MANUFACTURE_STR, MANUFACTURE_STR[0]);\n",
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n    usb_midi_get_manufacturer_str(ptr);\n#else\n"
         "    memcpy(ptr, MANUFACTURE_STR, MANUFACTURE_STR[0]);\n#endif\n"),
        ("    memcpy(ptr, product_string, product_string[0]);\n",
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n    usb_midi_get_product_str(ptr);\n#else\n"
         "    memcpy(ptr, product_string, product_string[0]);\n#endif\n"),
        # uac_get_string() only exists with the audio class compiled in
        ("    const u8 *pstr = uac_get_string(id);\n    if (pstr != NULL) {\n        return pstr;\n    }\n",
         "#if TCFG_USB_SLAVE_AUDIO_ENABLE\n"
         "    const u8 *pstr = uac_get_string(id);\n    if (pstr != NULL) {\n        return pstr;\n    }\n"
         "#endif\n"),
    ])

    patch_file(USB + "/device/user_setup.c", [
        ('#include "usb/device/uac_audio.h"\n',
         '#include "usb/device/uac_audio.h"\n#include "usb_midi.h"\n'),
        # CLEAR_FEATURE(ENDPOINT_HALT) etc. on the MIDI endpoints must not stall
        ("    case 4:\n        break;\n    }\n    return en;\n",
         "    case 4:\n        break;\n    }\n"
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n"
         "    if (ep == USB_MIDI_EP_IN || ep == USB_MIDI_EP_OUT) {\n        en = 1;\n    }\n#endif\n"
         "    return en;\n"),
        # jack-name strings
        ("        case USB_DT_STRING:\n            switch (LOBYTE(req->wValue)) {\n",
         "        case USB_DT_STRING:\n"
         "#if TCFG_USB_SLAVE_MIDI_ENABLE\n"
         "            if (usb_midi_string_request(usb_device, req)) {\n"
         "                ret = 1;\n                break;\n            }\n#endif\n"
         "            switch (LOBYTE(req->wValue)) {\n"),
    ])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fresh", action="store_true", help="delete and recreate the working copy")
    ap.add_argument("--no-usb-midi", action="store_true", help="leave the SDK USB stack untouched")
    ap.add_argument("--no-smcpad-hw", action="store_true", help="demo-board peripherals, no SMC-PAD drivers")
    ap.add_argument("--no-ble-smcpad", action="store_true", help="keep trans_data's demo BLE (ble_trans.c), EDR on")
    a = ap.parse_args()
    if a.no_smcpad_hw and not a.no_ble_smcpad:
        print("note: --no-smcpad-hw implies --no-ble-smcpad (the BLE/Sinco consumer is smcpad_app.c)")
        a.no_ble_smcpad = True

    if not os.path.isdir(PRISTINE):
        sys.exit(f"pristine SDK missing: {PRISTINE} (run firmware/builder/setup_toolchain.py)")
    if a.fresh and os.path.isdir(WORK):
        print("removing", WORK)
        shutil.rmtree(WORK)
    if not os.path.isdir(WORK):
        print(f"copying {PRISTINE} -> {WORK}")
        os.makedirs(os.path.dirname(WORK), exist_ok=True)
        subprocess.run(["cp", "-a", PRISTINE, WORK], check=True)

    src_b, dst_b = os.path.join(PRISTINE, BOARD), os.path.join(WORK, BOARD)

    # 1. board .c / _cfg.h derived from the AC636N demo board
    for s, d in (("board_ac636n_demo.c", "board_smcpad.c"), ("board_ac636n_demo_cfg.h", "board_smcpad_cfg.h")):
        t = rd(os.path.join(src_b, s))
        t = t.replace("board_ac636n_demo_global_build_cfg.h", "board_smcpad_global_build_cfg.h")
        t = t.replace("CONFIG_BOARD_AC636N_DEMO", "CONFIG_BOARD_SMCPAD")
        if a.no_smcpad_hw:
            note = " * Peripherals/pins are still the AC636N DEMO board's: do NOT flash. */\n"
        else:
            note = " * SMC-PAD pins (docs/pinmap.md), drivers untested on hardware: do NOT flash. */\n"
        hdr = "/* GENERATED by firmware/sdk_overlay/apply.py from %s (SDK v2.4.0).\n" % s + note
        edits = []
        if d == "board_smcpad_cfg.h":
            edits += [] if a.no_usb_midi else USB_MIDI_BOARD_EDITS
            edits += [] if a.no_smcpad_hw else SMCPAD_BOARD_EDITS
            edits += [] if a.no_ble_smcpad else BLE_BOARD_EDITS
        elif not a.no_smcpad_hw:
            edits += SMCPAD_BOARD_C_EDITS
        for old, new in edits:
            t = replace_once(t, old, new, d)
        wr(os.path.join(dst_b, d), hdr + t)

    # 2. our build cfg
    shutil.copyfile(os.path.join(HERE, "board_smcpad_global_build_cfg.h"),
                    os.path.join(dst_b, "board_smcpad_global_build_cfg.h"))
    print("  wrote", os.path.join(BOARD, "board_smcpad_global_build_cfg.h"))

    # 3. board selection
    t = rd(os.path.join(src_b, "board_config.h"))
    t = replace_once(t, "#define CONFIG_BOARD_AC636N_DEMO\n",
                     "// #define CONFIG_BOARD_AC636N_DEMO\n#define CONFIG_BOARD_SMCPAD   // M-VAVE SMC-PAD (sdk_overlay)\n",
                     "board_config.h")
    t = replace_once(t, '#include "board_ac636n_demo_cfg.h"\n',
                     '#include "board_ac636n_demo_cfg.h"\n#include "board_smcpad_cfg.h"\n', "board_config.h")
    wr(os.path.join(dst_b, "board_config.h"), t)

    # 4. compile board_smcpad.c
    t = rd(os.path.join(src_b, "Makefile"))
    line = "\t../../../../apps/spp_and_le/board/br25/board_ac636n_demo.c \\\n"
    t = replace_once(t, line, line + line.replace("board_ac636n_demo.c", "board_smcpad.c"), "board Makefile")
    if not a.no_usb_midi:
        line = "\t../../../../apps/common/device/usb/device/usb_device.c \\\n"
        t = replace_once(t, line, line + line.replace("usb_device.c", "usb_midi.c"), "board Makefile")
    if not a.no_smcpad_hw:
        line = "\t../../../../apps/spp_and_le/board/br25/board_smcpad.c \\\n"
        t = replace_once(t, line, line + "".join(line.replace("board_smcpad.c", f)
                                                 for f in SMCPAD_FILES if f.endswith(".c")),
                         "board Makefile")
    if not a.no_ble_smcpad:
        board_line = "\t../../../../apps/spp_and_le/board/br25/%s \\\n"
        t = replace_once(t, BLE_TRANS_LINE, "".join(board_line % f for _, f in BLE_FILES if f.endswith(".c")),
                         "board Makefile")
    wr(os.path.join(dst_b, "Makefile"), t)

    # 5. allow "no uboot debug UART" (stock image has no UTTX)
    rule = "cpu/br25/tools/isd_config_rule.c"
    t = rd(os.path.join(PRISTINE, rule))
    t = replace_once(t, "#ifndef CONFIG_UBOOT_DEBUG_PIN\n",
                     "#if !defined(CONFIG_UBOOT_DEBUG_PIN) && !defined(CONFIG_NO_UBOOT_DEBUG_PIN)\n", rule)
    wr(os.path.join(WORK, rule), t)

    # 6. USB-MIDI device class (or restore the pristine USB files)
    usb_files = [USB + "/usb_common_def.h", USB + "/usb_config.c", USB + "/device/usb_device.c",
                 USB + "/device/descriptor.c", USB + "/device/user_setup.c"]
    if a.no_usb_midi:
        for rel in usb_files:
            shutil.copyfile(os.path.join(PRISTINE, rel), os.path.join(WORK, rel))
        for f in ("usb_midi.c", "usb_midi.h"):
            p = os.path.join(WORK, USB, "device", f)
            if os.path.exists(p):
                os.remove(p)
        print("  USB stack: pristine (no USB-MIDI)")
    else:
        apply_usb_midi()

    # 7. SMC-PAD hardware drivers (or remove them)
    for f in SMCPAD_FILES:
        p = os.path.join(dst_b, f)
        if a.no_smcpad_hw:
            if os.path.exists(p):
                os.remove(p)
        else:
            shutil.copyfile(os.path.join(HERE, "board", f), p)
            print("  wrote", os.path.join(BOARD, f))
    if a.no_smcpad_hw:
        shutil.copyfile(os.path.join(PRISTINE, ADC_API), os.path.join(WORK, ADC_API))
    else:
        patch_file(ADC_API, ADC_API_EDITS)

    # 8. trans_data start hook: usb_start() for MIDI, smcpad_start()
    edits = [] if a.no_usb_midi else [APP_TD_USB_EDIT]
    edits += [] if a.no_smcpad_hw else [APP_TD_SMCPAD_EDIT]
    patch_file(APP_TD, edits)

    # 9. SMC-PAD BLE server, Sinco config protocol, BLE-MIDI (or remove them)
    for sub, f in BLE_FILES:
        p = os.path.join(dst_b, f)
        if a.no_ble_smcpad:
            if os.path.exists(p):
                os.remove(p)
        else:
            shutil.copyfile(os.path.join(HERE, sub, f), p)
            print("  wrote", os.path.join(BOARD, f))
    t = rd(os.path.join(PRISTINE, APP_CONFIG))
    if not a.no_ble_smcpad:
        t = replace_after(t, *APP_CONFIG_SM, APP_CONFIG)
    wr(os.path.join(WORK, APP_CONFIG), t)

    print("OK. Build: bash firmware/builder/build_ufw.sh ac636n_spp_and_le", WORK)


if __name__ == "__main__":
    main()
