#!/bin/bash
# Build an AC63 SDK target on x86_64 Linux (or inside firmware/builder/Dockerfile) and package
# it into an update .ufw WITHOUT JieLi's proprietary 'host-client' and without touching any USB device.
#
#   build_ufw.sh [make_target] [sdk_dir]
#   e.g. build_ufw.sh ac636n_spp_and_le /workspace/work/sdk-smcpad
#   env: SMCPAD_WORK=<dir> workspace root (default /workspace)
#        OUT=<dir>         output directory (default $SMCPAD_WORK/out/<target>)
#        KEYFILE=<file>    JieLi .key file -> isd_download -key (generated from your stock .fwsc)
#        OTA_BIN=<file>    BLE-update loader packed into the .ufw (default: the SDK's ota.bin).
#                          The pad's *current* bootloader has to start it: stock SMC-PAD_005
#                          = AC63 SDK v1.0.0 uboot, whose loader is v1.0.0's ota.bin;
#                          the v2.x loaders do not start on it.
#
# Output: $OUT/{app.bin,jl_isd.fw,update.ufw,build.log,SHA256SUMS}
# SAFETY: refuses to run if any JieLi USB device/dongle is attached. Without KEYFILE the image
# uses the SDK default unconfigured key.
set -euo pipefail
WORK="${SMCPAD_WORK:-/workspace}"
TARGET="${1:-ac636n_spp_and_le}"
SDK="${2:-$WORK/work/sdk-smcpad}"
CPU=br25
TOOLS="$SDK/cpu/$CPU/tools"
OUT="${OUT:-$WORK/out/$TARGET}"
OTA_BIN="${OTA_BIN:-$TOOLS/ota.bin}"
[ -s "$OTA_BIN" ] || { echo "OTA_BIN $OTA_BIN missing"; exit 1; }
KEYARGS=()
if [ -n "${KEYFILE:-}" ]; then
    [ -s "$KEYFILE" ] || { echo "KEYFILE $KEYFILE missing"; exit 1; }
    KEYARGS=(-key "$KEYFILE")
fi
export OBJDUMP=/opt/jieli/pi32v2/bin/objdump OBJCOPY=/opt/jieli/pi32v2/bin/objcopy \
       OBJSIZEDUMP=/opt/jieli/pi32v2/bin/objsizedump
export QT_QPA_PLATFORM=offscreen   # isd_download is a Qt binary
ulimit -n 8192 2>/dev/null || true

log() { echo "[build] $*"; }
[ -x /opt/jieli/common/bin/clang ] || { echo "toolchain missing: run firmware/builder/setup_toolchain.py"; exit 1; }
[ -x /opt/utils/isd_download ] || { echo "post-build tools missing: run firmware/builder/setup_toolchain.py"; exit 1; }
if lsusb 2>/dev/null | grep -qi -E "4353:|4c4a:|jieli|e2b7:"; then
    echo "a JieLi USB device is connected; refusing to run isd_download"; exit 1
fi

mkdir -p "$OUT"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
log "make $TARGET (-j$JOBS)"
rm -f "$TOOLS/sdk.elf"
( cd "$SDK" && make "$TARGET" -j"$JOBS" ) > "$OUT/build.log" 2>&1 || true  # SDK post-build step invokes missing host-client; expected
[ -s "$TOOLS/sdk.elf" ] || { tail -30 "$OUT/build.log"; echo "link failed"; exit 1; }
log "sdk.elf: $(stat -c %s "$TOOLS/sdk.elf") bytes"

# --- post-build, equivalent to cpu/br25/tools/download.sh without host-client ---
cd "$TOOLS"
sec() { $OBJCOPY -O binary -j "$1" sdk.elf "$2"; }
sec .text text.bin; sec .data data.bin; sec .data_code data_code.bin
: > bank.bin
for o in aec wav ape flac m4a amr dts fm mp3 wma; do
    sec ".overlay_$o" "${o}o.bin"
    /opt/utils/remove_tailing_zeros -i "${o}o.bin" -o "$o.bin" -mark ff >/dev/null
done
$OBJDUMP -section-headers -address-mask=0x1ffffff sdk.elf > "$OUT/sections.txt"
$OBJSIZEDUMP -lite -skip-zero -enable-dbg-info sdk.elf | sort -k 1 > "$OUT/symbol_tbl.txt"
cat text.bin data.bin data_code.bin aec.bin wav.bin ape.bin flac.bin m4a.bin amr.bin dts.bin \
    fm.bin mp3.bin wma.bin bank.bin > app.bin
log "app.bin: $(stat -c %s app.bin) bytes"

# --- packaging (download/data_trans/download.bat equivalent) in a staging dir -------------------
STAGE="$OUT/stage"; rm -rf "$STAGE"; mkdir -p "$STAGE"; cd "$STAGE"
cp "$TOOLS"/{isd_config.ini,uboot.boot,app.bin,cfg_tool.bin,br25loader.bin,flash_params.bin} .
cp "$OTA_BIN" ota.bin
log "ota.bin: $OTA_BIN ($(sha1sum < ota.bin | cut -c1-12))"
cp "$TOOLS/script.ver" . 2>/dev/null || true
if [ -n "${NO_TONE:-}" ]; then
    : > tone.cfg            # no prompt tones (stock SMC-PAD has none): ~34 KB less code area
else
    cp "$TOOLS/tone.cfg" . 2>/dev/null || : > tone.cfg
fi
log "isd_download (image only; no device attached)"
timeout 60 /opt/utils/isd_download isd_config.ini -tonorflash -dev br25 -boot 0x12000 -div8 -wait 300 \
    -uboot uboot.boot -app app.bin cfg_tool.bin -res tone.cfg -uboot_compress \
    -flash-params flash_params.bin "${KEYARGS[@]}" > "$OUT/isd_download.log" 2>&1 || true
[ -s jl_isd.fw ] || { cat "$OUT/isd_download.log"; echo "isd_download produced no jl_isd.fw"; exit 1; }
/opt/utils/fw_add -noenc -fw jl_isd.fw -add ota.bin -type 100 -out jl_isd.fw >> "$OUT/isd_download.log" 2>&1
[ -f script.ver ] && /opt/utils/fw_add -noenc -fw jl_isd.fw -add script.ver -out jl_isd.fw >> "$OUT/isd_download.log" 2>&1
/opt/utils/ufw_maker -fw_to_ufw jl_isd.fw >> "$OUT/isd_download.log" 2>&1
mv jl_isd.ufw "$OUT/update.ufw"
cp app.bin jl_isd.fw "$OUT/"
[ -f jl_isd.bin ] && cp jl_isd.bin "$OUT/"
cd "$OUT" && sha256sum app.bin jl_isd.fw update.ufw $( [ -f jl_isd.bin ] && echo jl_isd.bin ) > SHA256SUMS
log "done:"; ls -la "$OUT" | sed 's/^/  /'
