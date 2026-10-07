# Clean-machine operator acceptance (#32)

Status: **blocked; no installed-product acceptance packet has been recorded**.
Passing checker tests or merging this tooling does not complete L03-06. Admission
requires one representative production PDF journey on Linux and Windows, from
packages built from the same candidate SHA, followed by human review.

## Blocking publication boundary

`LoopLibInteraction/sources/actionlistrunsubmitter.cpp` creates both staging and
publication destinations in `QTemporaryDir`. Core signs off the exact temporary
publication, then the worker returns the reopened document and deletes those
directories. `EditorHost::fixSignOff()` exposes that receipt in memory. The Fix
pane displays shortened digests, but offers no complete receipt export. A later
Save As is a separate write; the current receipt does not establish its identity.

Do not copy the CLI receipt, drive a test host, or relabel the accessibility smoke
probe as the installed Editor journey. Those are different evidence. In
particular, the accessibility probe uses controlled inspection states.

The required contract decision is to bind the Editor's reviewed Core plan and
candidate to a durable operator-selected destination, retain Core's receipt for
those published bytes, and export that receipt through the product. Preserve
Core's existing approval, cancellation, destination and revalidation authority.
This PR does not invent that contract or change Core. Until the decision and its
implementation are accepted, stop the journey at publication and record the
unavailable lane rather than producing a passing packet.

The preview, compare/Fix and native qualification work in PRs #227, #228 and #244
also needs integration and exact-candidate qualification. Package inspection on
the current base is separately blocked by the manifest prerequisite in PR #230.
Merging any of those PRs requires the user's approval.

## Capture after the blockers are resolved

1. Select a production PDF and profile that exercise a real actionable finding,
   preview and governed correction. Record why the PDF represents the intended
   production workload. Use the same original bytes, recipe and effective
   profile on both platforms. Keep private PDFs and all evidence outside Git.
2. Build final MSI/AppImage packages from one full candidate SHA. Preserve the
   packages, hosted run URLs, and existing package-boundary inspector JSON.
   Verify the packages before using clean machines without checkout/build
   dependencies. Install the MSI, or extract the final AppImage and launch its
   packaged application. Record the commands, exit status, installation root,
   OS/version, architecture, host identity, UTC time and package digest.
3. Start screen recording before launching the installed `LoopEditor`. Hash the
   launched executable and map it to the inspected payload. Record the actual
   command and package/source identities. No alternate harness or Core mock may
   supply any step.
4. Open the PDF; inspect; locate a real finding by its ID; review the preview;
   approve that exact plan; publish to the durable destination; revalidate the
   published bytes; inspect the Core sign-off. Retain the original PDF, published
   PDF, initial exported inspection and complete Core receipt. Record each step's
   observation and its video offset. Review fidelity/limitations and the full
   revalidation coverage, not just a green summary.
5. Assemble each packet below. Hash its members and write `packet.json`. A human
   watches the video, checks the logs and receipt identities, confirms the PDF is
   representative and every step used the installed product and real Core, then
   writes `review.json` approving the exact packet digest. Agent-generated
   approval is not human review. Changing any member invalidates that review.
6. Run the pair checker from this checkout, keeping packets outside it:

   ```text
   python -m scripts.qualification.verify_operator_acceptance --source-sha <full-sha> <linux-packet> <windows-packet>
   ```

   Attach the resulting command/output, immutable packet locations, package
   digests, candidate SHA and review to the issue/PR. Failed, skipped or
   unavailable lanes keep #32 open. **Closes #32** is appropriate only after the
   acceptance evidence and human review are complete.

## Internal packet layout

This is a private qualification-tool input, not a PDF, persistence or public
product schema. The checker validates byte binding and recorded assertions;
it cannot determine from JSON whether a host was clean or whether a video shows
real Core execution. Those facts remain explicit human review responsibilities.

`packet.json` requires `kind: "loop-operator-acceptance-packet"`, `version: 1`,
`platform: "linux" | "windows"`, `source_sha`, `status: "passed"`, and
`unavailable: []`. `members` maps every retained member's relative path to its
SHA-256. It excludes only `packet.json` and `review.json`; missing, changed,
unbound or escaping files fail. The following keys each name a nonempty member:

| Key | Retained evidence |
| --- | --- |
| `package` | Final MSI or AppImage; original filename retained |
| `package_evidence` | Existing final package-boundary inspector JSON |
| `environment` | Platform, OS, OS version, architecture, host, recorded UTC; `clean_machine: true` |
| `installation` | Command, installed root, source SHA, package SHA-256, `status: "passed"`, integer `exit_code: 0` |
| `launch` | Command, executable absolute path, installed root, source SHA, package SHA-256, payload path and executable SHA-256; `surface: "LoopEditor"`, `scope: "installed-product"`, `status: "passed"` |
| `source_pdf`, `published_pdf` | Exact original and durable published bytes |
| `inspection` | Real initial preflight report, complete and bound to the source/profile |
| `receipt` | Existing Core governed approval/revalidation/sign-off JSON, wrapped in `governed` |
| `video`, `log` | Full journey recording and retained commands/observations/hosted run references |

`steps` is an array in exactly this order: `open`, `inspect`, `locate`, `preview`,
`approve`, `publish`, `revalidate`, `sign-off`. Each step requires
`status: "passed"`, `implementation: "product-core"`, a nonempty `observation`
and finite nonnegative `video_seconds` in journey order. `locate` requires a
`finding_id` present in the inspection. From `preview` onward, `plan_digest`
must match Core's signed-off plan. The pair must share plan/source/profile
identities; each platform's retained output must match its own receipt.

`review.json` requires `kind: "human"`, `decision: "approve"`, `reviewer`,
`reviewed_utc`, `rationale` and `packet_sha256`. Its `attestations` object requires
`installed_product`, `real_core`, `representative_pdf` and `complete_journey`,
all true. The rationale records the representative workload and any limitations
considered. No command in this tooling creates or substitutes that approval.

## Proof and handoff

The negative packet tests run on Linux and Windows in `ci.yml`. Their packages,
PDFs, observations and reviews are synthetic test data and qualify no product.
The architecture lane binds those tests to the checker and this procedure.
Run the normal `scripts/agent/check-change.py --base origin/dev` gate for the
tooling diff as well as the pair checker for acceptance evidence.

Quality review: this change reuses the existing package and governed receipt
validators. It keeps evidence admission separate from product/Core authority and
does not create passing runtime evidence or human approval.
