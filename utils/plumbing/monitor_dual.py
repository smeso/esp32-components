#!/usr/bin/env python3
# Watch the two serial consoles of the two-board rig until each board reports
# its doctest result. Both ports are opened and reset so the two firmwares
# start together, then their output is streamed to stdout, every line tagged
# with the board it came from, and the run succeeds only when both boards
# report "[doctest] Status: SUCCESS!".
#
# A board that reports a failure already decides the run: the other one is then
# reported as ABORTED and is not watched until the timeout any more (the two
# firmwares stay in lockstep, so the peer of a broken board can stay silent for
# minutes) and the monitor exits as soon as the outcome is known.
#
# The reset pulses the reset line (RTS) with the boot pin deasserted, the same
# hard reset esp-idf-monitor performs on its reset key: the boards restart and
# boot their firmware instead of the ROM downloader. USB-Serial/JTAG boards
# (the built-in adapter of the rig boards, /dev/ttyACM*) are given the longer
# release delay esptool uses for them. Boards without the auto-reset circuit
# must be reset by hand while the monitor is waiting.
#
# Usage: monitor_dual.py <port-a> <port-b> <baud> <timeout-s>
import os
import re
import sys
import threading
import time
from typing import TextIO

import serial  # pyserial

# A suite result is the "[doctest] Status:" line doctest prints at the end of
# the run: "[doctest] Status: SUCCESS!" when it passed, "[doctest] Status:
# FAILURE!" when it did not (see doctest.h).
#
# The rig runs doctest with no-colors, but a build without that option colors
# the line: doctest wraps each field of it separately, so escapes end up both
# between "[doctest]" and "Status:" and before the result ("\x1b[0;36m[doctest]
# \x1b[0mStatus: \x1b[0;32mSUCCESS!\x1b[0m"). Skipping them only before the
# result would miss the line altogether on such a build, and the monitor would
# sit out its timeout instead of reporting the result.
COLOR_ESCAPE = rb"\x1b\[[0-9;]*m"
STATUS_LINE = re.compile(
    rb"\[doctest\](?: |" + COLOR_ESCAPE + rb")*Status:"
    rb"(?: |" + COLOR_ESCAPE + rb")*(SUCCESS|FAILURE)!"
)
STATUS_BY_RESULT = {
    b"SUCCESS": "success",
    b"FAILURE": "failed",
}

# \r\n is kept together so a line break is either of the three sequences; a
# bare \r ends a line too (progress lines on the console end with one).
LINE_BREAK = re.compile(r"(\r\n|\r|\n)")


class ConsoleWriter:
    """Interleave both boards' output on one stream, tagging every line with
    the board it came from.

    Reads return arbitrary chunks, so a chunk usually starts or ends in the
    middle of a line: the tag is only written where a line actually starts,
    never in the middle of one, and an unfinished line is terminated before
    the other board takes it over so the two boards never share a line.

    The stream is flushed after every chunk so the boards' output shows up
    while the monitor is waiting.
    """

    def __init__(self, stream: TextIO) -> None:
        self._stream = stream
        self._lock = threading.Lock()
        self._at_line_start = True
        self._line_owner: str | None = None

    def write(self, name: str, chunk: bytes) -> None:
        """Show one chunk read from board ``name``."""
        text = chunk.decode(errors="replace")
        if not text:
            return
        with self._lock:
            self._stream.write("".join(self._render(name, text)))
            self._stream.flush()

    def _tag(self, name: str) -> str:
        self._line_owner = name
        return f"[{name}] "

    def _render(self, name: str, text: str) -> list[str]:
        out: list[str] = []
        for index, piece in enumerate(LINE_BREAK.split(text)):
            if index % 2:
                # An odd piece is a line break: it ends the current line.
                out.append(piece)
                self._at_line_start = True
                self._line_owner = None
            elif piece:
                if self._at_line_start:
                    out.append(self._tag(name))
                elif self._line_owner != name:
                    out.append("\n")
                    out.append(self._tag(name))
                out.append(piece)
                self._at_line_start = False
        return out


def doctest_status(seen: bytes) -> str | None:
    """Return the result the console buffer reports, or None while the board
    has not printed its doctest status line yet."""
    match = STATUS_LINE.search(seen)
    return STATUS_BY_RESULT[match.group(1)] if match else None


def open_port(
    port: str, baud: int, deadline: float, stop: threading.Event
) -> serial.Serial | None:
    """Open the port, retrying while the board re-enumerates after a reset.
    Returns None when the monitor was stopped before the port came back: the
    run is over, so there is nothing left to open it for."""
    while not stop.is_set():
        try:
            return serial.Serial(port, baud, timeout=0.2)
        except serial.SerialException:
            if time.time() >= deadline:
                raise
            stop.wait(0.2)
    return None


def close_port(ser: serial.Serial | None) -> None:
    """Close the port, ignoring one that is already gone."""
    if ser is None:
        return
    try:
        ser.close()
    except OSError:
        pass


def reset_device(ser: serial.Serial, uses_usb: bool) -> None:
    """Best-effort hard reset through the auto-reset circuit so both firmwares
    start together once both consoles are being watched.

    This is the reset esp-idf-monitor performs on its reset key (and the one
    collect_gcov_uart.py uses): the boot pin is left deasserted and the reset
    line (RTS) is pulsed, so the chip restarts and boots the application.

    esptool's "download" sequences (which assert the boot pin to pull GPIO0
    low while releasing the reset) are deliberately not used here: they make
    the ROM enter download mode, where the chip waits for a download instead
    of running the firmware until it is reset (or powered) by hand.

    Boards without the auto-reset circuit must be reset by hand while the
    monitor is waiting."""
    try:
        ser.reset_input_buffer()
        ser.dtr = False  # boot pin high: run the application, not the bootloader
        ser.rts = True  # reset line low: hold the chip in reset
        time.sleep(0.2 if uses_usb else 0.1)
        ser.rts = False  # reset line high: release, the chip boots the application
        time.sleep(0.2 if uses_usb else 0.1)
    except (OSError, ValueError):
        pass


def watch(
    name: str,
    port: str,
    baud: int,
    deadline: float,
    stop: threading.Event,
    lock: threading.Lock,
    results: dict[str, str],
    writer: ConsoleWriter,
) -> None:
    """Watch one board's console until it reports its doctest result.

    Watching ends as soon as the board reports its result, when the deadline
    passes, or when ``stop`` is set: a board that fails decides the run, so the
    peer (which may stay silent for a long time) is given up on and reported as
    ABORTED instead of being waited for until the timeout."""
    status = "timeout"
    uses_usb = "ttyacm" in os.path.basename(port).lower()
    ser = None
    try:
        ser = open_port(port, baud, deadline, stop)
        if ser is None:
            status = "aborted"
            return
        reset_device(ser, uses_usb)
        seen = b""
        while True:
            if time.time() >= deadline:
                break
            if stop.is_set():
                status = "aborted"
                break
            try:
                chunk = ser.read(4096)
            except OSError:
                # The USB-Serial/JTAG port re-enumerates across a reset:
                # reopen it and keep watching.
                close_port(ser)
                ser = open_port(port, baud, deadline, stop)
                if ser is None:
                    status = "aborted"
                    break
                continue
            if not chunk:
                continue
            writer.write(name, chunk)
            seen += chunk
            reported = doctest_status(seen)
            if reported is None:
                continue
            status = reported
            if reported == "failed":
                # The run is lost, the peer's result cannot change that.
                stop.set()
            break
    except OSError as err:
        with lock:
            print(f"[{name}] cannot open {port}: {err}", file=sys.stderr)
        status = "error"
    finally:
        close_port(ser)
        with lock:
            results[name] = status


def main() -> int:
    if len(sys.argv) != 5:
        print(
            "usage: monitor_dual.py <port-a> <port-b> <baud> <timeout-s>",
            file=sys.stderr,
        )
        return 2

    port_a, port_b = sys.argv[1], sys.argv[2]
    baud, timeout = int(sys.argv[3]), float(sys.argv[4])
    deadline = time.time() + timeout

    lock = threading.Lock()
    stop = threading.Event()
    results: dict[str, str] = {}
    writer = ConsoleWriter(sys.stdout)
    threads = []
    for name, port in (("A", port_a), ("B", port_b)):
        thread = threading.Thread(
            target=watch,
            args=(name, port, baud, deadline, stop, lock, results, writer),
        )
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    print()
    print("== two-board rig results ==")
    ok = True
    for name, port in (("A", port_a), ("B", port_b)):
        status = results.get(name, "error")
        print(f"{name} ({port}): {status.upper()}")
        if status != "success":
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
