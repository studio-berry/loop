#!/usr/bin/env bash
# Validates a Loop-PDF AppImage on a clean machine (MIC-301 Linux half).
#
# Usage:
#   scripts/smoke-test-appimage.sh /path/to/Loop-pdf-VERSION-x86_64.AppImage [test.pdf] [--operator]
#
# Asserts the shipped layout resolves, runs PdfTool preflight against a fixture,
# and scans the tree for payloads the default V1 bundle must not ship (MIC-343).

set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <AppImage> [test.pdf] [--operator]" >&2
    exit 1
fi

APPIMAGE_PATH="$(readlink -f "$1")"
if [[ ! -f "$APPIMAGE_PATH" ]]; then
    echo "AppImage not found: $APPIMAGE_PATH" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TEST_PDF=""
OPERATOR_MODE=""
for argument in "${@:2}"; do
    if [[ "$argument" == "--operator" ]]; then
        if [[ -n "$OPERATOR_MODE" ]]; then
            echo "The --operator option may be supplied only once" >&2
            exit 1
        fi
        OPERATOR_MODE="--operator"
    elif [[ -z "$TEST_PDF" ]]; then
        TEST_PDF="$argument"
    else
        echo "Unknown option or duplicate test PDF: $argument" >&2
        exit 1
    fi
done
if [[ -z "$TEST_PDF" ]]; then
    TEST_PDF="${REPO_ROOT}/loop-preflight/testdata/fixtures/bleed-adequate.pdf"
fi
if [[ ! -f "$TEST_PDF" ]]; then
    echo "Test PDF not found: $TEST_PDF" >&2
    exit 1
fi

EXTRACT_ROOT="$(mktemp -d)"
cleanup() {
    rm -rf "$EXTRACT_ROOT"
}
trap cleanup EXIT

chmod +x "$APPIMAGE_PATH"
(
    cd "$EXTRACT_ROOT"
    "$APPIMAGE_PATH" --appimage-extract >/dev/null
)

ROOT="${EXTRACT_ROOT}/squashfs-root"
if [[ ! -d "$ROOT" ]]; then
    echo "AppImage extract failed: missing squashfs-root under $EXTRACT_ROOT" >&2
    exit 1
fi

BIN_DIR="${ROOT}/usr/bin"
LIB_DIR="${ROOT}/usr/lib"
PLUGINS_DIR="${ROOT}/usr/lib/loop"
PROFILES_DIR="${ROOT}/usr/share/loop/profiles"

assert_file() {
    local path="$1"
    local label="$2"
    if [[ ! -e "$path" ]]; then
        echo "Missing ${label}: ${path}" >&2
        exit 1
    fi
    echo "OK: ${label}"
}

echo "Smoke-testing AppImage at ${APPIMAGE_PATH}"
if [[ -n "${LOOP_SOURCE_SHA:-}" ]]; then
    if ! [[ "$LOOP_SOURCE_SHA" =~ ^[0-9a-fA-F]{40}$ ]]; then
        echo "LOOP_SOURCE_SHA must be a full 40-character Git SHA" >&2
        exit 1
    fi
    echo "Package source SHA: ${LOOP_SOURCE_SHA,,}"
fi

assert_file "${BIN_DIR}/LoopEditor" "Editor"
assert_file "${BIN_DIR}/PdfTool" "PdfTool"
assert_file "${PROFILES_DIR}/loop-default.json" "Default preflight profile"
assert_file "${PROFILES_DIR}/schemas/profile.schema.json" "Profile schema"
assert_file "${PROFILES_DIR}/schemas/report.schema.json" "Report schema"

OCR_PLUGIN="${PLUGINS_DIR}/libOcrPlugin.so"
if [[ -e "$OCR_PLUGIN" ]]; then
    echo "OcrPlugin found at ${OCR_PLUGIN}. V1 ships OCR as CLI-only (MIC-343)." >&2
    exit 1
fi
echo "OK: OcrPlugin absent (V1 CLI-only OCR surface, MIC-343)"

OCR_SIDECAR_HITS=()
while IFS= read -r hit; do
    OCR_SIDECAR_HITS+=("$hit")
done < <(find "$ROOT" -type f \( -name 'LoopOcrService' -o -name 'LoopOcrService.exe' \) 2>/dev/null)

if [[ "${#OCR_SIDECAR_HITS[@]}" -gt 0 ]]; then
    echo "LoopOcrService sidecar found in the V1 AppImage; OCR is CLI-only and the sidecar must not be bundled:" >&2
    printf '  %s\n' "${OCR_SIDECAR_HITS[@]}" >&2
    exit 1
fi
echo "OK: LoopOcrService sidecar absent (V1 CLI-only OCR surface)"

# The package must be self-contained. Remove developer Qt/toolchain search
# paths before every packaged process and do not inherit LD_LIBRARY_PATH.
export PATH="/usr/bin:/bin"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export LD_LIBRARY_PATH="$LIB_DIR:$LIB_DIR/x86_64-linux-gnu"
unset QT_PLUGIN_PATH QML2_IMPORT_PATH QML_IMPORT_PATH QT_QPA_PLATFORM_PLUGIN_PATH
unset QTDIR QT_ROOT_DIR Qt6_DIR LOOP_QT_ROOT
unset CMAKE_PREFIX_PATH CMAKE_TOOLCHAIN_FILE VCPKG_ROOT LD_PRELOAD

PDF_TOOL="${BIN_DIR}/PdfTool"
PROFILE_PATH="${PROFILES_DIR}/loop-default.json"

set +e
PREFLIGHT_OUTPUT="$("$PDF_TOOL" preflight "$TEST_PDF" --profile "$PROFILE_PATH" --console-format text 2>&1)"
PREFLIGHT_EXIT=$?
set -e

if [[ "$PREFLIGHT_EXIT" -ne 0 && "$PREFLIGHT_EXIT" -ne 1 ]]; then
    echo "PdfTool preflight failed with unexpected exit code ${PREFLIGHT_EXIT}:" >&2
    echo "$PREFLIGHT_OUTPUT" >&2
    exit 1
fi
echo "OK: PdfTool preflight completed (exit ${PREFLIGHT_EXIT})"

set +e
HELP_OUTPUT="$("$PDF_TOOL" help 2>&1)"
HELP_EXIT=$?
set -e
if [[ "$HELP_EXIT" -ne 0 ]]; then
    echo "PdfTool help failed with exit code ${HELP_EXIT}:" >&2
    echo "$HELP_OUTPUT" >&2
    exit 1
fi
echo "OK: PdfTool help"

run_quick_smoke() {
    local label="$1"
    local output
    local exit_code
    if [[ "$label" == "native" ]]; then
        unset QT_QUICK_BACKEND
    else
        export QT_QUICK_BACKEND=software
    fi
    set +e
    output="$("${BIN_DIR}/LoopEditor" --quick-smoke 2>&1)"
    exit_code=$?
    set -e
    if [[ "$exit_code" -ne 0 ]]; then
        echo "LoopEditor ${label} Quick startup failed with exit code ${exit_code}:" >&2
        echo "$output" >&2
        exit 1
    fi
    echo "OK: LoopEditor ${label} Quick startup"
}

run_quick_smoke native
run_quick_smoke software
unset QT_QUICK_BACKEND

if [[ "$OPERATOR_MODE" == "--operator" ]]; then
    set +e
    "${BIN_DIR}/LoopEditor" "$TEST_PDF" >"${EXTRACT_ROOT}/LoopEditor-operator.log" 2>&1 &
    EDITOR_PID=$!
    sleep 5
    if kill -0 "$EDITOR_PID" 2>/dev/null; then
        kill "$EDITOR_PID" 2>/dev/null || true
        wait "$EDITOR_PID" 2>/dev/null || true
        OPERATOR_EXIT=0
    else
        wait "$EDITOR_PID"
        OPERATOR_EXIT=$?
    fi
    set -e
    if [[ "$OPERATOR_EXIT" -ne 0 ]]; then
        echo "LoopEditor operator launch failed with exit code ${OPERATOR_EXIT}:" >&2
        cat "${EXTRACT_ROOT}/LoopEditor-operator.log" >&2
        exit 1
    fi
    echo "OK: LoopEditor operator launch remained alive for 5 seconds"
fi

# docs/PACKAGING_LICENSING.md: default bundle is C++/Qt only.
FORBIDDEN_HITS=()
while IFS= read -r hit; do
    FORBIDDEN_HITS+=("$hit")
done < <(find "$ROOT" -type f \( \
    -name 'python*' -o -name 'java' -o -name 'javaw' -o -name 'gs' -o -name 'gswin*.exe' \
    \) 2>/dev/null | head -n 20)

if [[ "${#FORBIDDEN_HITS[@]}" -gt 0 ]]; then
    echo "Forbidden payload found in the AppImage tree (docs/PACKAGING_LICENSING.md):" >&2
    printf '  %s\n' "${FORBIDDEN_HITS[@]}" >&2
    exit 1
fi
echo "OK: no Ghostscript / JRE / Python payload in the default bundle"

WIDGETS_HITS=()
while IFS= read -r hit; do
    WIDGETS_HITS+=("$hit")
done < <(find "$ROOT" -type f \( \
    -name 'Qt6Widgets.*' -o -name 'libQt6Widgets.so*' \
    -o -name 'Qt6QuickWidgets.*' -o -name 'libQt6QuickWidgets.so*' \
    -o -name 'Qt6PrintSupport.*' -o -name 'libQt6PrintSupport.so*' \
\) 2>/dev/null)

if [[ "${#WIDGETS_HITS[@]}" -gt 0 ]]; then
    echo "Forbidden Widgets-bound Qt payload found in the AppImage tree:" >&2
    printf '  %s\n' "${WIDGETS_HITS[@]}" >&2
    exit 1
fi
echo "OK: no Widgets-bound Qt payload in the release bundle"

echo "AppImage smoke test passed."
exit 0
