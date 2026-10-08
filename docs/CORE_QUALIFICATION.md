# L01-07 Core qualification runbook

[Issue #21](https://github.com/studio-berry/loop/issues/21) prepares and admits one
exact source SHA for the [L01 Evidence Core gate](https://github.com/studio-berry/loop/issues/2).
The preparation PR may merge while admission is pending. Its tooling verifies
CI/package provenance; the complete issue dossier and reviewer decision establish
module admission. Release promotion remains a separate R00 decision.

## Prerequisites and candidate freeze

Review the acceptance evidence for every other L01 child below. An issue closure,
merged PR, or legacy qualification claim alone does not satisfy a row.

| Issues | Required acceptance evidence |
| --- | --- |
| #15 | Reviewed reset inventory, capability/gap dispositions, and current catalog diff. |
| #16 | Receipt identity and golden vectors; missing coverage, unsupported, budget-limited, cancelled, and parser-error paths stay non-PASS. |
| #17 | Scheduler/result fencing under reorder, timeout, retry, cancel, and reopen. |
| #18 | Independent standards, signature, conversion, and fidelity reports bound to exact output bytes, validator versions, and visible limitations. |
| #19 | Strict Linux/Windows resource matrix, declared budgets, measured memory/time, cancellation/recovery, and hostile workload dispositions. |
| #20 | Worker crash/timeout/resource fault injection, host recovery, no silent in-process fallback, and telemetry/content inspection. |
| #113–120 | Accepted regression fixtures and before/after reports, unchanged golden corpus, current catalog dispositions, and no clean false PASS on the named defects. |

After these outcomes and the preparation PR are accepted and integrated into
`dev`, select candidate `C` as a full 40-character SHA. Verify the live `dev` ref
against the local ref before selection; the current workspace or old branch head
is not an implicit candidate. Record the comparison base, committed catalog,
profile/corpus manifest digests, and each child acceptance link.

Use a qualification branch pinned to `C` for dispatch and keep it fixed through
review. Follow repository approval requirements for upstream sync, pushes,
packaging/installation, and external writes. A source repair creates a new
candidate and new evidence packet; do not combine checks from multiple SHAs.
Route repairs to the owning child issue and reuse its outcome PR when one exists.

## Run and inspect the required lanes

Dispatch full `CI` on the pinned qualification ref. Ordinary PR CI skips the full
Linux/Windows jobs, so its green aggregate is insufficient. Record the explicit
run ID and verify its `headSha == C`; never select evidence solely by latest run
or branch name. Inspect `source_integrity`, `linux / build`, and `windows / build`,
including the Widgets-absent and normal builds/tests and preflight corpus gate.

```sh
gh workflow run ci.yml --repo studio-berry/loop --ref "$QUALIFICATION_REF"
gh run list --repo studio-berry/loop --workflow ci.yml \
  --branch "$QUALIFICATION_REF" --event workflow_dispatch \
  --json databaseId,headSha,status,conclusion,url
```

Check test registration, counts, output, and individual skips, not just the step
exit status. Run the repository's mapped Core/preflight proofs, generated-catalog
and architecture checks. For any implementation PR, run `check-change.py` against
its accepted base and retain the exact report. An incomplete report is not proof.

Reproduce #18's accepted independent claim matrix, #19's strict resource workflow,
and #20's process-isolation faults on `C` on both required platforms. Retain
fixture/profile/output digests, commands, tool versions, budgets, measurements,
run IDs, and terminal dispositions. Source guards and self-validation do not
substitute for the external oracle or installed-runtime results. A negative PDF
fixture may correctly yield Fail/Incomplete while its behavioral test passes;
an incomplete inspection must never be relabelled as a passing PDF.

Dispatch `Linux_AppImage` and `Windows_MSI` with `source_sha=C` on the pinned ref.
The workflow ref must resolve to `C`: checkout uses the workflow run's immutable
commit, and `source_sha` is an identity assertion rather than a checkout selector.
Inspect their exact-checkout guards, installed/relocated PdfTool preflight and
runtime smokes, dependency inspection, and package lifecycle results. Capture
package run IDs and input/check-out SHA, artifact IDs, names, hashes, and expiry
dates. A package workflow's run SHA alone does not establish its checked-out
`source_sha`. Record every skip, including hosted Windows operator-launch skips,
and decide whether it omitted a required Core lane. A qualifying skip blocks
admission; unrelated conditional skips need an explicit disposition.

```sh
gh workflow run LinuxInstall.yml --repo studio-berry/loop \
  --ref "$QUALIFICATION_REF" --field source_sha="$C"
gh workflow run WindowsInstall.yml --repo studio-berry/loop \
  --ref "$QUALIFICATION_REF" --field source_sha="$C"
```

For the `package-release` repository-dispatch proxy, pass
`client_payload.source_ref=QUALIFICATION_REF` together with `source_sha=C`.
The proxy defaults to `stable` when `source_ref` is absent; in that case `C` must
be the stable workflow commit. Keep the chosen branch or tag pinned throughout
both dispatches. A moved ref is rejected by the SHA guard and requires new runs.

## Check the collected provenance

Keep downloads, transcripts, and generated reports outside the repository. Download
the package-boundary evidence and the actual AppImage/MSI bytes from the recorded
package runs. Unpack the GitHub artifact archives before checking: the inspector
hashes package files, not GitHub's enclosing ZIP archives.

```sh
gh run download "$LINUX_PACKAGE_RUN_ID" --repo studio-berry/loop \
  --name loop-package-boundary-linux-evidence --dir "$EVIDENCE_DIR/linux"
gh run download "$WINDOWS_PACKAGE_RUN_ID" --repo studio-berry/loop \
  --name loop-package-boundary-windows-evidence --dir "$EVIDENCE_DIR/windows"
gh run download "$LINUX_PACKAGE_RUN_ID" --repo studio-berry/loop \
  --name "$APPIMAGE_NAME" --dir "$EVIDENCE_DIR/packages"
gh run download "$WINDOWS_PACKAGE_RUN_ID" --repo studio-berry/loop \
  --name "$MSI_NAME" --dir "$EVIDENCE_DIR/packages"
```

From the repository root, capture the selected full CI snapshot and run the checker
(the example uses a POSIX shell; PowerShell accepts the same CLI arguments):

```sh
gh run view "$CI_RUN_ID" --repo studio-berry/loop \
  --json databaseId,url,headSha,status,conclusion,jobs > "$EVIDENCE_DIR/ci-run.json"
python scripts/qualification/check_core_qualification.py \
  --candidate-sha "$C" --ci-run "$EVIDENCE_DIR/ci-run.json" \
  --linux-evidence "$EVIDENCE_DIR/linux/evidence.json" \
  --windows-evidence "$EVIDENCE_DIR/windows/evidence.json" \
  --linux-package "$EVIDENCE_DIR/packages/$APPIMAGE_NAME" \
  --windows-package "$EVIDENCE_DIR/packages/$MSI_NAME" \
  > "$EVIDENCE_DIR/core-provenance.txt"
```

The checker reads existing evidence formats and makes no network calls or issue
changes. Exit 0 means the CI/package provenance subset was verified; its output
explicitly keeps module admission **PENDING REVIEW**. Invalid input, missing or
duplicate required jobs/steps, nonterminal/failed/skipped required lanes, mixed
SHAs, incomplete package inspection, and mismatched package names/sizes/digests
return a nonzero exit with a reason and no successful provenance report.
Unrelated conditional skips, such as the fast-lane steps in a full build, are
allowed. Required step names follow the current reusable workflows; renamed lanes
must be reconciled deliberately rather than silently guessed.

Snapshots are supplied evidence, not independently authenticated records. The
reviewer must inspect the live run/job links and actual test/smoke/oracle output.
The checker does not establish test counts, per-test skips, installed smoke
success, external oracle agreement, resource metrics, or worker containment.

## Issue dossier and reviewer decision

Post the complete dossier on #21 and link that exact comment from parent #2.
Do not commit a dossier into the frozen candidate or treat this preparation PR
as closing #21. Retain the full sanitized evidence outside the source tree; record
important measured results and identities in the issue before expiring Actions
artifacts are lost. Refresh expired or unavailable required evidence before review.

Use this review checklist in the issue comment:

| Field/row | Record |
| --- | --- |
| Candidate | Full `C`, comparison base, qualification ref, catalog/profile/corpus digests. |
| Child acceptance | One row per #15–20 and #113–120 with acceptance evidence and reviewer disposition. |
| Source/tests | Run/job IDs and links, commands, platform/toolchain, test counts, per-test skips, expected terminal behavior. |
| Independent claims | Claim scope, exact PDF/output digests, validator/version, result, supported limits. |
| Resources/isolation | Budget/workload identities, measured maxima, cancellation/recovery and fault/telemetry results. |
| Installed runtime | Exact package and fixture identities, commands, smoke/lifecycle transcripts, skipped/unavailable lanes. |
| Artifacts | Package/run/artifact IDs, byte counts, SHA-256, source SHA, retention/expiry, provenance-check result. |
| Deviations | Every limitation/skip, owner, evidence, and explicit qualifying/nonqualifying disposition. |
| Decision | Named reviewer, timestamp, exact `C`, admit/blocked decision, remaining risk. |

Admission requires all required L01 outcomes and lanes to be supported on `C`
without qualifying skips. Missing, stale, unavailable, cancelled, failed, or
unreviewed proof keeps admission blocked. Only after the named reviewer accepts
that packet may #21 be closed and the parent gate updated. The handoff records
changed files, observed verification, remaining risks, and a 1–3 sentence quality
review summary.
