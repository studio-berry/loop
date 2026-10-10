# Repository governance decision

This record implements [G00 #108](https://github.com/studio-berry/loop/issues/108).
The reviewed integration source is `9fea0095221911fa81dcfa816a49d090995bddbd`.
GitHub issues, native children, pull requests, milestones, default branch, and
roadmap sources were read on 2026-10-10. This PR's check-change report identifies
the implementation head. The review base identifies the source inventory,
not a runtime or release acceptance claim.

## Product identity

[#112](https://github.com/studio-berry/loop/issues/112) retains the existing product
and persistence identities. Repository links use the current full name.

| Item | Decision | Authority |
| --- | --- | --- |
| Product and display name | Loop | Root CMake constants and `pdfapplicationidentity.cpp` |
| Repository | `studio-berry/loop` | GitHub repository metadata and `agent-policy.json` |
| Source version | `0.2.1-alpha` | `LOOP_VERSION=0.2.1`, `LOOP_VERSION_PRERELEASE=alpha` |
| Windows numeric version | `0.2.1.0` | `LOOP_WINDOWS_VERSION` |
| Default and release branch | `stable` | Live GitHub default and branch policy |
| Integration and topic source | `dev` | Branch policy |
| Qualification branch | `unstable` | Branch policy |
| Organization and domain | `Loop`, `io.github.mberrys` | Application identity |
| Flatpak, desktop, AppStream package ID | `io.github.mberrys.Loop-pdf` | CMake, Flatpak and Desktop manifests |
| MSIX identity and publisher | `mberrys.Loop-pdf`, `CN=F0582764-7439-4F99-A76D-473EDF10E031` | `AppxManifest.xml.in` |
| MSI UpgradeCode | `26336d8a-b2e7-44fc-9a73-68aa99900c7a` | `WixInstaller/Product.wxs.in` |
| Application/settings identity | `LoopEditor`, `PdfTool`, existing per-tool names | `pdfapplicationidentity.cpp`, `pdfsettings.cpp` |
| AppImage and Windows asset filenames | Existing `Loop-pdf-*` and `mberrys.Loop-pdf_*` filenames | Paired producer, release collector and asset verifier |
| AppImage update destination | `studio-berry/loop` releases | `LinuxInstall.yml` |

The `Loop-pdf` tokens in package IDs and asset filenames are compatibility
identities. They are retained deliberately. No settings path, keystore path,
MSI upgrade identity, MSIX publisher, or Flatpak identity changes.
Existing upgrade and uninstall behavior therefore needs no new migration.
This change supplies source checks, not a packaging dry run or installation proof.

README links and its build instructions use the current repository.
The stale `master` and broken-DEB narrative is removed because current CI has no
DEB lane. AppStream's inherited PDF4QT release history is removed from current
Loop metadata. It remains recoverable from the pinned Git history; it did not
describe Loop's `0.2.1-alpha` source version. Release publication remains owned
by [R00 #14](https://github.com/studio-berry/loop/issues/14).

## Legacy provenance

[Legacy issue provenance](LEGACY_ISSUE_PROVENANCE.md) owns the distinction between
the retired repository and the reset sequence. Legacy commit subjects and old
changelog fragments remain immutable history. A bare number in a pre-reset record
means a retired issue, even when the reset sequence now contains that number.

The machine-read product follow-ups now point to
[#231](https://github.com/studio-berry/loop/issues/231), the existing Quick
coverage owner. This mapping does not claim that those GUI commands are delivered.
The preflight overlay's live titles, states, and milestones are refreshed.
Its backlog dispositions and reviewed P3 deferrals remain independent of issue
closure.

The archived Issues and Sessions databases return deleted markers. The Issues
archive view returns no rows. Deleted roadmap, orchestration, architecture, and
vision pages still expose retrievable historical text, but lack current authority.
Legacy issue bodies and their original repository remain unavailable.
This recorded source limitation satisfies the loss-disposition branch of #111;
recovering the ledger remains with the provenance maintainers under that record.

## Milestone decision

[#207](https://github.com/studio-berry/loop/issues/207) retires the pre-reset
release-milestone synchronizer. It cannot write to GitHub, even with `--apply`.
Its dispatch workflow and manifest are removed. Historical descriptions remain
under [github-milestones](github-milestones/README.md) with explicit retired status.

At review, GitHub has L01-L14 milestones 1-14, R00 15, G00 16, and X00 17.
L01 is closed; the other milestones are open. Those states are observations,
not module admission decisions. No milestone is created, closed, or rewritten.
Removal of the workflow reaches the default branch through the normal promotion
sequence after this PR merges into `dev`.

## Source and proof ownership

The [source register](GOVERNANCE_SOURCE_REGISTER.md) and its
[JSON source](governance/source-register.json) freeze the named intake for
L01-L14, R00, and X00. They reuse #15, #47, #141, and existing module children.
Unavailable issue bodies stay unknown. No new implementation issue was needed.

The [proof table](ARCHITECTURE_PROOF_DISPOSITIONS.md) and its
[JSON source](governance/proof-obligations.json) cover the clock/RNG TODO,
D1-D5, and all eight pending quality metrics. They retain declared limits.
D5 has binding behavioral fixtures and a CI comparator; its architecture
declaration is still `declared`. No metric baseline, fixed cap, sealed expected
output, public schema, or product API is changed.

The registers implement #209 and #210 as ownership and disposition records.
Their acceptance does not complete the separately owned proof or product issues.

## Verification and limits

Regenerate the two tables with
`python scripts/ci/check_governance_registers.py --write`.
Check local coverage with the default command. `--verify-github` compares the
frozen tracker snapshot with live titles, states, milestones, and PR identities.

The focused negative tests reject a missing module, missing obligation, invented
enforcement, wrong issue identity, retired repository link, and unapproved package
identity change. The retirement test refuses writes before any external command.
Both CI and check-change run the governance checks.

Required proof includes `python scripts/generate-architecture-catalogs.py --check --verify-github`,
the focused Python tests, and `python scripts/agent/check-change.py --base origin/dev`.
Pending measurements, independent oracle results, and release admission remain
with their named owners. Review of this PR is the maintainer decision checkpoint.

Quality review: the change reuses existing product identities, tracker owners,
catalogs, and behavioral tests. It removes the obsolete milestone writer and
keeps source availability, implementation, measured proof, and release admission
separate without adding product abstractions.
