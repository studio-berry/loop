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

set -euo pipefail

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
    expected_sha="$4"

    # GitHub requires an Accept header to receive the raw binary payload.
    # asset-id downloads are immutable and version-carrying.
    curl --fail --location \
      -H "Accept: application/octet-stream" \
      -H "X-GitHub-Api-Version: 2022-11-28" \
      "https://api.github.com/repos/${repo}/releases/assets/${asset_id}" \
      -o "$output"
    ;;

  --url)
    if [ "$#" -ne 3 ]; then
      echo "usage: download_verified.sh --url <url> <output> <sha256>" >&2
      exit 2
    fi
    url="$1"
    output="$2"
    expected_sha="$3"
    curl --fail --location "$url" -o "$output"
    ;;

  *)
    echo "download_verified.sh: unknown mode '$mode'" >&2
    echo "usage: download_verified.sh <--gh-asset|--url> ..." >&2
    exit 2
    ;;
esac

actual_sha="$(sha256sum "$output" | awk '{print $1}')"
if [ "$actual_sha" != "$expected_sha" ]; then
  echo "download_verified.sh: SHA256 mismatch for '$output'" >&2
  echo "  expected: $expected_sha" >&2
  echo "  actual:   $actual_sha" >&2
  rm -f "$output"
  exit 1
fi

echo "download_verified.sh: verified $output (${actual_sha})"