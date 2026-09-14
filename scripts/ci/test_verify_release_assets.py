"""Release assembly rejects missing, unrelated, and substituted package assets."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from scripts.ci.verify_release_assets import verify


class ReleaseAssetTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.version = "0.2.1-alpha"
        self.source_sha = "a" * 40
        self.linux = "Loop-pdf-0.2.1-alpha-x86_64.AppImage"
        self.windows = "mberrys.Loop-pdf_0.2.1-alpha.msi"
        names = [
            self.linux, self.windows, f"{self.linux}.zsync",
            "mberrys.Loop-pdf_0.2.1-alpha.msix", "Loop-pdf-Windows-0.2.1-alpha.zip",
            "Loop-0.2.1-alpha-linux.spdx.json", "Loop-0.2.1-alpha-windows.spdx.json",
            "Loop-0.2.1-alpha-linux-THIRD_PARTY_NOTICES.txt",
            "Loop-0.2.1-alpha-windows-THIRD_PARTY_NOTICES.txt",
        ]
        for name in names:
            (self.artifacts / name).write_bytes(b"package")
        self.pair = {
            "schema_version": 1,
            "kind": "loop-package-boundary-pair",
            "status": "passed",
            "source_sha": self.source_sha,
            "packages": {
                platform: {"name": name, "size": 7, "sha256": hashlib.sha256(b"package").hexdigest()}
                for platform, name in (("linux", self.linux), ("windows", self.windows))
            },
        }
        self.pair_path = self.root / "pair.json"

    def verify(self):
        self.pair_path.write_text(json.dumps(self.pair), encoding="utf-8")
        verify(self.artifacts, self.pair_path, self.version, self.source_sha)

    def test_complete_unsigned_and_signed_assets(self):
        self.verify()
        (self.artifacts / f"{self.linux}.sig").write_bytes(b"signature")
        self.verify()

    def test_rejects_package_substitution_with_same_size(self):
        for name in (self.linux, self.windows):
            with self.subTest(name=name):
                (self.artifacts / name).write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "digest"):
                    self.verify()
                (self.artifacts / name).write_bytes(b"package")

    def test_rejects_wrong_source_and_failed_evidence(self):
        self.pair["source_sha"] = "b" * 40
        with self.assertRaisesRegex(ValueError, "passed pair evidence"):
            self.verify()
        self.pair["source_sha"] = self.source_sha
        self.pair["status"] = "failed"
        with self.assertRaisesRegex(ValueError, "passed pair evidence"):
            self.verify()

    def test_rejects_missing_installer_or_licensing_asset(self):
        for name in (self.windows, "Loop-0.2.1-alpha-linux.spdx.json"):
            with self.subTest(name=name):
                (self.artifacts / name).unlink()
                with self.assertRaisesRegex(ValueError, "missing="):
                    self.verify()
                (self.artifacts / name).write_bytes(b"package")

    def test_rejects_ci_evidence_among_release_assets(self):
        (self.artifacts / "crash.dmp").write_bytes(b"diagnostic")
        with self.assertRaisesRegex(ValueError, "unexpected=.*crash.dmp"):
            self.verify()

    def test_rejects_empty_asset_and_package_from_another_version(self):
        (self.artifacts / self.windows).write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "nonempty regular file"):
            self.verify()
        (self.artifacts / self.windows).write_bytes(b"package")
        self.pair["packages"]["windows"]["name"] = "mberrys.Loop-pdf_0.2.0-alpha.msi"
        with self.assertRaisesRegex(ValueError, "identity"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
