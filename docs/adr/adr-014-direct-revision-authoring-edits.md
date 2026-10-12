# ADR-014: Direct revision for interactive authoring edits

**Status:** accepted
**Implemented-at:** issue #231 PR 2 (`gh-231-quick-mutations`: #233, #234, #242)
**Last-verified:** 2026-10-11 @ 4e58124c357a85a61b76376fa6fdc038ab42ca1f
**Superseded-by:** none
**Date:** 2026-10-11
**Deciders:** Loop owner (epic #231); interaction and Core boundary owners

## Context

The epic's second PR group (#233 bookmarks and links, #234 annotation authoring,
#242 forms, attachments, and metadata) turns the Quick shell from a read-only
surface into one that edits the document. #234 makes the first question a
prerequisite: *"Decide first how an annotation edit relates to governed
operations (direct revision vs. planned operation) and record it, as ADR-012 did
for drag commit."* This ADR answers that question for the whole authoring
family, because all three issues demand the same property — *"document
mutations routed through `CommandCatalog` into Core and produce a new revision"*
(#233, #242; #234 says "created through `CommandCatalog` into Core").

The product has exactly one mutation path today, and it is the governed one.
`docs/INTERACTION_CONTRACT.md:33-34` states it:

> Commit is not this layer's. A completed drag is emitted as a `DragSession`; the
> owner routes it through P4-S2's `CommandCatalog`, which stays the only mutation
> path.

ADR-012 resolved the *geometry* case by routing the drag commit through the Fix
workspace: the command binds parameters for a recipe that offers
`translate-page-box`, and *"Plan, approval and execute stay the Fix workspace's,
so the command catalog is still the only route in and the Fix workspace is still
the only place a document changes."* That answer fits a production fix — a page
box is document truth whose change belongs to a reviewed plan. It does not fit
authoring: an operator drawing a highlight, adding a bookmark, or typing into a
form field is composing content, not proposing a repair, and a plan/approve
round trip per edit would make the Fix workspace the authoring surface.

What exists to build on:

- **Command surface.** `docs/loop-shell-actions.json` declares 109 actions
  (`expected_action_count: 109`). 34 of the commands these three issues name are
  already declared but unbound — 16 bookmark/link ids (#233), 18 annotation ids
  (#234), and `actionSend_by_E_Mail` (#242) — and they are recorded in the
  handler-coverage ratchet (`docs/shell-handler-coverage.json`) that PR 1
  landed. The form, attachment, and metadata families have **no command ids at
  all**; only `actionProperties` and `actionSend_by_E_Mail` exist in those
  neighbourhoods. #242's command surface therefore has to be created, not just
  bound.
- **Core writers.** `LoopLibCore/sources/pdfannotation.{h,cpp}` carries the
  annotation type model (`AnnotationType`, `PDFAnnotationBorder`,
  `PDFAppeareanceStreams`, `PDFAnnotationQuadrilaterals`, icon-fit and callout
  types); `pdfoutline.{h,cpp}` models outline items with mutation primitives
  (`PDFOutlineItem::addChild`, `insertChild`); `pdfform.{h,cpp}` models form
  fields (`PDFFormField`, `FieldType`, flags) with the
  `PDFDocumentModifier`/`PDFModifiedDocument` machinery beside it;
  `pdfsecurityhandler.{h,cpp}` exposes document security state. These are the
  primitives a direct edit composes; none of them is wired to Quick today.
- **Revision and history contracts.** `docs/REVISION_CONTEXT.md` makes
  `PDFDocumentContext` the revision authority and requires every consumer to
  compare the complete `PDFRevisionIdentity`; invariant I07 pins *"Stale
  revision results are discarded and are not current"*.
  `docs/OPERATION_HISTORY.md` is the append-only history: *"Undo/redo remains an
  in-memory interaction feature"* while *"Every accepted or rolled-back event
  creates one `PDFRollbackPoint`"* — the model #234 names for undo (#124), with
  permission refusal owned by #122 and review display by #125.

## Candidates

| Option | Shape | Admissible? |
| --- | --- | --- |
| A. Direct revision per edit | handler → `CommandCatalog` → a typed direct-edit request on `DocumentFacade` → Core writer primitive → one history event → new revision | **Recommended** |
| B. Planned operation per edit | every edit becomes a recipe/Fix plan the operator previews and approves | Technically, but rejected |
| C. QML-side model edits | Quick models the outline/annotations and writes them in QML | **No** — a second mutation path; #233's acceptance states the model is never edited in QML |

**Recommendation: A.** Authoring edits are low-risk, interactive, and
reversible by construction through the append-only history — the exact profile
the governed Fix flow does not serve. Option B would put plan/approve in front
of every highlight and contradict the epic's own seam, which reserves governed
Fix and publication operations for PR 3. Option C is ruled out by the
interaction contract regardless of convenience.

Under Option A every authoring edit still inherits the governed properties,
because they are properties of the *path*, not of the Fix flow:

1. **One route.** The handler is a `CommandCatalog::Handler`; nothing else may
   call a Core writer (`docs/INTERACTION_CONTRACT.md:33-34`).
2. **Fenced.** The edit carries the `PDFRevisionIdentity` it was begun against;
   a mismatch fails closed with a visible reason before any mutation (I07;
   #233's failure case is exactly this).
3. **Provenance.** The accepted edit appends one operation-history event and
   produces one `PDFRollbackPoint` — undoable by construction under #124's
   model; wiring `actionUndo`/`actionRedo` remains a separate decision, as
   ADR-012 Q4 already ruled for the drag commit.
4. **Refused, not repaired.** A permission-restricted document
   (`pdfsecurityhandler`: modify-annotations / fill-forms not granted) or an
   XFA-only form (#242) returns a typed refusal instead of guessing or
   half-writing.

## Exact deltas the recommendation requires

1. **Command ids for #242** (the only new ones; #233's 16 and #234's 18 are
   already declared): one id per mutation family —
   `actionSetFormFieldValue`, `actionAddAttachment`, `actionRemoveAttachment`,
   `actionEditDocumentMetadata` — plus the existing `actionSend_by_E_Mail`.
   Each: `disposition: KEEP`, scalar parameters only (the protected schema
   admits `string`/`integer`/`number`/`boolean`,
   `docs/schemas/loop-shell-actions.schema.json`), `capability: document.modify`,
   `availability: implemented` when its handler lands, and
   `expected_action_count` bumped accordingly (109 → 113). The exact names may
   be adjusted by the slice only within this delta's shape; a different shape is
   a new decision.
2. **A typed direct-edit request on `DocumentFacade`** (interaction layer): the
   mutation-gateway shape every authoring handler submits to — the edit kind,
   its scalar payload, and the originating `PDFRevisionIdentity`. Stale or
   unauthorized requests return a typed refusal; the gateway never mutates
   partially.
3. **Core writer primitives** (`LoopLibCore/sources/**`, protected): one per
   edit family — annotation creation per subtype with its appearance stream,
   outline item create/reorder/import/export, hyperlink creation per destination
   variant, form value set, attachment add/remove, XMP metadata update. Each
   follows the existing modification machinery (`PDFDocumentModifier` /
   `PDFModifiedDocument`), saves as a new artifact, and appends the history
   event and rollback point.
4. **Handler registration.** `EditorHost` registers the 35 existing ids plus the
   4 new ones; each maps its scalars into the direct-edit request, checks the
   fence first, and surfaces typed refusals (stale revision, permission denied,
   XFA-only form, unsupported subtype).
5. **Evidence lanes.** Per-subtype annotation round-trips with
   appearance-stream render comparison; bookmark import/export round-trip on the
   fixture set; per-destination-variant read-back; form round-trips (text,
   checkbox, choice; signature field excluded; XFA refusal); attachment and XMP
   metadata round-trips; stale-fence and permission refusals; quick and
   accessibility lanes.

## Cost and risk

| Option | Cost | Risk |
| --- | --- | --- |
| A (recommended) | 4 new catalog ids + count bump, one facade gateway, six Core writer families, 39 handlers, the evidence lanes above | Medium — real document mutation; mitigated by the fence, typed refusals, and history-per-edit |
| B planned operation | recipe surface per edit family; Fix flow as the authoring UI | High product cost for worse authoring UX; blurs the PR2/PR3 seam |
| C QML-side edits | none | Blocked by the interaction contract; creates the second mutation path the product forbids |

## Smallest implementation slice that would prove it

One vertical per issue, each proven end to end before the next:

1. **#233:** bookmark create/reorder/import/export plus one hyperlink variant
   through catalog → facade → Core outline writer → read-back test; then the
   remaining destination variants.
2. **#234:** highlight through the same path with an appearance-stream
   comparison; then the remaining markup, shapes, inline text, and sticky-note
   subtypes; refusal cases for stale revision and missing modify-annotations.
3. **#242:** form text fill through the new ids; then checkbox/choice,
   attachments (extract through the atomic writer, refuse executable auto-open),
   XMP metadata, XFA refusal, and the send-by-email hand-off of the published
   revision.

## Verification note

This record was written on the PR 2 branch before its slices started; the
implementation PR's gate (`scripts/agent/check-change.py` plus the mapped
suites) is the evidence that proves the deltas above. No build or test lane was
run for the ADR itself.

Refs #231, #233, #234, #242, #124, #122, #125, ADR-012
