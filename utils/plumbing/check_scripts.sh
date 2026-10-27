#!/usr/bin/env bash
# Run all script checks: shellcheck, ruff and cmake-lint over all the files
#
# Usage:
#   check_scripts.sh
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$DIR/check_shell.sh"
"$DIR/check_python.sh"
"$DIR/check_cmake.sh"
