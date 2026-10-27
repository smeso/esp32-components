#!/usr/bin/env bash
# Discover the testable components in the repo.
#
# A component is testable if it has a CMakeLists.txt and a test/ directory.
# The doctest, esp-dsp, esp-mqtt, esp_gcov and utils directories are excluded.
#
# Usage:
#   discover_components <root>
#
# Prints one component name per line, sorted.
set -euo pipefail

discover_components() {
    local root="$1"
    for component in "$root"/*/; do
        local name
        name="$(basename "$component")"
        case "$name" in
        doctest | esp-dsp | esp-mqtt | esp_gcov | utils) continue ;;
        esac
        if [ -f "$component/CMakeLists.txt" ] && [ -d "$component/test" ]; then
            printf '%s\n' "$name"
        fi
    done | sort
}
