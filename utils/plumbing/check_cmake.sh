#!/usr/bin/env bash
# Run cmake-lint over all CMake files, excluding the
# vendored doctest, esp-dsp, esp-mqtt and esp_gcov subtrees.
#
# Usage:
#   check_cmake.sh
set -euo pipefail

ROOT="${ROOT:-$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")}"

mapfile -d '' cmake_files < <(find "$ROOT/" \( -name 'CMakeLists.txt' -o -name '*.cmake' \) \
    -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/doctest/*' -not -path '*/esp_gcov/*' -print0 | sort -z)

if ((${#cmake_files[@]})); then
    echo "checking ${#cmake_files[@]} CMake file(s) with cmake-lint"
    cmake-lint "${cmake_files[@]}"
fi
