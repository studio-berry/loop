#!/usr/bin/env bash
# Run MIC-304 libFuzzer harnesses locally with ASan/UBSan.
#
# Prerequisites:
#   - Clang with libFuzzer (LOUPE_BUILD_FUZZERS=ON)
#   - Qt + vcpkg deps (see scripts/setup-dev-env.sh)
#
# Usage:
#   ./scripts/fuzz-bughunt.sh [seconds_per_target]
#   ./scripts/fuzz-bughunt.sh 30 fuzz_images   # single target, 30s

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${LOUPE_FUZZ_BUILD_DIR:-${REPO_ROOT}/build-fuzz}"
SECONDS_PER_TARGET="${1:-120}"
shift || true

if [[ $# -gt 0 ]]; then
    TARGETS=("$@")
else
    TARGETS=()
fi

export LD_LIBRARY_PATH="${LOUPE_QT_ROOT:-/opt/Qt/6.11.1/gcc_64}/lib:${LD_LIBRARY_PATH:-}"

python3 "${REPO_ROOT}/scripts/ci/check_fuzz_corpus.py"

if [[ ${#TARGETS[@]} -gt 0 ]]; then
    "${REPO_ROOT}/scripts/fuzz-run-targets.sh" "${BUILD_DIR}" "${SECONDS_PER_TARGET}" "${TARGETS[@]}"
else
    "${REPO_ROOT}/scripts/fuzz-run-targets.sh" "${BUILD_DIR}" "${SECONDS_PER_TARGET}"
fi
