#!/usr/bin/env bash
# Build utils/test_app/ with code coverage enabled and run it under
# QEMU, collecting the .gcda coverage data over the apptrace UART.
#
# The console stays on UART0 (CONFIG_ESP_CONSOLE_UART_DEFAULT) so the doctest
# output is still visible, and the gcov apptrace dump runs on UART1
# (CONFIG_APPTRACE_DEST_UART_NUM=1 in sdkconfig.defaults.coverage.uart). QEMU
# exposes UART0 on its first -serial line (the console, checked for
# "[doctest] Status: SUCCESS!") and UART1 on the second -serial line (the
# coverage stream, collected by plumbing/collect_gcov_qemu.py).
#
# The emulated UART cannot deliver the synchronous host responses that the
# plain esp_gcov file I/O waits for, so the build uses a fire-and-forget UART
# override. On-device coverage (run-device-tests-coverage.sh)
# leaves the plain esp_gcov implementations in place.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> TEST_COMPONENTS=<component> \
#   run-qemu-tests-coverage.sh [output-dir]
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"
: "${TEST_COMPONENTS:?TEST_COMPONENTS must be set (e.g. es8311)}"

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"
CONSOLE_PORT="${CONSOLE_PORT:-24679}"
COV_PORT="${COV_PORT:-24680}"

case "$TARGET" in
esp32 | esp32s2 | esp32s3)
    QEMU_BIN=qemu-system-xtensa
    GCOV_EXECUTABLE=xtensa-esp32-elf-gcov
    ;;
*)
    QEMU_BIN=qemu-system-riscv32
    GCOV_EXECUTABLE=riscv32-esp-elf-gcov
    ;;
esac

TMPDIR="/tmp/run_qemu_tests_cov_${TARGET}_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS" | md5sum | cut -d' ' -f1)"
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

OUT_DIR="${TMPDIR}/coverage"

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

idf.py \
    --ccache \
    -C "${TEST_APP}" \
    -DIDF_TARGET="${TARGET}" \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DCOVERAGE_ENABLED=1 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSDKCONFIG="${TMPDIR}/config" \
    -DSDKCONFIG_DEFAULTS="${TEST_APP}/sdkconfig.defaults;${TEST_APP}/sdkconfig.defaults.coverage.uart;${TMPDIR}/sdkconfig.defaults" \
    -B "${TMPDIR}/build" \
    build \
    merge-bin \
    --pad-to-size=2MB \
    -o "${TMPDIR}/build/qemu_flash.bin"

"$ROOT/utils/plumbing/collect_gcov_qemu.py" \
    "${QEMU_BIN}" \
    "${TARGET}" \
    "${TMPDIR}/build/qemu_flash.bin" \
    "${CONSOLE_PORT}" \
    "${COV_PORT}" \
    "${OUT_DIR}"

# The .gcda data covers every instrumented file in the build, so keep the
# source files of the other components out of the coverage calculation when
# only a subset of the components is tested (TEST_COMPONENTS).
# gcovr's filters are combined with AND, so several components are matched
# with a single alternation regex instead of one -f option per component.
GCOVR_FILTERS=()
if [ -n "$TEST_COMPONENTS" ]; then
    read -r -a _components <<<"${TEST_COMPONENTS//;/ }"
    _alternation=$(
        IFS='|'
        echo "${_components[*]}"
    )
    GCOVR_FILTERS+=(-f "/.*/components/(${_alternation})/.*")
    unset _components _alternation
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
    "${OUT_DIR}"

gcovr \
    -r "${ROOT}" \
    --fail-under-branch 100 \
    --txt-metric branch \
    --gcov-executable "${GCOV_EXECUTABLE}" \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_cov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${OUT_DIR}"

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
    "${OUT_DIR}"
