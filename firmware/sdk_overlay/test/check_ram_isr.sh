#!/bin/bash
# Post-build verification: every smcpad function placed in RAM (.data_code)
# must not reference flash (0x1Exxxxx): no calls, gotos or literal loads.
# Usage: bash check_ram_isr.sh [sdk-dir] [out-dir]
WORK="${SMCPAD_WORK:-/workspace}"
SDK=${1:-$WORK/work/sdk-smcpad}
OUT=${2:-$WORK/out/ac636n_spp_and_le}
OBJDUMP=/opt/jieli/pi32v2/bin/objdump
ELF=$SDK/cpu/br25/tools/sdk.elf
rc=0
while read -r addr size name; do
    start=$((16#$addr)); end=$((start + 16#$size))
    hits=$($OBJDUMP -d --start-address=$start --stop-address=$end "$ELF" 2>/dev/null |
           grep -E '^ +[0-9a-f]+:' | grep -E ': *(ffffffffff)?e[0-9a-f]{5} *>|0x1e[0-9a-f]{5}|: 1e[0-9a-f]{5}')
    if [ -n "$hits" ]; then
        echo "FLASH REF in $name:"; echo "$hits"; rc=1
    else
        printf "ok  %-24s 0x%s  %4d B\n" "$name" "$addr" $((16#$size))
    fi
done < <(awk '/smcpad_(hw|app|logic)\.c/ && $4 == ".data_code" {print $1, $5, $6}' "$OUT/symbol_tbl.txt")
echo "RAM tables:"; awk '/smcpad_(hw|app|logic)\.c/ && $4 == ".data" {printf "    %-24s 0x%s\n", $6, $1}' "$OUT/symbol_tbl.txt"
# Heap budget: BSS growth (preset RAM image, Sinco/BLE buffers) shrinks the heap. Floor 40 KB
# (MIN_HEAP to override): 912 had ~50 KB here and ~87 KB measured free at runtime on the pad, so
# the earlier 48 KB floor was very conservative; 913's chords take ~6 KB of patterns.
MAP=$SDK/cpu/br25/tools/sdk.map
hb=$(awk '/_HEAP_BEGIN = \./ {print $1; exit}' "$MAP"); he=$(awk '/_HEAP_END = / {print $1; exit}' "$MAP")
if [ -z "$hb" ] || [ -z "$he" ]; then
    echo "HEAP: _HEAP_BEGIN/_HEAP_END not found in $MAP"; rc=1
else
    heap=$(( he - hb )); min=${MIN_HEAP:-40960}
    if [ $heap -lt $min ]; then echo "HEAP TOO SMALL: $heap B < $min B"; rc=1
    else echo "heap ok: $heap B (>= $min B)"; fi
fi
# Code size: app.bin sits at flash 0x1120 and cfg_tool.bin follows; everything must end below
# the pattern slots at 0x30000 (smcpad_patstore.h). 0x2D000 leaves room for both headers.
APPBIN=$OUT/stage/app.bin
if [ -f "$APPBIN" ]; then
    sz=$(stat -c %s "$APPBIN"); max=$((0x2D000))
    if [ "$sz" -gt "$max" ]; then echo "CODE TOO BIG: app.bin $sz B > $max B (pattern area 0x30000)"; rc=1
    else echo "code ok: app.bin $sz B (<= $max B, pattern area at 0x30000)"; fi
else
    echo "CODE SIZE: $APPBIN not found"; rc=1
fi
exit $rc
