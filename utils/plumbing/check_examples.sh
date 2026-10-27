#!/usr/bin/env bash
# Run check_project.sh on every example project.
#
# An example project is any subdirectory of an `examples/` directory that has
# its own CMakeLists.txt (i.e. is a top-level ESP-IDF project).
#
# Usage:
#   TARGET=<esp32|esp32c3|...> check_examples.sh
#   TARGET=<esp32|esp32c3|...> TEST_COMPONENTS=<component> check_examples.sh
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. esp32, esp32c3)}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(realpath "$DIR/../..")"

TEST_COMPONENTS="${TEST_COMPONENTS:-}"

mapfile -t PROJECTS < <(
    if [ -n "$TEST_COMPONENTS" ]; then
        # Only the examples of the requested component(s) are checked.
        for COMPONENT in ${TEST_COMPONENTS//;/ }; do
            EXAMPLES_DIR="$ROOT/$COMPONENT/examples"
            [ -d "$EXAMPLES_DIR" ] || continue
            find "$EXAMPLES_DIR" -mindepth 1 -maxdepth 1 -type d -name '.*' -prune -o \
                -print
        done
    else
        find "$ROOT" -type d -name examples \
            -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/doctest/*' -not -path '*/esp_gcov/*' -print0 |
            while IFS= read -r -d '' EXAMPLES_DIR; do
                find "$EXAMPLES_DIR" -mindepth 1 -maxdepth 1 -type d -name '.*' -prune -o \
                    -print
            done
    fi |
        while IFS= read -r PROJECT; do
            if [ -f "$PROJECT/CMakeLists.txt" ]; then
                printf '%s\n' "$PROJECT"
            fi
        done |
        sort -u
)

if [ "${#PROJECTS[@]}" -eq 0 ]; then
    echo "No example projects found"
    exit 0
fi

echo "Found ${#PROJECTS[@]} example projects"
for PROJECT in "${PROJECTS[@]}"; do
    echo "==> Checking example: $PROJECT"
    # TEST_COMPONENTS only selects which example projects to check, while
    # SPLIT_CHECKS runs the check per repository component (for the test app);
    # neither must leak into check_project.sh, where they would ask the
    # example build to also compile the component's test suite (and its
    # test-only dependencies).
    TARGET="$TARGET" TEST_COMPONENTS="" SPLIT_CHECKS="" \
        "$DIR/../check_project.sh" "$PROJECT"
done

echo "All examples passed"
