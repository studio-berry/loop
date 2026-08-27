#!/usr/bin/env bash
# Run libFuzzer harnesses: regression corpus first, then time-bounded mutation.
#
# Usage:
#   scripts/fuzz-run-targets.sh <build_dir> <seconds_per_target> [target ...]
#
# Environment:
#   LOOP_FUZZ_CORPUS_DIR  - corpus root (default: <repo>/Fuzz/corpus)
#   LOOP_FUZZ_SCRATCH_DIR - writable mutation corpus (default: <build_dir>/fuzz-corpus-scratch)

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <build_dir> <seconds_per_target> [target ...]" >&2
    exit 1
fi

BUILD_DIR="$1"
SECONDS_PER_TARGET="$2"
shift 2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CORPUS_DIR="${LOOP_FUZZ_CORPUS_DIR:-${REPO_ROOT}/Fuzz/corpus}"
SCRATCH_DIR="${LOOP_FUZZ_SCRATCH_DIR:-${BUILD_DIR}/fuzz-corpus-scratch}"

if [[ $# -gt 0 ]]; then
    TARGETS=("$@")
else
    TARGETS=(fuzz_pdf_parser fuzz_stream_filters fuzz_content_stream fuzz_images)
fi

count_seed_files() {
    local dir="$1"
    if [[ ! -d "${dir}" ]]; then
        echo 0
        return
    fi
    find "${dir}" -maxdepth 1 -type f ! -name '.gitkeep' | wc -l | tr -d ' '
}

for target in "${TARGETS[@]}"; do
    bin="${BUILD_DIR}/usr/bin/${target}"
    if [[ ! -x "${bin}" ]]; then
        echo "Missing fuzz binary: ${bin}" >&2
        exit 1
    fi

    harness_corpus="${CORPUS_DIR}/${target}"
    scratch_corpus="${SCRATCH_DIR}/${target}"
    mkdir -p "${scratch_corpus}"

    seed_count="$(count_seed_files "${harness_corpus}")"
    if [[ "${seed_count}" -gt 0 ]]; then
        echo "======== ${target} regression (${seed_count} seed(s)) ========"
        "${bin}" -runs=0 -print_final_stats=1 "${harness_corpus}"
    else
        echo "======== ${target} regression (no seeds) ========"
    fi

    echo "======== ${target} mutation (${SECONDS_PER_TARGET}s) ========"
    if [[ "${seed_count}" -gt 0 ]]; then
        "${bin}" -max_total_time="${SECONDS_PER_TARGET}" -print_final_stats=1 \
            "${scratch_corpus}" "${harness_corpus}"
    else
        "${bin}" -max_total_time="${SECONDS_PER_TARGET}" -print_final_stats=1 \
            "${scratch_corpus}"
    fi
done
