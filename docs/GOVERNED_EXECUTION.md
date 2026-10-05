# Governed execution (issues #369 and #370)

Core corrective workflows publish a new artifact only after an explicit, plan-bound
approval. Technical and visual previews are separate review artifacts. Preflight finding
waivers are not operation approval.

Architecture contracts D1–D5 for 0.3.0 qualification are recorded in
[`docs/adr/adr-011-architecture-contracts-d1-d5.md`](adr/adr-011-architecture-contracts-d1-d5.md).
Canonical JSON bytes are pinned in [`docs/CANONICAL_JSON.md`](CANONICAL_JSON.md).

## Plan digest

`pdf::computeOperationPlanDigest()` hashes the canonical `operation-plan` envelope:

- schema kind/version
- trusted source SHA-256
- merged save policy
- analyzed repair plans

Any change to the plan, source identity, or save policy produces a new digest. Approvals
bind to one digest and become invalid when the plan changes.

`pdf::computeActionListPlanDigest()` binds the same identities for Action List plans: the
`action-list-plan` envelope carries the merged operation save policy, so a save-policy
change moves the Action List digest and invalidates approvals bound to it.

Destination path, overwrite/collision policy, and publication target are **not** part of
the semantic plan digest (D3). They are bound later as execution inputs to
`publishGovernedArtifact` / `PDFSaveRequest` / `PDFSafeFileWriter`. Changing only the
destination must not silently change semantic operation identity. Trusted-source and
in-place overwrite refusal stays fail-closed.

## Registry and plan identity

`pdf::PDFRepairRegistry` is the single registration authority for repair operations.
`registerOperation` refuses a null operation, an empty id, and an id that is already
registered (the first registration wins), so a duplicate cannot silently shadow a
built-in. `PDFRepairRegistry::instance()` remains the production registry; a
default-constructed registry is isolated for tests and tools.

`PDFRepairRegistry::digest()` is the versioned registry identity: SHA-256 over the
canonical `{"id", "version"}` set of every registered operation, sorted by id.
`computeOperationPlanDigest()` carries it as `registry_digest` inside the
`operation-plan` envelope, so any change to the registered id/version set moves every
plan digest and invalidates approvals bound to an earlier registry. The envelope
`schema_version` stays `"1.0"`: the added key is non-breaking because nothing durable
stores raw envelopes.

`PDFRepairTransaction::add()` validates `parameters` against the operation's
`parameterSchema()` with the shared `validateJsonSchemaFragment()` — the same validator
the Action List planner uses — and refuses before any candidate work. The validator
fails whenever it records a violation, so an unknown key or a missing required
parameter is refused on the Action List planning path too. A repeated
`--param` key is refused by `PdfTool repair` for the same reason: last-wins assignment
hides which value the operator meant.

`PDFRepairTransactionOptions::expectedSourceSha256` binds a transaction to the source
revision it was planned against. Bound transactions require `sourcePath` to be
readable without a password. Both `analyze()` and `apply()` reopen that source,
verify its byte hash, and compare its parsed contents with the transaction source.
A changed document retaining its original provenance hash or a changed source file
is refused before candidate computation. Missing or unreadable source paths fail
explicitly. An empty expected digest keeps the historical behavior.

Migration decision: the legacy `pdftool addbleed` and `rgbtocmyk` commands remain
registry-metadata consumers that bypass `PDFRepairTransaction`; converging them onto
the generic `repair` path is L04-05 (#37) scope.

## Previews

| Artifact | Schema | Fidelity | Contents |
| --- | --- | --- | --- |
| Technical preview | `loop.technical-preview` | `exact` | Structural/metadata diff only (`renderVisualDiff=false`) |
| Visual preview | `loop.visual-preview` | `simulated` | Rendered page diffs only (structural comparison disabled) |

Both previews carry the same `plan_digest`, `source_sha256`, and `candidate_sha256`, and
each serializes its artifact-computation fidelity as `fidelity_mode`.

`fidelity_mode` describes how faithfully the preview artifact was computed — it is not the
interactive canvas render authority of `PreviewStateModel::Authority`
(exact/approximate/authoritative, where `authoritative` means output-preview origin). A
technical preview is `exact`: it structurally compares the serialized and reopened
candidate bytes, so it reflects the real output. A visual preview is `simulated`: it is a
rendered simulation and is never proof of print safety.

Previews may materialize an isolated candidate for review. They have **no publication
authority** (D2): they must not become an alternate path to a durable destination write.
Only `pdf::publishGovernedArtifact()` publishes approved candidate bytes.

A preview is bound to one exact plan. Both builders refuse an empty or malformed
`planDigest`, and refuse a digest that does not equal
`computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy())`.
The refusal happens before any candidate write, so a mismatched plan cannot leave a
preview artifact. The production caller (`PdfTool repair`) computes the digest from the
same three inputs, so the binding does not change its behavior.

### Preview residue contract

A preview writes only to caller-named paths: the candidate path and, when requested, the
render directory. On cancel or failure it removes what *that call* created — the candidate
file it wrote, the render PNGs it rendered, and a parent directory it created via `mkpath`
when it is now empty. Candidate removal requires a confirmed write by that preview;
refused destinations and cancellation before serialization preserve existing files. A completed preview leaves its candidate and renders for review;
only cancellation, an incomplete comparison, or a hard failure removes them.

## Approval

`PDFGovernedExecutionApproval` records:

- `plan_digest`
- `source_sha256`
- `candidate_sha256`
- `effective_profile_digest` (optional; empty = the caller binds no profile)
- `PDFApprovalRecord` (`human`, `policy`, or `system`)

`PreflightDecision` records (accept/waive/override) never qualify as operation approval.
An approval that names a `preflight-decision:` reference is rejected at publish time.

An approval is only a *current authorized* approval when the authorization resolver
accepts it. `pdf::resolveApprovalAuthorization()` is the one decision point, called by
every gateway function (`validateGovernedApproval`, `finalizeGovernedPublication`,
`publishGovernedArtifact`, and `validateGovernedSignOff`) before any write. It fails
closed on short codes:

- **approver** — the record must carry an affirmative non-`None` decision and a
  non-empty actor. `PDFApprovalAuthorizationPolicy.authorizedActorIds` and
  `authorizedKinds` are allowlists: a non-empty list restricts; an empty list declares
  no restriction.
- **expiry** — `PDFApprovalRecord.expiresUtc` (null = no declared expiry) is refused when
  it is at or before `PDFApprovalAuthorizationContext.evaluatedUtc` (`approval-expired`),
  and a declared expiry that cannot be evaluated is refused. `policy.requireExpiry`
  additionally refuses an approval that declares no expiry. Malformed declared expiry values invalidate the parsed approval.
- **waiver exclusion** — finding waivers and preflight decisions remain excluded.
- **revocation** — an append-only `ApprovalRevoked` history event whose
  `approval.decisionReference` equals the approval's reference revokes it
  (`approval-revoked`). This mirrors certificate invalidation: one chain, no second
  registry. The resolver reads the chain through `PDFOperationHistoryStore::events()`
  when a history store is in scope. An unavailable or compromised chain is refused as `approval-history`.
- **profile binding** — when the caller supplies `expectedProfileDigest`, the approval's
  `effective_profile_digest` must equal it (`profile-binding`), so an approval taken
  against one effective profile cannot authorize a run under another.

`PdfTool repair` and `PdfTool action-list run/batch` resolve revocation against the
output document's `.loop-history` chain. PageMaster exports and the Editor Action List
worker have no operation-history store in scope, so their contexts leave `history` null
and revocation cannot be resolved there; #37 routes them through the mutation gateway
but the chain wiring remains open on those two surfaces and is left to #39.

## Publish gate

`pdf::publishGovernedArtifact()` validates an approval against the expected plan and
artifact digests and writes through `PDFSafeFileWriter`. It is the low-level write
primitive; production surfaces publish through the mutation gateway below, which wraps
it with staging, revalidation, ordering, and a terminal receipt.

`PdfTool repair` includes `plan_digest`, `technical_preview`, and `visual_preview` in
its report. Commit requires a matching governed approval (policy-generated by default,
or supplied through `--approval-file`).

## Mutation gateway (#37)

`pdf::executeGovernedMutation()` in `pdfgovernedexecution.{h,cpp}` is the one
cancellation-safe entry point for an approved corrective mutation. Editor, PdfTool,
PageMaster, and the Action List worker all route their publication through it, so no
adapter owns an alternate mutation path. Its request (`PDFGovernedMutationRequest`)
carries the governed approval envelope, the authorization context (#36), the plan and
source identities, the exact reviewed candidate bytes (or a caller-owned staged path),
the destination and overwrite policy (execution inputs, never in the plan digest, D3),
the profile and its digest, the sign-off actor/policy, an optional
`PDFOperationControl`, an optional `beforeCommit` seam, and optional operation-history
wiring.

Order of operations:

1. **malformed request** — a missing plan/source/candidate identity, an empty
   destination, or an invalid approval is `refused` before anything is inspected.
2. **approval identity** — the approval must name the exact plan, source, and reviewed
   candidate bytes (`approval-stale`); a `preflight-decision:` reference is
   `approval-invalid`.
3. **authorization** — `resolveApprovalAuthorization()` is the single decision point
   (`approval-unauthorized`, `approval-expired`, `approval-revoked`,
   `profile-binding`).
4. **already-terminal replay** � with a verified history store in scope, an `Accepted`
   `FixApplied` event whose approval decision reference or plan digest matches the
   request refuses with `already-terminal`. A supplied execution id is also refused
   when its mutation history is terminal. Distinct plans may produce identical bytes.
   Every accepted summary records the gateway receipt to preserve the plan identity.
   History artifact identities must match the source and candidate before staging.
5. **cancel check** — a cancelled control refuses with status `cancelled`.
6. **stage** — the reviewed bytes are written to an isolated staging path beside the
   destination (or the caller's staged path is verified against them). The destination
   is still untouched.
7. **chain start** — with a history store, the execution is begun (when none was
   supplied) and an `approval`-bound `FixApplied`/`Running` event is appended.
8. **finalize against the staged bytes** — the staged file is reopened, revalidated,
   and signed off (`finalizeGovernedPublication`). A failure removes the staging file
   and leaves the destination untouched (`revalidation-failed`).
9. **`beforeCommit` seam + cancel check** — the test/qualification seam runs while the
   destination is still untouched; a cancel here is `cancelled`. History integrity, replay, and
   authorization are checked again at the current UTC time before the commit, so a
   late revocation or expiry refuses publication.
10. **atomic commit** — the reviewed bytes are committed through `PDFSafeFileWriter`
    under the requested overwrite policy (`destination-conflict` when `Fail` meets an
    existing file, otherwise `commit-failed`).
11. **read-back** — the committed bytes are hashed and compared with the reviewed
    candidate (`read-back-failed`).
12. **receipt + chain completion** — with a history store, an `Accepted`
    `FixApplied` event records the artifact, revalidation report digest, sign-off, and
    result summary (`history-failed` if it cannot be persisted).

**Terminal semantics.** Any refusal, failure, or cancellation **before** the commit
leaves **no** destination artifact — this closes the write-then-finalize hole on every
surface. A failure **after** the commit (commit, read-back, or chain completion) is
terminal `failed` with the artifact present and the reason recorded; the published
identity is never silently rolled back. History/provenance append failure does not
corrupt published artifact identity (D4).

**Receipt.** `PDFGovernedMutationReceipt` (schema `loop.governed-mutation-receipt`)
records `status` ∈ {`published`, `refused`, `failed`, `cancelled`}, a stable
`reason_code`, the plan/source/candidate digests, `destination_path`, `published_sha256`
(set whenever the destination holds the reviewed bytes, empty for a pre-commit
nonpublication), `destination_touched`, the optional history `execution_id`, and the
revalidation + sign-off **only** when the staged bytes passed finalization. Surfaces
embed it in their existing report shapes additively (a `receipt` field); they do not
synthesize their own.

Reason codes: `malformed-request`, `approval-stale`, `approval-invalid`,
`approval-unauthorized`, `approval-expired`, `approval-revoked`, `profile-binding`,
`already-terminal`, `cancelled`, `staging-failed`, `staging-mismatch`,
`revalidation-failed`, `destination-conflict`, `commit-failed`, `read-back-failed`,
`history-failed`. `published` carries an empty reason code, except a PageMaster
`forcePreflight` publication whose revalidation failed (`revalidation-forced`).

Two request switches make the gateway cover the non-preflight surfaces without
weakening the preflight ones: `requireRevalidation=false` commits without
revalidation/sign-off (an unsigned publication, e.g. a PageMaster export with no
profile, recorded `not-certified`); `publishOnRevalidationFailure=true` keeps a
PageMaster `forcePreflight` output whose profile failed (recorded
`revalidation-failed`) — it is the operator's explicit override, not the default.

**Chain wiring gaps.** `PdfTool repair` and `PdfTool action-list run/batch` supply the
output `.loop-history` store, so the gateway scans it for replay and appends the
execution/Running/Failed/Accepted events there. PageMaster exports and the Editor
Action List worker still have no operation-history store in scope, so their gateway
requests leave `history` null: they publish through the gateway but the chain append
and revocation resolution remain open on those surfaces and are left to #39.

**Out of scope (#37).** Editor Save As and document I/O
(`editorhost.cpp` → `DocumentFacade` → `PDFDocumentFileWriter::write`) are document
persistence, not a governed corrective mutation, and stay outside the gateway. The
legacy ad-hoc repair commands (`addbleed`, `rgbtocmyk`, `redact`, `optimize`,
`flattentransparency`, `removeexternallinks`, `decrypt`/`encrypt`,
`separate`/`unite`) remain deferred from #33; they are not registered operations and
are not converged here. Workflows and CI are untouched in this slice.

## Publication vs durable completion (D4)

Filesystem publication and durable provenance/history completion are separate states:

1. **staged** — analyzed plan and optional isolated candidate
2. **published** — destination bytes committed by the governed write gateway
3. **revalidated** — published bytes read back and evaluated with the effective profile
4. **durable completion** — `loop.governed-sign-off` plus provenance/history append

Crash or failure at each boundary preserves the last valid durable state. A published
artifact without durable completion is observable and recoverable; it must never be
silently represented as fully signed off. History/provenance append failure must not
retroactively corrupt published artifact identity.

## Published-byte revalidation and sign-off

Issue #370 adds one shared post-publication gate: the destination bytes are read back,
hashed, reopened by the PDF reader, and evaluated with the effective preflight profile.
The pathless canonical report receives its own digest. A `loop.governed-sign-off`
certificate is eligible only when all of these identities agree:

- plan, source, and reviewed candidate
- exact published bytes
- revalidation report
- effective profile

The certificate approval is separate from the approval that authorized the candidate.
Operation history records the accepted certificate as `CertificateIssued`, preserving
one provenance chain. A failed revalidation records a failed certificate event and does
not claim publication sign-off.

The same gate is used by `PdfTool repair`, `PdfTool action-list run/batch`, PageMaster
exports, and the Editor Action List worker. Action List results expose the plan and
source identities, while the Editor pane shows the plan digest and publication status.
PageMaster manifests carry governed evidence per output. Runs without a preflight
profile may write only an explicitly `not-certified` result and cannot issue sign-off.

### Revalidation state and reason (#38)

`PDFGovernedExecutionRevalidation` carries an explicit `state` and fail-closed
`reason_code`/`reason`, serialized into its JSON (`state`, `reason_code`, `reason`):

| State | Meaning |
| --- | --- |
| `complete` | Bytes verified and the effective profile verdict is `pass`. The only state eligible for sign-off. |
| `incomplete` | The inspection could not finish (a non-passing, non-blocking verdict such as `unsupported-scope`). |
| `error` | A hard byte/reader/profile failure or a definite failing verdict. |

Byte-level failures are named on every early return instead of leaving the operator with
a bare verdict: `expected-digest-missing` (an empty or malformed expected digest is
refused outright — an empty value never silently skips byte binding),
`artifact-unreadable`, `artifact-digest-mismatch`, and `artifact-reopen-failed`.
`isSignOffEligible()` requires `state == "complete"` in addition to `bytesVerified`, the
artifact/report/profile digests, and a passing verdict.

Surfaces map the state rather than collapsing every failure to `failed`: `PdfTool
repair` reports `incomplete`/`error` (plus `revalidation_state`/`revalidation_reason_code`),
the Action List governed summary carries an explicit `status`/`reason_code`/`state`, and
PageMaster manifests add `state`/`reason_code` beside the existing governed status.

### Impact-driven scope and finding delta (#38)

`revalidateGovernedArtifact` and `finalizeGovernedPublication` accept an optional
`PDFGovernedRevalidationScope` (also carried on `PDFGovernedMutationRequest`). The
default — no plan, no baseline — is a full-profile inspection, so every pre-#38 caller
keeps its behavior. When a scope is supplied:

- A **targeted** plan (from `planRevalidation` / `planRepairStepPreflight`) is honored
  only when the declared impact is complete and a baseline inspection is supplied.
  Undeclared, incomplete, or document-wide impact, and a missing baseline, fall back to
  a **full** run with the reason recorded (`impact-undeclared`, `impact-incomplete`,
  `impact-document-wide`, `baseline-unavailable`). Check and evidence selection is
  derived from the declared impact and the enabled profile checks, so a supplied plan
  cannot independently omit an affected check. A reused baseline must be complete,
  cover the enabled checks, and bind the same effective profile digest; missing or
  mismatched profile identity selects a full run (`baseline-profile-mismatch`).
- The finding delta between the baseline inspection and the published-bytes inspection
  is computed with `computeFindingDelta`, so a targeted run that omits a check carries
  its findings forward instead of falsely resolving them.
- The scope mode (`targeted`/`full`), its reason, the effective plan, the impact, the
  finding delta, and the baseline report digest are folded into the revalidation report.
  `reportSha256` digests that whole report, so the sign-off binds the delta: a
  certificate cannot be issued for a revalidation whose scope or delta was stripped.

### Pre-publication bytes can never close an operation (#38)

Only `finalizeGovernedPublication` against the published (or staged) artifact path can
produce an eligible revalidation and a valid `loop.governed-sign-off`. Inspecting an
in-memory candidate or pre-publication bytes — `PDFRepairTransaction::validateCandidate`,
an Action List step/terminal gate, or a PageMaster in-memory `revalidate` — yields a
verdict and evidence, never a sign-off. Publication without that finalize stays an
unsigned, `not-certified` artifact (D4). The Editor Action List worker routes through the
mutation gateway and finalizes against the exact staged bytes it reopens into the applied
candidate, so the sign-off is never bound to a second serialization or to bytes the
operator did not receive.

### Sign-off staleness and provenance reconstruction (#39)

A stored sign-off is a statement about exact bytes, so consuming one re-reads the bytes:

- `verifyGovernedSignOffAgainstArtifact(signOff, publishedPath)` reopens and re-hashes the
  artifact and refuses (`invalid-document-changed`) when the digest no longer matches
  `signOff.publishedSha256`. `PDFGovernedExecutionSignOff::fromJson` round-trips a stored
  sign-off; a malformed one is refused.
- The evidence-bundle path wires this in: when a `publishedArtifactPath` is supplied the
  bundle re-hashes the bytes and refuses a stale sign-off. A sign-off with no artifact and
  no output identity is now refused explicitly instead of synthesizing the output identity
  from the sign-off alone (the old silent-synthesis path).
- `reconstructGovernedPublicationAudit` (Core) reconstructs plan, approval, execution,
  artifacts, validation, and sign-off for one published output from the canonical chain,
  refusing a chain that fails `verify()` (see `docs/OPERATION_HISTORY.md`).

Producer identity completeness (#39): the gateway's running and failed `FixApplied` events
now bind `effectiveProfileDigest` (not only the accepted completion), so a reader can
attribute which profile an attempt ran under; the legacy `PdfTool add-bleed` accepted
approval now carries `evidenceSha256` and a `decisionReference` bound to the published
output (a targeted identity fix — add-bleed is not converged onto the gateway; that stays
#37). No CLI entry point for the reader yet (deferred).

## Cross-surface equality (D5)

CLI, Quick/Editor, and headless surfaces must agree on canonical plan identity and
governed output identity under pinned writer inputs. Exact PDF writer byte identity is
intentionally **not** guaranteed when the writer stamps clock- or random-derived
fields; fail-closed and parity proofs use governed digests or structural comparison
instead (disposition of legacy #656).

`scripts/ci/check_governed_parity.py` validates these records without opening a PDF.
Use `--compare-identity` when several reports are expected to describe the same
plan/source/profile tuple.
