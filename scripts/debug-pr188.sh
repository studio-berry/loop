#!/usr/bin/env bash
# PR #188 debug harness — writes NDJSON to debug-b0e75b.log
set -euo pipefail

LOG_PATH="${1:-debug-b0e75b.log}"
PDFTOOL="${PDFTOOL:-build/usr/bin/PdfTool}"
SESSION_ID="b0e75b"
RUN_ID="pr188-cloud"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$REPO_ROOT"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

write_debug_log() {
    local hypothesis_id="$1"
    local location="$2"
    local message="$3"
    local data_json="$4"
    local ts
    ts=$(date +%s%3N)
    printf '{"sessionId":"%s","runId":"%s","hypothesisId":"%s","location":"%s","message":"%s","data":%s,"timestamp":%s}\n' \
        "$SESSION_ID" "$RUN_ID" "$hypothesis_id" "$location" "$message" "$data_json" "$ts" >> "$LOG_PATH"
}

invoke_pdftool_json() {
  local -a args=("$@")
  local stdout stderr exit_code
  set +e
  stdout=$("$PDFTOOL" "${args[@]}" 2> >(stderr=$(cat); echo -n "$stderr" >&2))
  exit_code=$?
  set -e
  echo "$exit_code"
  echo "$stdout"
}

write_debug_log "H0" "debug-pr188.sh:start" "debug harness started" \
    "{\"pdfTool\":\"$PDFTOOL\",\"exists\":$(test -x "$PDFTOOL" && echo true || echo false)}"

if [[ ! -x "$PDFTOOL" ]]; then
    write_debug_log "H0" "debug-pr188.sh:missing-binary" "PdfTool not found" "{\"path\":\"$PDFTOOL\"}"
    exit 2
fi

# H1: JSON envelope contract
mapfile -t help_out < <("$PDFTOOL" help --console-format json 2>/dev/null; echo "EXIT:$?")
help_exit="${help_out[-1]#EXIT:}"
help_stdout=$(printf '%s\n' "${help_out[@]:0:${#help_out[@]}-1}")
help_schema=$(printf '%s' "$help_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("schema_version",""))' 2>/dev/null || echo "")
help_envelope_exit=$(printf '%s' "$help_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("exit_code",""))' 2>/dev/null || echo "")
write_debug_log "H1" "PdfTool help json" "envelope smoke" \
    "{\"exitCode\":$help_exit,\"schemaVersion\":\"$help_schema\",\"envelopeExit\":$help_envelope_exit,\"exitMatches\":$(test "$help_exit" = "$help_envelope_exit" && echo true || echo false)}"

# H2: OCR partial failure exit code vs plugin whitelist (0/1 only)
MOCK_SIDECAR="loupe-ocr/tools/mock_ocr_sidecar.py"
FIXTURE="loupe-preflight/testdata/fixtures/image-dpi-low.pdf"
if [[ -f "$MOCK_SIDECAR" && -f "$FIXTURE" ]]; then
    export LOUPE_OCR_MOCK_MODE="malformed-json"
    mapfile -t ocr_out < <("$PDFTOOL" ocr "$FIXTURE" --console-format json --sidecar "$MOCK_SIDECAR" 2>/dev/null; echo "EXIT:$?")
    unset LOUPE_OCR_MOCK_MODE
    ocr_exit="${ocr_out[-1]#EXIT:}"
    ocr_stdout=$(printf '%s\n' "${ocr_out[@]:0:${#ocr_out[@]}-1}")
    ocr_envelope_exit=$(printf '%s' "$ocr_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("exit_code",""))' 2>/dev/null || echo "")
    ocr_status=$(printf '%s' "$ocr_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("status",""))' 2>/dev/null || echo "")
    plugin_ok=$(test "$ocr_exit" = "0" -o "$ocr_exit" = "1" && echo true || echo false)
    write_debug_log "H2" "PdfTool ocr malformed-json" "OCR partial failure contract" \
        "{\"processExit\":$ocr_exit,\"envelopeExit\":$ocr_envelope_exit,\"status\":\"$ocr_status\",\"pluginWouldAccept\":$plugin_ok,\"testExpectsExit1\":true}"
fi

# H3: Repair commit gate requires profile
BLEED_FIXTURE="loupe-preflight/testdata/fixtures/bleed-missing.pdf"
if [[ -f "$BLEED_FIXTURE" ]]; then
    mapfile -t repair_out < <("$PDFTOOL" repair "$BLEED_FIXTURE" --operation add-bleed --bleed_mm=3 --overwrite debug-pr188-out.pdf --console-format json 2>/dev/null; echo "EXIT:$?")
    repair_exit="${repair_out[-1]#EXIT:}"
    repair_stdout=$(printf '%s\n' "${repair_out[@]:0:${#repair_out[@]}-1}")
    repair_status=$(printf '%s' "$repair_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("status",""))' 2>/dev/null || echo "")
    rm -f debug-pr188-out.pdf
    write_debug_log "H3" "PdfTool repair no profile" "repair postflight gate" \
        "{\"processExit\":$repair_exit,\"status\":\"$repair_status\"}"
fi

# H4: Capabilities discovery deterministic
mapfile -t capA_out < <("$PDFTOOL" capabilities --console-format json 2>/dev/null; echo "EXIT:$?")
mapfile -t capB_out < <("$PDFTOOL" capabilities --console-format json 2>/dev/null; echo "EXIT:$?")
capA_stdout=$(printf '%s\n' "${capA_out[@]:0:${#capA_out[@]}-1}")
capB_stdout=$(printf '%s\n' "${capB_out[@]:0:${#capB_out[@]}-1}")
capA_exit="${capA_out[-1]#EXIT:}"
capB_exit="${capB_out[-1]#EXIT:}"
identical=$(test "$capA_stdout" = "$capB_stdout" && echo true || echo false)
cmd_count=$(printf '%s' "$capA_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d.get("data",{}).get("commands",[])))' 2>/dev/null || echo 0)
write_debug_log "H4" "PdfTool capabilities" "deterministic discovery" \
    "{\"exitA\":$capA_exit,\"exitB\":$capB_exit,\"identicalStdout\":$identical,\"commandCount\":$cmd_count}"

# H5: Preflight nested report boundary
if [[ -f "$BLEED_FIXTURE" ]]; then
    PROFILE="loupe-preflight/profiles/loupe-default.json"
    if [[ -f "$PROFILE" ]]; then
        mapfile -t pf_out < <("$PDFTOOL" preflight "$BLEED_FIXTURE" --profile "$PROFILE" --console-format json 2>/dev/null; echo "EXIT:$?")
        pf_exit="${pf_out[-1]#EXIT:}"
        pf_stdout=$(printf '%s\n' "${pf_out[@]:0:${#pf_out[@]}-1}")
        report_schema=$(printf '%s' "$pf_stdout" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("data",{}).get("report",{}).get("schema_version",""))' 2>/dev/null || echo "")
        write_debug_log "H5" "PdfTool preflight" "nested report schema" \
            "{\"processExit\":$pf_exit,\"reportSchema\":\"$report_schema\"}"
    fi
fi

write_debug_log "H0" "debug-pr188.sh:done" "debug harness finished" "{}"
