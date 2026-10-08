#ifndef CONFIG_BOARD_SMCPAD_POST_BUILD_CFG_H
#define CONFIG_BOARD_SMCPAD_POST_BUILD_CFG_H

/*
 * M-VAVE SMC-PAD (JieLi AC636N / BR25): settings consumed by cpu/br25/tools/isd_config_rule.c
 * to generate isd_config.ini (flash layout, boot config). Same structure as the SDK's
 * board_ac636n_demo_global_build_cfg.h (ac63_bt_sdk_release_v2.4.0), values changed to
 * match the stock SMC-PAD_005 partition layout.
 *
 * Stock flash map (from the SMC-PAD_005 partition layout, checked against the BLE dump;
 * see docs/build_host.md):
 *   0x00000-0x25FFF  PRCT (header + code)      -> ours is bigger; VM simply starts later
 *   0x26000-0x74FFF  VM (auto: code end .. BTIF)
 *   0x75000          BTIF      4 KB
 *   0x76000          EXIF      4 KB  (RCSP OTA info)
 *   0x77000          USRFLASH 28 KB  (the 8 presets)          <- must survive
 *   0x7E000          USRTRIM   4 KB  (pad calibration+trimdat) <- must survive
 *   0x7F000          key_mac   4 KB  (placed by isd_download)
 * OPT = 1 means "don't touch this area when downloading/updating".
 *
 * Stock isd_config [SYS_CFG_PARAM]: SPI 2_3_0_0, UTRX PB05, RESET PB01_00_0, UPDATE_JUMP 0, VLVD 4.
 */

#ifdef CONFIG_BOARD_SMCPAD

/* ---- affect both compiling and post-build ---------------------------------------------- */
#define CONFIG_DOUBLE_BANK_ENABLE               0
#define CONFIG_APP_OTA_ENABLE                   1       // stock has JieLi RCSP OTA (AE00) and an EXIF area
#define CONFIG_UPDATE_JUMP_TO_MASK              0       // stock: UPDATE_JUMP 0
#define CONFIG_LP_TOUCH_KEY_EN                  0
#define CONFIG_UPDATE_WITH_MD5_CHECK_EN         0       // stock ships md5.bin as a resource; not needed yet
#define CONFIG_ANC_ENABLE                       0

#define FLASH_SIZE_256K                         0x40000
#define FLASH_SIZE_512K                         0x80000
#define FLASH_SIZE_1M                           0x100000
#define FLASH_SIZE_2M                           0x200000
#define FLASH_SIZE_4M                           0x400000
#define CONFIG_FLASH_SIZE                       FLASH_SIZE_512K   // 4 Mbit part on the SMC-PAD

/* ---- post-build only ------------------------------------------------------------------- */
#define CONFIG_DB_UPDATE_DATA_GENERATE_EN       0
#define CONFIG_ONLY_GRENERATE_ALIGN_4K_CODE     1

#ifdef CONFIG_BR30_C_VERSION
#define CONFIG_SUPPORTED_CHIP_VERSION           C
#else
#define CONFIG_SUPPORTED_CHIP_VERSION           B,D,E,M,N,O,P
#endif

#define CONFIG_CHIP_NAME                        AC636N
#define CONFIG_PID                              AC636N_TRANS    // = stock; RCSP OTA matches on PID
#define CONFIG_VID                              0.01            // = stock

#define CONFIG_PLL_SOURCE_USING_LRC             0               // BT needs the crystal

#ifdef CONFIG_256K_FLASH
#define ALIGN_UNIT_256B                         1
#else
#define ALIGN_UNIT_256B                         0
#endif

#define CONFIG_UART_UPDATE_PIN                  PB05            // stock: UTRX PB05

/* No uboot debug UART (stock has no UTTX). The SDK's isd_config_rule.c defaults it to PA05;
 * our apply.sh patches the rule so CONFIG_NO_UBOOT_DEBUG_PIN suppresses that default. */
#define CONFIG_NO_UBOOT_DEBUG_PIN               1

/* Long-press reset: stock PB01_00_0 = time 00 = disabled. */
#define CONFIG_RESET_PIN                        PB01
#define CONFIG_RESET_TIME                       00
#define CONFIG_RESET_LEVEL                      0

#define CONFIG_VDDIO_LVD_LEVEL                  4               // stock: VLVD 4

/* VM: auto-placed between the end of the code and BTIF (as in stock). */
#define CONFIG_VM_LEAST_SIZE                    8K
#define CONFIG_VM_OPT                           1

#define CONFIG_BTIF_ADDR                        0x75000
#define CONFIG_BTIF_LEN                         0x1000
#define CONFIG_BTIF_OPT                         1

#define CONFIG_EXIF_ADDR                        0x76000
#define CONFIG_EXIF_LEN                         0x1000
#define CONFIG_EXIF_OPT                         1

#define CONFIG_RESERVED_AREA1                   USRFLASH        // presets (8 x 3539 B)
#define CONFIG_RESERVED_AREA1_ADDR              0x77000
#define CONFIG_RESERVED_AREA1_LEN               0x7000
#define CONFIG_RESERVED_AREA1_OPT               1

#define CONFIG_RESERVED_AREA2                   USRTRIM         // pad calibration
#define CONFIG_RESERVED_AREA2_ADDR              0x7E000
#define CONFIG_RESERVED_AREA2_LEN               0x1000
#define CONFIG_RESERVED_AREA2_OPT               1

#endif /* CONFIG_BOARD_SMCPAD */
#endif /* CONFIG_BOARD_SMCPAD_POST_BUILD_CFG_H */
