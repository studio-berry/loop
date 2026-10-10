#!/usr/bin/env python3
"""Report retirement of the pre-reset release-milestone synchronizer."""

from __future__ import annotations

import argparse


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="retired; writes are refused")
    args = parser.parse_args(argv)
    if args.apply:
        parser.error("milestone synchronization is retired; manage L01-L14, R00, G00 and X00 through reviewed issues")
    print("Milestone synchronization is retired. No GitHub changes are planned.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
