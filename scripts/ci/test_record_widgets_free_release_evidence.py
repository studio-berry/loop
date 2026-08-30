#!/usr/bin/env python3
"""Tests for Widgets-free release evidence recording."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "record_widgets_free_release_evidence.py"
PREPARE = ROOT / "scripts" / "ci" / "prepare_widgets_free_qt.py"


def load_module():
    spec = importlib.util.spec_from_file_location("record_widgets_free_release_evidence", SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RecordWidgetsFreeReleaseEvidenceTest(unittest.TestCase):
    def test_records_sha_and_release_cache_identity(self) -> None:
        prepare_spec = importlib.util.spec_from_file_location("prepare_widgets_free_qt", PREPARE)
        if prepare_spec is None or prepare_spec.loader is None:
            raise AssertionError(f"unable to load {PREPARE}")
        prepare = importlib.util.module_from_spec(prepare_spec)
        prepare_spec.loader.exec_module(prepare)

        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "qt"
            for relative in prepare.REQUIRED_QT_CONFIGS:
                path = source / "lib" / "cmake" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("# fixture\n", encoding="utf-8")
            forbidden = source / "lib" / "cmake" / "Qt6Widgets" / "Qt6WidgetsConfig.cmake"
            forbidden.parent.mkdir(parents=True, exist_ok=True)
            forbidden.write_text("# forbidden\n", encoding="utf-8")
            filtered = root / "filtered"
            manifest = root / "manifest.json"
            manifest_data = prepare.stage_prefix(source, filtered)
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")

            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                "\n".join(
                    [
                        "LOUPE_LOUPE_DISTRIBUTION:BOOL=ON",
                        "LOUPE_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
                        f"Qt6_DIR:PATH={filtered / 'lib' / 'cmake' / 'Qt6'}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            fake_repo = root / "repo"
            fake_repo.mkdir()
            subprocess.run(["git", "init", "--quiet", str(fake_repo)], check=True)
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(fake_repo),
                    "-c",
                    "user.name=Test",
                    "-c",
                    "user.email=test@example.invalid",
                    "commit",
                    "--allow-empty",
                    "-m",
                    "fixture",
                ],
                check=True,
                capture_output=True,
            )

            with mock.patch.dict(os.environ, {"GITHUB_SHA": ""}):
                evidence = module.record_evidence(
                    fake_repo,
                    filtered,
                    manifest,
                    build,
                    "linux",
                    "6.11.1",
                    "passed",
                    "passed",
                    "passed",
                )

            self.assertEqual(evidence["status"], "passed")
            self.assertEqual(evidence["platform"], "linux")
            self.assertRegex(evidence["source_sha"], r"^[0-9a-f]{40}$")
            self.assertEqual(evidence["configure"]["qt6_dir"], str(filtered / "lib" / "cmake" / "Qt6"))


if __name__ == "__main__":
    raise SystemExit(unittest.main())
