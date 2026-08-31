#!/usr/bin/env python3
"""Verify one installed Loupe product profile against the checked-in manifest."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from product_surface import (
    MANIFEST_RELATIVE,
    PROFILES,
    SCHEMA_RELATIVE,
    ContractError,
    load_json,
    load_manifest,
    run_verification,
    validate_manifest,
)


def _path_from_root(root: Path, value: str | None) -> Path | None:
    if value is None:
        return None
    path = Path(value)
    return path if path.is_absolute() else root / path


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--manifest-path", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--install-dir", type=Path)
    parser.add_argument("--install-manifest", type=Path)
    parser.add_argument("--pdf-tool", type=Path)
    parser.add_argument("--discovery-json", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    root = args.root.resolve()
    manifest_path = _path_from_root(root, str(args.manifest_path) if args.manifest_path else None)
    build_dir = _path_from_root(root, str(args.build_dir) if args.build_dir else None)
    install_dir = _path_from_root(root, str(args.install_dir) if args.install_dir else None)
    install_manifest = _path_from_root(root, str(args.install_manifest) if args.install_manifest else None)
    pdf_tool = _path_from_root(root, str(args.pdf_tool) if args.pdf_tool else None)
    discovery_json = _path_from_root(root, str(args.discovery_json) if args.discovery_json else None)

    try:
        schema = load_json(root / SCHEMA_RELATIVE)
        schema_errors = []
        if not isinstance(schema, dict) or schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
            schema_errors.append(f"{SCHEMA_RELATIVE.as_posix()} must be a draft 2020-12 schema")
        manifest = load_manifest(root, manifest_path)
        errors = schema_errors + validate_manifest(manifest, root)
        if not errors:
            errors.extend(
                run_verification(
                    root,
                    manifest,
                    args.profile,
                    build_dir=build_dir,
                    install_dir=install_dir,
                    install_manifest_path=install_manifest,
                    pdf_tool=pdf_tool,
                    discovery_json=discovery_json,
                )
            )
    except ContractError as exc:
        errors = [str(exc)]

    if errors:
        for error in errors:
            print(f"product-surface: {error}", file=sys.stderr)
        return 1

    scope = f"installed at {install_dir}" if install_dir is not None else "source-only"
    print(f"product-surface verified for {args.profile} ({scope})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
