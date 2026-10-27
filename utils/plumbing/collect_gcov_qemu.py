#!/usr/bin/env python3
# Collect .gcda coverage data streamed by the test app over the apptrace UART
# (UART1, the second -serial line) under QEMU. The app uses the QEMU-only
# fire-and-forget apptrace protocol (see gcov_rtio_uart.c); this side only has
# to receive and interpret the stream. UART0 stays on the first -serial line
# as the regular console, so the doctest output (including "[doctest] Status:
# SUCCESS!") remains available while the coverage data is collected.
#
# The matching .gcno notes files are copied from the build tree into the output
# directory, so the output can be handed to gcov/gcovr directly.
#
# Usage: collect_gcov_qemu.py <qemu-binary> <machine> <flash-image> <console-port> <cov-port> <out-dir>
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

# Host file I/O command IDs, matching esp_gcov/gcov_rtio.c.
CMD_FOPEN = 0x0
CMD_FCLOSE = 0x1
CMD_FWRITE = 0x2
CMD_FREAD = 0x3
CMD_FSEEK = 0x4
CMD_FTELL = 0x5
CMD_FSTOP = 0x6
CMD_FEOF = 0x7

# How long to wait for QEMU to come up / the dump to finish.
CONNECT_TMO_S = 30
RECV_TMO_S = 5
DUMP_TMO_S = 60


def main() -> None:
    if len(sys.argv) != 7:
        print(
            f"usage: {sys.argv[0]} <qemu-binary> <machine> <flash-image> "
            "<console-port> <cov-port> <out-dir>",
            file=sys.stderr,
        )
        sys.exit(2)

    qemu_bin, machine, image, console_port, cov_port, out_dir = (
        sys.argv[1],
        sys.argv[2],
        sys.argv[3],
        int(sys.argv[4]),
        int(sys.argv[5]),
        sys.argv[6],
    )
    os.makedirs(out_dir, exist_ok=True)

    # The .gcda path the device sends points into the build tree (e.g.
    # build/esp-idf/main/CMakeFiles/__idf_main.dir/test_runner.cpp.gcda).
    # The matching .gcno notes file is produced next to it during the build;
    # copy it into the output directory so that gcov/gcovr can later combine
    # the pair. The build directory is derived from the flash image path.
    build_dir = os.path.dirname(os.path.abspath(image))

    # UART0 (console, first -serial) uses nowait so it is always reachable and
    # its output can be checked while the tests run. UART1 (apptrace, second
    # -serial) uses wait=on: QEMU boots only after the collector connects, so
    # no early dump data is lost.
    cmd = [
        qemu_bin,
        "-nographic",
        "-monitor",
        "none",
        "-machine",
        machine,
        "-serial",
        f"tcp::{console_port},server=on,nowait",
        "-serial",
        f"tcp::{cov_port},server=on,wait=on",
        "-drive",
        f"file={image},if=mtd,format=raw",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + CONNECT_TMO_S
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", cov_port), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        sock.settimeout(RECV_TMO_S)

        # The console (UART0) stays on the first -serial line during coverage
        # runs; read it in parallel so the doctest status line is checked too.
        console = socket.create_connection(("127.0.0.1", console_port), timeout=2)
        console.settimeout(0.1)
        console_buf = b""
        doctest_ok = False

        buf = b""
        files: dict[str, object] = {}
        paths: list[str] = []
        current: str | None = None
        t0 = time.time()

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
                        # ROM boot output before the apptrace stream; resync.
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
                elif cmd_id == CMD_FCLOSE:
                    if len(buf) < 5:
                        return True
                    buf = buf[5:]
                elif cmd_id == CMD_FWRITE:
                    if len(buf) < 9:
                        return True
                    length = struct.unpack("<I", buf[5:9])[0]
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

        while time.time() - t0 < DUMP_TMO_S:
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
                if not parse():
                    break
            except TimeoutError:
                pass
            try:
                cchunk = console.recv(65536)
                if cchunk:
                    console_buf += cchunk
                    if b"[doctest] Status: SUCCESS!" in console_buf:
                        doctest_ok = True
            except TimeoutError:
                pass

        # The doctest summary is printed before esp_gcov_dump(), so it normally
        # arrives before the FSTOP marker, but allow a short grace period for
        # it to appear if it is still in flight.
        grace = time.time() + 2
        while not doctest_ok and time.time() < grace:
            try:
                cchunk = console.recv(65536)
                if not cchunk:
                    break
                console_buf += cchunk
                if not doctest_ok and b"[doctest] Status: SUCCESS!" in console_buf:
                    doctest_ok = True
                    print("tests successful", file=sys.stderr)
            except TimeoutError:
                pass

        console.close()
        for f in files.values():
            f.close()
        sock.close()

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
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
