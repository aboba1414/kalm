#!/usr/bin/env python3
"""Entry point for Kalm (Qt + KDE Kirigami GUI & CLI)."""

from __future__ import annotations

import sys

from kalm.cli import run_cli


def main() -> int:
    return run_cli(sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
