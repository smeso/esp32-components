#!/usr/bin/env bash
# Run the full local CI pipeline for every supported target.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> fast check"
TARGET="" "$DIR/check_fast.sh"

echo "==> split fast check"
TARGET="" SPLIT_CHECKS=1 "$DIR/check_fast.sh"

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

for TARGET in esp32s3 esp32c3 esp32; do
    echo "==> Check clang-tidy for $TARGET"
    TARGET="$TARGET" "$DIR/check_clang_tidy.sh"
    echo "==> Check clang-tidy split for $TARGET"
    TARGET="$TARGET" SPLIT_CHECKS=1 "$DIR/check_clang_tidy.sh"
done

for TARGET in esp32s3 esp32c3 esp32; do
    echo "==> QEMU tests for $TARGET"
    TARGET="$TARGET" "$DIR/check_tests.sh"
    # The chip targets only support the UBSan builds (SANITIZERS 1 and 2); see
    # plumbing/sanitizers.sh.
    echo "==> QEMU tests with sanitizers for $TARGET"
    for SANITIZERS in 1 2; do
        echo "==> QEMU tests with SANITIZERS=$SANITIZERS for $TARGET"
        TARGET="$TARGET" SANITIZERS="$SANITIZERS" "$DIR/check_tests.sh"
    done
done

for TARGET in esp32s3 esp32c3 esp32; do
    echo "==> Check examples for $TARGET"
    TARGET="$TARGET" "$DIR/plumbing/check_examples.sh"
done

# esp32s31 esp32c61 esp32h2
# shellcheck disable=SC2043
for TARGET in esp32c6; do
    echo "==> Check clang-tidy for $TARGET"
    TARGET="$TARGET" SPLIT_CHECKS=1 "$DIR/check_clang_tidy.sh"
    echo "==> Check build for $TARGET"
    TARGET="$TARGET" SPLIT_CHECKS=1 "$DIR/plumbing/basic_build.sh"
    echo "==> Check examples for $TARGET"
    TARGET="$TARGET" "$DIR/plumbing/check_examples.sh"
done

# echo "==> Check build for esp32p4"
# TARGET="esp32p4" SPLIT_CHECKS=1 "$DIR/plumbing/basic_build.sh"
# echo "==> Check examples for esp32p4"
# TARGET="esp32p4" "$DIR/plumbing/check_examples.sh"

echo "All CI passed"
