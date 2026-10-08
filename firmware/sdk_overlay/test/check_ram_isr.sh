#!/bin/bash
# Post-build verification: every smcpad function placed in RAM
# (.data_code) must not reference flash (0x1Exxxxx): no calls, gotos or literal loads.
# Usage: bash check_ram_isr.sh [sdk-dir] [out-dir]
SDK=${1:-/workspace/work/sdk-smcpad}
OUT=${2:-/workspace/out/smcpad_midi}
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
exit $rc
