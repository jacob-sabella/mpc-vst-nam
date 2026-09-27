#!/usr/bin/env bash
# Development deploy over an existing install: push vst/build/nam_vst.so and the skin to a device,
# keeping the previous copies in /storage/nam-skin-backups/, and restart MPC (unsaved project state
# is lost -- save first). Build first with vst/build.sh and vst/gen_skin.sh.
#   scripts/deploy.sh <device-ip>
set -euo pipefail
cd "$(dirname "$0")/.."
HOST="root@${1:?usage: scripts/deploy.sh <device-ip>}"
SO_PATH=$(python3 -c 'import json; print(json.load(open("vst/vst.json"))["install_path"])')
SKIN_NAME=$(python3 -c 'import json; c=json.load(open("vst/vst.json")); print(c["vendor"]+" - VST - "+c["name"])')
SKIN_DIR="$(dirname "$(dirname "$SO_PATH")")/$SKIN_NAME"
[ -f vst/build/nam_vst.so ] && [ -d "vst/build/skin/$SKIN_NAME/Plugin Skins" ] || { echo "build first" >&2; exit 1; }

scp -q vst/build/nam_vst.so "$HOST:$SO_PATH.new"
tar -C "vst/build/skin/$SKIN_NAME" -cf - "Plugin Skins" | ssh "$HOST" "rm -rf /tmp/skin.new && mkdir -p /tmp/skin.new && tar -C /tmp/skin.new -xf -"
ssh "$HOST" "set -e; TS=\$(date +%Y%m%d-%H%M%S); B=/storage/nam-skin-backups; mkdir -p \$B
  systemctl stop acvs
  [ -f '$SO_PATH' ] && cp '$SO_PATH' \$B/nam_vst.so.\$TS
  [ -d '$SKIN_DIR/Plugin Skins' ] && cp -r '$SKIN_DIR/Plugin Skins' \$B/skin.\$TS
  mv '$SO_PATH.new' '$SO_PATH'
  mkdir -p '$SKIN_DIR'; rm -rf '$SKIN_DIR/Plugin Skins'; mv '/tmp/skin.new/Plugin Skins' '$SKIN_DIR/Plugin Skins'
  systemctl start acvs; echo deployed \$TS"
