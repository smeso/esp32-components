#!/usr/bin/env bash
# Update components/esp-mqtt from https://github.com/espressif/esp-mqtt.git
# by copying the contents of the upstream repository.
#
# Usage:
#   fetch_esp_mqtt.sh <commit>
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: ${BASH_SOURCE[0]##*/} <commit>" >&2
    exit 1
fi

COMMIT="$1"
ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
REMOTE_URL="https://github.com/espressif/esp-mqtt.git"
REMOTE_NAME="esp-mqtt"
PREFIX="components/esp-mqtt/"

cd "$ROOT"

if ! git remote get-url "$REMOTE_NAME" >/dev/null 2>&1; then
    git remote add "$REMOTE_NAME" "$REMOTE_URL"
fi

git fetch "$REMOTE_NAME"

if git rev-parse --verify -q "$COMMIT^{commit}" >/dev/null; then
    REV="$COMMIT"
else
    REV="$REMOTE_NAME/$COMMIT"
fi

rm -rf "${PREFIX:?}"
mkdir -p "$PREFIX"
git archive "$REV" | tar -x -C "$PREFIX"

git remote remove "$REMOTE_NAME"

git add "$PREFIX"
git commit -m "esp-mqtt: bump to $COMMIT"

echo "updated $PREFIX @ $COMMIT"
