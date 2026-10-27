#!/usr/bin/env bash
# Download doctest.h at a given commit into doctest/doctest/.
#
# Usage:
#   fetch_doctest.sh <commit>
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: ${BASH_SOURCE[0]##*/} <commit>" >&2
    exit 1
fi

COMMIT="$1"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/doctest/doctest/doctest.h"

mkdir -p "$(dirname "$DEST")"
curl -fsSL "https://raw.githubusercontent.com/doctest/doctest/$COMMIT/doctest/doctest.h" -o "$DEST"
echo "downloaded doctest.h @ $COMMIT to $DEST"
