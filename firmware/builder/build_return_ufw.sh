#!/bin/bash
# Build a "return file": stock SMC-PAD firmware (app, bootloader, config unpacked from the
# official SMC-PAD_005.fwsc) packaged with the AC63 SDK v2.4.0 BLE updater (only needed if a
# test image ever replaced the stock bootloader with v2.4.0; standard builds keep the stock
# v1.0.0 bootloader so the official SMC-PAD_005.fwsc flashes straight back).
#
#   build_return_ufw.sh [stock_unpacked_dir] [out_dir]
set -euo pipefail
WORK="${SMCPAD_WORK:-/workspace}"
STOCK="${1:-$WORK/stock_unpacked}"
OUT="${2:-$WORK/out/return005}"
SDK="${SDK:-$WORK/work/sdk-smcpad}"
KEYFILE="${KEYFILE:-$WORK/keys/smcpad.key}"
T="$SDK/cpu/br25/tools"
OTA="${OTA:-$WORK/src/ac63_bt_sdk_release_v2.4.0/cpu/br25/tools/ota.bin}"
for f in files/app.bin files/cfg_tool.bin files/config.dat files/md5.bin top/uboot.boot; do
    [ -s "$STOCK/$f" ] || { echo "missing $STOCK/$f"; exit 1; }
done
[ "$(sha1sum < "$STOCK/top/uboot.boot" | cut -c1-40)" = 1453beb02e787a9c7420650c1be52df582dc23cc ] \
    || { echo "stock uboot.boot is not the SDK v1.0.0 one"; exit 1; }
[ "$(sha1sum < "$OTA" | cut -c1-40)" = a7db46c5ce22e747d038ef19c1e4dcca22a2fcd3 ] \
    || { echo "$OTA is not the SDK v2.4.0 ota.bin"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/stage"; cd "$OUT/stage"
cp "$T/isd_config.ini" "$T/br25loader.bin" "$T/script.ver" .
cp "$STOCK/top/uboot.boot" uboot.boot
cp "$STOCK"/files/{app.bin,cfg_tool.bin,config.dat,md5.bin} .
cp "$OTA" ota.bin
python3 - <<'EOF'
import re
t = open("isd_config.ini", encoding="latin-1").read()
t = re.sub(r"(?m)^VLVD = 4;\s*\n", "", t)
t = re.sub(r"(?m)^EOFFSET = 0;[^\n]*\n", "", t)
t = t.replace("UPDATE_JUMP = 0;", "UPDATE_JUMP = 0;\nVLVD = 4;", 1)
t = t.replace("VM_OPT = 1;", "VM_OPT = 0;")
open("isd_config.ini", "w", encoding="latin-1").write(t)
EOF
export QT_QPA_PLATFORM=offscreen
timeout 60 /opt/utils/isd_download isd_config.ini -tonorflash -dev br25 -boot 0x12000 -div8 -wait 300 \
    -uboot uboot.boot -app app.bin cfg_tool.bin -res config.dat md5.bin -uboot_compress \
    -key "$KEYFILE" > "$OUT/isd_download.log" 2>&1 || true
[ -s jl_isd.fw ] || { cat "$OUT/isd_download.log"; echo "isd_download produced no jl_isd.fw"; exit 1; }
/opt/utils/fw_add -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw >> "$OUT/isd_download.log" 2>&1
/opt/utils/fw_add -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw >> "$OUT/isd_download.log" 2>&1
/opt/utils/ufw_maker -fw_to_ufw jl_isd.fw >> "$OUT/isd_download.log" 2>&1
mv jl_isd.ufw "$OUT/update.ufw"
cd "$OUT" && sha256sum update.ufw > SHA256SUMS
echo "return file: $OUT/update.ufw"
