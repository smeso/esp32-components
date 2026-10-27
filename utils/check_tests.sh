#!/usr/bin/env bash
# Run the QEMU test suites for every component that ships a test/ directory.
#
# A component is considered testable when it has its own CMakeLists.txt and a
# test/ directory. The vendored doctest, esp-dsp, esp-mqtt and esp_gcov
# components and the utils directory are always skipped.
#
# Each testable component is passed to plumbing/run-qemu-tests.sh (and its coverage
# variant) via TEST_COMPONENTS, so the shared utils/test_app firmware only
# links that component's tests.
#
# When SANITIZERS is set the component runs under the selected sanitizer build
# and the coverage run is skipped (a sanitizer build does not collect
# coverage).
#
# Usage:
#   TARGET=<esp32|esp32c3|...> check_tests.sh
#   TARGET=<esp32|esp32c3|...> SANITIZERS=<1|2> check_tests.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(realpath "$DIR/..")"

# shellcheck disable=SC1091
source "$DIR/plumbing/components_discovery.sh"

if [ -n "${TEST_COMPONENTS:-}" ]; then
    read -r -a COMPONENTS <<<"${TEST_COMPONENTS//;/ }"
else
    mapfile -t COMPONENTS < <(discover_components "$ROOT")
fi

if [ "${#COMPONENTS[@]}" -eq 0 ]; then
    echo "No testable components found"
    exit 0
fi

echo "Found ${#COMPONENTS[@]} testable components"
for COMPONENT in "${COMPONENTS[@]}"; do
    echo "==> QEMU tests: $COMPONENT"
    TARGET="$TARGET" TEST_COMPONENTS="$COMPONENT" "$DIR/plumbing/run-qemu-tests.sh"
    if [ -n "${SANITIZERS:-}" ]; then
        echo "==> Skipping QEMU coverage tests (SANITIZERS set): $COMPONENT"
    else
        echo "==> QEMU coverage tests: $COMPONENT"
        TARGET="$TARGET" TEST_COMPONENTS="$COMPONENT" "$DIR/plumbing/run-qemu-tests-coverage.sh"
    fi
done

echo "All tests passed"
