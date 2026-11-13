#!/usr/bin/env bash
# Everything a release zip needs, in one go (mpc-vst-plugins' vst-release workflow runs this):
#   vst/build/nam_vst.so                                   -> payload/vst/
#   vst/build/aarch64/nam_vst.so                           -> the Gen2 (aarch64) zip, made by the workflow when it exists
#   vst/build/skin/<vendor> - VST - <name>/                -> payload/Synths/
#   vst/build/pluginlist-entry.xml
# Needs Docker with armhf and arm64 emulation, python3 with Pillow, and an mpc-vst-plugins checkout (MPC_VST).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${MPC_VST:?set MPC_VST to an mpc-vst-plugins checkout}"
vst/build.sh
vst/build.sh aarch64
vst/gen_skin.sh
ls -la vst/build
