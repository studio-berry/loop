#!/usr/bin/env python3
"""Install a reviewed, SHA-256-pinned external oracle ZIP into an empty directory."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import urllib.request
import zipfile
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unpack(archive: Path, expected_sha256: str, destination: Path) -> dict:
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256) or digest(archive) != expected_sha256:
        raise ValueError("Oracle distribution SHA-256 mismatch")
    if destination.exists():
        raise ValueError("Oracle destination must not exist")
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        if sum(member.file_size for member in bundle.infolist()) > 4 * 1024**3:
            raise ValueError("Oracle distribution exceeds the extraction budget")
        if len(names) != len({name.casefold() for name in names}):
            raise ValueError("Duplicate oracle bundle member")
        for member in bundle.infolist():
            relative = Path(member.filename)
            mode = member.external_attr >> 16
            if relative.is_absolute() or ".." in relative.parts or "\\" in member.filename or ":" in member.filename or any(ord(char) < 32 for char in member.filename) or stat.S_ISLNK(mode):
                raise ValueError("Unsafe oracle bundle member")
        bundle.extractall(destination)
        for member in bundle.infolist():
            if member.external_attr >> 16 & stat.S_IXUSR:
                (destination / member.filename).chmod(0o755)
    lock = json.loads((destination / "oracles.json").read_text(encoding="utf-8"))
    if lock.get("schema") != "loop.oracle-distribution" or lock.get("schema_version") != 1:
        raise ValueError("Unrecognized oracle distribution manifest")
    if set(lock.get("tools", {})) != {"qpdf", "pdfsig", "verapdf", "ghostscript"}:
        raise ValueError("Oracle distribution must include all four tools")
    for item in [*lock["tools"].values(), *lock.get("profiles", {}).values(), lock["pillow_wheel"]]:
        candidate = (destination / item["path"]).resolve()
        if not candidate.is_relative_to(destination.resolve()) or not candidate.is_file() or digest(candidate) != item["sha256"]:
            raise ValueError("Oracle binary or profile identity mismatch")
    if set(lock.get("profiles", {})) != {"rgb", "cmyk", "gray"}:
        raise ValueError("Pinned Ghostscript default ICC profiles are required")
    for item in lock["tools"].values():
        if not item.get("upstream") or not item.get("version") or not item.get("license"):
            raise ValueError("Tool provenance, version and open-source license are required")
    return lock


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    if not args.url.startswith("https://"):
        parser.error("Oracle bundle URL must use HTTPS")
    archive = args.destination.with_suffix(".zip")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(args.url, timeout=120) as source, archive.open("wb") as output:
        while chunk := source.read(1024 * 1024):
            output.write(chunk)
    lock = unpack(archive, args.sha256, args.destination)
    github_path = os.environ.get("GITHUB_PATH")
    if github_path:
        with Path(github_path).open("a", encoding="utf-8") as output:
            for directory in sorted({str((args.destination / item["path"]).resolve().parent) for item in lock["tools"].values()}):
                output.write(directory + "\n")
    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with Path(github_env).open("a", encoding="utf-8") as output:
            output.write("LOOP_PILLOW_WHEEL=" + str((args.destination / lock["pillow_wheel"]["path"]).resolve()) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
