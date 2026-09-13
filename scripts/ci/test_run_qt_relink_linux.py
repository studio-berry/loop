"""Executable regression tests for the Linux AppImage Qt relink proof."""

from __future__ import annotations

import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RELINK_SCRIPT = ROOT / "scripts" / "ci" / "run_qt_relink_test.sh"


class LinuxQtRelinkTests(unittest.TestCase):
    def test_extracted_appimage_payload_is_relinked_and_launched(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            appimage = root / "Loop-pdf-test-x86_64.AppImage"
            transcript = root / "qt-relink.txt"
            appimage.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env bash
                    set -euo pipefail
                    test "${1:-}" = "--appimage-extract"
                    mkdir -p squashfs-root/usr/bin squashfs-root/usr/lib
                    printf 'recipient-replaceable-qt\n' > squashfs-root/usr/lib/libQt6Core.so.6
                    printf '%s\n' \
                      '#!/usr/bin/env bash' \
                      'set -euo pipefail' \
                      'test "${1:-}" = "--quick-smoke"' \
                      'qt="$(dirname "$0")/../lib/libQt6Core.so.6"' \
                      'test -f "${qt}.loop-relink-bak"' \
                      'test -f "${qt}.loop-relink-replacement"' \
                      'cmp "${qt}.loop-relink-bak" "$qt"' \
                      > squashfs-root/usr/bin/LoopEditor
                    chmod +x squashfs-root/usr/bin/LoopEditor
                    """
                ),
                encoding="utf-8",
            )
            appimage.chmod(0o755)

            source_sha = "a" * 40
            result = subprocess.run(
                ["bash", str(RELINK_SCRIPT), str(appimage), "--output", str(transcript)],
                check=False,
                capture_output=True,
                text=True,
                env={**os.environ, "LOOP_SOURCE_SHA": source_sha, "QT_QPA_PLATFORM": "offscreen"},
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            evidence = transcript.read_text(encoding="utf-8")
            self.assertIn(f"source_sha={source_sha}", evidence)
            self.assertIn("Qt relink test PASSED", evidence)


if __name__ == "__main__":
    unittest.main()
