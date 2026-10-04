#!/usr/bin/env bash
# layout.conf + params.json -> vst/build/skin/<vendor> - VST - <name>/ with mpc-vst-plugins' gen_vst.py,
# then skin_post.py's synthwave knob strips. Needs python3 with Pillow and an mpc-vst-plugins checkout
# (MPC_VST). DOCKER=1 runs it in python:3.11-slim instead of the host python.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/../../mpc-vst-plugins}"
[ -f "$MPC_VST/tools/gen_vst.py" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
SKIN="build/skin/$(python3 -c 'import json; c=json.load(open("vst.json")); print(c["vendor"]+" - VST - "+c["name"])')/Plugin Skins"
rm -rf build/skin
# skin artwork renderer (static, so the same binary runs in the DOCKER=1 container)
mkdir -p build
cc -O2 -static -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
if [ "${DOCKER:-}" = 1 ]; then
  MV="$(cd "$MPC_VST" && pwd)"
  docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -v "$MV":/mv:ro -w /w -e HOME=/tmp \
    -e SHADOW_TITLE_FONT=/w/fonts/Orbitron.ttf -e SHADOW_LABEL_FONT=/w/fonts/TitilliumWeb-SemiBold.ttf \
    python:3.11-slim sh -c "pip install -q --target /tmp/p pillow 2>/dev/null &&
      PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json && PYTHONPATH=/tmp/p python3 skin_post.py '$SKIN'"
else
  # skin_post.py draws the button labels in the title font too
  export SHADOW_TITLE_FONT="$PWD/fonts/Orbitron.ttf" SHADOW_LABEL_FONT="$PWD/fonts/TitilliumWeb-SemiBold.ttf"
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
  python3 skin_post.py "$SKIN"
fi
python3 gen_entry.py
