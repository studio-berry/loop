# ADR-012: A governed move command for the drag-commit hand-off

**Status:** proposed
**Implemented-at:** not implemented
**Last-verified:** 2026-10-03 @ 0f2f7599e3e17ce4e57bfdedfce85582c8f51115
**Superseded-by:** none
**Date:** 2026-10-03
**Deciders:** Loop owner (issue #104); interaction boundary owners

## Context

`InteractionController` emits exactly one `dragCompleted(DragSession)` per
completed drag and deliberately stops there. The Quick host is the owner and it
drops the session:

```cpp
// LoopEditor/editorhost.cpp:3600-3607
void EditorHost::onDragCompleted(pdfinteraction::DragSession session)
{
    Q_UNUSED(session);
    if (m_session->interaction())
    {
        m_session->interaction()->refreshOverlay();
    }
}
```

The signal is connected at `LoopEditor/editorhost.cpp:2963-2967`. The emission
site is `LoopLibInteraction/sources/interactioncontroller.cpp:343-351`:

```cpp
const std::optional<DragSession> session = m_state.completeDrag(token());
if (session.has_value())
{
    Q_EMIT dragCompleted(*session);
}
```

`InteractionState::completeDrag` (`LoopLibInteraction/sources/interactionstate.cpp:203-226`)
already enforces the two preconditions the issue names: a stale fence cancels
with `RevisionChanged` instead of committing, and a sub-threshold gesture
returns `std::nullopt`. So the defect is not the gesture machinery; it is that
the one consumer of the completed session discards it.

The contract already promises the fix route. `docs/INTERACTION_CONTRACT.md:33-34`:

> Commit is not this layer's. A completed drag is emitted as a `DragSession`; the
> owner routes it through P4-S2's `CommandCatalog`, which stays the only mutation
> path.

`CommandCatalog` is documented as "the one command registry" whose IDs are "the
Editor action IDs recorded in `docs/loop-shell-actions.json`; a command ID that
is not in that file does not exist"
(`LoopLibInteraction/sources/commanddescriptor.h:35-39`). No action ID in that
file is a move or translate; all 107 are navigation, create, color, bookmark, or
render actions. The named command was never added.

This is a contract change, not a patch, for three independent reasons:

1. A new command ID changes `expected_action_count` (currently `107`,
   `docs/loop-shell-actions.json:5`). The count is asserted in
   `scripts/verify-command-catalog.py:129-133` and
   `scripts/verify-loop-shell-contract.py:106-109`, and the accepting schema is
   protected (`docs/schemas/loop-shell-actions.schema.json`).
2. The mutation for any document-truth-bearing target lands in
   `LoopLibCore/sources/**`, listed under `protected_paths` in
   `agent-policy.json`.
3. `AGENTS.md:38` — "Do not invent a public contract when a protected interface,
   schema, persistence format, central type, or root build contract must change;
   stop and report the contract change."

`docs/QUICK_CANVAS_EDITOR_GAPS.md` (commit `8362ae45`) already records the gap as
a finding. This ADR is the decision package the owner asked for: it answers the
four open questions in issue #104, names a recommendation with the alternatives
it rejects, and states the exact deltas that recommendation would require.

### What a completed drag actually addresses

The hit-test vocabulary is `InteractionTargetKind` — `DragHandle`, `Finding`,
`Guide`, `PageBox`, `Page`, `None`
(`LoopLibInteraction/sources/interactiontarget.h:46-66`). An `InteractionTarget`
is a name (`kind`, `pageIndex`, `id`) plus page-space `pageBounds`; it carries no
`PDFObjectReference` (`interactiontarget.h:77-92`). Two Core-backed sources ship:
`EvidenceHitTestSource` over `pdf::PDFEvidenceRecord` (`id`, 1-based `page`,
page-space `geometry` — `LoopLibCore/sources/pdfevidencegraph.h:55-68`) and
`PageBoxHitTestSource` over media/crop/bleed/trim/art (`docs/INTERACTION_CONTRACT.md`,
"Hit testing").

`DragSession` (`LoopLibInteraction/sources/interactionstate.h:78-108`) captures
`target`, `originPx`, `originPagePoint`, `grabOffset`, `currentPx`, `pageDelta`,
`button`, `modifiers`, `exceededThreshold`, and presentation-only
`previewPageBounds`. It does **not** carry the `RevisionFencedToken` the gesture
was begun against; only `InteractionState` holds it, and `completeDrag` consumes
it at emission. That omission matters for Q3 below.

The unresolved semantic that gates Q1 and Q2: of the current target kinds, only
`PageBox` unambiguously names document truth (page geometry) that a move would
edit. A `Finding`'s geometry is a derived `PDFEvidenceRecord`, and `Guide` is
presentation state; moving either is not a document edit and should not produce
a revision. This ADR therefore scopes the document mutation to page boxes and
records what must be decided before findings or guides are draggable.

## The four open questions

### Q1 — Command identity and granularity

**Candidates**

| Option | Shape | Admissible? |
| --- | --- | --- |
| A. One kind-agnostic `actionMoveSelection` | single ID; `targetKind` dispatches in the handler | Yes |
| B. Per-kind IDs (`actionMovePageBox`, `actionMoveFinding`, `actionMoveText`, …) | one ID per `InteractionTargetKind` | Technically, but rejected |
| C. Route the session to Core without a catalog command | handler calls the operation directly | **No** — `docs/INTERACTION_CONTRACT.md:33-34` makes the catalog the only mutation path; `interactioncontroller.h:63-66` forbids a second one |

**Recommendation: A.** One ID keeps the catalog a single list, keeps the count
delta to +1, and keeps the handler's dispatch explicit on a typed
`InteractionTargetKind`. Option B multiplies `expected_action_count` by the
number of draggable kinds and forces each ID through its own capability
classification and handler registration for no added safety — the kind is
already a field the handler must switch on either way. Option C is ruled out by
the contract regardless of how convenient it is.

### Q2 — Payload shape

The protected schema admits only `string`, `integer`, `number`, `boolean`
parameter values (`docs/schemas/loop-shell-actions.schema.json:55`).

| Option | Payload | Admissible? |
| --- | --- | --- |
| A. Page-space delta + target identity | `targetKind:string`, `targetId:string`, `page:integer`, `dx:number`, `dy:number` | Yes |
| B. Affine transform matrix | an object/array parameter | **No** — no such parameter type exists; adding one edits the protected schema |
| C. Serialized `DragSession` | an object parameter | **No** — same schema block, and it couples the command contract to an interaction-layer value type |

**Recommendation: A.** `DragSession` already holds everything Option A needs —
`target.kind`, `target.id`, `target.pageIndex`, and `pageDelta` (issue #141 AC4).
The handler reads those and submits five scalars. Option B is the "correct"
geometry answer in the abstract, but it cannot be expressed without editing
`docs/schemas/loop-shell-actions.schema.json`, which is protected and explicitly
out of scope; a general rotate/scale vocabulary is a separate decision. Option C
would make the catalog's parameter contract depend on a transient interaction
struct, which the catalog's own docs reject ("There is deliberately no generic
'invoke any QObject method' route", `commandcatalog.h`).

`dx`/`dy` are the page-space translation `pageDelta`, not the preview
`previewPageBounds` — the preview is presentation-only
(`interactionstate.h:106-108`).

### Q3 — Undoability and revision fencing

**Undoability.** Two histories exist and must not be conflated.
`docs/OPERATION_HISTORY.md` states that "Undo/redo remains an in-memory
interaction feature" while the operation history is "an append-only Core service
for production workflows", and that "Every accepted or rolled-back event creates
one `PDFRollbackPoint`". Every governed Core mutation already appends
`FixApplied` events and enforces retention (the add-bleed path:
`PdfTool/pdftooladdbleed.cpp` `appendAddBleedProvenance`, `execution.operationId
= "add-bleed"`).

- **Recommended:** the move is **undoable by construction** through the same
  append-only operation history — the accepted event yields a rollback point and
  advances the document revision. This is the governed shape every other Core
  mutation uses.
- **Rejected:** a fire-and-forget mutation with no history event. It would be
  the only Core write in the product with no provenance, breaking invariant I08
  ("Provenance events are append-only", `docs/architecture-invariants.json`) in
  spirit and leaving the drag unrollback-able.

**Revision fencing.** `docs/REVISION_CONTEXT.md`: `PDFDocumentContext` is the
revision authority, and "Every cache entry or asynchronous result that can
outlive a mutation carries a `PDFRevisionIdentity` … Consumers must compare the
complete value … A mismatch is discarded". `DocumentFacade::currentRevision()`
(`LoopLibInteraction/sources/documentfacade.h:213`) is the read-only view of that
fence. Invariant I07 pins the rule ("Stale revision results are discarded and
are not current").

The gap: `InteractionState::completeDrag` checks the fence **at emission**
(`interactionstate.cpp:203-213`), but the emitted `DragSession` drops the token,
so between emission and handler invocation there is no fence the handler can
re-check. **Recommendation: make the command participate in the same fence by
carrying the gesture's originating revision into the mutation path and having the
Core transaction reject a stale commit** (the repair transaction already carries
revision/plan identity — `PdfTool/pdftoolrepair.cpp:328` computes `planDigest`
from the source digest). This requires `DragSession` to expose the token it was
completed against; that is an interaction-layer delta, not a protected one.

### Q4 — Interaction with `actionUndo` / `actionRedo`

Both are `declared` / `unclassified` with no handler in the Quick shell:
`docs/loop-shell-actions.json:1045` (`actionRedo`) and `:1459` (`actionUndo`);
`ShellToolBar.qml:107-117` invokes them and `LoopEditor/editorhost.cpp:233-234`
only routes their label group. They are inert today, exactly as the drag commit
is.

**Recommendation:** do **not** couple the two. The move command should produce
the operation-history event and rollback point that an undo affordance *will*
consume, but wiring `actionUndo`/`actionRedo` to that history is a separate
decision with its own scope (it would also have to decide redo semantics the
append-only chain does not currently express). Claiming the move command is
"undoable" in the UI before `actionUndo` has a handler would repeat the
inert-control defect this ADR is documenting. State the move as undoable by
construction through history; leave the buttons as they are until a separate
decision wires them.

## Recommendation

Adopt **Option A for Q1, Option A for Q2, the governed-history/undoable answer
for Q3, and decoupling from `actionUndo`/`actionRedo` for Q4**: one
`actionMoveSelection` command carrying `targetKind`, `targetId`, `page`, `dx`,
`dy`, committing through a **new governed Core repair operation** that follows
the existing add-bleed shape, participating in the existing revision fence by
carrying the originating revision.

The recommendation is **conditional** on the owner settling the scope question
stated in Context: as of this base only `PageBox` has document truth to move.
The slice below is deliberately scoped to page boxes; findings, guides, and text
are out of scope until their geometry semantics are decided.

## Exact contract deltas the recommendation requires

1. **Action-schema entry.** Add one action to `docs/loop-shell-actions.json`:
   - `id: "actionMoveSelection"`, `disposition: "KEEP"`, `target: "Document"`,
     `menu_group: "Edit"`.
   - `command.label_key: "command.actionMoveSelection.label"` (pattern
     `^command\.action[A-Za-z0-9_]+\.label$`, schema line 78-79).
   - `parameters`: five entries of the existing scalar types only —
     `targetKind:string`, `targetId:string`, `page:integer`, `dx:number`,
     `dy:number` — all `required: true`.
   - `capability: "document.modify"` (already in the enum); `cancellable: false`.
   - `availability: "implemented"`.
2. **`expected_action_count` 107 → 108** (`docs/loop-shell-actions.json:5`).
   Satisfies the assertions at `scripts/verify-command-catalog.py:129-133` and
   `scripts/verify-loop-shell-contract.py:106-109`.
3. **Implemented-set registration.** Add `actionMoveSelection` to
   `SHELL_IMPLEMENTED_COMMANDS` in `scripts/verify-command-catalog.py:59-72`
   (the handler is registered by `EditorHost`, like the other shell commands).
   Without this, `check_implemented_set` reports "implemented without a
   registered handler"; without `availability: implemented` the parameterized
   command is rejected ("a declared command cannot promise parameters it never
   reads").
4. **New Core operation's public surface** (lands in protected
   `LoopLibCore/sources/**`, so this is the part the owner authorizes). Follow
   `PDFRepairOperation` (`LoopLibCore/sources/pdfrepairoperation.h:169-232`) and
   register it beside the add-bleed primitive
   (`LoopLibCore/sources/pdfrepairprimitives.cpp:131-141`, registered at
   `:632`):
   - `id()` — e.g. `"translate-page-box"`, `version()` 1.
   - `risk()` — `Low` or `Medium` (a box translation is not destructive).
   - `domains()` — `PDFRepairDomain::PageGeometry` (plus `Structure` if the
     metadata update qualifies).
   - `savePolicy()` — `saveAsNewArtifact(...)`, matching add-bleed's
     "preserve the trusted source" policy.
   - `impact()` — declare domains/`allPages`/`impactComplete` explicitly. An
     undeclared impact forces full revalidation (`docs/OPERATION_IMPACT.md`), so
     the operation must name its scope rather than inherit the conservative
     default.
   - `parameterSchema()` — JSON Schema fragment for `targetId`, `page`, `dx`,
     `dy`.
   - `analyze()` → `PDFRepairPlan` with `targets`, and `apply()` → `PDFRepairResult`;
     the existing `PDFRepairTransaction` (`add` → `analyze` → `apply` →
     `validateCandidate`) and plan-digest/revalidation path
     (`PdfTool/pdftoolrepair.cpp:310-453`, `:754-784`) are reused, not
     reinvented.
   - Its descriptor joins the generated correction-operation catalog
     (`docs/generated/correction-operation-catalog.json`) via
     `scripts/generate-architecture-catalogs.py`.
5. **Handler registration.** `EditorHost` registers the command's
   `CommandCatalog::Handler` in `registerFeatureHandlers`
   (`LoopEditor/editorhost.cpp:2993-3006`), maps `DragSession` fields to the five
   parameters, and invokes through the catalog — the pattern already used for
   the shell commands. The handler rejects a stale fence with a typed error and
   `finishInvocation(..., Failed, ...)`; it never silently drops.
6. **Revision-fence participation.** Extend `DragSession`
   (`LoopLibInteraction/sources/interactionstate.h:78-108`) to expose the
   `RevisionFencedToken` it was completed against, so the handler and/or the
   Core transaction can compare it with `DocumentFacade::currentRevision()`
   before mutating and fail closed on mismatch (I07).
7. **Undo participation.** The accepted operation appends operation-history
   events and produces a rollback point, consistent with
   `docs/OPERATION_HISTORY.md`; `actionUndo`/`actionRedo` remain unchanged by
   this recommendation.

## Cost and risk

| Option | Cost | Risk |
| --- | --- | --- |
| A (recommended) | one schema entry + count bump, one verify-set edit, one Core operation, one handler, one `DragSession` field; touches protected `LoopLibCore/sources/**` | Medium — real document mutation; mitigated by reuse of the existing repair/transaction/revalidation shape and a declared impact |
| B per-kind commands | one entry, count bump, capability and handler per kind | Higher maintenance for no behavior gain; each new kind re-opens the schema/count surface |
| B matrix payload | edits the protected `docs/schemas/loop-shell-actions.schema.json` (new parameter type) | Blocked by policy; broad schema change for a feature scoped to translate-only |
| C bypass catalog | no schema/count delta | Blocked by `docs/INTERACTION_CONTRACT.md:33-34`; creates a second mutation path the controller contract forbids |
| Non-undoable mutation | no history event | Violates I08 provenance expectations; drag becomes the only unrollback-able Core write |

## Smallest implementation slice that would prove it

1. Add `actionMoveSelection` (five scalar parameters, `document.modify`,
   implemented) and bump `expected_action_count` to 108; register it in
   `SHELL_IMPLEMENTED_COMMANDS`. Prove with `scripts/verify-command-catalog.py`
   and `scripts/verify-loop-shell-contract.py`.
2. Add the `translate-page-box` `PDFRepairOperation` in LoopLibCore with a
   declared impact and parameter schema; prove with a `core`-boundary test
   (`UnitTestsRepairOperation` / `UnitTestsRepairDiff` family) that a page-box
   translation produces one expected change, that a null/undeclared impact is
   refused, and that the plan digest is stable.
3. Register the handler in `EditorHost`, map `DragSession` → five parameters,
   and reject a stale revision with a typed error. Prove with
   `UnitTestsEditorHost`: a completed drag invokes exactly one command; a
   sub-threshold gesture invokes none; each of the nine
   `InteractionCancelReason` values commits nothing; a fence mismatch fails
   closed.
4. Add the `DragSession` revision accessor and assert in
   `UnitTestsInteractionController` that the completed session reports the
   token it was fenced against.

## Local verification limit

No build or test lane was run for this ADR. `build-local/` does not exist and
`C:/.dev/repos/loop/.local-vcpkg/` (the `CMAKE_TOOLCHAIN_FILE` referenced by
every cache under `C:/.dev/build/`) is absent; restoring vcpkg and configuring
are approval-required under `agent-policy.json`. This is a documentation-only
decision package: every claim cites the file and line it was read from. The
documentation subsystem's binding lane is
`scripts/generate-architecture-catalogs.py`; the interaction, core, quick, and
pdftool lanes that the implementation slice would exercise were **not** run and
are listed as unresolved in the evidence manifest.

Refs #104, #141
