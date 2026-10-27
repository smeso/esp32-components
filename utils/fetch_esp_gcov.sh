#!/usr/bin/env bash
# Update components/esp_gcov from https://github.com/espressif/idf-extra-components.git
# by copying only the contents of SUBDIR from the upstream repository.
#
# Usage:
#   fetch_esp_gcov.sh <commit>
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: ${BASH_SOURCE[0]##*/} <commit>" >&2
    exit 1
fi

COMMIT="$1"
ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
REMOTE_URL="https://github.com/espressif/idf-extra-components.git"
REMOTE_NAME="idf-extra-components"
PREFIX="components/"
SUBDIR="esp_gcov/"

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

rm -rf "${PREFIX:?}/${SUBDIR:?}"
mkdir -p "$PREFIX/$SUBDIR"
git archive "$REV" "$SUBDIR" | tar -x -C "$PREFIX"

git remote remove "$REMOTE_NAME"

git add "$PREFIX/$SUBDIR"
git commit -m "esp_gcov: bump to $COMMIT"

echo "updated $PREFIX/$SUBDIR @ $COMMIT"
