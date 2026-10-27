#!/usr/bin/env bash
# Run ruff over all Python scripts, excluding the vendored doctest,
# esp-dsp, esp-mqtt and esp_gcov subtrees.
#
# Usage:
#   check_python.sh
set -euo pipefail

ROOT="${ROOT:-$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")}"

mapfile -d '' py_files < <(find "$ROOT" -type f -name '*.py' \
    -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/doctest/*' -not -path '*/esp_gcov/*' -print0 | sort -z)

status=0

if ((${#py_files[@]})); then
    echo "checking ${#py_files[@]} Python file(s) with ruff"
    if ! ruff check "${py_files[@]}"; then
        status=1
    fi
fi

if ((status)); then
    echo "error: lint checks failed" >&2
fi
exit "$status"
