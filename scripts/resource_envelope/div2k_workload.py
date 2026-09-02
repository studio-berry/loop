#!/usr/bin/env python3
"""Validate the external DIV2K bleed corpus and build deterministic PDF workloads.

The corpus is intentionally external to the Loop repository.  The default
location is the generated corpus used by the bleed research lab::

    C:\\.dev\\repos\\l-bleed\\results\\DIV2K

Examples::

    python scripts/resource_envelope/div2k_workload.py validate \
        --corpus-root C:\\.dev\\repos\\l-bleed\\results\\DIV2K
    python scripts/resource_envelope/div2k_workload.py manifest \
        --corpus-root C:\\.dev\\repos\\l-bleed\\results\\DIV2K \
        --output C:\\temp\\loop-div2k-manifest.json --sample-count 256
    python scripts/resource_envelope/div2k_workload.py build-pdf \
        --manifest C:\\temp\\loop-div2k-manifest.json \
        --output C:\\temp\\loop-div2k-10000-pages.pdf --page-count 10000

The PDF writer deliberately supports the corpus' 8-bit RGB, non-interlaced PNG
files without requiring a package installation.  Workload outputs and their
manifests belong outside the repository.
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from pathlib import Path
from typing import Sequence

from scripts.resource_envelope.corpus import (
    DEFAULT_CORPUS_ROOT,
    CorpusError,
    Example,
    canonical_json,
    create_manifest,
    load_examples,
    select_examples,
    sha256_bytes,
    sha256_file,
)
from scripts.resource_envelope.pdf_builder import build_pdf, png_pixels

__all__ = [
    "DEFAULT_CORPUS_ROOT",
    "CorpusError",
    "Example",
    "build_pdf",
    "canonical_json",
    "create_manifest",
    "load_examples",
    "png_pixels",
    "select_examples",
    "sha256_bytes",
    "sha256_file",
]

_png_pixels = png_pixels


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate the external DIV2K corpus")
    validate.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    validate.add_argument("--hash-all", action="store_true", help="hash every pair file")

    manifest = subparsers.add_parser("manifest", help="write a deterministic selection manifest")
    manifest.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    manifest.add_argument("--output", type=Path, required=True)
    manifest.add_argument("--sample-count", type=int, default=256)
    manifest.add_argument("--seed", type=int, default=0)
    manifest.add_argument("--hash-all", action="store_true")

    pdf = subparsers.add_parser("build-pdf", help="build an external deterministic image-heavy PDF")
    pdf.add_argument("--manifest", type=Path, required=True)
    pdf.add_argument("--output", type=Path, required=True)
    pdf.add_argument("--page-count", type=int, default=10000)
    pdf.add_argument("--page-seed", type=int, default=0)
    pdf.add_argument("--summary-output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate":
            summary, examples, digest = load_examples(args.corpus_root, hash_all=args.hash_all)
            print(json.dumps({
                "dataset": summary["dataset"],
                "examples": len(examples),
                "sources": summary.get("total_sources"),
                "corpus_digest": digest,
                "hash_all": args.hash_all,
            }, indent=2))
        elif args.command == "manifest":
            manifest = create_manifest(
                args.corpus_root, args.output, args.sample_count, args.seed, args.hash_all
            )
            print(json.dumps({
                "output": str(args.output.resolve()),
                "examples": manifest["selection"]["actual_count"],
                "corpus_digest": manifest["corpus"]["corpus_digest"],
            }, indent=2))
        else:
            summary = build_pdf(args.manifest, args.output, args.page_count, args.page_seed)
            if args.summary_output:
                args.summary_output.parent.mkdir(parents=True, exist_ok=True)
                args.summary_output.write_bytes(canonical_json(summary))
            print(json.dumps(summary, indent=2))
    except (CorpusError, OSError, ValueError, zlib.error) as exc:
        print(f"resource-envelope error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
