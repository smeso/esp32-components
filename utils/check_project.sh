#!/usr/bin/env bash
# Check an ESP-IDF project: run the static checks (clang-tidy, cmake-lint,
# ruff, shellcheck, format) and then build it. For targets supported by QEMU
# (esp32, esp32s3, esp32c3) also run the firmware under QEMU, verifying that
# it prints to standard output; other targets are only compiled.
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> check_project.sh <path-to-esp-idf-project>
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"

PROJECT="${1:?usage: $0 <path-to-esp-idf-project>}"
PROJECT="$(realpath "$PROJECT")"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$TARGET" in
esp32 | esp32s3)
    QEMU_BIN=qemu-system-xtensa
    ;;
esp32c3)
    QEMU_BIN=qemu-system-riscv32
    ;;
*)
    # Not supported by QEMU: compile only.
    QEMU_BIN=""
    ;;
esac

echo "==> clang-tidy"
TEST_APP="$PROJECT" TARGET="$TARGET" "$DIR/check_clang_tidy.sh"

echo "==> cmake-lint"
ROOT="$PROJECT" "$DIR/plumbing/check_cmake.sh"

echo "==> ruff"
ROOT="$PROJECT" "$DIR/plumbing/check_python.sh"

echo "==> shellcheck"
ROOT="$PROJECT" "$DIR/plumbing/check_shell.sh"

echo "==> format check"
ROOT="$PROJECT" "$DIR/format.sh" --check

TMPDIR="/tmp/check_project_${TARGET}_$(printf '%s' "$PROJECT" | md5sum | cut -d' ' -f1)"
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

echo "==> build"
# Pad the QEMU flash image to the project's configured flash size so the
# firmware's image header matches what QEMU exposes (a smaller image makes
# the flash probe fail). Default to 2MB when the size is not configured.
PAD_SIZE="$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE_\([0-9]*\)MB=.*/\1MB/p' \
    "$PROJECT/sdkconfig.defaults" | head -1)"
PAD_SIZE="${PAD_SIZE:-2MB}"
idf.py \
    --ccache \
    -C "$PROJECT" \
    -DIDF_TARGET="$TARGET" \
    -DSDKCONFIG="${TMPDIR}/config" \
    -B "${TMPDIR}/build" \
    build \
    merge-bin \
    --pad-to-size="$PAD_SIZE" \
    -o "${TMPDIR}/build/qemu_flash.bin"

if [ -n "$QEMU_BIN" ]; then
    echo "==> QEMU run"
    pytest "$DIR/pytest/pytest_generic_app.py" \
        --build-dir="${TMPDIR}/build" \
        --target="$TARGET" \
        --embedded-services qemu \
        --qemu-image-path "${TMPDIR}/build/qemu_flash.bin" \
        --qemu-prog-path "$QEMU_BIN" \
        --qemu-extra-args "-M $TARGET"
fi

echo "Project check passed"
