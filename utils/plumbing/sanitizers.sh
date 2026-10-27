#!/usr/bin/env bash
# Shared SANITIZERS handling for the plumbing test runner scripts.
#
# SANITIZERS selects an optional sanitizer build of utils/test_app:
#   1 - undefined behavior sanitizer (UBSan) in a Debug build
#   2 - UBSan in a regular build
#   3 - thread sanitizer (TSan) in a Debug build (linux target only)
#   4 - TSan in a regular build (linux target only)
#
# The linux builds for 1 and 2 also enable the address and leak sanitizers
# (ASan and LSan); the chip targets cannot run those (no compiler-rt runtime)
# and lower every UBSan check to a trap instruction instead, which aborts the
# chip on a violation (see test_app/CMakeLists.txt).
#
# The runtime options exported here turn on every available check and make a
# violation kill the process immediately instead of just printing a report.
#
# Usage:
#   source sanitizers.sh
#   sanitizers_setup linux|chip
set -euo pipefail

# sanitizers_setup <linux|chip>
#   Validate SANITIZERS (1-4) against the platform and export the runtime
#   options of the enabled sanitizers. Sets SANITIZER_DEBUG to 1 for the
#   Debug variants (1 and 3), 0 otherwise.
# shellcheck disable=SC2034 # SANITIZER_DEBUG is consumed by the callers
sanitizers_setup() {
    local platform="$1"
    SANITIZERS="${SANITIZERS:-}"
    SANITIZER_DEBUG=0
    case "$SANITIZERS" in
    "")
        return 0
        ;;
    1 | 2)
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:abort_on_error=1"
        ;;
    3 | 4)
        if [ "$platform" != linux ]; then
            echo "error: SANITIZERS=$SANITIZERS (thread sanitizer) is only supported on the linux target" >&2
            exit 1
        fi
        # The FreeRTOS linux port emulates the tick interrupt with SIGALRM, so
        # everything the tests run from a simulated ISR executes inside a POSIX
        # signal handler. TSan's async-signal-safety check then reports calls
        # that would be illegal in a real signal handler but are fine in the
        # simulator's interrupt context, e.g. the leds_effects ISR test posts a
        # command from the tick hook and that post allocates the command on the
        # heap (like every other post does, to keep the stack small). Disable
        # the check: it only reflects the signal-based ISR emulation, not a bug
        # the test suite is meant to catch.
        export TSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:report_signal_unsafe=0"
        ;;
    *)
        echo "error: SANITIZERS must be 1, 2, 3 or 4, got '$SANITIZERS'" >&2
        exit 1
        ;;
    esac
    case "$SANITIZERS" in
    1 | 3) SANITIZER_DEBUG=1 ;;
    esac
    if [ "$platform" = linux ] && { [ "$SANITIZERS" = 1 ] || [ "$SANITIZERS" = 2 ]; }; then
        export ASAN_OPTIONS="abort_on_error=1:detect_leaks=1:halt_on_error=1:print_stacktrace=1"
    fi
}
