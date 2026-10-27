#!/usr/bin/env bash
# Build utils/test_app/ and run its pytest suite under QEMU.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> TEST_COMPONENTS=<component> run-qemu-tests.sh
#
# Optional:
#   SANITIZERS=<1|2>   UBSan build; 1 = Debug, 2 = regular (see sanitizers.sh)
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"
: "${TEST_COMPONENTS:?TEST_COMPONENTS must be set (e.g. es8311)}"
: "${SANITIZERS:=}"

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"

# Optional sanitizer build (see sanitizers.sh). Only UBSan (1 or 2) runs on
# the chip targets: the address/leak/thread sanitizers have no compiler-rt
# runtime there, so TSan (3 or 4) is rejected.
# shellcheck disable=SC1091
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sanitizers.sh"
sanitizers_setup chip
# test_app/CMakeLists.txt selects the sanitizer build from the environment
# ($ENV{SANITIZERS}); pass it on to the build instead of a -D cache variable,
# which CMake would report as unused.
export SANITIZERS

case "$TARGET" in
esp32 | esp32s2 | esp32s3)
    QEMU_BIN=qemu-system-xtensa
    ;;
*)
    QEMU_BIN=qemu-system-riscv32
    ;;
esac

TMPDIR="/tmp/run_qemu_tests_${TARGET}_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS/$SANITIZERS" | md5sum | cut -d' ' -f1)"
if ! mkdir "$TMPDIR" 2>/dev/null; then
    # Already exists (e.g. from a previous run): only reuse it if it is a real
    # directory we own. Refuse symlinks or directories planted by other users.
    if [ ! -d "$TMPDIR" ] || [ -L "$TMPDIR" ] || [ ! -O "$TMPDIR" ]; then
        echo "error: refusing unsafe tmpdir: $TMPDIR" >&2
        exit 1
    fi
fi
chmod 700 "$TMPDIR"
rm -rf -- "${TMPDIR:?}/"*
if [ -z "${PRESERVE_TMPDIR:-}" ]; then
    trap 'rm -rf "$TMPDIR"' EXIT
fi

# ESP-IDF regenerates <build>/specs/picolibc.specs on every run with a fresh
# mtime. ccache hashes -specs= files with the compiler_check strategy, so hash
# content instead of mtime to get cache hits across runs.
export CCACHE_COMPILERCHECK=content

# The Debug SANITIZERS variants (1) build with -Og instead of the default -Os:
# append the Kconfig override (later defaults files win) and mirror what the
# coverage scripts do.
sanitizer_args=()
if [ -n "$SANITIZERS" ]; then
    if [ "$SANITIZER_DEBUG" -eq 1 ]; then
        printf '%s\n' 'CONFIG_COMPILER_OPTIMIZATION_DEBUG=y' >"${TMPDIR}/sdkconfig.defaults"
        sanitizer_args+=(-DCMAKE_BUILD_TYPE=Debug
            -DSDKCONFIG_DEFAULTS="${TEST_APP}/sdkconfig.defaults;${TMPDIR}/sdkconfig.defaults")
    fi
fi

idf.py \
    --ccache \
    -C "${TEST_APP}" \
    -DIDF_TARGET="${TARGET}" \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DSDKCONFIG="${TMPDIR}/config" \
    -B "${TMPDIR}/build" \
    "${sanitizer_args[@]}" \
    build \
    merge-bin \
    --pad-to-size=2MB \
    -o "${TMPDIR}/build/qemu_flash.bin"

pytest "$ROOT/utils/pytest/pytest_test_app.py" \
    --build-dir="${TMPDIR}/build" \
    --target="${TARGET}" \
    --embedded-services qemu \
    --qemu-image-path "${TMPDIR}/build/qemu_flash.bin" \
    --qemu-prog-path "${QEMU_BIN}" \
    --qemu-extra-args "-M ${TARGET}"
