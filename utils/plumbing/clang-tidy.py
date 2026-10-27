#!/usr/bin/env python3
"""Build clang-tidy command lines from a compile_commands.json database.

Usage:
  clang-tidy.py <directory>
"""

import argparse
import json
import os
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="directory containing compile_commands.json")
    args = parser.parse_args()

    build_dir = os.path.realpath(args.directory)
    db_path = os.path.join(build_dir, "compile_commands.json")
    with open(db_path, "r", encoding="utf-8") as f:
        entries = json.load(f)

    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        idf_path = os.path.realpath(idf_path)

    # Vendored third-party components are not this repository's code (they are
    # pinned snapshots of upstream projects) and clang-tidy reports many false
    # positives in them, so skip them like format.sh / plumbing/check_examples.sh do.
    vendored = ("/esp-dsp/", "/esp-mqtt/", "/doctest/", "/esp_gcov/")

    commands = []
    for entry in entries:
        if idf_path:
            file_path = os.path.realpath(entry["file"])
            if os.path.commonpath([file_path, idf_path]) == idf_path:
                continue
        if os.path.commonpath([file_path, build_dir]) == build_dir:
            continue
        if any(v in file_path for v in vendored):
            continue
        commands.append(
            ["clang-tidy", "-warnings-as-errors=*", entry["file"], "--"]
            + shlex.split(entry["command"])
        )

    error = False
    for command in commands:
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError:
            error = True
    if error:
        sys.exit(1)


if __name__ == "__main__":
    main()
