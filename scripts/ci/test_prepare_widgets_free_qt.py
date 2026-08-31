#!/usr/bin/env python3
"""Tests for the Widgets-free Qt prefix preparation helper."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "prepare_widgets_free_qt.py"


def load_module():
    spec = importlib.util.spec_from_file_location("prepare_widgets_free_qt", SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PrepareWidgetsFreeQtTest(unittest.TestCase):
    def _make_prefix(self, root: Path) -> Path:
        prefix = root / "qt"
        for relative in load_module().REQUIRED_QT_CONFIGS:
            path = prefix / "lib" / "cmake" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# fixture\n", encoding="utf-8")
        (prefix / "lib").mkdir(parents=True, exist_ok=True)
        (prefix / "lib" / "libQt6Core.so").write_text("core\n", encoding="utf-8")
        (prefix / "include" / "QtCore").mkdir(parents=True, exist_ok=True)
        (prefix / "include" / "QtCore" / "qcore.h").write_text("// core\n", encoding="utf-8")
        (prefix / "lib" / "cmake" / "Qt6Widgets").mkdir(parents=True, exist_ok=True)
        (prefix / "lib" / "cmake" / "Qt6Widgets" / "Qt6WidgetsConfig.cmake").write_text(
            "# forbidden\n", encoding="utf-8"
        )
        (prefix / "include" / "QtWidgets").mkdir(parents=True, exist_ok=True)
        (prefix / "include" / "QtWidgets" / "qwidget.h").write_text("// forbidden\n", encoding="utf-8")
        (prefix / "bin").mkdir(parents=True, exist_ok=True)
        (prefix / "bin" / "Qt6Widgets.dll").write_text("forbidden\n", encoding="utf-8")
        return prefix

    def test_stage_excludes_widgets_and_retains_required_modules(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self._make_prefix(root)
            destination = root / "filtered"
            manifest = module.stage_prefix(source, destination)

            self.assertFalse((destination / "include" / "QtWidgets").exists())
            self.assertFalse((destination / "bin" / "Qt6Widgets.dll").exists())
            self.assertFalse((destination / "lib" / "cmake" / "Qt6Widgets").exists())
            self.assertTrue((destination / "lib" / "libQt6Core.so").is_file())
            self.assertGreaterEqual(len(manifest["excluded_paths"]), 3)
            module.validate_prefix(destination)

    def test_stage_refuses_existing_destination(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self._make_prefix(root)
            destination = root / "filtered"
            destination.mkdir()
            with self.assertRaises(module.QualificationError):
                module.stage_prefix(source, destination)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
