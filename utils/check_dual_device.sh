#!/usr/bin/env bash
# Build and run the two-board rig tests: the multidevice_a and multidevice_b
# firmwares from utils/multidevice_test are built for the two boards, flashed
# one per board, and both serial consoles are watched until each board reports
# its doctest result. The run succeeds only when both boards pass.
#
# The rig checks the wiring between two boards (see the connection table in
# utils/multidevice_test/README.md): each board runs one suite and the two
# firmwares stay in lockstep over UART1, so a test only completes when both
# devices are connected and running.
#
# Usage:
#   [TARGET=<esp32s3>] [PORT_A=<serial-device>] [PORT_B=<serial-device>] \
#       check_dual_device.sh
#
# TARGET defaults to esp32s3. PORT_A and PORT_B default to /dev/ttyACM0 and
# /dev/ttyACM1 (the two boards of the rig) and can be overridden with the
# environment.
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the script
# exits so it can be reused by a later run.
set -euo pipefail

TARGET="${TARGET:-esp32s3}"
PORT_A="${PORT_A:-/dev/ttyACM0}"
PORT_B="${PORT_B:-/dev/ttyACM1}"
BAUD="${BAUD:-115200}"
# The two firmwares rendezvous over UART1 and wait for each other for up to a
# minute (see multidevice_test.hpp), so a board can legitimately stay silent
# long after the monitor has opened its console. The monitor gives up when this
# timeout passes, which would then be reported instead of the boards' own
# result, so it must be well above the rendezvous budget.
MONITOR_TIMEOUT_S="${MONITOR_TIMEOUT_S:-180}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(realpath "$DIR/..")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"

if [[ "$PORT_A" == "$PORT_B" ]]; then
    echo "error: PORT_A and PORT_B must be two different serial devices (both are $PORT_A)" >&2
    exit 1
fi
for port in "$PORT_A" "$PORT_B"; do
    if [[ ! -e "$port" ]]; then
        echo "error: serial device $port not found" >&2
        exit 1
    fi
done

TMPDIR="/tmp/check_dual_device_${TARGET}_$(printf '%s' "$TEST_APP" | md5sum | cut -d' ' -f1)"
if ! mkdir "$TMPDIR" 2>/dev/null; then
    # Already exists (e.g. from a previous run): only reuse it if it is a real
    # directory we own. Refuse symlinks or directories planted by other users.
    if [[ ! -d "$TMPDIR" || -L "$TMPDIR" || ! -O "$TMPDIR" ]]; then
        echo "error: refusing unsafe tmpdir: $TMPDIR" >&2
        exit 1
    fi
fi
chmod 700 "$TMPDIR"
rm -rf -- "${TMPDIR:?}/"*
if [[ -z "${PRESERVE_TMPDIR:-}" ]]; then
    trap 'rm -rf "$TMPDIR"' EXIT
fi

# ESP-IDF regenerates <build>/specs/picolibc.specs on every run with a fresh
# mtime. ccache hashes -specs= files with the compiler_check strategy, so hash
# content instead of mtime to get cache hits across runs.
export CCACHE_COMPILERCHECK=content

# Build the firmware of one suite. MULTIDEVICE_SUITE selects the source file
# compiled into the multidevice_test component (see its CMakeLists.txt) and the
# doctest suite the shared runner runs, so the two builds produce one image per
# board with no other component tests linked in.
build_firmware() {
    local suite="$1" build="$2" config="$3"
    echo "==> Building $suite for $TARGET"
    idf.py --ccache \
        -C "$TEST_APP" \
        -DIDF_TARGET="$TARGET" \
        -DMULTIDEVICE_SUITE="$suite" \
        -DSDKCONFIG="$config" \
        -B "$build" \
        build
}

# Flash a firmware on one board.
flash_firmware() {
    local build="$1" port="$2"
    echo "==> Flashing on $port"
    idf.py -C "$TEST_APP" -B "$build" -p "$port" flash
}

echo "==> Building both rig firmwares ($TARGET)"
build_firmware multidevice_a "$TMPDIR/build_a" "$TMPDIR/config_a"
build_firmware multidevice_b "$TMPDIR/build_b" "$TMPDIR/config_b"

# Flashing resets a board, so a board can already be running its suite while
# the other one is still being flashed. The two firmwares wait for each other
# for up to a minute, but to be sure that both runs are captured the monitor
# below resets both boards through the serial port again once it is listening
# on both.
echo "==> Flashing the boards"
flash_firmware "$TMPDIR/build_a" "$PORT_A"
flash_firmware "$TMPDIR/build_b" "$PORT_B"

echo "==> Watching both consoles (timeout: ${MONITOR_TIMEOUT_S}s)"
monitor_rc=0
"$DIR/plumbing/monitor_dual.py" "$PORT_A" "$PORT_B" "$BAUD" "$MONITOR_TIMEOUT_S" || monitor_rc=$?

if ((monitor_rc != 0)); then
    echo "error: the rig tests did not pass on both boards" >&2
    exit "$monitor_rc"
fi

echo "the rig tests passed on both boards"
