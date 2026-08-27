#!/usr/bin/env python3
"""Negative coverage for scripts/verify-widgets-free-release-profile.py."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
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
    def _make_qt_prefix(self, root: Path) -> Path:
        module = _load_verifier_module()
        prefix = root / "qt"
        for relative in module.REQUIRED_QT_CONFIGS:
            path = prefix / "lib" / "cmake" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# fixture\n", encoding="utf-8")
        return prefix

    def test_static_contract_passes(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)

    def test_cache_without_widgets_requirement_passes(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
                        "LOOP_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            module.validate_cmake_cache(cache)

    def test_cache_requiring_widgets_fails(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
                        "LOOP_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=ON",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(module.ContractError) as ctx:
                module.validate_cmake_cache(cache)
            self.assertIn("LOOP_CONFIGURE_REQUIRES_WIDGETS", str(ctx.exception))

    def test_cache_with_widgets_bound_target_fails(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
                        "LOOP_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
                        "LOOP_BUILD_CANVAS_BENCHMARK:BOOL=ON",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(module.ContractError) as ctx:
                module.validate_cmake_cache(cache)
            self.assertIn("CANVAS_BENCHMARK", str(ctx.exception))

    def test_filtered_qt_prefix_requires_modules_and_rejects_widgets(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            prefix = self._make_qt_prefix(Path(tmp))
            module.validate_qt_prefix(prefix)
            forbidden = prefix / "lib" / "Qt6Widgets.so"
            forbidden.write_text("forbidden\n", encoding="utf-8")
            with self.assertRaises(module.ContractError):
                module.validate_qt_prefix(prefix)

    def test_filtered_qt_prefix_rejects_printsupport(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            prefix = self._make_qt_prefix(Path(tmp))
            forbidden = prefix / "lib" / "Qt6PrintSupport.so.6"
            forbidden.write_text("forbidden\n", encoding="utf-8")
            with self.assertRaises(module.ContractError):
                module.validate_qt_prefix(prefix)

    def test_configure_failure_reports_the_diagnostic_tail(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            diagnostic_tail = "vcpkg actionable failure: sentinel-at-end"
            completed = subprocess.CompletedProcess(
                args=["cmake"],
                returncode=1,
                stdout=("configure transcript\n" + ("x" * 9000) + "\n" + diagnostic_tail),
                stderr="",
            )
            with patch.object(module.subprocess, "run", return_value=completed):
                with self.assertRaises(module.ContractError) as ctx:
                    module.run_release_profile_configure(Path(tmp) / "build", [])
            self.assertIn(diagnostic_tail, str(ctx.exception))

    def test_cache_must_use_filtered_qt_prefix(self) -> None:
        module = _load_verifier_module()
        with tempfile.TemporaryDirectory() as tmp:
            prefix = self._make_qt_prefix(Path(tmp))
            cache = Path(tmp) / "CMakeCache.txt"
            cache.write_text(
                "\n".join(
                    [
                        "LOUPE_LOUPE_DISTRIBUTION:BOOL=ON",
                        "LOUPE_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
                        f"Qt6_DIR:PATH={prefix / 'lib' / 'cmake' / 'Qt6'}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            module.validate_cmake_cache(cache, prefix)

            cache.write_text(
                cache.read_text(encoding="utf-8").replace(str(prefix), str(Path(tmp) / "other")),
                encoding="utf-8",
            )
            with self.assertRaises(module.ContractError):
                module.validate_cmake_cache(cache, prefix)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
