Category: fixed
Audience: developers
Breaking-Change: no
Summary: Repair release-package qualification metadata, scope fontconfig to Unix, fail Windows packaging on Sentry debug-file upload errors, install Linux gperf/vcpkg autotools dependencies, make Linux and Windows package trees self-describing for headless PdfTool and Quick startup smoke, bundle QtQuick.Controls into the Linux AppImage via linuxdeployqt qmldir scanning, stage CMake-emitted Loop QML module trees and copy only the real shared LoopLibQuickplugin artifact on Windows while leaving the static LoopEditorQuick plugin linked into LoopEditor, including WiX harvesting of built Loop QML modules, and authenticate immutable GitHub release-asset downloads in Linux packaging CI while retaining SHA-256 verification and curl retries.
