#!/usr/bin/env bash
# Build utils/test_app/ for the FreeRTOS POSIX/Linux simulator with code
# coverage enabled and run its doctest suites directly on the host, then show
# the coverage with gcovr.
#
# The Linux target is a host build: idf.py produces a native ELF that the test
# runner executes in place (no QEMU, no serial port). Unlike the chip targets,
# the esp_gcov runtime is not available (its esp_trace/app_trace dependencies
# are not supported by the Linux simulator), so the coverage build instruments
# the components under test with --coverage instead and flushes the collected
# .gcda data with the host libgcov's __gcov_dump() after the suites. gcovr then
# reports the results; no minimum coverage is enforced here.
#
# Usage:
#   TEST_COMPONENTS=<component> run-linux-tests-coverage.sh
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"
TEST_COMPONENTS="${TEST_COMPONENTS:-}"

TMPDIR="/tmp/run_linux_tests_cov_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS" | md5sum | cut -d' ' -f1)"
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

export COVERAGE_ENABLED=1

# Coverage is a separate CI mode from the optional SANITIZERS builds; drop any
# inherited SANITIZERS so this build is not accidentally instrumented twice.
unset SANITIZERS

# CONFIG_APP_REPRODUCIBLE_BUILD=y (in the project sdkconfig.defaults) asks the
# build system to remap paths relative to the toolchain sysroot, but a host
# toolchain reports no sysroot, so the configure step fails. Drop the option
# for the Linux build; the tests do not care about reproducible binaries.
grep -v '^CONFIG_APP_REPRODUCIBLE_BUILD=' "${TEST_APP}/sdkconfig.defaults" \
    >"${TMPDIR}/sdkconfig.defaults"

# The task_queue ISR test posts from the FreeRTOS tick hook, so the hook must
# be enabled for the simulator build. Device builds keep the default (off) and
# are unaffected; the linux-only tick-hook mock in the mocks component backs
# the ESP-IDF hook API, which esp_system does not build for linux.
echo 'CONFIG_FREERTOS_USE_TICK_HOOK=y' >>"${TMPDIR}/sdkconfig.defaults"

# Coverage is collected from a Debug build of the test app. ESP-IDF picks the
# optimization from the Kconfig COMPILER_OPTIMIZATION_* choice, so
# -DCMAKE_BUILD_TYPE=Debug alone would leave the sdkconfig.defaults default of
# CONFIG_COMPILER_OPTIMIZATION_SIZE=y (-Os) in place; append the -Og override
# (later options in the defaults file win). The non-coverage scripts keep the
# default build.
echo 'CONFIG_COMPILER_OPTIMIZATION_DEBUG=y' >>"${TMPDIR}/sdkconfig.defaults"

# linux is a preview target, so the build needs --preview.
idf.py \
    --ccache \
    --preview \
    -C "${TEST_APP}" \
    -DIDF_TARGET=linux \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DCOVERAGE_ENABLED=1 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSDKCONFIG="${TMPDIR}/config" \
    -DSDKCONFIG_DEFAULTS="${TMPDIR}/sdkconfig.defaults" \
    -B "${TMPDIR}/build" \
    build

# Run the host ELF directly. The FreeRTOS Linux simulator keeps the scheduler
# running after app_main() returns, so the process does not exit on its own:
# wait for the doctest success marker in the log and then stop it. The runner
# flushes the .gcda data with __gcov_dump() after the suites, so killing the
# process afterwards loses nothing.
"${TMPDIR}/build/test_app.elf" >"${TMPDIR}/test.log" 2>&1 &
pid=$!
for _ in $(seq 1 120); do
    if grep -q '\[doctest\] Status: SUCCESS!' "${TMPDIR}/test.log" 2>/dev/null; then
        break
    fi
    if grep -q '\[doctest\] Status: FAILURE!' "${TMPDIR}/test.log" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
cat "${TMPDIR}/test.log"
if ! grep -q '\[doctest\] Status: SUCCESS!' "${TMPDIR}/test.log"; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "error: test suite did not report success" >&2
    exit 1
fi
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# The .gcda files are written next to the .gcno notes in the build tree.
# Report the coverage of the components under test; no minimum is enforced.
#
# When only a subset of the components is tested (TEST_COMPONENTS), the
# .gcda data still covers every instrumented file in the build, so keep the
# source files of the other components out of the coverage calculation.
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
    --gcov-executable gcov \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_gcov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${TMPDIR}/build"

gcovr \
    -r "${ROOT}" \
    --fail-under-branch 100 \
    --txt-metric branch \
    --gcov-executable gcov \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_gcov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${TMPDIR}/build"

gcovr \
    -r "${ROOT}" \
    --fail-under-decision 100 \
    --decision \
    --txt-metric decision \
    --gcov-executable gcov \
    "${GCOVR_FILTERS[@]}" \
    -e ".*/(doctest|esp-dsp|esp-mqtt|esp_gcov)/.*" \
    -e ".*/test_.*" \
    -e ".*/mock_.*" \
    "${TMPDIR}/build"
