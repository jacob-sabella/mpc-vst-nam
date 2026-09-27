#!/usr/bin/env bash
# Scan the repository's full git history and working tree for credentials with TruffleHog (Docker).
# Exits non-zero on any finding, verified or not. CI runs this in .github/workflows/secrets.yml.
#   scripts/secret-scan.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE="ghcr.io/trufflesecurity/trufflehog:${TRUFFLEHOG_VERSION:-3.97.9}"
ARGS=(--no-update --fail --results=verified,unverified,unknown)
docker run --rm -v "$PWD:/repo:ro" "$IMAGE" git file:///repo "${ARGS[@]}"
# Working tree too, for uncommitted files; .git is covered above and vst/build is build output.
EXCLUDE=$(mktemp); trap 'rm -f "$EXCLUDE"' EXIT
printf '%s\n' '^/repo/\.git/' '^/repo/vst/build/' > "$EXCLUDE"
docker run --rm -v "$PWD:/repo:ro" -v "$EXCLUDE:/exclude.txt:ro" "$IMAGE" filesystem /repo "${ARGS[@]}" \
  --exclude-paths=/exclude.txt
