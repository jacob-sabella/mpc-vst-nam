#!/usr/bin/env bash
# Check a built nam_vst.so against what MPC OS can load: it exports only VSTPluginMain, and is either a 32-bit ARM ELF needing
# nothing newer than GLIBC_2.32 (Gen1: MPC OS 2.x, the catalog's limit) or an aarch64 ELF needing nothing newer than GLIBC_2.39
# (Gen2: MPC OS 3.x only). Appends a summary to $GITHUB_STEP_SUMMARY when set.
#   scripts/check_binary.sh vst/build/nam_vst.so vst/build/aarch64/nam_vst.so
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: scripts/check_binary.sh <nam_vst.so>..."; exit 2; }
for SO in "$@"; do
if file -b "$SO" | grep -q 'ELF 32-bit LSB.*ARM'; then MAX_GLIBC=GLIBC_2.32; kind="ARM ELF"; why="MPC OS 2.x, the catalog limit"
elif file -b "$SO" | grep -q 'ELF 64-bit LSB.*aarch64'; then MAX_GLIBC=GLIBC_2.39; kind="aarch64 ELF"; why="MPC OS 3.x on Gen2"
else echo "::error::$SO is neither a 32-bit ARM nor an aarch64 ELF"; exit 1; fi
exports=$(readelf --dyn-syms -W "$SO" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sort -u)
[ "$exports" = VSTPluginMain ] || { echo "::error::unexpected exports: $exports"; exit 1; }
glibc=$(readelf -V "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
[ "$(printf '%s\n%s\n' "$glibc" "$MAX_GLIBC" | sort -V | tail -1)" = "$MAX_GLIBC" ] \
  || { echo "::error::$glibc is newer than $MAX_GLIBC ($why)"; exit 1; }
echo "$SO: $kind, exports VSTPluginMain only, highest glibc symbol $glibc"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "## $SO"
    echo "- md5: \`$(md5sum "$SO" | cut -d' ' -f1)\`"
    echo "- size: $(du -h "$SO" | cut -f1)"
    echo "- highest glibc symbol: $glibc"
  } >> "$GITHUB_STEP_SUMMARY"
fi
done
