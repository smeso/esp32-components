#!/usr/bin/env bash
# Run the full local CI pipeline.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> ci.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> fast check"
TARGET="$TARGET" "$DIR/check_fast.sh"

echo "==> split fast check"
TARGET="$TARGET" SPLIT_CHECKS=1 "$DIR/check_fast.sh"

# check_fast.sh runs the linux tests in one host build, and those support
# every sanitizer variant (SANITIZERS 1-4: UBSan+ASan+LSan and TSan); see
# plumbing/sanitizers.sh.
echo "==> fast check with sanitizers"
for SANITIZERS in 1 2 3 4; do
    echo "==> fast check with SANITIZERS=$SANITIZERS"
    TARGET="" SANITIZERS="$SANITIZERS" "$DIR/check_fast.sh"
done
echo "==> split fast check with sanitizers"
for SANITIZERS in 1 2 3 4; do
    echo "==> fast check with SANITIZERS=$SANITIZERS"
    TARGET="" SPLIT_CHECKS=1 SANITIZERS="$SANITIZERS" "$DIR/check_fast.sh"
done

echo "==> QEMU tests"
TARGET="$TARGET" "$DIR/check_tests.sh"

# The chip targets only support the UBSan builds (SANITIZERS 1 and 2); see
# plumbing/sanitizers.sh.
echo "==> QEMU tests with sanitizers"
for SANITIZERS in 1 2; do
    echo "==> QEMU tests with SANITIZERS=$SANITIZERS"
    TARGET="$TARGET" SANITIZERS="$SANITIZERS" "$DIR/check_tests.sh"
done

echo "==> Check examples"
TARGET="$TARGET" "$DIR/plumbing/check_examples.sh"

echo "CI passed"
