#!/usr/bin/env bash
# Checkout the exact pinned upstream firmware; do not edit upstream in this step.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-"${PROJECT_ROOT}/.upstream/xiaozhi-esp32"}"
VALUES="$(python3 - "$PROJECT_ROOT/version-lock.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    manifest = json.load(f)
fw = manifest["firmware"]
print(fw["repository"])
print(fw["tag"])
print(fw["commit"])
PY
)"
mapfile -t FIELDS <<< "$VALUES"
URL="${FIELDS[0]}"
TAG="${FIELDS[1]}"
SHA="${FIELDS[2]}"

if [ -e "$DEST" ]; then
  echo "Refusing to overwrite existing path: $DEST" >&2
  exit 2
fi

mkdir -p "$(dirname "$DEST")"
git clone --depth 1 --branch "$TAG" "$URL" "$DEST"
ACTUAL="$(git -C "$DEST" rev-parse HEAD)"
if [ "$ACTUAL" != "$SHA" ]; then
  echo "Unexpected upstream commit: $ACTUAL (expected $SHA)" >&2
  exit 3
fi
git -C "$DEST" switch --detach "$SHA"
echo "Pinned XiaoZhi firmware: $TAG ($SHA)"
python3 "$PROJECT_ROOT/scripts/p0_check.py" --source "$DEST"
