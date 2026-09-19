# Preflight workflow Stage 02 — pinpoint and inspect acceptance

The existing Core report, finding model, Inspector, viewport and overlay builder remain authoritative. This stage closes **navigation and presentation integration defects**, not the separate Stage 01 restriction implementations or Stage 03 governed repair path.

## Retained evidence versus current navigation

A stale report is retained for operator review. It does **not** remain a current canvas hit-test source, focused overlay, selected finding or corrective-operation entry point. The Editor gates canvas projection on a completed current preflight state (pass, findings or incomplete). Starting a rerun also hides the previous projection until a new current result is accepted. A cancelled/invalid run cannot turn the retained report into an unverified current target.

The authority order for selecting a finding is: verify the controller's navigation request and current document revision, bind Inspector facts from the report, then select the finding and navigate through the existing canvas adapter. A rejected request clears the previous finding presentation; it cannot leave the Inspector claiming the previous selected object remains current.

## Targeting capability

The check-owned `FindingTargetingCapabilityRegistry` determines whether a report finding is an overlay/hit-test target. An unknown check stays visible in the report and can take the operator to its page, but does not gain object targeting or evidence overlays merely because it contains coordinates. Report-backed pages, object IDs and bounds remain the only source of object-location claims.

The Inspector explicitly states when page or object targeting is unavailable, including missing report IDs or geometry. No dictionary excerpts, DPI, layer, ink or other object facts are fetched from a second backend for presentation. Check status, incomplete/budget reasons and corrective availability are still projected from report data and the implemented-fixup registry.

## Acceptance evidence

- `UnitTestsFindingNavigation`: stale revision takes precedence over capability; invalid and unregistered requests clear old focus, mode and evidence.
- `UnitTestsPreflightInteraction`: stale overlays are suppressed without deleting report rows; unsupported checks cannot acquire an object hit target; incomplete-budget Inspector facts do not invent object fields.
- `UnitTestsShellInspectorDispatch`: select a real report finding, mark profile stale, verify retained report cannot select Inspector or move canvas, then accept a new run and restore current navigation; unknown check falls back to page without an object marker; PDF bytes remain unchanged.

Run the affected Qt targets and `python scripts/agent/check-change.py --base origin/dev`. Full user-flow approval of Stage 02 also depends on Stage 01's still-open restriction qualification and the subsequent recheck and sign-off stages. This PR does not close already-completed #127/#195/#196 again.
