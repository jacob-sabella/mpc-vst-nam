#!/usr/bin/env bash
# layout.conf + params.json -> vst/build/skin/<vendor> - VST - <name>/ with mpc-vst-plugins' gen_vst.py,
# then skin_post.py's synthwave knob strips. Needs python3 with Pillow and an mpc-vst-plugins checkout
# (MPC_VST). DOCKER=1 runs it in python:3.11-slim instead of the host python.
# SHADOW_SKIN_MPC_OS=2 (the release sets it) converts the finished skin to the MPC OS 2.x shape, which 3.x reads too
# (mpc-vst-plugins docs/OS2_SKINS.md). The conversion runs after skin_post.py, which edits the 3.x shape.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/../../mpc-vst-plugins}"
[ -f "$MPC_VST/tools/gen_vst.py" ] || { echo "need an mpc-vst-plugins checkout (MPC_VST)" >&2; exit 1; }
SKIN="build/skin/$(python3 -c 'import json; c=json.load(open("vst.json")); print(c["vendor"]+" - VST - "+c["name"])')/Plugin Skins"
MPC_OS="${SHADOW_SKIN_MPC_OS:-3}"
unset SHADOW_SKIN_MPC_OS
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
if [ "$MPC_OS" = 2 ]; then
  python3 - "$MPC_VST/tools" "$SKIN/TUI.json" <<'PY'
import json, sys
sys.path.insert(0, sys.argv[1])
from shadow_skin import to_mpc2x
tui = json.load(open(sys.argv[2]))
json.dump(to_mpc2x(tui), open(sys.argv[2], "w"), indent=4)
print("skin converted to the MPC OS 2.x shape")
PY
  python3 "$MPC_VST/tools/skin_compat.py" check "$SKIN/TUI.json" "$SKIN/Q-Links.json"
fi
python3 gen_entry.py
