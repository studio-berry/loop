#!/usr/bin/env python3
"""Tests for the fuzz corpus manifest checker."""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from scripts.ci.check_fuzz_corpus import (  # noqa: E402
    HARNESS_TARGETS,
    validate_corpus,
    validate_manifest,
)


class FuzzCorpusValidationTests(unittest.TestCase):
    def test_repository_manifest_passes(self):
        self.assertEqual(validate_corpus(), [])

    def test_rejects_hash_named_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            harness_dir = root / "Fuzz" / "corpus" / "fuzz_images"
            harness_dir.mkdir(parents=True)
            seed = harness_dir / "0bf1b28b05310e55396a21cca5a040b7fb61394a"
            seed.write_bytes(b"seed")

            manifest = {
                "schema_version": 1,
                "cases": [
                    {
                        "id": "bad-hash-name",
                        "path": "Fuzz/corpus/fuzz_images/0bf1b28b05310e55396a21cca5a040b7fb61394a",
                        "harness": "fuzz_images",
                        "origin": "fuzz-finding",
                        "issue": 64,
                        "sha256": "unused",
                        "expected": "terminates-without-crash",
                        "minimized": True,
                    }
                ],
            }
            violations = validate_manifest(manifest, root)
            self.assertTrue(any("hash-named" in reason for _, reason in violations))

    def test_rejects_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            harness_dir = root / "Fuzz" / "corpus" / "fuzz_images"
            harness_dir.mkdir(parents=True)
            seed = harness_dir / "sample.bin"
            seed.write_bytes(b"seed")

            manifest = {
                "schema_version": 1,
                "cases": [
                    {
                        "id": "sample",
                        "path": "Fuzz/corpus/fuzz_images/sample.bin",
                        "harness": "fuzz_images",
                        "origin": "synthetic",
                        "issue": 64,
                        "sha256": "0" * 64,
                        "expected": "terminates-without-crash",
                        "minimized": True,
                    }
                ],
            }
            violations = validate_manifest(manifest, root)
            self.assertTrue(any("sha256 mismatch" in reason for _, reason in violations))

    def test_rejects_unowned_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for harness in HARNESS_TARGETS:
                (root / "Fuzz" / "corpus" / harness).mkdir(parents=True)

            orphan = root / "Fuzz" / "corpus" / "fuzz_images" / "orphan.bin"
            orphan.write_bytes(b"orphan")

            manifest = {"schema_version": 1, "cases": []}
            violations = validate_manifest(manifest, root)
            self.assertTrue(any("missing from manifest" in reason for _, reason in violations))

    def test_rejects_duplicate_ids(self):
        manifest = {
            "schema_version": 1,
            "cases": [
                {
                    "id": "dup",
                    "path": "Fuzz/corpus/fuzz_images/a.bin",
                    "harness": "fuzz_images",
                    "origin": "synthetic",
                    "issue": 1,
                    "sha256": "0" * 64,
                    "expected": "terminates-without-crash",
                    "minimized": True,
                },
                {
                    "id": "dup",
                    "path": "Fuzz/corpus/fuzz_images/b.bin",
                    "harness": "fuzz_images",
                    "origin": "synthetic",
                    "issue": 1,
                    "sha256": "1" * 64,
                    "expected": "terminates-without-crash",
                    "minimized": True,
                },
            ],
        }
        violations = validate_manifest(manifest)
        self.assertTrue(any("duplicate id" in reason for _, reason in violations))


if __name__ == "__main__":
    unittest.main()
