#!/usr/bin/env bash
set -euo pipefail
install_tree="$1"
kit="$2"
package="$3"
source_sha="$4"
evidence="$5"
openbox > "$evidence/window-manager.txt" 2>&1 &
window_manager=$!
trap 'kill "$window_manager" 2>/dev/null || true' EXIT
for attempt in {1..50}; do
    if xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id'; then break; fi
    sleep 0.1
done
xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id'

pwsh scripts/run-product-quick-a11y-smoke.ps1 -BuildDir "$kit" -InstallTree "$install_tree" -Backend native -Platform xcb -SourceSha "$source_sha" -Package "$package" -EvidenceJson "$evidence/native.json"
pwsh scripts/run-product-quick-a11y-smoke.ps1 -BuildDir "$kit" -InstallTree "$install_tree" -Backend software -Platform xcb -SourceSha "$source_sha" -Package "$package" -EvidenceJson "$evidence/software.json"
pwsh scripts/run-installed-quick-a11y-atspi.ps1 -BuildDir "$kit" -InstallTree "$install_tree" -OutputDirectory "$evidence/atspi" -SourceSha "$source_sha" -Package "$package" -EvidenceJson "$evidence/native-accessibility.json"
python3 scripts/ci/verify_quick_accessibility_evidence.py --native "$evidence/native.json" --software "$evidence/software.json" --native-accessibility "$evidence/native-accessibility.json" --require-native-accessibility --source-sha "$source_sha" --install-tree "$install_tree" --package-boundary "$evidence/package-boundary.json"
