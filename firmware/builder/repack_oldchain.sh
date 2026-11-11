#!/bin/bash
# Repackage a build_ufw.sh staging directory into the *stock-era* update format used by
# SMC-PAD_005.fwsc (AC63 SDK v0.9-v2.0):
#   isd_download (SDK v1.0.0 Linux binary; needs libcrypto 1.0.0 + ICU 55 in $OLDLIBS_DIR)
#   fw_add.exe + ufw_maker.exe (SDK v1.0.0 Windows binaries, run under wine)
# Why: the stock updater (SDK v1.0.0 ota.bin) stalls at 3.29 % on files built by the v2.4.0 tools
# (sections wp_flash.bin / farg.cfg, no flash2.bin); this script produces the exact stock section
# list: flash.bin, info.log, isd_config.ini, ota.bin, script.ver, flash2.bin, blimit.bin, tail.bin
# (no USRFLASH: presets and calibration are preserved).
#
#   repack_oldchain.sh <stage_dir> <out.ufw>
#     stage_dir  a build_ufw.sh stage dir (app.bin, cfg_tool.bin, uboot.boot)
#   env: SMCPAD_WORK   workspace root (default /workspace)
#        OLDCHAIN_DIR  SDK v1.0.0 tools dir (default $SMCPAD_WORK/oldchain)
#        OLDLIBS_DIR   legacy shared libs for v1.0.0 isd_download (default $SMCPAD_WORK/oldlibs)
#        KEYFILE       (default $SMCPAD_WORK/keys/smcpad.key, produced by prepare_stock.py)
#        OTA_BIN       (default $SMCPAD_WORK/loaders/ota_sdk_v1.0.0.bin = stock updater)
#        RES           resource files for -res (default: empty tone.cfg, matching NO_TONE=1)
#        PRCT_LEN / VM_ADR / VM_LEN / BTIF_ADR / EXIF_ADR  flash layout
set -euo pipefail
WORK="${SMCPAD_WORK:-/workspace}"
STAGE="$1"; OUTUFW="$2"
O="${OLDCHAIN_DIR:-$WORK/oldchain}"
OLDLIBS="${OLDLIBS_DIR:-$WORK/oldlibs}"
KEYFILE="${KEYFILE:-$WORK/keys/smcpad.key}"
OTA_BIN="${OTA_BIN:-$WORK/loaders/ota_sdk_v1.0.0.bin}"
PRCT_LEN="${PRCT_LEN:-0x30000}"
VM_ADR="${VM_ADR:-0x50000}"
VM_LEN="${VM_LEN:-0x25000}"
BTIF_ADR="${BTIF_ADR:-0x75000}"
EXIF_ADR="${EXIF_ADR:-0x76000}"
for f in "$O/isd_download" "$O/fw_add.exe" "$O/ufw_maker.exe" "$O/script.ver" "$OTA_BIN" "$KEYFILE" \
         "$STAGE/app.bin" "$STAGE/cfg_tool.bin" "$STAGE/uboot.boot"; do
    [ -s "$f" ] || { echo "missing $f"; exit 1; }
done
W=$(mktemp -d /tmp/oldchain.XXXXXX)
cd "$W"
cp "$STAGE/app.bin" "$STAGE/cfg_tool.bin" "$STAGE/uboot.boot" .
cp "$OTA_BIN" ota.bin
cp "$O/script.ver" .
RESARGS=()
if [ -n "${RES:-}" ]; then
    for r in $RES; do cp "$r" .; RESARGS+=("$(basename "$r")"); done
else
    : > tone.cfg; RESARGS=(tone.cfg)
fi
cat > isd_config.ini <<EOF
[EXTRA_CFG_PARAM]
NEW_FLASH_FS=YES;
CHIP_NAME=AC636N;
ENTRY=0x1E00120;
PID=AC636N_TRANS;
VID=0.01;
RESERVED_OPT=0;
DOWNLOAD_MODEL=usb;
[CHIP_VERSION]
SUPPORTED_LIST=B,C,D,E,F;
[SYS_CFG_PARAM]
SPI=2_3_0_0;
UTRX=PB05;
RESET=PB01_00_0;
UPDATE_JUMP=0;
VLVD=4;
[RESERVED_CONFIG]
BTIF_ADR=$BTIF_ADR;
BTIF_LEN=0x1000;
BTIF_OPT=1;
EXIF_ADR=$EXIF_ADR;
EXIF_LEN=0x1000;
EXIF_OPT=1;
PRCT_ADR=0;
PRCT_LEN=$PRCT_LEN;
PRCT_OPT=2;
VM_ADR=$VM_ADR;
VM_LEN=$VM_LEN;
VM_OPT=0;
USRFLASH_ADR=0x77000;
USRFLASH_LEN=0x7000;
USRFLASH_OPT=1;
USRTRIM_ADR=0x7E000;
USRTRIM_LEN=0x1000;
USRTRIM_OPT=1;
[BURNER_CONFIG]
SIZE=32;
EOF
export QT_QPA_PLATFORM=offscreen
LD_LIBRARY_PATH="$OLDLIBS" timeout 60 "$O/isd_download" isd_config.ini -tonorflash -dev br25 \
    -boot 0x12000 -div8 -wait 300 -uboot uboot.boot -app app.bin cfg_tool.bin -res "${RESARGS[@]}" \
    -uboot_compress -key "$KEYFILE" > isd_download.log 2>&1 || true
[ -s jl_isd.fw ] || { cat isd_download.log; echo "old isd_download produced no jl_isd.fw"; exit 1; }
export WINEDEBUG=-all WINEPREFIX="${WINEPREFIX:-/tmp/.wine-jl}"
wine "$O/fw_add.exe" -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw > wine.log 2>&1
wine "$O/fw_add.exe" -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw >> wine.log 2>&1
wine "$O/ufw_maker.exe" -fw_to_ufw jl_isd.fw >> wine.log 2>&1
[ -s jl_isd.ufw ] || { cat wine.log; echo "ufw_maker.exe produced no jl_isd.ufw"; exit 1; }
cp jl_isd.ufw "$OUTUFW"
echo "stock-format ufw: $OUTUFW ($(stat -c %s "$OUTUFW") B, ota.bin $(sha1sum < ota.bin | cut -c1-12))"
rm -rf "$W"
