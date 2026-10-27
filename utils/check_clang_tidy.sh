#!/usr/bin/env bash
# Build utils/test_app/ with the clang toolchain and run
# utils/plumbing/clang-tidy.py over the resulting compile database.
#
# The build is not required to succeed: only the generated
# compile_commands.json is needed for clang-tidy, so a failed (e.g. link)
# step is fine. The temporary build directory is removed when the script
# exits, unless PRESERVE_TMPDIR is set.
#
# Set PRESERVE_TMPDIR=1 to keep the temporary build directory after the
# script exits so it can be reused by a later run.
#
# With SPLIT_CHECKS=1, the check is run for each testable component separately
# (via TEST_COMPONENTS if set, otherwise all testable components are
# discovered), like check_fast.sh does.
#
# Usage:
#   TARGET=<esp32|esp32c3|...> check_clang_tidy.sh
#   SPLIT_CHECKS=1 TARGET=<esp32|esp32c3|...> check_clang_tidy.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"

ROOT="$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)")"
TEST_APP="${TEST_APP:-$ROOT/utils/test_app}"
TEST_COMPONENTS="${TEST_COMPONENTS:-}"

# shellcheck disable=SC1091
source "$ROOT/utils/plumbing/components_discovery.sh"

TMPDIR="/tmp/check_clang_tidy_${TARGET}_$(printf '%s' "$TEST_APP" | md5sum | cut -d' ' -f1)"
if ! mkdir "$TMPDIR" 2>/dev/null; then
    # Already exists (e.g. from a previous run): only reuse it if it is a real
    # directory we own. Refuse symlinks or directories planted by other users.
    if [ ! -d "$TMPDIR" ] || [ -L "$TMPDIR" ] || [ ! -O "$TMPDIR" ]; then
        echo "error: refusing unsafe tmpdir: $TMPDIR" >&2
        exit 1
    fi
    chmod 700 "$TMPDIR"
    rm -rf -- "${TMPDIR:?}/"*
    if [ -z "${PRESERVE_TMPDIR:-}" ]; then
        trap 'rm -rf "$TMPDIR"' EXIT
    fi
fi

# The build is expected to fail (e.g. at link time), but that is fine: the
# compile_commands.json database is generated during configuration and is all
# clang-tidy.py needs.
run_tidy() {
    local test_components="$1"

    SDKCONFIG_DEFAULTS="${TEST_APP}/sdkconfig.defaults"
    if [ -f "${TEST_APP}/sdkconfig.defaults.clang" ]; then
        SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};${TEST_APP}/sdkconfig.defaults.clang"
    fi
    IDF_TOOLCHAIN=clang idf.py \
        --ccache \
        -C "${TEST_APP}" \
        -DIDF_TARGET="${TARGET}" \
        -DTEST_COMPONENTS="${test_components}" \
        -DSDKCONFIG="${TMPDIR}/config" \
        -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
        -B "${TMPDIR}/build" \
        build >/dev/null 2>/dev/null ||
        true

    "$ROOT/utils/plumbing/clang-tidy.py" "${TMPDIR}/build"
}

if [ -n "${SPLIT_CHECKS:-}" ]; then
    if [ -n "${TEST_COMPONENTS:-}" ]; then
        read -r -a COMPONENTS <<<"${TEST_COMPONENTS//;/ }"
    else
        mapfile -t COMPONENTS < <(discover_components "$ROOT")
    fi

    for COMPONENT in "${COMPONENTS[@]}"; do
        echo "==> clang-tidy: $COMPONENT"
        run_tidy "$COMPONENT"
    done
else
    run_tidy "${TEST_COMPONENTS}"
fi
