#!/usr/bin/env python3
# Collect .gcda coverage data streamed by the test app over the single UART
# of a real device (no QEMU, no JTAG). The device has only one UART wired to
# the host, and the coverage build (sdkconfig.defaults.coverage.uart.device)
# disables the console so the gcov apptrace channel can take that same UART:
# the app runs the doctest suites and then streams the .gcda data using the
# same fire-and-forget apptrace command stream as collect_gcov_qemu.py (see
# gcov_rtio_uart.c), ending with the FSTOP command. Because the console is
# disabled, the status markers ("[doctest] Status: SUCCESS!" and "coverage
# dump ready") are sent by test_runner.cpp through the apptrace channel, so
# this collector checks them in the same byte stream as the dump.
#
# Unlike collect_gcov_qemu.py (QEMU, with a dedicated apptrace UART) there is no
# second UART to keep the two streams apart, so this collector opens the one
# serial port, checks the doctest status in the stream and parses the
# interleaved apptrace commands from the same bytes. Like collect_gcov_jtag.py
# it targets real hardware: the device is reset through the RTS/DTR auto-reset
# circuit (or manually, if the board does not wire it) so the run and the dump
# are deterministic.
#
# The matching .gcno notes files are copied from the build tree into the output
# directory, so the output can be handed to gcov/gcovr directly.
#
# Usage: collect_gcov_uart.py <port> <build-dir> <out-dir> [<baud>]
import os
import shutil
import struct
import sys
import time

import serial  # pyserial

# Host file I/O command IDs, matching esp_gcov/gcov_rtio.c.
CMD_FOPEN = 0x0
CMD_FCLOSE = 0x1
CMD_FWRITE = 0x2
CMD_FREAD = 0x3
CMD_FSEEK = 0x4
CMD_FTELL = 0x5
CMD_FSTOP = 0x6
CMD_FEOF = 0x7

# Status markers sent by the test app (test_runner.cpp). In the single-UART
# device build the console is disabled, so they are sent through the apptrace
# channel and arrive interleaved with the dump.
READY_MARKER = b"coverage dump ready"
SUCCESS_MARKER = b"[doctest] Status: SUCCESS!"

# How long to wait for the app to boot, run the doctest suites and reach the
# dump (READY_TMO_S), and for the dump itself to finish (DUMP_TMO_S).
READY_TMO_S = 120
DUMP_TMO_S = 60
# Serial read timeout.
READ_TMO_S = 0.1
# Upper bound for a single FWRITE payload; guards the parser against
# console bytes that happen to look like a plausible command header.
MAX_WRITE_LEN = 1 << 20


def reset_device(ser: serial.Serial) -> None:
    """Best-effort reset through the classic ESP auto-reset circuit (RTS wired
    to EN, DTR to GPIO0). Boards that do not wire RTS/DTR ignore this; the app
    can then be reset manually while the collector is waiting."""
    try:
        ser.reset_input_buffer()
        ser.dtr = False  # GPIO0 high: boot normally, not into the bootloader
        ser.rts = True  # EN low: hold the chip in reset
        time.sleep(0.1)
        ser.rts = False  # EN high: release, the chip boots
        time.sleep(0.1)
    except (OSError, ValueError):
        # DTR/RTS are not available (e.g. an adapter without the auto-reset
        # circuit); the device must be reset manually.
        pass


def main() -> None:
    if len(sys.argv) not in (4, 5):
        print(
            f"usage: {sys.argv[0]} <port> <build-dir> <out-dir> [<baud>]",
            file=sys.stderr,
        )
        sys.exit(2)

    port, build_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    # The dump shares the (disabled) console UART, so the baud rate is the
    # apptrace baud (CONFIG_APPTRACE_UART_BAUDRATE, passed by the caller).
    baud = int(sys.argv[4]) if len(sys.argv) == 5 else 115200
    os.makedirs(out_dir, exist_ok=True)

    files: dict[str, object] = {}
    try:
        ser = serial.Serial(port, baud, timeout=READ_TMO_S)
    except serial.SerialException as err:
        print(f"cannot open {port}: {err}", file=sys.stderr)
        sys.exit(1)

    try:
        # The dump is fire-and-forget and starts right after the doctest
        # suites finish, so the device must be reset while this collector is
        # already listening; a run that started earlier would already have
        # streamed (and lost) its dump.
        reset_device(ser)

        buf = b""
        console_buf = b""
        paths: list[str] = []
        current: str | None = None
        ready = False
        doctest_ok = False
        dump_done = False
        deadline = time.time() + READY_TMO_S + DUMP_TMO_S

        def parse() -> bool:
            nonlocal buf, current
            while True:
                if len(buf) < 1:
                    return True
                cmd_id = buf[0]
                if cmd_id == CMD_FOPEN:
                    end1 = buf.find(b"\x00", 1)
                    if end1 == -1:
                        return True
                    end2 = buf.find(b"\x00", end1 + 1)
                    if end2 == -1:
                        return True
                    path = buf[1:end1].decode(errors="replace")
                    buf = buf[end2 + 1 :]
                    if not path.startswith("/"):
                        # Console output before the apptrace stream; resync.
                        buf = buf[1:]
                        continue
                    out_path = os.path.join(out_dir, os.path.basename(path))
                    if out_path not in files:
                        files[out_path] = open(out_path, "wb")  # noqa: SIM115
                        paths.append(out_path)
                        # Copy the .gcno notes file emitted by the compiler so
                        # that gcov can pair it with the collected .gcda data.
                        gcno_path = path[: -len(".gcda")] + ".gcno"
                        gcno_src = os.path.join(
                            build_dir, os.path.relpath(gcno_path, build_dir)
                        )
                        if gcno_src.startswith(build_dir + os.sep) and os.path.isfile(
                            gcno_src
                        ):
                            shutil.copyfile(
                                gcno_src, out_path[: -len(".gcda")] + ".gcno"
                            )
                    current = out_path
                elif not paths:
                    # No .gcda file has been opened yet, so every command
                    # other than a valid FOPEN is console output preceding the
                    # dump (the apptrace UART transport adds no framing): drop
                    # one byte and resync instead of misparsing it.
                    buf = buf[1:]
                elif cmd_id == CMD_FCLOSE:
                    if len(buf) < 5:
                        return True
                    buf = buf[5:]
                elif cmd_id == CMD_FWRITE:
                    if len(buf) < 9:
                        return True
                    length = struct.unpack("<I", buf[5:9])[0]
                    if length > MAX_WRITE_LEN:
                        # Console bytes that look like a FWRITE header; resync.
                        buf = buf[1:]
                        continue
                    if len(buf) < 9 + length:
                        return True
                    if current:
                        f = files.get(current)
                        if f:
                            f.write(buf[9 : 9 + length])
                    buf = buf[9 + length :]
                elif cmd_id == CMD_FREAD:
                    if len(buf) < 9:
                        return True
                    buf = buf[9:]
                elif cmd_id == CMD_FSEEK:
                    if len(buf) < 13:
                        return True
                    buf = buf[13:]
                elif cmd_id == CMD_FTELL or cmd_id == CMD_FEOF:
                    if len(buf) < 5:
                        return True
                    buf = buf[5:]
                elif cmd_id == CMD_FSTOP:
                    buf = buf[1:]
                    return False
                else:
                    buf = buf[1:]

        while time.time() < deadline and not dump_done:
            chunk = ser.read(65536)
            if not chunk:
                continue
            buf += chunk
            console_buf += chunk
            if not ready and READY_MARKER in console_buf:
                ready = True
                print("app ready (coverage dump started)", file=sys.stderr)
            if not doctest_ok and SUCCESS_MARKER in console_buf:
                doctest_ok = True
                print("tests successful", file=sys.stderr)
            if not parse():
                dump_done = True
    finally:
        ser.close()
        for f in files.values():
            f.close()

    if not ready:
        print(
            f'warning: did not see "{READY_MARKER.decode()}" on {port}; '
            "if the device was already running, reset it and run again",
            file=sys.stderr,
        )
    if not dump_done and paths:
        print(
            "warning: dump did not finish (no FSTOP seen); "
            "collected .gcda data may be incomplete",
            file=sys.stderr,
        )
    if not paths:
        print("no .gcda files collected", file=sys.stderr)
        sys.exit(1)
    for p in paths:
        print(p)
    if not doctest_ok:
        print(console_buf.decode(errors="replace"))
        print(
            'console did not report "[doctest] Status: SUCCESS!"',
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
