# Restore Quick GUI coverage: handler-coverage contract gate (epic #231)

Category: added
Audience: developers
Breaking-Change: no
Summary: `docs/shell-handler-coverage.json` records the KEEP and ABSORB commands the Quick shell has not bound a handler for yet (70 at b08e2183). `scripts/verify-command-catalog.py` now fails when a KEEP or ABSORB action is unbound without a ratchet entry, when a recorded gap stays after its handler lands, or when a baseline entry is not a KEEP or ABSORB command, so the count cannot regress and the list can only shrink; `--record-ratchet` rewrites the baseline from the contract. Negative-case tests in `scripts/ci/test_command_catalog.py` prove each rule can fail, the check runs as its own step in `ci.yml`'s source_integrity job, `scripts/agent/check-change.py` reports the check and its cases as whole-tree evidence, and `docs/LOOP_SHELL_CONTRACT.md` documents the gate.
