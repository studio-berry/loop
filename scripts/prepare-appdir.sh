#!/usr/bin/env bash
# Prepare a CMake install tree as an AppImage AppDir for appimagetool.
#
# linuxdeployqt used to copy the desktop entry and icon to the AppDir root and
# write AppRun. The Qt deploy-script closure handles libraries and plugins, but
# appimagetool still requires a top-level .desktop file (and AppRun at runtime).
#
# Usage: scripts/prepare-appdir.sh <install-dir>

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <install-dir>" >&2
    exit 1
fi

INSTALL_DIR="$(readlink -f "$1")"
DESKTOP_SRC="${INSTALL_DIR}/usr/share/applications/io.github.mberrys.Loop-pdf.desktop"

if [[ ! -d "$INSTALL_DIR" ]]; then
    echo "Install directory not found: $INSTALL_DIR" >&2
    exit 1
fi

if [[ ! -f "$DESKTOP_SRC" ]]; then
    echo "Desktop entry not found: $DESKTOP_SRC" >&2
    exit 1
fi

DESKTOP_NAME="$(basename "$DESKTOP_SRC")"
ICON_NAME="$(awk -F= '/^Icon=/{gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$DESKTOP_SRC")"
if [[ -z "$ICON_NAME" ]]; then
    echo "Desktop entry is missing Icon=: $DESKTOP_SRC" >&2
    exit 1
fi

ICON_SRC=""
ICON_EXT=""
for candidate in \
    "${INSTALL_DIR}/usr/share/icons/hicolor/scalable/apps/${ICON_NAME}.svg" \
    "${INSTALL_DIR}/usr/share/icons/hicolor/128x128/apps/${ICON_NAME}.png"; do
    if [[ -f "$candidate" ]]; then
        ICON_SRC="$candidate"
        ICON_EXT="${candidate##*.}"
        break
    fi
done

if [[ -z "$ICON_SRC" ]]; then
    echo "Icon for ${ICON_NAME} not found under ${INSTALL_DIR}/usr/share/icons" >&2
    exit 1
fi

cp "$DESKTOP_SRC" "${INSTALL_DIR}/${DESKTOP_NAME}"
cp "$ICON_SRC" "${INSTALL_DIR}/${ICON_NAME}.${ICON_EXT}"

# The Qt deploy script only needs QSQLITE for preflight history writes. Optional
# vendor SQL drivers reference libraries we do not ship and fail package-boundary
# inspection (libqsqlmimer.so -> libmimerapi.so, libqsqloci.so -> libclntsh).
sql_dir="${INSTALL_DIR}/plugins/sqldrivers"
if [[ -d "$sql_dir" ]]; then
    find "$sql_dir" -maxdepth 1 -type f ! -name 'libqsqlite.so' -delete
fi

cat > "${INSTALL_DIR}/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${HERE}/usr/bin/LoopEditor" "$@"
EOF
chmod +x "${INSTALL_DIR}/AppRun"

echo "Prepared AppDir under ${INSTALL_DIR} (${DESKTOP_NAME}, ${ICON_NAME}.${ICON_EXT}, AppRun)"
