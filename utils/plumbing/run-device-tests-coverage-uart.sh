#!/usr/bin/env bash
# Build utils/test_app/ with code coverage enabled, flash it to a real
# device and collect the .gcda coverage data over its UART.
#
# This targets devices that have no JTAG and only one UART wired to the
# host. The build uses the UART apptrace destination
# (sdkconfig.defaults.coverage.uart.device), which disables the console so
# the gcov apptrace channel can take that same UART (UART0), and the
# fire-and-forget UART file I/O override (gcov_rtio_uart.c), so the UART
# wired to the host carries both the status markers (the doctest result and
# "coverage dump ready", sent by the app through the apptrace channel) and
# the coverage dump. plumbing/collect_gcov_uart.py opens that one serial
# port, resets the device through the RTS/DTR auto-reset circuit (or waits
# for a manual reset), checks the doctest status in the stream and parses the
# interleaved apptrace command stream to reconstruct the .gcda files.
#
# Unlike plumbing/run-device-tests-coverage.sh (JTAG over the built-in
# USB-Serial/JTAG controller) this works on every target with a plain UART,
# including esp32/esp32s2/esp32c2.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> PORT=<serial-device> \
#   TEST_COMPONENTS=<component> run-device-tests-coverage-uart.sh
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"
: "${PORT?PORT must be set (e.g. /dev/ttyUSB0)}"
: "${TEST_COMPONENTS:?TEST_COMPONENTS must be set (e.g. es8311)}"

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"

TMPDIR="/tmp/run_device_tests_cov_uart_${TARGET}_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS" | md5sum | cut -d' ' -f1)"
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

SDKCONFIG_DEFAULTS="${TEST_APP}/sdkconfig.defaults;${TEST_APP}/sdkconfig.defaults.coverage.uart.device;${TMPDIR}/sdkconfig.defaults"

idf.py \
    --ccache \
    -C "${TEST_APP}" \
    -DIDF_TARGET="${TARGET}" \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DCOVERAGE_ENABLED=1 \
    -DSDKCONFIG="${TMPDIR}/config" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
    -B "${TMPDIR}/build" \
    -p "${PORT}" \
    build flash

# The dump shares the (disabled) console UART, so the collector must read at
# the apptrace baud rate rather than the default console baud; take it from
# the generated sdkconfig to stay in sync with the build.
APPTRACE_BAUD="$(grep -E '^CONFIG_APPTRACE_UART_BAUDRATE=' "${TMPDIR}/config" | cut -d= -f2)"

"$ROOT/utils/plumbing/collect_gcov_uart.py" \
    "${PORT}" \
    "${TMPDIR}/build" \
    "${COV_OUT_DIR}" \
    "${APPTRACE_BAUD}"

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
