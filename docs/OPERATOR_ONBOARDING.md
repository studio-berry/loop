# Operator help, onboarding and samples (#121)

The installed Editor opens the welcome guide on a fresh settings profile. F1
and the workspace rail's Help button reopen it. Each finding has a Help button
that opens its check topic. Help has four sections: getting started, all seven
workspaces, finding measures/coverage/limitations, and sample jobs. It requires
no network connection or source checkout.

The embedded `operator/check-catalog.json` is the generated
`docs/generated/preflight-check-catalog.json` itself. The presenter returns its
measures, limitations, coverage and finding conditions without rewriting them.
The Findings section also displays the global claim and not-covered list.
Unknown checks expose unavailable coverage. Help never derives a PDF verdict or
authorizes a publication.

The private `OperatorHelp/welcomeSeen` UI preference uses the Editor's existing
QSettings destination, including `--config`. Continuing to the workspace
acknowledges seeing the guide; it is not a completion or acceptance certificate.
Escape leaves the welcome pending. F1 remains available after acknowledgement.

## Redistributable learning corpus

`LoopEditor/operator/samples` contains four original geometric PDFs and a
synthetic RGB pixel, two profiles using the accepted preflight profile format,
and one recipe using the accepted Action List format. There are no external
artworks, personal data, embedded fonts or third-party images. The corpus is
licensed MIT with its own bundled copyright/permission notice.

| Job | Profile and expected Core outcome |
| --- | --- |
| Clear inspection | Shipped default checks: pass within that scope |
| Colour finding | Default: fail for a DeviceRGB image |
| Unsupported declaration | Learning profile adds conformance-claims: incomplete for an unsupported PDF/X-5n declaration; the sample is not PDF/X-conformant |
| Bleed correction | Default: bleed finding, then real governed add-bleed planning, approval, recheck and temporary-publication sign-off |

Opening a sample copies its PDF, profile, license and optional recipe below the
platform's AppLocalData samples directory. Core's existing import routes select
the learning profile/recipe. Any edited local copy is preserved and reopening
fails with an explicit instruction to move it aside for a fresh copy. An edited
open document disables sample replacement. Operators must select their intended
production profile again before checking a real job.

`scripts/ci/generate_operator_samples.py` regenerates deterministic members.
`manifest.json` records provenance, member digests and expected outcomes. Sample
JSON/license members use LF on both platforms so their identities remain stable.
The entire corpus, catalog and guide are embedded in the Editor executable;
there is no new external installation layout or Core persistence contract.

## Proof and limitations

Run:

```text
python -m unittest scripts.ci.test_check_operator_help -q
python -m scripts.ci.check_operator_help
ctest --test-dir <configured-build> -C Release -R ^UnitTestsShellWorkspace$ --output-on-failure
python scripts/agent/check-change.py --base origin/dev --build-dir <configured-build>
```

`UnitTestsShellWorkspace` checks every catalog finding's measures, limitations
and coverage, and renders every check topic to compare its visible labels to
the catalog. It checks welcome persistence with isolated settings, refusal to
overwrite an edited sample, all four real host preflight outcomes and the real
sample recipe's governed Core correction. The source-integrity CI lane checks
catalog resource binding, workspace coverage and deterministic licensed members.

Those are build/test-host observations. Clean-machine installed onboarding on
Linux and Windows is still required at one final candidate SHA, with package
digests, OS/profile metadata, commands or hosted run, sample/member identities,
recording/logs and human review. No clean-machine lane has been recorded by this
implementation. The package prerequisite (#230) and preview/Fix/native work
(#227, #228, #244) need integration and qualification with user-approved merges.

Full delivery sign-off is blocked by #32's publication boundary: the Editor
currently signs off a temporary artifact that the worker deletes. A later Save
As is a separate write. The guide states this limitation rather than claiming
that saved bytes share a receipt. #121 remains open until the required installed
journeys and durable delivery evidence are complete.

Quality review: help consumes the generated catalog directly and routes samples
through existing Core-controlled imports, planning and execution. The change
adds no alternate verdict, publication authority or acceptance certificate, and
protects existing sample edits.
