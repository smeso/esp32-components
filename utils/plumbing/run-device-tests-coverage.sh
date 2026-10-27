#!/usr/bin/env bash
# Build utils/test_app/ with code coverage enabled, flash it to a real
# device and collect the .gcda coverage data over the built-in
# USB-Serial/JTAG controller (targets such as ESP32-S3, ESP32-C3/C6/H2).
#
# The build uses the JTAG apptrace destination
# (sdkconfig.defaults.coverage.jtag) and the app blocks in esp_gcov_dump()
# after the doctest suites; plumbing/collect_gcov_jtag.py starts OpenOCD on the built-in
# adapter, resets the app over JTAG (`reset run`) and pulls the .gcda files
# with `esp gcov dump` (the same hard-coded-dump flow as ESP-IDF's gcov
# example CI).
#
# Unlike plumbing/run-qemu-tests-coverage.sh this runs on real hardware: the firmware
# is flashed with idf.py flash and uses the plain esp_gcov implementations
# whose synchronous responses OpenOCD answers over JTAG. The QEMU-only
# fire-and-forget UART file I/O override (gcov_rtio_uart.c) is not used here.
#
# Usage:
#   TARGET=<esp32s3|esp32c3|...> [PORT=<serial-device>] \
#   TEST_COMPONENTS=<component> run-device-tests-coverage.sh
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32s3, esp32c3)}"
: "${PORT?PORT must be set (e.g. /dev/ttyACM0)}"
: "${TEST_COMPONENTS:?TEST_COMPONENTS must be set (e.g. es8311)}"

case "$TARGET" in
esp32 | esp32s2 | esp32c2)
    echo "error: coverage collection over JTAG is not supported for $TARGET" >&2
    exit 1
    ;;
esac

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"

TMPDIR="/tmp/run_device_tests_cov_${TARGET}_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS" | md5sum | cut -d' ' -f1)"
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

COV_OUT_DIR="${TMPDIR}/coverage"

export COVERAGE_ENABLED=1

# Coverage is a separate CI mode from the optional SANITIZERS builds; drop any
# inherited SANITIZERS so this build is not accidentally instrumented twice.
unset SANITIZERS

# Coverage is collected from a Debug build of the test app. ESP-IDF picks the
# optimization from the Kconfig COMPILER_OPTIMIZATION_* choice, so
# -DCMAKE_BUILD_TYPE=Debug alone would leave the sdkconfig.defaults default of
# CONFIG_COMPILER_OPTIMIZATION_SIZE=y (-Os) in place; append the -Og override
# (later defaults files win). The non-coverage scripts keep the default build.
echo 'CONFIG_COMPILER_OPTIMIZATION_DEBUG=y' >"${TMPDIR}/sdkconfig.defaults"

SDKCONFIG_DEFAULTS="${TEST_APP}/sdkconfig.defaults;${TEST_APP}/sdkconfig.defaults.coverage.jtag;${TMPDIR}/sdkconfig.defaults"

idf.py \
    --ccache \
    -C "${TEST_APP}" \
    -DIDF_TARGET="${TARGET}" \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DCOVERAGE_ENABLED=1 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSDKCONFIG="${TMPDIR}/config" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
    -B "${TMPDIR}/build" \
    -p "${PORT}" \
    build flash

"$ROOT/utils/plumbing/collect_gcov_jtag.py" \
    "${TARGET}" \
    "${TMPDIR}/build" \
    "${COV_OUT_DIR}" \
    "${PORT}"

case "$TARGET" in
esp32 | esp32s2 | esp32s3)
    GCOV_EXECUTABLE=xtensa-esp32-elf-gcov
    ;;
*)
    GCOV_EXECUTABLE=riscv32-esp-elf-gcov
    ;;
esac

# The .gcda data covers every instrumented file in the build, so keep the
# source files of the other components out of the coverage calculation when
# only a subset of the components is tested (TEST_COMPONENTS).
GCOVR_FILTERS=()
if [ -n "$TEST_COMPONENTS" ]; then
    for _component in ${TEST_COMPONENTS//;/ }; do
        GCOVR_FILTERS+=(-f "/.*/components/${_component}/.*")
    done
    unset _component
fi

gcovr \
    -r "${ROOT}" \
    --fail-under-line 100 \
    --txt-metric line \
    --gcov-executable "${GCOV_EXECUTABLE}" \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_cov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${COV_OUT_DIR}"

gcovr \
    -r "${ROOT}" \
    --fail-under-branch 100 \
    --txt-metric branch \
    --gcov-executable "${GCOV_EXECUTABLE}" \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_cov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${COV_OUT_DIR}"

gcovr \
    -r "${ROOT}" \
    --fail-under-decision 100 \
    --decision \
    --txt-metric decision \
    --gcov-executable "${GCOV_EXECUTABLE}" \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_cov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${COV_OUT_DIR}"
