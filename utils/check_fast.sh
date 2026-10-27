#!/usr/bin/env bash
# Run the fast local CI checks:
#   1. format.sh --check                    - verify C/C++, Python, shell formatting
#   2. plumbing/check_scripts.sh            - ruff, shellcheck, cmake-lint
#   3. check_clang_tidy.sh                  - clang-tidy over the clang toolchain build
#   4. plumbing/run-linux-tests.sh          - doctest suites on the Linux simulator
#   5. plumbing/run-linux-tests-coverage.sh - doctest suites on the Linux simulator with coverage
#
# When SANITIZERS is set the tests run under the selected sanitizer build and
# the coverage run (step 5) is skipped (a sanitizer build does not collect
# coverage).
#
# With SPLIT_CHECKS=1, steps 3-5 are run for each testable component
# separately (via TEST_COMPONENTS if set, otherwise all testable components
# are discovered), like check_tests.sh does.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> check_fast.sh
#   check_fast.sh               # skips clang-tidy (TARGET not set)
#   SPLIT_CHECKS=1 check_fast.sh
#   SANITIZERS=<1|2|3|4> check_fast.sh
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(realpath "$DIR/..")"

# shellcheck disable=SC1091
source "$DIR/plumbing/components_discovery.sh"

echo "==> format check"
"$DIR/format.sh" --check

echo "==> script checks"
"$DIR/plumbing/check_scripts.sh"

if [ -n "${SPLIT_CHECKS:-}" ]; then
    if [ -n "${TEST_COMPONENTS:-}" ]; then
        read -r -a COMPONENTS <<<"${TEST_COMPONENTS//;/ }"
    else
        mapfile -t COMPONENTS < <(discover_components "$ROOT")
    fi

    for COMPONENT in "${COMPONENTS[@]}"; do
        if [[ -n "${TARGET:-}" ]]; then
            echo "==> clang-tidy: $COMPONENT"
            TARGET="$TARGET" TEST_COMPONENTS="$COMPONENT" "$DIR/check_clang_tidy.sh"
        fi
        echo "==> linux tests: $COMPONENT"
        TEST_COMPONENTS="$COMPONENT" "$DIR/plumbing/run-linux-tests.sh"
        if [ -n "${SANITIZERS:-}" ]; then
            echo "==> Skipping linux tests with coverage (SANITIZERS set): $COMPONENT"
        else
            echo "==> linux tests with coverage: $COMPONENT"
            TEST_COMPONENTS="$COMPONENT" "$DIR/plumbing/run-linux-tests-coverage.sh"
        fi
    done
else
    if [[ -n "${TARGET:-}" ]]; then
        echo "==> clang-tidy"
        TARGET="$TARGET" "$DIR/check_clang_tidy.sh"
    else
        echo "WARNING: TARGET not set, skipping clang-tidy check"
    fi

    echo "==> linux tests"
    "$DIR/plumbing/run-linux-tests.sh"

    if [ -n "${SANITIZERS:-}" ]; then
        echo "==> Skipping linux tests with coverage (SANITIZERS set)"
    else
        echo "==> linux tests with coverage"
        "$DIR/plumbing/run-linux-tests-coverage.sh"
    fi
fi

echo "fast checks passed"
