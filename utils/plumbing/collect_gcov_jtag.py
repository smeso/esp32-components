#!/usr/bin/env python3
# Collect .gcda coverage data from a real device over its built-in
# USB-Serial/JTAG controller using OpenOCD's `esp gcov dump` command.
#
# The on-device coverage build for targets with a built-in USB-Serial/JTAG
# controller (ESP32-C3/C6/H2, ESP32-S3, ...) uses the JTAG apptrace
# destination (CONFIG_APPTRACE_DEST_JTAG) instead of UART: on those boards the
# main UART is usually not wired to the host, while the USB-Serial/JTAG
# controller is the one connection that is always present. The app runs the
# doctest suites, prints a "coverage dump ready" marker over the secondary
# USB-Serial/JTAG console, calls esp_gcov_dump() and then blocks waiting for
# the host to connect (hard-coded dump mode). This collector starts OpenOCD on
# the built-in adapter, reboots the app over JTAG (`reset run`), waits for the
# marker again, then issues `esp gcov dump`; OpenOCD answers the app's
# synchronous file I/O commands over JTAG, writing the .gcda files on the host.
#
# This is the same flow that ESP-IDF's own gcov example CI uses
# (examples/system/gcov/pytest_gcov.py, the `esp gcov dump` hard-coded dump).
#
# Usage: collect_gcov_jtag.py <target> <build-dir> <out-dir> [<port>]
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

# OpenOCD telnet server port.
TELNET_PORT = 4444

# How long to wait for OpenOCD to come up / the dump to finish.
OCD_START_TMO_S = 30
DUMP_TMO_S = 60
# How long to wait for the app to reach esp_gcov_dump().
READY_TMO_S = 60

# Marker printed by test_runner.cpp just before esp_gcov_dump() blocks.
READY_MARKER = b"coverage dump ready"
# Doctest summary line printed by the test app on the console.
SUCCESS_MARKER = b"[doctest] Status: SUCCESS!"


def find_openocd() -> str:
    """Locate the OpenOCD binary (OPENOCD env / PATH / IDF tools dir)."""
    exe = os.getenv("OPENOCD")
    if exe and os.path.isfile(exe):
        return exe
    which = shutil.which("openocd")
    if which:
        return which
    tools = os.path.expanduser(
        os.path.join(
            os.getenv("IDF_TOOLS_PATH", "~/.espressif"), "tools", "openocd-esp32"
        )
    )
    if os.path.isdir(tools):
        for ver in sorted(os.listdir(tools), reverse=True):
            cand = os.path.join(tools, ver, "openocd-esp32", "bin", "openocd")
            if os.path.isfile(cand):
                return cand
    raise RuntimeError("openocd not found; set OPENOCD or run export.sh")


def find_scripts() -> str:
    """Locate the OpenOCD script dir (OPENOCD_SCRIPTS env or default)."""
    scripts = os.getenv("OPENOCD_SCRIPTS")
    if scripts and os.path.isdir(scripts):
        return scripts
    bin_path = find_openocd()
    cand = os.path.join(
        os.path.dirname(os.path.dirname(bin_path)), "share", "openocd", "scripts"
    )
    if os.path.isdir(cand):
        return cand
    raise RuntimeError("OpenOCD scripts dir not found; set OPENOCD_SCRIPTS")


class Telnet:
    """Minimal telnet client (OpenOCD's telnet server echoes, so read until
    the prompt '> ' after a command)."""

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def read_until(self, marker: bytes, timeout: float) -> bytes:
        self.sock.settimeout(timeout)
        data = b""
        while not data.endswith(marker):
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)

    def close(self) -> None:
        self.sock.close()


def wait_ready(port: str) -> bool:
    """Best-effort wait for the app's 'coverage dump ready' marker on the
    USB-Serial/JTAG serial port. The secondary console TX is non-blocking and
    drops bytes when the host is not reading, so missing the marker is
    possible; the dump retry loop in main() is the real safety net, this is
    only an optimization to reduce failed dump attempts. Returns True if the
    '[doctest] Status: SUCCESS!' marker was seen."""
    import serial  # pyserial

    deadline = time.time() + READY_TMO_S
    buf = b""
    doctest_ok = False
    while time.time() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.2) as ser:
                while time.time() < deadline:
                    chunk = ser.read(4096)
                    if chunk:
                        buf += chunk
                        if not doctest_ok and SUCCESS_MARKER in buf:
                            doctest_ok = True
                            print("tests successful", file=sys.stderr)
                        if READY_MARKER in buf:
                            print(f"app ready (marker seen on {port})", file=sys.stderr)
                            return doctest_ok
        except (OSError, serial.SerialException) as err:
            # Port may still be claimed by the flasher or re-enumerating.
            print(f"waiting for {port}: {err}", file=sys.stderr)
            time.sleep(0.5)
    print(
        f'warning: did not see "{READY_MARKER.decode()}" on {port}; '
        "relying on dump retries",
        file=sys.stderr,
    )
    if not doctest_ok:
        print(buf.decode(errors="replace"))
    return doctest_ok


def main() -> None:
    if len(sys.argv) not in (4, 5):
        print(
            f"usage: {sys.argv[0]} <target> <build-dir> <out-dir> [<port>]",
            file=sys.stderr,
        )
        sys.exit(2)

    target, build_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    port = sys.argv[4] if len(sys.argv) == 5 else None
    os.makedirs(out_dir, exist_ok=True)

    # Start OpenOCD first; the app is reset over JTAG (`reset run`) below, so
    # no readiness wait is needed before attaching.
    cfg = f"board/{target}-builtin.cfg"
    openocd = find_openocd()
    scripts = find_scripts()
    cmd = [openocd, "-s", scripts, "-f", cfg]
    print(f"starting: {' '.join(cmd)}", file=sys.stderr)
    ocd_log = tempfile.NamedTemporaryFile(  # noqa: SIM115
        prefix="collect_gcov_jtag_ocd_", suffix=".log", delete=False, mode="w"
    )
    ocd_log.close()
    log_out = open(ocd_log.name, "w")  # noqa: SIM115
    proc = subprocess.Popen(cmd, stdout=log_out, stderr=subprocess.STDOUT)

    try:
        # Wait for the telnet server (OpenOCD only opens it after the adapter
        # and target are configured successfully).
        deadline = time.time() + OCD_START_TMO_S
        tn = None
        last_err = None
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(
                    f"OpenOCD exited early (code {proc.returncode}); "
                    "is the USB-Serial/JTAG device connected? "
                    f"log: {ocd_log.name}"
                )
            try:
                tn = Telnet("127.0.0.1", TELNET_PORT, timeout=2)
                break
            except OSError as err:
                last_err = err
                time.sleep(0.2)
        if tn is None:
            raise RuntimeError(
                f"timed out waiting for OpenOCD telnet server ({last_err}); "
                f"log: {ocd_log.name}"
            )
        # Drain the connection banner (it ends with the OpenOCD prompt).
        tn.read_until(b"> ", timeout=10)

        # OpenOCD does not create the parent directories of the .gcda paths it
        # receives; `esp gcov dump` would fail with "Failed to open file" (the
        # ESP-IDF gcov example pre-creates them for the same reason). Recreate
        # the directory structure that the build tree uses below out_dir.
        for root, _dirs, _files in os.walk(build_dir):
            rel = os.path.relpath(root, build_dir)
            if rel.startswith("esp-idf"):
                os.makedirs(os.path.join(out_dir, rel), exist_ok=True)

        # The app was flashed and is running already, but to make the dump
        # deterministic (and independent of the flasher's reset behavior) reset
        # it over JTAG: after `reset run` the app boots, runs the doctest
        # suites, prints the readiness marker and blocks in esp_gcov_dump().
        # This mirrors the ESP-IDF gcov example flow.
        ready_thread = None
        doctest_ok = False
        if port:
            # The secondary USB-Serial/JTAG console drops bytes while the host
            # is not reading, so start reading *before* the reset, otherwise the
            # marker printed right after boot could be lost.
            ready_result: dict[str, bool] = {}

            def wait_ready_thread() -> None:
                ready_result["doctest_ok"] = wait_ready(port)

            ready_thread = threading.Thread(target=wait_ready_thread, daemon=True)
            ready_thread.start()
        tn.write(b"reset run\n")
        if ready_thread is not None:
            ready_thread.join(timeout=READY_TMO_S + 5)
            doctest_ok = ready_result.get("doctest_ok", False)
        else:
            # Without a serial port give the app enough time to boot and
            # finish the doctest suites; the dump retry loop below covers the
            # rest.
            time.sleep(8)
        tn.read_until(b"> ", timeout=10)

        # The .gcda path the device sends points into the build tree (e.g.
        # <build>/esp-idf/main/CMakeFiles/__idf_main.dir/test_runner.cpp.gcda).
        # Strip the build-dir components and re-root the remaining path (which
        # starts with esp-idf/...) under out_dir, so the dump lands in a clean
        # directory next to the .gcno notes files. See esp_gcov_filename_alloc()
        # in OpenOCD for the prefix/strip semantics: each strip step removes one
        # directory component after the leading '/'.
        strip = len(os.path.abspath(build_dir).split(os.sep)) - 1
        paths: list[str] = []
        resp = ""
        # `esp gcov dump` stops after ~3s without dump data, so if the app has
        # not reached esp_gcov_dump() yet (e.g. the readiness marker was missed
        # or the suites are still running) the first attempt yields nothing.
        # Retry a few times: once the app is blocked in esp_gcov_dump() it
        # stays there until a dump succeeds, so re-issuing the command is safe.
        for attempt in range(1, 6):
            tn.write(f"esp gcov dump {out_dir} {strip}\n".encode())
            # Read until the OpenOCD prompt returns, which means the dump
            # finished (the app sent FSTOP, OpenOCD disconnected and cleaned
            # up) or its idle timeout fired.
            try:
                resp = tn.read_until(b"> ", timeout=DUMP_TMO_S).decode(errors="replace")
            except TimeoutError:
                raise RuntimeError("timed out waiting for `esp gcov dump` to finish")
            # A successful dump always writes at least one .gcda (the test
            # runner itself is compiled with --coverage); check the filesystem
            # rather than parsing the response, which varies between OpenOCD
            # versions.
            paths = []
            for root, _dirs, files in os.walk(out_dir):
                for name in files:
                    if name.endswith(".gcda"):
                        paths.append(os.path.join(root, name))
            paths.sort()
            if paths:
                break
            print(f"attempt {attempt}: no .gcda yet, retrying", file=sys.stderr)
            time.sleep(2)
        tn.close()
        print(resp, file=sys.stderr)

        if not paths:
            raise RuntimeError("no .gcda files collected")
        for p in paths:
            print(p)
        if port and not doctest_ok:
            print(
                'console did not report "[doctest] Status: SUCCESS!"',
                file=sys.stderr,
            )
            sys.exit(1)
        # Copy the matching .gcno notes files from the build tree next to each
        # .gcda so that gcov/gcovr can pair them (same layout as the QEMU and
        # UART collectors).
        for gcda in paths:
            rel = os.path.relpath(gcda, out_dir)
            gcno_src = os.path.join(build_dir, rel[: -len(".gcda")] + ".gcno")
            if os.path.isfile(gcno_src):
                gcno_dst = gcda[: -len(".gcda")] + ".gcno"
                os.makedirs(os.path.dirname(gcno_dst), exist_ok=True)
                shutil.copyfile(gcno_src, gcno_dst)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
