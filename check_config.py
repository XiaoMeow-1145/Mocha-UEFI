#!/usr/bin/env python3
"""Fail the build if Kconfig drops a requested firmware option."""

import argparse
from pathlib import Path


def read_config(path):
    settings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_"):
            name, value = line.split("=", 1)
            settings[name] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            settings[line[2 : -len(" is not set")]] = "n"
    return settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("fragments", type=Path, nargs="+")
    args = parser.parse_args()

    expected = {"CONFIG_ARM": "y", "CONFIG_ARM64": "n", "CONFIG_CMD_BOOTEFI_BINARY": "y"}
    for fragment in args.fragments:
        expected.update(read_config(fragment))
    actual = read_config(args.config)
    mismatches = [f"{name}: wanted {value}, got {actual.get(name, 'n')}"
                  for name, value in expected.items() if actual.get(name, "n") != value]
    if mismatches:
        lines = ["Effective U-Boot configuration mismatch:", *mismatches, ""]
        parser.exit(1, chr(10).join(lines))


if __name__ == "__main__":
    main()
