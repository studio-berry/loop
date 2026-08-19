# Loupe-PDF development environment variables.
# Usage (bash/zsh):
#   source scripts/dev-env.sh
#
# Optional overrides before sourcing:
#   export LOUPE_QT_VERSION=6.11.1
#   export LOUPE_VCPKG_ROOT=/path/to/vcpkg
#   export LOUPE_REPO_ROOT=/path/to/Loupe-pdf

if [[ -n "${BASH_SOURCE[0]:-}" && "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "Source this file instead of executing it:" >&2
    echo "  source scripts/dev-env.sh" >&2
    exit 1
fi

_loupe_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LOUPE_REPO_ROOT="${LOUPE_REPO_ROOT:-${_loupe_repo_root}}"

export LOUPE_QT_VERSION="${LOUPE_QT_VERSION:-6.11.1}"
export LOUPE_QT_ROOT="${LOUPE_QT_ROOT:-/opt/Qt/${LOUPE_QT_VERSION}/gcc_64}"
export QT_ROOT_DIR="${QT_ROOT_DIR:-${LOUPE_QT_ROOT}}"
export CMAKE_PREFIX_PATH="${LOUPE_QT_ROOT}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

export VCPKG_ROOT="${VCPKG_ROOT:-/opt/vcpkg}"
export VCPKG_INSTALLED_DIR="${VCPKG_INSTALLED_DIR:-/opt/vcpkg_installed}"
export VCPKG_OVERLAY_PORTS="${VCPKG_OVERLAY_PORTS:-${LOUPE_REPO_ROOT}/vcpkg/overlays/linux:${LOUPE_REPO_ROOT}/vcpkg/overlays/general}"

# GCC build (avoid misconfigured /usr/bin/c++ on some cloud images).
export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"

# Headless CLI default; unset or set QT_QPA_PLATFORM=xcb for GUI on DISPLAY.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

if [[ -d "${LOUPE_QT_ROOT}/lib" ]]; then
    case ":${LD_LIBRARY_PATH:-}:" in
        *":${LOUPE_QT_ROOT}/lib:"*) ;;
        *) export LD_LIBRARY_PATH="${LOUPE_QT_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" ;;
    esac
fi

export LOUPE_BUILD_DIR="${LOUPE_BUILD_DIR:-${LOUPE_REPO_ROOT}/build}"
export PATH="${LOUPE_BUILD_DIR}/usr/bin:${PATH}"

unset _loupe_repo_root
