#!/usr/bin/env bash
# Check a built nam_vst.so against what MPC OS can load: a 32-bit ARM ELF that exports only
# VSTPluginMain and needs nothing newer than GLIBC_2.36. Appends a summary to $GITHUB_STEP_SUMMARY
# when set.
#   scripts/check_binary.sh vst/build/nam_vst.so
set -euo pipefail
SO="${1:?usage: scripts/check_binary.sh <nam_vst.so>}"
MAX_GLIBC=GLIBC_2.36

file -b "$SO" | grep -q 'ELF 32-bit LSB.*ARM' || { echo "::error::$SO is not a 32-bit ARM ELF"; exit 1; }
exports=$(readelf --dyn-syms -W "$SO" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sort -u)
[ "$exports" = VSTPluginMain ] || { echo "::error::unexpected exports: $exports"; exit 1; }
glibc=$(readelf -V "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
[ "$(printf '%s\n%s\n' "$glibc" "$MAX_GLIBC" | sort -V | tail -1)" = "$MAX_GLIBC" ] \
  || { echo "::error::$glibc is newer than the device's $MAX_GLIBC"; exit 1; }
echo "$SO: ARM ELF, exports VSTPluginMain only, highest glibc symbol $glibc"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "## $(basename "$SO")"
    echo "- md5: \`$(md5sum "$SO" | cut -d' ' -f1)\`"
    echo "- size: $(du -h "$SO" | cut -f1)"
    echo "- highest glibc symbol: $glibc"
  } >> "$GITHUB_STEP_SUMMARY"
fi
