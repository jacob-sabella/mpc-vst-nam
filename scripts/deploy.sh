#!/usr/bin/env bash
# Development deploy over an existing install: push a build and the skin to a device, keeping the previous
# copies in /storage/nam-skin-backups/, and restart MPC (unsaved project state is lost -- save first).
# Build first with vst/build.sh (and vst/build.sh aarch64 for Gen2) and vst/gen_skin.sh.
#
#   scripts/deploy.sh <device-ip>                    MPC OS 3.9.1 / Force: /storage/Synths/NAM + its skin folder (default)
#   scripts/deploy.sh <device-ip> --vst2 <drive-root> [--arch armv7|aarch64]
#                                                    MPC OS 3.10.1+: "<drive-root>/VST2 Plugins/<vendor> - VST - NAM/",
#                                                    laid out like the release's SDK zip (plugin.json, <so>-arm32.so or
#                                                    -arm64.so, version.xml, Plugin Skins/). <drive-root> is the drive
#                                                    MPC reads, e.g. /sdcard or an attached USB drive; --arch defaults to armv7.
#
# Either way only the binary, the skin and plugin.json are replaced. models/, cabs/, models-removed/ and
# favorites.txt in the plugin folder are never touched. Nothing is installed by this script on first use in
# --vst2 mode beyond the folder itself: enable Preferences > Enhancements > External Plugin Support, then restart.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST="root@${1:?usage: scripts/deploy.sh <device-ip> [--vst2 <drive-root> [--arch armv7|aarch64]]}"
shift
VST2_ROOT="" ARCH=armv7
while [ $# -gt 0 ]; do
  case $1 in
    --vst2) VST2_ROOT="${2:?--vst2 needs a drive root, e.g. /sdcard}"; shift ;;
    --arch) ARCH="${2:?--arch needs armv7 or aarch64}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
case $ARCH in armv7|aarch64) ;; *) echo "--arch must be armv7 or aarch64" >&2; exit 2 ;; esac

SKIN_NAME=$(python3 -c 'import json; c=json.load(open("vst/vst.json")); print(c["vendor"]+" - VST - "+c["name"])')
SKIN_SRC="vst/build/skin/$SKIN_NAME"

if [ -z "$VST2_ROOT" ]; then
  # MPC OS 3.9.1 / Force: the .so under Synths/NAM, the skin folder beside it, registered in MPC.settings by install.sh.
  SO_PATH=$(python3 -c 'import json; print(json.load(open("vst/vst.json"))["install_path"])')
  SKIN_DIR="$(dirname "$(dirname "$SO_PATH")")/$SKIN_NAME"
  [ -f vst/build/nam_vst.so ] && [ -d "$SKIN_SRC/Plugin Skins" ] || { echo "build first" >&2; exit 1; }

  scp -q vst/build/nam_vst.so "$HOST:$SO_PATH.new"
  tar -C "$SKIN_SRC" -cf - "Plugin Skins" | ssh "$HOST" "rm -rf /tmp/skin.new && mkdir -p /tmp/skin.new && tar -C /tmp/skin.new -xf -"
  ssh "$HOST" "set -e; TS=\$(date +%Y%m%d-%H%M%S); B=/storage/nam-skin-backups; mkdir -p \$B
    systemctl stop acvs
    [ -f '$SO_PATH' ] && cp '$SO_PATH' \$B/nam_vst.so.\$TS
    [ -d '$SKIN_DIR/Plugin Skins' ] && cp -r '$SKIN_DIR/Plugin Skins' \$B/skin.\$TS
    mv '$SO_PATH.new' '$SO_PATH'
    mkdir -p '$SKIN_DIR'; rm -rf '$SKIN_DIR/Plugin Skins'; mv '/tmp/skin.new/Plugin Skins' '$SKIN_DIR/Plugin Skins'
    systemctl start acvs; echo deployed \$TS"
  exit 0
fi

# MPC OS 3.10.1+: stage the SDK-format folder locally, then swap in only the files this script owns.
case $ARCH in
  armv7)   SO_SRC=vst/build/nam_vst.so;         SO_NAME=nam_vst-arm32.so; KEY=file32 ;;
  aarch64) SO_SRC=vst/build/aarch64/nam_vst.so; SO_NAME=nam_vst-arm64.so; KEY=file64 ;;
esac
[ -f "$SO_SRC" ] && [ -d "$SKIN_SRC/Plugin Skins" ] && [ -f "$SKIN_SRC/version.xml" ] && [ -f vst/build/pluginlist-entry.xml ] \
  || { echo "build first ($SO_SRC, $SKIN_SRC, vst/build/pluginlist-entry.xml)" >&2; exit 1; }

DEST="${VST2_ROOT%/}/VST2 Plugins/$SKIN_NAME"
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/pkg"
cp "$SO_SRC" "$STAGE/pkg/$SO_NAME"
cp "$SKIN_SRC/version.xml" "$STAGE/pkg/"
cp -r "$SKIN_SRC/Plugin Skins" "$STAGE/pkg/"
# plugin.json as release.py writes it (Akai's plugin folder format), from the plugin-list entry the build made.
python3 - "$STAGE/pkg/plugin.json" "$KEY" "$SO_NAME" <<'PY'
import json, re, sys
out, key, so = sys.argv[1:4]
a = dict(re.findall(r'(\w+)="([^"]*)"', open("vst/build/pluginlist-entry.xml").read()))
eff = a.get("isInstrument", "0") == "0"
pj = {"name": a["name"], "manufacturer": a["manufacturer"], "version": a["version"], "format": "vst2",
      "kind": "effect" if eff else "instrument", "uid": "0x%08x" % int(a["uid"], 16),
      "category": a.get("category") or ("Effect" if eff else "Synth"),
      "numInputs": int(a.get("numInputs", "2" if eff else "0")), "numOutputs": int(a.get("numOutputs", "2")), key: so}
open(out, "w").write(json.dumps(pj, indent=2) + "\n")
PY
python3 -m json.tool "$STAGE/pkg/plugin.json" >/dev/null

ssh "$HOST" "rm -rf /tmp/nam-vst2.new && mkdir -p /tmp/nam-vst2.new"
tar -C "$STAGE/pkg" -cf - . | ssh "$HOST" "tar -C /tmp/nam-vst2.new -xf -"
# Only the .so, plugin.json, version.xml and Plugin Skins are replaced; every other entry in the folder (models/, cabs/,
# models-removed/, favorites.txt, ...) stays. The other arch's .so, if present, is left alone too.
ssh "$HOST" "set -e; TS=\$(date +%Y%m%d-%H%M%S); B=/storage/nam-skin-backups; mkdir -p \$B
  D='$DEST'; N=/tmp/nam-vst2.new
  systemctl stop acvs
  if [ -d \"\$D\" ]; then
    mkdir -p \$B/vst2.\$TS
    for f in '$SO_NAME' plugin.json version.xml 'Plugin Skins'; do if [ -e \"\$D/\$f\" ]; then cp -r \"\$D/\$f\" \$B/vst2.\$TS/; fi; done
  fi
  mkdir -p \"\$D\"
  rm -rf \"\$D/Plugin Skins\"
  mv \"\$N/Plugin Skins\" \"\$D/Plugin Skins\"
  for f in '$SO_NAME' plugin.json version.xml; do mv \"\$N/\$f\" \"\$D/\$f\"; done
  rmdir \$N
  systemctl start acvs; echo deployed to \"\$D\" \$TS"
