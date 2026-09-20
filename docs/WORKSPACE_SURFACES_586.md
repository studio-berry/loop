# Workspace surfaces (#586)

The shell's rail used to be honest about what it did not have: #560 made every
destination declare whether it was ready, and the Production Preview, Pages /
Production and Inspect entries rendered `WorkspacePlaceholderPane`. #586 replaces
those placeholders with real surfaces assembled from the 0.3.0-A/B/C contracts.
They are presentation and operator intent: no PDF truth, no mutation, no second
revision or approval model.

The surfaces are `LoopEditor/qml/ProductionPreviewPane.qml`,
`PagesProductionPane.qml`, `InspectPane.qml` and the governed-correction sections
of `ActionListPane.qml`, plus the shared `StateBadge.qml`. `EditorHost` exposes
their state; QML renders it and forwards intent.

## Fix — the governed-correction route

`ActionListPane` is the Fix workspace. It presents the run the shell already
owns (`pdfinteraction::ActionListController`) as an operator lifecycle:

| State | Meaning |
| --- | --- |
| `idle` | Nothing is planned for this document. |
| `planned` | A recipe is being validated or planned. |
| `preview-ready` | A plan exists for the current revision, with its preview, and no review decision has been recorded. |
| `approved` | The operator approved that exact plan digest for that revision; execution is armed. |
| `executing` | The approved plan is running. |
| `succeeded` | The correction was executed, rechecked and published. |
| `stale` | The plan or preview belongs to a different document revision. |
| `rejected` | The operator rejected the plan; the plan stays visible for inspection. |
| `cancelled` | The run was cancelled; nothing was published. |
| `failed` | The run failed. |

`EditorHost::fixLifecycleStateName()` is the only projection, and
`pdfquick::tokens::resolveFixLifecycleStateVisual()` gives each state its own
kind, colour role, shape and spoken name, so no two states are read apart by
colour alone.

The lifecycle is not a second approval model. The draft review decision
(approve / reject / replan) is the operator's intent for the plan on screen,
bound to the digest the host recorded: `fixExecutionArmed()` is true only while
that digest is the current plan for the current revision. The approval that
authorises a publication is still Core's plan-bound approval, built by
`ActionListRunSubmitter` from the confirmed plan digest.

Beyond the recipe editor and step list, the surface carries:

- **Plan identity** — document key and revision, planned revision, source digest,
  recipe id/hash, plan digest, effective profile digest, the reviewed digest and
  whether the plan still matches the open revision.
- **Preview** — the technical and visual preview status of each step, the
  candidate digest, changed pages and an explicit incomplete flag.
- **Recheck** — the run status, the postflight outcome, and the finding delta
  (cleared, remaining, introduced, not fully rechecked).
- **Sign off** — Core's governed status, plan digest, published digest and the
  certified-preflight state.
- **Recorded revisions** — the document's rollback points, with an explicit
  confirmation before `requestFixRollback()` writes a new sibling revision and
  appends a rolled-back event through Core. Existing history is never rewritten
  and the open document is never overwritten.

## Inspect — the contextual inspector

`InspectPane` renders `InspectorModel`. Selecting a page, image, finding or
separation gives that selection's facts; selecting a planned or executed step
(`inspectActionListStep`) gives an `operation` selection carrying operation id
and version, step status, target, resolved parameters, impact domains, risk,
save policy with its rationale, postflight requirement, unresolved risk,
approval state, output identity, finding delta, recipe id/hash, plan digest,
source digest and whether the plan still matches the revision.

Its one affordance forwards the existing corrective-operation intent
(`InspectorModel::requestCorrectiveOperation`), which the host turns into a
recipe selection; the pane itself applies nothing.

## Production Preview — identity-bound before/after

`ProductionPreviewPane` presents the production/preview state, the current page's
render fidelity, the plate/ink model, and the technical/visual preview DTOs of the
planned or executed candidate. It never renders a second PDF path: the canvas
remains the single render surface, which is how this stays coordinated with #163
rather than racing it.

Its first job is honesty about identity. `previewIdentity` carries the document
key, revision, production state and the preview's own summary; when the preview or
the plan no longer describes the open revision, `previewStaleReason` is non-empty,
the pane says **Not current evidence** and names the reason, and the correction
preview says the same in its own block. A stale render is never presented as
current approval evidence.

## Pages / Production

`PagesProductionPane` presents the page inventory (page number, size in
millimetres, rotation), the document facts that matter for production, the
production state, and the existing page-geometry and assembly commands
(`actionPageGeometry`, `actionInsertPageNumbers`, `actionFirstPageOnRightSide`,
the page-layout commands) through the command catalog. It adds no route of its
own; activating a page row uses the same navigation the rest of the shell does.

## Compare

Compare remains a visible but disabled destination, per the #560 decision: the
placeholder pane is still what the stack holds for it, `isWorkspaceEnabled(
Compare)` is false, and the accessibility smoke asserts that entering it is
refused rather than that it renders something.

## A crash the surfaces exposed

Wiring the Fix route surfaced a real defect on the way in. `ActionListCatalog`
identifies a recipe by planning it against a default-constructed `PDFDocument`,
and the plan's first step is `sourceSha256ForActionList()`, which serialised that
document to hash it. The serialiser dereferences storage the default document
never initialises, so **any** valid recipe in the recipes directory took the
editor down at start-up, and importing one did too. Nothing covered it: the
recipes directory is empty in every test and smoke environment.

`sourceSha256ForActionList()` now refuses a document that carries neither source
bytes nor pages, so the plan fails with the existing source-identity diagnostic
instead of crashing. The catalog then reads the recipe hash off that result:
`recipeHash` covers the recipe alone and the executor sets it before it needs a
source, while the document-bound plan digest is only produced by a real run.
Without that second change every recipe would have been permanently
un-plannable, because the shell gates planning on a non-empty recipe hash.

`UnitTestsEditorHost::fixReviewBindsToThePlannedDigestAndTheCurrentRevision`
imports a recipe and plans it, which is the regression test for both halves.

## Verification

- `UnitTestsEditorHost` — the idle lifecycle and every fail-closed guard (nothing
  is approved, rejected, executed, inspected or rolled back before a plan exists);
  the review decision binding to the plan digest and the revision; rejected and
  replanned transitions; the stale transition after a revision change, where the
  approval stops arming execution and cannot be re-made against the superseded
  digest; rollback points read from a real recorded history, the refusal of an
  unknown revision, and the new sibling revision plus rolled-back event.
- `UnitTestsLoopStateVisual` — the lifecycle mapping table, that every state
  carries a distinct shape and spoken name, and that no lifecycle state claims a
  pass.
- `ProductQuickAccessibilitySmoke` (Windows and Linux) — every enabled
  destination resolves to its real pane with a screen-reader name and role, the
  disabled destination cannot be entered, the Fix lifecycle exposes its shape and
  words, and its review controls are present and reachable.
- `scripts/ci/check_qml_mirror_parity.py` — the smoke QML stays byte-identical to
  the product QML, so the accessibility evidence describes what ships.
- `scripts/ci/check_preflight_truth_source.py` — the GUI layers call no mutator,
  writer, artifact store or `PDFRepairTransaction::apply()`; new panes join that
  scan automatically.
