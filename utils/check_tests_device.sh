#!/usr/bin/env bash
# Run the device test suites for every component that ships a test/ directory.
#
# A component is considered testable when it has its own CMakeLists.txt and a
# test/ directory. The vendored doctest, esp-dsp, esp-mqtt and esp_gcov
# components and the utils directory are always skipped.
#
# Each testable component is passed to plumbing/run-device-tests.sh (and its
# coverage variant) via TEST_COMPONENTS, so the shared utils/test_app firmware
# only links that component's tests. The device test suites also run under
# every sanitizer the chip targets support (the UBSan builds SANITIZERS=1 and
# 2; see plumbing/sanitizers.sh).
#
# Coverage collection uses the built-in USB-Serial/JTAG controller
# (plumbing/run-device-tests-coverage.sh) when test_jtag.sh verifies that JTAG
# works on the device, and falls back to UART collection
# (plumbing/run-device-tests-coverage-uart.sh) otherwise.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> PORT=<serial-device> check_tests_device.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"
: "${PORT:?PORT must be set (e.g. /dev/ttyUSB0)}"

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

echo "Checking JTAG connectivity ..."
if TARGET="$TARGET" "$DIR/test_jtag.sh"; then
    COVERAGE_SCRIPT="$DIR/plumbing/run-device-tests-coverage.sh"
    echo "JTAG works; collecting coverage over JTAG"
else
    COVERAGE_SCRIPT="$DIR/plumbing/run-device-tests-coverage-uart.sh"
    echo "JTAG unavailable; collecting coverage over UART"
fi

echo "Found ${#COMPONENTS[@]} testable components"
for COMPONENT in "${COMPONENTS[@]}"; do
    echo "==> Device tests: $COMPONENT"
    TARGET="$TARGET" PORT="$PORT" TEST_COMPONENTS="$COMPONENT" "$DIR/plumbing/run-device-tests.sh"
    # The chip targets only support the UBSan builds (SANITIZERS 1 and 2): the
    # address/leak/thread sanitizers have no compiler-rt runtime there, so TSan
    # (3 or 4) is rejected by plumbing/sanitizers.sh.
    echo "==> Device sanitizer tests: $COMPONENT"
    for SANITIZERS in 1 2; do
        echo "==> Device tests with SANITIZERS=$SANITIZERS: $COMPONENT"
        TARGET="$TARGET" PORT="$PORT" TEST_COMPONENTS="$COMPONENT" SANITIZERS="$SANITIZERS" \
            "$DIR/plumbing/run-device-tests.sh"
    done
    echo "==> Device coverage tests: $COMPONENT"
    TARGET="$TARGET" PORT="$PORT" TEST_COMPONENTS="$COMPONENT" "$COVERAGE_SCRIPT"
done

echo "All tests passed"
