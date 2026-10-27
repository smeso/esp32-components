#!/usr/bin/env bash
# Build utils/test_app/ for the FreeRTOS POSIX/Linux simulator and run its
# doctest suites directly on the host.
#
# The Linux target is a host build: idf.py produces a native ELF that the
# test runner executes in place (no QEMU, no serial port).
#
# Usage:
#   TEST_COMPONENTS=<component> run-linux-tests.sh
#
# Optional:
#   SANITIZERS=<1|2|3|4>  1/2 = UBSan+ASan+LSan, 3/4 = TSan;
#                         1 and 3 are Debug builds (see sanitizers.sh)
#   TSan (3/4) needs ASLR off for the test process; the script runs the tests
#   under 'setarch -R' for that, or fails early if the environment blocks it.
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"
TEST_COMPONENTS="${TEST_COMPONENTS:-}"
: "${SANITIZERS:=}"

# Optional sanitizer build (see sanitizers.sh). All four variants run on the
# linux target: UBSan/ASan/LSan (1 or 2) and TSan (3 or 4).
# shellcheck disable=SC1091
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sanitizers.sh"
sanitizers_setup linux
# test_app/CMakeLists.txt selects the sanitizer build from the environment
# ($ENV{SANITIZERS}); pass it on to the build instead of a -D cache variable,
# which CMake would report as unused.
export SANITIZERS

# TSan (SANITIZERS 3/4) needs a fixed process address space: on kernels with
# high mmap ASLR entropy the randomized layout can collide with the region TSan
# reserves for its shadow memory. The personality() flags are inherited by
# children, so first check /proc/self/personality for ADDR_NO_RANDOMIZE
# (0x00040000): if ASLR is already off the tests inherit that. Otherwise run
# the test ELF under 'setarch -R', which needs the personality() syscall that
# some container seccomp policies block.
tsan_wrapper=()
if [ "$SANITIZERS" = 3 ] || [ "$SANITIZERS" = 4 ]; then
    # /proc/self/personality prints the personality() bitmask in hex (no 0x).
    personality="$(cat /proc/self/personality)"
    if [ $((0x${personality} & 0x00040000)) -eq 0 ]; then
        if ! setarch "$(uname -m)" -R true 2>/dev/null; then
            echo "error: SANITIZERS=$SANITIZERS (thread sanitizer) requires ASLR to be disabled, but it cannot be disabled in this environment (the personality() syscall is blocked, e.g. by a container seccomp policy). Re-run under 'setarch $(uname -m) -R' or with ASLR already disabled." >&2
            exit 1
        fi
        tsan_wrapper=(setarch "$(uname -m)" -R)
    fi
fi

TMPDIR="/tmp/run_linux_tests_$(printf '%s' "$TEST_APP/$TEST_COMPONENTS/$SANITIZERS" | md5sum | cut -d' ' -f1)"
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

# The Debug SANITIZERS variants (1 and 3) build with -Og instead of the
# default -Os: append the Kconfig override (later options in the defaults file
# win), like the coverage script does.
sanitizer_args=()
if [ -n "$SANITIZERS" ]; then
    if [ "$SANITIZER_DEBUG" -eq 1 ]; then
        echo 'CONFIG_COMPILER_OPTIMIZATION_DEBUG=y' >>"${TMPDIR}/sdkconfig.defaults"
        sanitizer_args+=(-DCMAKE_BUILD_TYPE=Debug)
    fi
fi

# linux is a preview target, so the build needs --preview.
idf.py \
    --ccache \
    --preview \
    -C "${TEST_APP}" \
    -DIDF_TARGET=linux \
    -DTEST_COMPONENTS="${TEST_COMPONENTS}" \
    -DSDKCONFIG="${TMPDIR}/config" \
    -DSDKCONFIG_DEFAULTS="${TMPDIR}/sdkconfig.defaults" \
    -B "${TMPDIR}/build" \
    "${sanitizer_args[@]}" \
    build

# Run the host ELF directly. The FreeRTOS Linux simulator keeps the scheduler
# running after app_main() returns, so the process does not exit on its own:
# wait for the doctest success marker in the log and then stop it.
"${tsan_wrapper[@]}" "${TMPDIR}/build/test_app.elf" >"${TMPDIR}/test.log" 2>&1 &
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
