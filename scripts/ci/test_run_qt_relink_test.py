"""Execute package scripts with fake payloads; no Qt build or MSI installation."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHA = "ABCDEF0123456789" + "01234567" * 3
STRIPPED = (
    "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH", "QTDIR", "QT_ROOT_DIR", "Qt6_DIR",
    "LOOP_QT_ROOT", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "VCPKG_ROOT",
)


@unittest.skipUnless(os.name == "posix", "POSIX fake AppImage requires bash")
class AppImageRelinkTests(unittest.TestCase):
    def run_fixture(self, *, missing_qt=False, smoke_exit=0, extract_exit=0):
        with tempfile.TemporaryDirectory(prefix="loop relink ") as directory:
            root = Path(directory)
            extraction = root / "extraction.txt"
            marker = root / "smoke.txt"
            transcript = root / "transcript.txt"
            appimage = root / "fake package.AppImage"
            editor = """#!/bin/bash
set -eu
[ "$1" = "--quick-smoke" ]
""" + "".join(f'[ -z "${{{name}:-}}" ]\n' for name in STRIPPED) + """
[ -z "${LD_PRELOAD:-}" ]
[ "$PATH" = "/usr/bin:/bin" ]
[ "$QT_QPA_PLATFORM" = "offscreen" ]
bin_dir="$(cd "$(dirname "$0")" && pwd)"
[ "$LD_LIBRARY_PATH" = "${bin_dir%/bin}/lib:${bin_dir%/bin}/lib/x86_64-linux-gnu" ]
[ "$(cat "$bin_dir/../lib/libQt6Core.so.6")" = "original Qt fixture" ]
[ -f "$bin_dir/../lib/libQt6Core.so.6.loop-relink-bak" ]
printf 'smoke reached' > "$LOOP_TEST_MARKER"
exit "$LOOP_TEST_SMOKE_EXIT"
"""
            appimage.write_text("""#!/bin/bash
set -eu
[ "$1" = "--appimage-extract" ]
printf '%s' "$PWD" > "$LOOP_TEST_EXTRACTION"
if [ "$LOOP_TEST_EXTRACT_EXIT" != 0 ]; then exit "$LOOP_TEST_EXTRACT_EXIT"; fi
mkdir -p squashfs-root/usr/bin squashfs-root/usr/lib
if [ "$LOOP_TEST_MISSING_QT" != 1 ]; then
    printf 'original Qt fixture' > squashfs-root/usr/lib/libQt6Core.so.6
fi
cat > squashfs-root/usr/bin/LoopEditor <<'EDITOR'
""" + editor + "EDITOR\nchmod +x squashfs-root/usr/bin/LoopEditor\n")
            environment = os.environ.copy()
            environment.update({name: "/fake/developer/path" for name in STRIPPED})
            environment.pop("LD_PRELOAD", None)
            environment.update(
                LOOP_SOURCE_SHA=SHA, QT_QPA_PLATFORM="offscreen",
                LD_LIBRARY_PATH="/fake/developer/lib",
                LOOP_TEST_EXTRACTION=str(extraction), LOOP_TEST_MARKER=str(marker),
                LOOP_TEST_SMOKE_EXIT=str(smoke_exit),
                LOOP_TEST_EXTRACT_EXIT=str(extract_exit),
                LOOP_TEST_MISSING_QT=str(int(missing_qt)),
            )
            result = subprocess.run(
                ["bash", str(ROOT / "scripts/ci/run_qt_relink_test.sh"),
                 str(appimage), "--output", str(transcript)],
                env=environment, capture_output=True, text=True, timeout=30,
            )
            self.assertTrue(extraction.exists(), result.stderr)
            self.assertFalse(Path(extraction.read_text()).exists(), "Extraction leaked")
            return result, marker.exists(), transcript.read_text() if transcript.exists() else ""

    def test_final_payload_smoke_strips_developer_environment_and_records_sha(self):
        result, launched, transcript = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(launched)
        self.assertIn(f"source_sha={SHA.lower()}", transcript)
        self.assertIn("target_library=usr/lib/libQt6Core.so.6", transcript)
        self.assertIn("replacement_kind=byte-identical-copy", transcript)

    def test_missing_packaged_qt_fails_before_launch(self):
        result, launched, transcript = self.run_fixture(missing_qt=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(launched)
        self.assertIn("libQt6Core not found", transcript)
        self.assertNotIn("PASSED", transcript)

    def test_smoke_failure_propagates_and_cleans_extraction(self):
        result, launched, transcript = self.run_fixture(smoke_exit=17)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(launched)
        self.assertIn("exit 17", transcript)
        self.assertNotIn("PASSED", transcript)

    def test_extraction_failure_propagates_and_cleans_extraction(self):
        result, launched, transcript = self.run_fixture(extract_exit=19)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(launched)
        self.assertNotIn("PASSED", transcript)


@unittest.skipUnless(os.name == "nt" and shutil.which("pwsh"), "Windows PowerShell required")
class MsiLifecycleTests(unittest.TestCase):
    def run_fixture(self, *, fail_smoke=False, fail_relink=False, fail_uninstall=False,
                    relink=True, stale=False):
        with tempfile.TemporaryDirectory(prefix="loop lifecycle ") as directory:
            root = Path(directory)
            (root / "ci").mkdir()
            shutil.copyfile(ROOT / "scripts/Invoke-MsiSmokeTest.ps1", root / "lifecycle.ps1")
            (root / "fake.msi").touch()
            (root / "smoke-test-install.ps1").write_text('''param($InstallDir, $SourceSha, [switch]$SkipEditorLaunch)
if (-not $global:installed) { throw "smoke ran after removal" }
$global:events.Add("smoke")
if ($env:LOOP_TEST_FAIL_SMOKE -eq '1') { throw "fixture smoke failed" }
''')
            (root / "ci/run_qt_relink_test.ps1").write_text('''param($InstallDir, $SourceSha, $OutputPath)
if (-not $global:installed) { throw "relink ran after removal" }
if ($SourceSha -ne $env:LOOP_TEST_SHA) { throw "source SHA lost" }
$global:events.Add("relink")
Set-Content -LiteralPath $OutputPath -Value "source_sha=$SourceSha"
if ($env:LOOP_TEST_FAIL_RELINK -eq '1') { throw "fixture relink failed" }
''')
            (root / "driver.ps1").write_text(r'''$ErrorActionPreference = "Stop"
$global:events = [Collections.Generic.List[string]]::new()
$global:installed = $env:LOOP_TEST_STALE -eq '1'
$global:fixtureInstall = Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "LOOP-FAKE-QUALIFICATION"
function Test-Path {
    param($LiteralPath)
    if ($LiteralPath -eq $global:fixtureInstall) { return $global:installed }
    if ($LiteralPath -like "$global:fixtureInstall*" -or
        $LiteralPath -eq (Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "share/loop")) { return $false }
    Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath
}
function Start-Process {
    param($FilePath, $ArgumentList, [switch]$Wait, [switch]$PassThru)
    if ($FilePath -ne 'msiexec.exe') { throw "unexpected process" }
    if ($ArgumentList.StartsWith('/i ')) {
        $global:installed = $true
        $global:events.Add("install")
    } elseif ($ArgumentList.StartsWith('/x ')) {
        $global:events.Add("uninstall")
        if ($env:LOOP_TEST_FAIL_UNINSTALL -eq '1') { return [pscustomobject]@{ExitCode=1603} }
        $global:installed = $false
    } else { throw "unexpected MSI arguments" }
    return [pscustomobject]@{ExitCode=0}
}
$arguments = @{
    MsiPath = (Join-Path $PSScriptRoot 'fake.msi')
    InstallDir = $global:fixtureInstall
    SourceSha = $env:LOOP_TEST_SHA
    LogDir = (Join-Path $PSScriptRoot 'logs')
    SkipEditorLaunch = $true
}
if ($env:LOOP_TEST_RELINK -eq '1') {
    $arguments.QtRelinkTranscript = Join-Path $PSScriptRoot 'relink.txt'
}
$failed = $false
try { & (Join-Path $PSScriptRoot 'lifecycle.ps1') @arguments }
catch { Write-Output "FAILURE: $_"; $failed = $true }
Write-Output ("EVENTS=" + ($global:events -join ','))
if ($failed) { exit 1 }
''')
            environment = os.environ.copy()
            environment.update(LOOP_TEST_SHA=SHA)
            for name, value in dict(FAIL_SMOKE=fail_smoke, FAIL_RELINK=fail_relink,
                                    FAIL_UNINSTALL=fail_uninstall, RELINK=relink,
                                    STALE=stale).items():
                environment[f"LOOP_TEST_{name}"] = str(int(value))
            return subprocess.run(
                ["pwsh", "-NoProfile", "-File", str(root / "driver.ps1")],
                env=environment, capture_output=True, text=True, timeout=30,
            )

    def test_relink_runs_between_smoke_and_uninstall(self):
        result = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("EVENTS=install,smoke,relink,uninstall", result.stdout)

    def test_optional_relink_preserves_default_lifecycle(self):
        result = self.run_fixture(relink=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("EVENTS=install,smoke,uninstall", result.stdout)

    def test_relink_failure_still_uninstalls(self):
        result = self.run_fixture(fail_relink=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fixture relink failed", result.stdout)
        self.assertIn("EVENTS=install,smoke,relink,uninstall", result.stdout)

    def test_smoke_failure_still_uninstalls(self):
        result = self.run_fixture(fail_smoke=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EVENTS=install,smoke,uninstall", result.stdout)

    def test_uninstall_failure_fails_qualification(self):
        result = self.run_fixture(fail_uninstall=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("msiexec failed", result.stdout)

    def test_cleanup_failure_preserves_primary_failure(self):
        result = self.run_fixture(fail_relink=True, fail_uninstall=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAILURE: fixture relink failed", result.stdout)
        self.assertIn("cleanup also failed", result.stdout)

    def test_stale_installation_is_rejected_without_mutation(self):
        result = self.run_fixture(stale=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already exists", result.stdout)
        self.assertIn("EVENTS=\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
