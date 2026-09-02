#!/usr/bin/env bash
# Verified-download helper for CI packaging steps.
#
# Used to fetch an external packaging tool and assert its SHA-256 before the
# binary is used. Supports two modes:
#
#   download_verified.sh --gh-asset <owner/repo> <asset-id> <output> <sha256>
#   download_verified.sh --url <url> <output> <sha256>
#
# --gh-asset resolves a GitHub release asset by its immutable asset id, so a
# future `continuous` version of the upstream release cannot silently change the
# artifact that lands in the build. --url is for versioned release archives.
#
# When GITHUB_TOKEN is set (GitHub Actions jobs provide it automatically), pass it
# to the Releases API so pinned public assets download reliably instead of
# failing with HTTP 403 from unauthenticated/rate-limited runner egress.

set -euo pipefail

temporary_output=""
cleanup() {
  if [[ -n "$temporary_output" ]]; then
    rm -f -- "$temporary_output"
  fi
}
trap cleanup EXIT

is_hex_digest() {
  [[ "$1" =~ ^[[:xdigit:]]{64}$ ]]
}

reject_mutable_url() {
  local normalized="${1,,}"
  case "$normalized" in
    *"/releases/download/continuous"*|*"/releases/download/latest"*|*"/releases/latest"*)
      echo "download_verified.sh: mutable release URL is forbidden: $1" >&2
      exit 2
      ;;
  esac
}

mode="$1"
shift

case "$mode" in
  --gh-asset)
    if [ "$#" -ne 4 ]; then
      echo "usage: download_verified.sh --gh-asset <owner/repo> <asset-id> <output> <sha256>" >&2
      exit 2
    fi
    repo="$1"
    asset_id="$2"
    output="$3"
    expected_sha="${4,,}"

    [[ "$repo" =~ ^[^/[:space:]]+/[^/[:space:]]+$ ]] || { echo "download_verified.sh: invalid GitHub repository: $repo" >&2; exit 2; }
    [[ "$asset_id" =~ ^[1-9][0-9]*$ ]] || { echo "download_verified.sh: asset id must be positive: $asset_id" >&2; exit 2; }
    is_hex_digest "$expected_sha" || { echo "download_verified.sh: expected SHA256 must be 64 hex characters" >&2; exit 2; }
    temporary_output="${output}.download.$$"

    # GitHub requires an Accept header to receive the raw binary payload.
    # asset-id downloads are immutable and version-carrying.
    curl_args=(
      --fail --location --retry 5 --retry-delay 2 --retry-all-errors
      -H "Accept: application/octet-stream"
      -H "X-GitHub-Api-Version: 2022-11-28"
    )
    if [[ -n "${GITHUB_TOKEN:-}" ]]; then
      curl_args+=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
    fi
    curl_args+=(
      "https://api.github.com/repos/${repo}/releases/assets/${asset_id}"
      -o "$temporary_output"
    )
    curl "${curl_args[@]}"
    ;;

  --url)
    if [ "$#" -ne 3 ]; then
      echo "usage: download_verified.sh --url <url> <output> <sha256>" >&2
      exit 2
    fi
    url="$1"
    output="$2"
    expected_sha="${3,,}"
    [[ "$url" == https://* ]] || { echo "download_verified.sh: URL must use HTTPS: $url" >&2; exit 2; }
    reject_mutable_url "$url"
    is_hex_digest "$expected_sha" || { echo "download_verified.sh: expected SHA256 must be 64 hex characters" >&2; exit 2; }
    temporary_output="${output}.download.$$"
    curl --fail --location "$url" -o "$temporary_output"
    ;;

  *)
    echo "download_verified.sh: unknown mode '$mode'" >&2
    echo "usage: download_verified.sh <--gh-asset|--url> ..." >&2
    exit 2
    ;;
esac

actual_sha="$(sha256sum "$temporary_output" | awk '{print $1}')"
if [ "$actual_sha" != "$expected_sha" ]; then
  echo "download_verified.sh: SHA256 mismatch for '$output'" >&2
  echo "  expected: $expected_sha" >&2
  echo "  actual:   $actual_sha" >&2
  exit 1
fi

mv -f -- "$temporary_output" "$output"
temporary_output=""
echo "download_verified.sh: verified $output (${actual_sha})"
