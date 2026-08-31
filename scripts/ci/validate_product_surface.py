#!/usr/bin/env python3
"""Validate both product-surface profiles using repository sources only."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

from product_surface import (  # noqa: E402
    MANIFEST_RELATIVE,
    PROFILES,
    SCHEMA_RELATIVE,
    ContractError,
    load_json,
    load_manifest,
    validate_manifest,
    validate_packaging_sources,
    validate_source,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    errors: list[str] = []
    try:
        schema = load_json(root / SCHEMA_RELATIVE)
        if not isinstance(schema, dict) or schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            errors.append(f"{SCHEMA_RELATIVE.as_posix()} must be a draft 2020-12 schema")
        manifest = load_manifest(root, root / MANIFEST_RELATIVE)
        errors.extend(validate_manifest(manifest, root))
        if not errors:
            for profile in PROFILES:
                errors.extend(validate_source(root, manifest, profile))
                errors.extend(validate_packaging_sources(root, manifest, profile))
    except ContractError as exc:
        errors.append(str(exc))

    if errors:
        for error in errors:
            print(f"product-surface: {error}", file=sys.stderr)
        return 1
    print("product-surface source contract verified for developer and loupe-release")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
