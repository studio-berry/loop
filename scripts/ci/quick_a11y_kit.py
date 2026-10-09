"""Bind an accessibility probe binary to the package's exact source commit."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

EXECUTABLES = {"ProductQuickAccessibilitySmoke", "ProductQuickAccessibilitySmoke.exe"}

def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root: Path, source_sha: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise ValueError("source SHA must be a full lowercase Git commit")
    record = json.loads((root / "identity.json").read_text(encoding="utf-8"))
    name = record["executable"]
    if name not in EXECUTABLES:
        raise ValueError("unexpected qualification executable")
    binary = root / name
    if record["source_sha"] != source_sha or record["executable_sha256"] != digest(binary):
        raise ValueError("qualification kit does not match the exact source/binary identity")
    return binary


def create(binary: Path, output: Path, source_sha: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise ValueError("source SHA must be a full lowercase Git commit")
    binary = binary.resolve(strict=True)
    if binary.name not in EXECUTABLES:
        raise ValueError("unexpected qualification executable")
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(binary, output / binary.name)
    record = {"source_sha": source_sha, "executable": binary.name, "executable_sha256": digest(output / binary.name)}
    (output / "identity.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    if args.binary:
        create(args.binary, args.output, args.source_sha)
    else:
        print(verify(args.output, args.source_sha))


if __name__ == "__main__":
    main()
