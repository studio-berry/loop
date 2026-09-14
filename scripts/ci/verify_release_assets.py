#!/usr/bin/env python3
"""Check the complete release asset set against paired package inspection evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[A-Za-z0-9.-]+)?")


def verify(artifacts: Path, pair_path: Path, version: str, source_sha: str) -> None:
    if not VERSION.fullmatch(version) or not re.fullmatch(r"[0-9a-fA-F]{40}", source_sha):
        raise ValueError("expected a release version and full source SHA")
    pair = json.loads(pair_path.read_text(encoding="utf-8"))
    if (
        pair.get("schema_version") != 1
        or pair.get("kind") != "loop-package-boundary-pair"
        or pair.get("status") != "passed"
        or pair.get("source_sha") != source_sha.lower()
    ):
        raise ValueError("release assets require passed pair evidence for the source SHA")

    packages = {
        "linux": f"Loop-pdf-{version}-x86_64.AppImage",
        "windows": f"mberrys.Loop-pdf_{version}.msi",
    }
    required = {
        *packages.values(),
        f"{packages['linux']}.zsync",
        f"mberrys.Loop-pdf_{version}.msix",
        f"Loop-pdf-Windows-{version}.zip",
        *(f"Loop-{version}-{platform}.spdx.json" for platform in packages),
        *(f"Loop-{version}-{platform}-THIRD_PARTY_NOTICES.txt" for platform in packages),
    }
    allowed = required | {f"{packages['linux']}.sig"}
    paths = {path.name: path for path in artifacts.iterdir()}
    missing = required - paths.keys()
    unexpected = paths.keys() - allowed
    if missing or unexpected:
        raise ValueError(f"release asset mismatch: missing={sorted(missing)}, unexpected={sorted(unexpected)}")
    for path in paths.values():
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"release asset must be a nonempty regular file: {path.name}")

    for platform, name in packages.items():
        inspected = pair.get("packages", {}).get(platform, {})
        path = paths[name]
        if inspected.get("name") != name or inspected.get("size") != path.stat().st_size:
            raise ValueError(f"inspected package identity does not match release asset: {name}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != inspected.get("sha256"):
            raise ValueError(f"release asset digest does not match inspected package: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--pair", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    try:
        verify(args.artifacts, args.pair, args.version, args.source_sha)
    except (OSError, ValueError) as exc:
        print(f"Release assets FAILED: {exc}", file=sys.stderr)
        return 1
    print(f"Release assets verified: version={args.version} source_sha={args.source_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
