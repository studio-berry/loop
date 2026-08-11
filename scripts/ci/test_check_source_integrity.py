#!/usr/bin/env python3
"""Tests for the tracked-source integrity check.

Conflict markers are assembled from fragments on purpose: a literal marker in
this file would make check_source_integrity.py flag the test itself.
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from scripts.ci.check_source_integrity import (  # noqa: E402
    MAX_TRACKED_BYTES,
    forbidden_path_reason,
    has_conflict_markers,
    oversized_reason,
    validate_repository,
)

OURS = "<" * 7
THEIRS = ">" * 7
DIVIDER = "=" * 7


class ForbiddenPathTests(unittest.TestCase):
    def test_rejects_build_trees(self):
        for path in (
            "build/CMakeFiles/rules.ninja",
            "build-fuzz-docker/usr/lib/libPdf4QtLibCore.so.1.6.0.0",
            "build-fuzz-docker/.ninja_log",
            ".docker-vcpkg",
            ".docker-vcpkg/installed/x64-linux/include/zlib.h",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_rejects_build_caches_anywhere(self):
        for path in ("CMakeCache.txt", "some/nested/dir/CMakeCache.txt"):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_rejects_one_off_debug_artifacts(self):
        for path in (
            "debug-b0e75b.log",
            "debug-pr188.log",
            "scripts/debug-pr188.sh",
            "scripts/debug-pr188.ps1",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(forbidden_path_reason(path))

    def test_keeps_legitimate_sources(self):
        for path in (
            "Fuzz/fuzz_pdf_parser.cpp",
            "Fuzz/corpus/regression/0a1b2c3d",
            "scripts/ci/check_source_integrity.py",
            "scripts/hooks/cc-guard-bash.sh",
            "build-notes.md",
            "buildsystem/toolchain.cmake",
            "Pdf4QtLibCore/sources/pdfdocument.cpp",
            "docs/adr/adr-003-pagemaster-export-orchestrator.md",
            "loupe-preflight/testdata/fixtures/image-dpi-excessive.pdf",
        ):
            with self.subTest(path=path):
                self.assertIsNone(forbidden_path_reason(path))

    def test_normalizes_windows_separators(self):
        self.assertIsNotNone(forbidden_path_reason("build-fuzz-docker\\config.h"))


class ConflictMarkerTests(unittest.TestCase):
    def test_detects_markers(self):
        text = "\n".join(
            [
                "/// Headless PageMaster export orchestrator (ADR-003).",
                f"{OURS} HEAD",
                "/// Locked stage order: assemble.",
                DIVIDER,
                "/// Locked stage order: assemble, flatten.",
                f"{THEIRS} origin/dev",
            ]
        )
        self.assertTrue(has_conflict_markers(text))

    def test_detects_bare_marker_line(self):
        self.assertTrue(has_conflict_markers(f"line\n{OURS}\nline"))

    def test_ignores_markdown_and_prose(self):
        for text in (
            f"Heading\n{DIVIDER}\n\nbody text",
            "shift left with a << b and right with c >> d",
            f"{'<' * 8} eight is not a marker",
            "a line that merely mentions a conflict marker",
        ):
            with self.subTest(text=text[:32]):
                self.assertFalse(has_conflict_markers(text))


class OversizedFileTests(unittest.TestCase):
    def test_accepts_files_at_or_under_the_cap(self):
        self.assertIsNone(oversized_reason("fixture.pdf", MAX_TRACKED_BYTES))
        self.assertIsNone(oversized_reason("fixture.pdf", 1_440_774))

    def test_rejects_files_over_the_cap(self):
        reason = oversized_reason("build-fuzz-docker/lib.so", MAX_TRACKED_BYTES + 1)
        self.assertIsNotNone(reason)
        self.assertIn("cap", reason)

    def test_allowlist_exempts_a_named_file(self):
        from scripts.ci import check_source_integrity

        original = dict(check_source_integrity.LARGE_FILE_ALLOWLIST)
        check_source_integrity.LARGE_FILE_ALLOWLIST["huge/fixture.pdf"] = "why"
        try:
            self.assertIsNone(
                oversized_reason("huge/fixture.pdf", MAX_TRACKED_BYTES + 1)
            )
        finally:
            check_source_integrity.LARGE_FILE_ALLOWLIST.clear()
            check_source_integrity.LARGE_FILE_ALLOWLIST.update(original)


class RepositoryScanTests(unittest.TestCase):
    """End-to-end scan over a throwaway repository containing each violation."""

    def _git(self, root, *args):
        subprocess.run(
            ["git", "-C", str(root), *args],
            check=True,
            capture_output=True,
        )

    def test_reports_each_violation_class(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._git(root, "init", "-q")

            (root / "clean.txt").write_text("ordinary tracked content\n")

            (root / "build-fuzz-docker").mkdir()
            (root / "build-fuzz-docker" / "config.h").write_text("generated\n")

            (root / "debug-abc123.log").write_text("scratch\n")

            (root / "conflicted.h").write_text(
                f"before\n{OURS} HEAD\nmine\n{DIVIDER}\ntheirs\n{THEIRS} origin/dev\nafter\n"
            )

            (root / "huge.bin").write_bytes(b"\0" * (MAX_TRACKED_BYTES + 1))

            self._git(root, "add", "-A")

            violations = dict(validate_repository(root))

            self.assertIn("build-fuzz-docker/config.h", violations)
            self.assertIn("debug-abc123.log", violations)
            self.assertIn("huge.bin", violations)
            self.assertEqual(
                violations.get("conflicted.h"), "unresolved merge-conflict marker"
            )
            self.assertNotIn("clean.txt", violations)

    def test_clean_repository_reports_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._git(root, "init", "-q")
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text("int main() { return 0; }\n")
            (root / "logo.png").write_bytes(b"\x89PNG\r\n\x1a\n\xff\xfe binary")
            self._git(root, "add", "-A")

            self.assertEqual(validate_repository(root), [])


if __name__ == "__main__":
    unittest.main()
