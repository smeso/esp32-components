#!/usr/bin/env bash
# Format C/C++ sources with clang-format, Python scripts with ruff,
# and shell scripts with shfmt.
#
# Usage:
#   format.sh          format all supported files in place
#   format.sh --check  only verify that the files are formatted (exit 1 if not)
set -euo pipefail

usage() {
    sed -n '2,7p' "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

# Pick the highest-versioned clang-format on PATH (clang-format-<N> first,
# then plain clang-format), comparing actual versions rather than suffixes.
find_clang_format() {
    local cmd ver best="" best_ver="0"
    while IFS= read -r cmd; do
        ver=$("$cmd" --version 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)*' | head -1 || true)
        if [[ -n "$ver" ]] && [[ "$(printf '%s\n%s\n' "$best_ver" "$ver" | sort -V | tail -1)" == "$ver" ]]; then
            best="$cmd"
            best_ver="$ver"
        fi
    done < <(compgen -c clang-format | grep -E '^clang-format(-[0-9]+)?$' | sort -u)
    printf '%s' "$best"
}

ROOT="${ROOT:-$(realpath "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)")}"

for tool in ruff shfmt; do
    if ! command -v "$tool" >/dev/null; then
        echo "error: $tool not found on PATH" >&2
        exit 1
    fi
done

CLANG_FORMAT="$(find_clang_format)"
if [[ -z "$CLANG_FORMAT" ]]; then
    echo "error: no clang-format found on PATH" >&2
    exit 1
fi

mapfile -d '' c_files < <(find "$ROOT" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -not -path '*/build/*' -not -path '*/doctest/*' -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/esp_gcov/*' -print0 | sort -z)
mapfile -d '' py_files < <(find "$ROOT" -type f -name '*.py' \
    -not -path '*/build/*' -not -path '*/doctest/*' -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/esp_gcov/*' -print0 | sort -z)
mapfile -d '' sh_files < <(find "$ROOT" -type f -name '*.sh' \
    -not -path '*/build/*' -not -path '*/doctest/*' -not -path '*/esp-dsp/*' -not -path '*/esp-mqtt/*' -not -path '*/esp_gcov/*' -print0 | sort -z)

check() {
    local f dirty=0
    for f in "${c_files[@]}"; do
        if ! "$CLANG_FORMAT" "$f" | cmp -s - "$f"; then
            echo "not formatted: ${f#"$ROOT"/}" >&2
            dirty=$((dirty + 1))
        fi
    done
    if ((${#py_files[@]})) && ! ruff format --check "${py_files[@]}"; then
        dirty=$((dirty + 1))
    fi
    for f in "${sh_files[@]}"; do
        if ! shfmt "$f" | cmp -s - "$f"; then
            echo "not formatted: ${f#"$ROOT"/}" >&2
            dirty=$((dirty + 1))
        fi
    done
    if ((dirty)); then
        echo "error: $dirty file(s) not formatted (run ${BASH_SOURCE[0]##*/})" >&2
        exit 1
    fi
    echo "all $((${#c_files[@]} + ${#py_files[@]} + ${#sh_files[@]})) file(s) are formatted"
}

case "${1:-}" in
-c | --check) check ;;
-h | --help) usage 0 ;;
"")
    echo "formatting $((${#c_files[@]})) C/C++ file(s) with $CLANG_FORMAT"
    "$CLANG_FORMAT" -i "${c_files[@]}"
    if ((${#py_files[@]})); then
        echo "formatting ${#py_files[@]} Python file(s) with ruff"
        ruff format "${py_files[@]}"
    fi
    echo "formatting $((${#sh_files[@]})) shell script(s) with shfmt"
    shfmt -w "${sh_files[@]}"
    ;;
*) usage 1 ;;
esac
