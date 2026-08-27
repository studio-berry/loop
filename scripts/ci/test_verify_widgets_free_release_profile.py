#!/usr/bin/env python3
"""Negative coverage for scripts/verify-widgets-free-release-profile.py."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-widgets-free-release-profile.py"


def _load_verifier_module():
    spec = importlib.util.spec_from_file_location("verify_widgets_free_release_profile", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VerifyWidgetsFreeReleaseProfileTest(unittest.TestCase):
    def test_static_contract_passes(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_cache_with_widgets_fails(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOUPE_LOUPE_DISTRIBUTION:BOOL=ON",
                        "Qt6Widgets_DIR:PATH=/tmp/Qt6Widgets",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(module.ContractError) as ctx:
                module.validate_cmake_cache(cache)
            self.assertIn("Qt6::Widgets", str(ctx.exception))

    def test_cache_with_widgets_bound_target_fails(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOUPE_LOUPE_DISTRIBUTION:BOOL=ON",
                        "LOUPE_BUILD_CANVAS_BENCHMARK:BOOL=ON",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(module.ContractError) as ctx:
                module.validate_cmake_cache(cache)
            self.assertIn("CANVAS_BENCHMARK", str(ctx.exception))


if __name__ == "__main__":
    raise SystemExit(unittest.main())
