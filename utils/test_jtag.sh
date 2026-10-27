#!/usr/bin/env bash
# Verify that the JTAG connection to a real device works.
#
# Starts OpenOCD on the board's built-in USB-Serial/JTAG adapter
# (board/<target>-builtin.cfg) and then, through its telnet server, issues
# commands that both write to and read back from the target over JTAG:
#   - `version`:         OpenOCD answers
#   - `scan_chain`:      the target's TAP is found (IDCODE read back)
#   - `halt`/`targets`:  the CPU can be halted through JTAG
#   - `reg`:             a scratch register is written and read back
#   - `mww`/`mdw`:       a scratch word in DRAM is written and read back
#
# The target is left running (`resume`) when the test finishes. Each check is
# independent: failures are reported and the script exits non-zero if any of
# them failed, but a failed check does not abort the remaining ones.
#
# Before starting OpenOCD the script verifies that the board's built-in
# USB-Serial/JTAG adapter (VID:PID 303a:1001) is present on the USB bus and
# that libusb can actually open it (its /dev/bus/usb node exists and is
# accessible), so a missing or unreachable board is reported with a clear
# message instead of OpenOCD's cryptic "could not find or open device" error.
#
# Usage:
#   TARGET=<esp32c3|esp32s3|...> [OPENOCD_TELNET_PORT=<port>] test_jtag.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32c3, esp32s3)}"

case "$TARGET" in
esp32 | esp32s2 | esp32c2)
    echo "error: the builtin JTAG is not available on $TARGET" >&2
    exit 1
    ;;
esac

TELNET_PORT="${OPENOCD_TELNET_PORT:-4444}"
TELNET_TMO_S=30

# Locate the OpenOCD binary (OPENOCD env / PATH / IDF tools dir).
find_openocd() {
    if [[ -n "${OPENOCD:-}" && -x "$OPENOCD" ]]; then
        printf '%s' "$OPENOCD"
        return
    fi
    if command -v openocd >/dev/null 2>&1; then
        command -v openocd
        return
    fi
    local tools cand ver
    tools="${IDF_TOOLS_PATH:-$HOME/.espressif}/tools/openocd-esp32"
    if [[ -d "$tools" ]]; then
        for ver in $(find "$tools" -maxdepth 1 -mindepth 1 -type d -printf '%f\n' | sort -V -r); do
            cand="$tools/$ver/openocd-esp32/bin/openocd"
            if [[ -x "$cand" ]]; then
                printf '%s' "$cand"
                return
            fi
        done
    fi
    return 1
}

# Locate the OpenOCD script dir (OPENOCD_SCRIPTS env or default).
find_scripts() {
    if [[ -n "${OPENOCD_SCRIPTS:-}" && -d "$OPENOCD_SCRIPTS" ]]; then
        printf '%s' "$OPENOCD_SCRIPTS"
        return
    fi
    local bin scripts
    bin="$(find_openocd)" || return 1
    scripts="$(dirname "$(dirname "$bin")")/share/openocd/scripts"
    if [[ -d "$scripts" ]]; then
        printf '%s' "$scripts"
        return
    fi
    return 1
}

OPENOCD="$(find_openocd)" || {
    echo "error: openocd not found; set OPENOCD or run export.sh" >&2
    exit 1
}
SCRIPTS="$(find_scripts)" || {
    echo "error: OpenOCD scripts dir not found; set OPENOCD_SCRIPTS" >&2
    exit 1
}

CFG="board/${TARGET}-builtin.cfg"
if [[ ! -f "$SCRIPTS/$CFG" ]]; then
    echo "error: $CFG not found in $SCRIPTS" >&2
    echo "       (TARGET must be a part with a built-in USB-Serial/JTAG adapter)" >&2
    exit 2
fi

# The built-in USB-Serial/JTAG adapter enumerates as VID 0x303a PID 0x1001 on
# every part that supports it (see interface/esp_usb_jtag.cfg). Check that it
# is actually present AND accessible before starting OpenOCD: otherwise OpenOCD
# only fails later with a cryptic "libusb_open() failed with
# LIBUSB_ERROR_NO_DEVICE" / "could not find or open device" error and no hint
# about what to do.
#
# Presence alone is not enough: the device can be enumerated (visible in sysfs)
# while libusb still cannot open it, e.g. when the USB device nodes under
# /dev/bus/usb are not available (container/VM without USB passthrough) or the
# user lacks permission. OpenOCD then reports LIBUSB_ERROR_NO_DEVICE even
# though the board looks connected, so also verify the /dev/bus/usb node that
# libusb actually opens.
find_usb_dev() {
    local dev v p
    for dev in /sys/bus/usb/devices/*/; do
        [[ -f "$dev/idVendor" ]] || continue
        v="$(<"$dev/idVendor")"
        p="$(<"$dev/idProduct")"
        if [[ "$v" == "303a" && "$p" == "1001" ]]; then
            printf '%s' "$dev"
            return 0
        fi
    done
    return 1
}

usb_dev="$(find_usb_dev)" || {
    echo "error: ESP32 USB-Serial/JTAG adapter (VID:PID 303a:1001) not found" >&2
    echo "       connect the $TARGET board over USB, use a cable that carries data," >&2
    echo "       and make sure the chip is powered." >&2
    exit 3
}

# Map the sysfs entry to the device node libusb opens: /dev/bus/usb/<bus>/<dev>.
busnum="$(<"$usb_dev/busnum")"
devnum="$(<"$usb_dev/devnum")"
usb_node="/dev/bus/usb/$(printf '%03d' "$busnum")/$(printf '%03d' "$devnum")"
if [[ ! -e "$usb_node" ]]; then
    echo "error: $TARGET board detected (VID:PID 303a:1001) but its USB device node" >&2
    echo "       $usb_node is not available, so OpenOCD cannot open it" >&2
    echo "       (libusb reports LIBUSB_ERROR_NO_DEVICE)." >&2
    echo "       The USB bus is not accessible from this environment; run the test" >&2
    echo "       on the host or with USB passthrough enabled." >&2
    exit 3
elif [[ ! -r "$usb_node" || ! -w "$usb_node" ]]; then
    echo "error: no permission to access $usb_node; add your user to the" >&2
    echo "       plugdev group or install udev rules for the ESP32 USB-Serial/JTAG" >&2
    echo "       adapter (e.g. the Espressif udev rules)." >&2
    exit 3
fi

LOG="$(mktemp -t test_jtag_ocd_XXXXXX.log)"
"$OPENOCD" -s "$SCRIPTS" -f "$CFG" -c "telnet_port $TELNET_PORT" >"$LOG" 2>&1 &
OCD_PID=$!
# Keep the OpenOCD log around on failure so it can be inspected; only remove
# it when the whole test passed.
failed=1
cleanup() {
    exec 3>&- 2>/dev/null || true
    kill "$OCD_PID" 2>/dev/null || true
    wait "$OCD_PID" 2>/dev/null || true
    ((failed == 0)) && rm -f "$LOG"
}
trap cleanup EXIT

# Read from the OpenOCD telnet server until its prompt ("> ") is seen and
# return everything read. OpenOCD terminates the banner and every command
# response with '\r> ' and no trailing newline, so a line-oriented `read`
# would block forever waiting for a newline that never comes; consume the
# stream one byte at a time and stop as soon as the data ends with the prompt.
read_until_prompt() {
    local buf="" byte
    while IFS= read -r -t "$TELNET_TMO_S" -N 1 byte <&3; do
        buf+="$byte"
        [[ "$buf" == *$'\r> ' || "$buf" == *$'\n> ' ]] && break
    done
    printf '%s' "$buf"
}

# Send a command over the OpenOCD telnet server and read everything up to the
# next prompt (the server echoes the command and wraps long lines with '> ').
ocd_cmd() {
    local cmd="$1" out
    printf '%s\n' "$cmd" >&3
    out="$(read_until_prompt)"
    out="${out//$'\r'/}"
    printf '%s' "$out"
}

pass=0
fail=0
check() {
    local desc="$1" out="$2" pattern="$3"
    if [[ "${out,,}" == *"${pattern,,}"* ]]; then
        echo "PASS: $desc"
        pass=$((pass + 1))
    else
        echo "FAIL: $desc (missing \"$pattern\" in output)" >&2
        echo "    ${out//$'\n'/$'\n    '}" >&2
        fail=$((fail + 1))
    fi
}

# Wait for the telnet server; OpenOCD only opens it after the adapter and
# target are configured successfully.
echo "starting OpenOCD ($CFG) ..."
connected=0
deadline=$((SECONDS + TELNET_TMO_S))
while ((SECONDS < deadline)); do
    if ! kill -0 "$OCD_PID" 2>/dev/null; then
        echo "error: OpenOCD exited early; log: $LOG" >&2
        tail -20 "$LOG" >&2 || true
        echo "       if the log shows a libusb/USB error, check that the $TARGET" >&2
        echo "       board is connected and that USB device access is permitted" >&2
        exit 1
    fi
    if { exec 3<>"/dev/tcp/127.0.0.1/$TELNET_PORT"; } 2>/dev/null; then
        connected=1
        break
    fi
    sleep 0.2
done
if ((!connected)); then
    echo "error: timed out waiting for OpenOCD telnet server on port $TELNET_PORT; log: $LOG" >&2
    tail -20 "$LOG" >&2 || true
    exit 1
fi

echo "Connected"

# Drain the banner up to the first prompt.
read_until_prompt >/dev/null

echo "== OpenOCD responds =="
out="$(ocd_cmd version)"
check "version" "$out" "Open On-Chip Debugger"

echo "== JTAG chain =="
out="$(ocd_cmd scan_chain)"
# scan_chain lists the TAPs (e.g. esp32s3.tap0 / esp32s3.tap1 for the dual-core
# part, esp32c3.tap0 for single-core), not the target names; check that the
# chip's TAP was found.
check "scan_chain finds ${TARGET}.tap" "$out" "${TARGET}.tap"

echo "== Halt CPU over JTAG =="
ocd_cmd halt >/dev/null
out="$(ocd_cmd targets)"
check "targets shows halted" "$out" "halted"

echo "== Register write/read =="
case "$TARGET" in
esp32s3)
    REG=a3
    SP_REG=a1
    ;;
*)
    REG=t0
    SP_REG=sp
    ;;
esac
ocd_cmd "reg $REG 0x5aa55aa5" >/dev/null
out="$(ocd_cmd "reg $REG")"
check "reg $REG read-back" "$out" "0x5aa55aa5"

echo "== Memory write/read =="
sp_hex="$(ocd_cmd "reg $SP_REG" | grep -oE '0x[0-9a-fA-F]+' | head -1 || true)"
if [[ -n "$sp_hex" ]]; then
    addr_hex="$(printf '0x%x' "$(((sp_hex - 0x200) & ~0x3))")"
    ocd_cmd "mww $addr_hex 0xdeadbeef" >/dev/null
    out="$(ocd_cmd "mdw $addr_hex")"
    check "memory word read-back at $addr_hex" "$out" "deadbeef"
else
    echo "SKIP: memory write/read (could not read $SP_REG)" >&2
fi

echo "== Resume =="
ocd_cmd resume >/dev/null || true

echo
echo "JTAG test: $pass passed, $fail failed"
if ((fail == 0)); then
    failed=0
fi
((fail == 0))
