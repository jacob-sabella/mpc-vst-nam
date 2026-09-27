#!/usr/bin/env bash
# Everything a release zip needs, in one go (mpc-vst-plugins' vst-release workflow runs this):
#   vst/build/nam_vst.so                                   -> payload/vst/
#   vst/build/skin/nam-jam interop - VST - Neural Amp Modeler/  -> payload/Synths/
#   vst/build/pluginlist-entry.xml
# Needs Docker with armhf emulation, python3 with Pillow, and an mpc-vst-plugins checkout (MPC_VST).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${MPC_VST:?set MPC_VST to an mpc-vst-plugins checkout}"
vst/build.sh
vst/gen_skin.sh
ls -la vst/build
