#!/usr/bin/env bash
# BSP-002 §3.3, §4.4 — agent Bash denylist (Claude Code PreToolUse / Cursor beforeShellExecution)
set -euo pipefail
here=$(dirname "$0")
cmd=$(python -c "import json,sys; d=json.load(sys.stdin); print(d.get('command') or d.get('tool_input',{}).get('command',''))")
deny() { echo "BLOCKED — $1" >&2; exit 2; }
[[ "$cmd" == *"--no-verify"* ]] && deny "BSP-002 §4.4: --no-verify bypasses required gates."
# Parses the command into shell segments instead of substring-matching it, so a
# push followed by an unrelated -f option (grep -f, rm -f) is not a force-push.
# Fails closed: a detector error denies the command.
python "$here/detect_force_push.py" "$cmd" \
  || deny "BSP-002 §3.3: force-push is blocked on protected branches."
branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
[[ "$cmd" =~ ^git[[:space:]]+commit ]] && [[ "$branch" =~ ^(main|master|stable)$ ]] \
  && deny "BSP-002 §3.3: commit directly on $branch. Create feature/<desc> first."
[[ "$cmd" =~ protection.*-X[[:space:]]+DELETE ]] && deny "BSP-002 §3.3: removing branch protection requires an exception under BSP-001 §8."
exit 0
