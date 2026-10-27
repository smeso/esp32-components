#!/usr/bin/env bash
# Build utils/test_app/ without flashing it.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> basic_build.sh
#   TARGET=<esp32|esp32c3|...> TEST_COMPONENTS=<component> basic_build.sh
#
# TEST_COMPONENTS limits the build to the given component(s) instead of all
# testable components.
#
# With SPLIT_CHECKS=1, the build is run for each testable component
# separately (via TEST_COMPONENTS), like check_fast.sh does.
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"
TEST_COMPONENTS="${TEST_COMPONENTS:-}"

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")"
PROJECT="${ROOT}/utils/test_app"

# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/components_discovery.sh"

TMPDIR="" # set by build()

build() {
    local components="${1:-}"
    local suffix=""
    if [ -n "$components" ]; then
        suffix="_$(printf '%s' "$components" | md5sum | cut -d' ' -f1)"
    fi
    TMPDIR="/tmp/basic_build_${TARGET}${suffix}_$(printf '%s' "$PROJECT" | md5sum | cut -d' ' -f1)"
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

    idf.py \
        --ccache \
        -C "${PROJECT}" \
        -DIDF_TARGET="${TARGET}" \
        -DTEST_COMPONENTS="${components}" \
        -DSDKCONFIG="${TMPDIR}/config" \
        -B "${TMPDIR}/build" \
        build
}

if [ -n "${SPLIT_CHECKS:-}" ]; then
    if [ -n "$TEST_COMPONENTS" ]; then
        # Build only the requested component(s) separately.
        read -r -a COMPONENTS <<<"${TEST_COMPONENTS//;/ }"
    else
        mapfile -t COMPONENTS < <(discover_components "$ROOT")
    fi

    for COMPONENT in "${COMPONENTS[@]}"; do
        echo "==> build: $COMPONENT"
        build "$COMPONENT"
    done
else
    # TEST_COMPONENTS is empty when unset, so this keeps the default of
    # building every testable component in a single build.
    build "$TEST_COMPONENTS"
fi

echo "build passed"
