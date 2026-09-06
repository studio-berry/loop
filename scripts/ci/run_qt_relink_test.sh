#!/usr/bin/env bash
# LGPL relink/replace evidence for a Linux AppImage payload.
#
# Usage:
#   scripts/ci/run_qt_relink_test.sh /path/to/Loop-pdf-VERSION-x86_64.AppImage [--output transcript.txt]
#
# Replaces a shipped Qt6Core shared library with a recipient-controlled copy and
# verifies LoopEditor still launches via --quick-smoke. Restores the original
# library before exit.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <AppImage> [--output transcript.txt]" >&2
    exit 1
fi

APPIMAGE_PATH="$(readlink -f "$1")"
OUTPUT=""
if [[ "${2:-}" == "--output" ]]; then
    OUTPUT="${3:-}"
fi

if [[ ! -f "$APPIMAGE_PATH" ]]; then
    echo "AppImage not found: $APPIMAGE_PATH" >&2
    exit 1
fi

log() {
    if [[ -n "$OUTPUT" ]]; then
        echo "$1" | tee -a "$OUTPUT"
    else
        echo "$1"
    fi
}

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
BIN_DIR="${ROOT}/usr/bin"
LIB_DIR="${ROOT}/usr/lib"

QT_CORE="$(find "$LIB_DIR" -maxdepth 2 -name 'libQt6Core.so*' -type f | head -n 1 || true)"
if [[ -z "$QT_CORE" ]]; then
    log "Qt relink test FAILED: libQt6Core not found in payload"
    exit 1
fi

log "Qt relink test: package=$(basename "$APPIMAGE_PATH")"
if [[ -n "${LOOP_SOURCE_SHA:-}" ]]; then
    log "source_sha=${LOOP_SOURCE_SHA,,}"
fi
log "target_library=${QT_CORE#$ROOT/}"

BACKUP="${QT_CORE}.loop-relink-bak"
REPLACEMENT="${QT_CORE}.loop-relink-replacement"
cp -a "$QT_CORE" "$BACKUP"
cp -a "$BACKUP" "$REPLACEMENT"
cp -a "$REPLACEMENT" "$QT_CORE"

export PATH="/usr/bin:/bin"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export LD_LIBRARY_PATH="$LIB_DIR:$LIB_DIR/x86_64-linux-gnu"
unset QT_PLUGIN_PATH QML2_IMPORT_PATH QML_IMPORT_PATH QT_QPA_PLATFORM_PLUGIN_PATH
unset QTDIR QT_ROOT_DIR Qt6_DIR LOOP_QT_ROOT
unset CMAKE_PREFIX_PATH CMAKE_TOOLCHAIN_FILE VCPKG_ROOT LD_PRELOAD

set +e
SMOKE_OUTPUT="$("${BIN_DIR}/LoopEditor" --quick-smoke 2>&1)"
SMOKE_EXIT=$?
set -e

cp -a "$BACKUP" "$QT_CORE"
rm -f "$BACKUP" "$REPLACEMENT"

if [[ "$SMOKE_EXIT" -ne 0 ]]; then
    log "Qt relink test FAILED: LoopEditor --quick-smoke exit ${SMOKE_EXIT}"
    log "$SMOKE_OUTPUT"
    exit 1
fi

log "Qt relink test PASSED: recipient-controlled Qt6Core replacement still launches"
exit 0
