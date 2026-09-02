"""Regression checks for workflow paths and mandatory fast-gate tests."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WorkflowContractTests(unittest.TestCase):
    def test_agent_fast_runs_its_dedicated_policy_tests(self):
        workflow = (ROOT / ".github/workflows/reusable-linux.yml").read_text(encoding="utf-8")
        self.assertIn("name: Test agent policy checker", workflow)
        self.assertIn("if: inputs.fast", workflow)
        self.assertIn("python3 -m unittest scripts.agent.test_check_change -v", workflow)

    def test_source_integrity_runs_loop_identity_contract(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("scripts/ci/test_check_loop_identity.py", workflow)
        self.assertIn("scripts/ci/check_loop_identity.py", workflow)

    def test_windows_installer_verifies_from_its_checkout_root(self):
        workflow = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn("working-directory: loop", workflow)
        self.assertIn(".\\scripts\\verify-loop-surface.ps1", workflow)
        self.assertNotIn(".\\loop\\scripts\\verify-loop-surface.ps1", workflow)

    def test_ci_runs_phase5_widgets_evidence_and_contract(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("python3 scripts/generate_phase5_widgets_evidence.py --check", workflow)
        self.assertIn("python3 scripts/verify_phase5_widgets_contract.py", workflow)
        self.assertIn("python3 scripts/verify-plugin-form-accounting.py", workflow)
        self.assertIn("python3 scripts/verify-widgets-library-consumer-graph.py", workflow)

    def test_windows_validation_runs_phase5_widgets_twin(self):
        workflow = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn(".\\scripts\\verify-phase5-widgets-contract.ps1", workflow)

    def test_linux_release_gate_verifies_loop_release_surface(self):
        workflow = (ROOT / ".github/workflows/reusable-linux.yml").read_text(encoding="utf-8")
        self.assertIn("LOOP_LOOP_DISTRIBUTION=ON", workflow)
        self.assertIn("-Profile loop-release", workflow)
        self.assertNotIn("-Profile developer", workflow)

    def test_linux_release_gate_qualifies_without_widgets(self):
        workflow = (ROOT / ".github/workflows/reusable-linux.yml").read_text(encoding="utf-8")
        self.assertIn("prepare_widgets_free_qt.py", workflow)
        self.assertIn("--qt-prefix", workflow)
        self.assertIn("--expect-configure-failure", workflow)
        self.assertIn("Build Widgets-absent release profile", workflow)
        self.assertIn("record_widgets_free_release_evidence.py", workflow)
        self.assertIn(
            "-DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake",
            workflow,
        )
        self.assertIn('-DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"', workflow)

    def test_windows_release_gate_qualifies_without_widgets(self):
        workflow = (ROOT / ".github/workflows/reusable-windows.yml").read_text(encoding="utf-8")
        self.assertIn("prepare_widgets_free_qt.py", workflow)
        self.assertIn("--qt-prefix", workflow)
        self.assertIn("--expect-configure-failure", workflow)
        self.assertIn("Build Widgets-absent release profile", workflow)
        self.assertIn("record_widgets_free_release_evidence.py", workflow)
        self.assertIn('"-DVCPKG_INSTALLED_DIR=$env:VCPKG_INSTALLED_DIR"', workflow)

    def test_package_workflows_require_and_record_exact_source_sha(self):
        linux = (ROOT / ".github/workflows/LinuxInstall.yml").read_text(encoding="utf-8")
        windows = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn("./vcpkg/vcpkg integrate install", linux)
        self.assertNotIn("./vcpkg integrate install", linux)
        self.assertIn("libfontconfig1-dev", linux)
        self.assertIn("runs-on: ubuntu-22.04", linux)
        self.assertIn("VCPKG_DEFAULT_BINARY_CACHE", linux)
        self.assertIn("VCPKG_BINARY_SOURCES=clear;files", linux)
        self.assertIn("./vcpkg-binary-cache", linux)
        self.assertIn("cmake --build build --target LoopEditor PdfTool ProductQuickAccessibilitySmoke release_translations -j6", linux)
        self.assertNotIn("--target all", linux)
        self.assertNotIn("ctest --test-dir build", linux)
        self.assertIn("Deploy Qt runtime closure to staged install tree", windows)
        self.assertIn("windeployqt.exe", windows)
        self.assertIn("--no-compiler-runtime", windows)
        self.assertIn("--qmldir", windows)
        self.assertIn("windeployqt-$name.txt", windows)
        self.assertIn("LoopEditor.exe", windows)
        self.assertIn("Qml2Imports=qml", windows)
        self.assertIn('Join-Path $installBin "qt.conf"', windows)
        self.assertIn("build\\LoopEditor\\Loop\\Quick", windows)
        self.assertIn("plugins/sqldrivers", linux)
        self.assertIn("build/LoopEditor/Loop/Quick", linux)
        self.assertIn('-qmldir="$GITHUB_WORKSPACE/loop/LoopEditor/qml"', linux)
        self.assertIn("VCPKG_BINARY_SOURCES=clear;files", windows)
        self.assertIn("./vcpkg_installed", windows)
        self.assertIn("./vcpkg-binary-cache", windows)
        self.assertIn("cmake --build build --target LoopEditor PdfTool ProductQuickAccessibilitySmoke release_translations --config Release -j6", windows)
        self.assertNotIn("--target all", windows)
        self.assertNotIn("ctest --test-dir build", windows)
        self.assertIn("--appimage-extract-and-run", linux)
        self.assertIn("linuxdeployqt.txt", linux)
        for workflow in (linux, windows):
            self.assertIn("source_sha:", workflow)
            self.assertRegex(workflow, r"source_sha:\n\s+description:.*\n\s+required:\s+true")
            self.assertIn("ref: ${{ inputs.source_sha }}", workflow)
            self.assertIn("Verify exact source SHA", workflow)
            self.assertIn("LOOP_SOURCE_SHA", workflow)
            self.assertIn("inspect_package_dependencies.py", workflow)
            self.assertIn("source-sha", workflow)
        self.assertIn("--expected-architecture x86-64", linux)
        self.assertIn("--expected-architecture x64", windows)
        self.assertIn("loop-package-boundary-linux-evidence", linux)
        self.assertIn("loop-package-boundary-windows-evidence", windows)

    def test_windows_release_msi_is_x64_and_uses_64_bit_program_files(self):
        workflow = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn('Platform=x64', workflow)
        self.assertIn('-arch x64', workflow)
        self.assertNotIn('Platform=x86', workflow)
        self.assertNotIn('-arch x86', workflow)
        self.assertNotIn('ProgramFilesX86', workflow)
        self.assertIn('GetFolderPath("ProgramFiles")', workflow)

    def test_release_draft_pairs_evidence_and_keeps_it_out_of_assets(self):
        workflow = (ROOT / ".github/workflows/CreateReleaseDraft.yml").read_text(encoding="utf-8")
        self.assertIn("source_sha:", workflow)
        self.assertIn("ref: ${{ inputs.source_sha }}", workflow)
        self.assertIn("compare_package_boundary_evidence.py", workflow)
        self.assertIn("loop-package-boundary-linux-evidence", workflow)
        self.assertIn("loop-package-boundary-windows-evidence", workflow)
        self.assertIn('--commit "$EXPECTED_SOURCE_SHA"', workflow)
        self.assertIn("Exclude CI evidence from release assets", workflow)
        self.assertIn("source_sha", workflow)

    def test_release_wix_template_does_not_unconditionally_ship_widgets(self):
        product = (ROOT / "WixInstaller/Product.wxs.in").read_text(encoding="utf-8")
        cmake = (ROOT / "WixInstaller/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn('Component Id="cmpQt6Widgets"', product)
        self.assertNotIn('Component Id="cmpQt6PrintSupport"', product)
        self.assertIn("LOOP_WIX_QT_WIDGETS_COMPONENT", product)
        self.assertIn("LOOP_WIX_QT_PRINTSUPPORT_COMPONENT", product)
        self.assertIn("LOOP_WIX_QT_PRINTSUPPORT_DIRECTORY", product)
        self.assertNotIn('Component Id="cmpqmodernwindowsstyle"', product)
        self.assertIn("LOOP_WIX_QT_STYLES_COMPONENT", product)
        self.assertIn("LOOP_WIX_QT_STYLES_DIRECTORY", product)
        self.assertIn("if(LOOP_LOOP_DISTRIBUTION)", cmake)
        self.assertIn("LOOP_WIX_QT_PRINTSUPPORT_COMPONENT", cmake)
        self.assertIn("LOOP_WIX_QT_STYLES_COMPONENT", cmake)
        self.assertIn("LOOP_WIX_QT_STYLES_DIRECTORY", cmake)

    def test_release_profile_does_not_stage_qt_style_plugins(self):
        root = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("if(NOT LOOP_LOOP_DISTRIBUTION)", root)
        self.assertIn("plugins/styles/", root)

    def test_wix_package_uses_the_64_bit_program_files_directory(self):
        product = (ROOT / "WixInstaller/Product.wxs.in").read_text(encoding="utf-8")
        self.assertIn('Platform="x64"', product)
        self.assertIn('Directory Id="ProgramFiles64Folder"', product)

    def test_wix_solution_declares_only_x64_configurations(self):
        solution = (ROOT / "WixInstaller/LOOP.sln.in").read_text(encoding="utf-8")
        self.assertIn("Debug|x64 = Debug|x64", solution)
        self.assertIn("Release|x64 = Release|x64", solution)
        self.assertNotIn("Debug|x86", solution)
        self.assertNotIn("Release|x86", solution)

    def test_msi_smoke_scans_current_and_legacy_share_locations(self):
        smoke = (ROOT / "scripts/Invoke-MsiSmokeTest.ps1").read_text(encoding="utf-8")
        self.assertIn('(Join-Path $InstallDir "share\\loop")', smoke)
        self.assertIn('(Join-Path (Split-Path -Parent $InstallDir) "share\\loop")', smoke)


if __name__ == "__main__":
    unittest.main()
