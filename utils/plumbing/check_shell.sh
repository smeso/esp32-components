#!/usr/bin/env bash
# Run shellcheck over all shell scripts, excluding the vendored doctest,
# esp-dsp, esp-mqtt and esp_gcov subtrees.
#
# Usage:
#   check_shell.sh
set -euo pipefail

ROOT="${ROOT:-$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")}"

mapfile -d '' sh_files < <(find "$ROOT" -type f -name '*.sh' \
    -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/doctest/*' -not -path '*/esp_gcov/*' -print0 | sort -z)

status=0

if ((${#sh_files[@]})); then
    echo "checking ${#sh_files[@]} shell script(s) with shellcheck"
    if ! shellcheck "${sh_files[@]}"; then
        status=1
    fi
fi

if ((status)); then
    echo "error: lint checks failed" >&2
fi
exit "$status"
