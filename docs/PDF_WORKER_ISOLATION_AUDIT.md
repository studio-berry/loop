# Untrusted PDF worker audit (#20)

## Scope and contract

This change hardens worker-open and worker-preflight. Global untrusted-PDF isolation is **not admitted** while the in-process entrypoints below remain.

Protocol v2 is newline-delimited JSON. Requests are at most 64 KiB including the newline; responses at most 64 MiB. Each frame binds version, UUID request ID, allowlisted operation, booleans and status. Admitted responses are reconstructed from validated operation fields; extra verdicts, report paths and nested artifact fields are discarded. A preflight request additionally binds the SHA-256 of immutable staged PDF and profile bytes. The supervisor resolves the staged profile through Core, then requires the returned Core receipt to match input, full revision, effective profile identity, coverage and enabled check set. Revision counters are decimal strings to preserve all 64 bits.

Receipt schema loop.inspection-receipt.v1 serializes the existing Core receipt. Its identity continues to use loop.inspection-receipt-identity.v1; it is not an attestation that independently verifies a compromised parser's findings. Session document IDs are request UUIDs rather than memory addresses. Open returns only an artifact identification result. Terminal preflight failures have incomplete fidelity and no admitted coverage. If snapshot staging fails before a digest is known, that digest is explicitly empty; only an incomplete receipt with no checks, evidence or profile coverage can carry an unknown identity.

One request deadline includes write and read. Cancellation signals the active supervisor operation, which terminates the worker. A crash, resource fault, malformed response or identity mismatch produces an incomplete receipt. Replacement is explicit and never replays the failed PDF. There is no in-process fallback.

## Containment

Linux requires Landlock ABI 3 or newer and seccomp on x86-64 or AArch64. It restricts input to a separate read-only snapshot directory, permits temporary writes, disables core dumps and dumpability, and limits address space and CPU. Seccomp denies networking, process creation, execution and io_uring. Thread creation remains available. Read-only runtime roots are library trees, font/ICC/locale data, font configuration, the loader cache, and the worker binary/library directories. /proc, /sys, /dev, /opt and general /etc or /usr access are not granted. These declared runtime roots still need distribution-specific qualification.

Windows requires Windows 10 process creation attributes. The launcher creates a unique AppContainer profile with zero capabilities, a private runtime copy, restricted inherited pipe handles, child-process denial and an atomically assigned Job Object. The detached worker uses inherited pipes without allocating a console host; console allocation would conflict with child-process denial. The job enforces one process, 768 MiB committed memory, 120 CPU seconds and termination on close. The worker verifies its AppContainer token, zero capabilities, job limits and child-process policy before parsing. The complete AppContainer package directory, private runtime and snapshots receive protected read/execute ACLs; temporary data receives low-integrity writable ACLs. No installed-directory ACL is modified.

The build discovers the worker's DLL closure with CMake runtime dependency discovery, rejects unresolved or ambiguous dependencies, stages declared non-system DLLs, and emits a runtime manifest. The launcher accepts only manifest files beneath worker-runtime/, copies them into a per-session directory and grants no write access there. System DLLs remain supplied by Windows. Windows reroutes the whitelisted base profile environment to the new AppContainer; the launcher does not reroute it twice. Native process, job and pipe references close before profile deletion. Qt's worker is a QCoreApplication and does not use a GUI platform plugin.

References: [Microsoft AppContainer launch](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer), [process attributes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute), [error-mode suppression](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-seterrormode).

## Customer-content handling

The isolated PdfTool route creates neither product logging nor a product crash-reporting session. Worker and launcher sources are covered by the no-telemetry boundary. Worker stderr is discarded at process creation. Parser errors are replaced by fixed messages, and supervisor-generated diagnostic messages never forward raw worker text. Successful user-visible receipts are inspection output; they must not be sent as telemetry.

Product-generated dumps are disabled. Administrator-installed debuggers, endpoint agents, OS dump collectors, Linux privileged tracing and Windows LocalDumps policies are external collection channels. Qualification must inspect the actual host policy and captured artifacts and explicitly disposition these channels; this source change cannot certify their absence.

## Remaining privileged entrypoints

| Entry point | Existing privileged behavior | Disposition |
| --- | --- | --- |
| PdfTool/pdftoolpreflight.cpp | Ordinary preflight calls inspectPreflightFile in the host | Release blocker; migrate or exclude from untrusted admission |
| PdfTool/pdftoolabstractapplication.cpp | Shared commands call readFromFile / readFromFileOnHeap | Release blocker; audit all parser consumers |
| PdfTool/pdftooldiff.cpp and PdfTool/pdftoolverifyredaction.cpp | Each command parses multiple PDFs directly in the host | Release blocker; separate migration |
| PdfTool/pdftoolrepairdiff.cpp | Repair comparison reopens input directly | Release blocker; separate migration |
| PdfTool/pdftoolunite.cpp | Batch unification parses each selected input in the host | Release blocker; separate migration |
| PdfTool/pdftoolverifysignatures.cpp | Signature verification parses document input in the host | Release blocker; separate migration |
| LoopLibCore/sources/pdfgovernedexecution.cpp and pdfrepairdiff.cpp | Governed validation and repair comparison reopen candidates in-process | Release blocker; migrate callers or exclude from untrusted admission |
| LoopLibCore/sources/pdfdocumentbuilder.cpp | In-memory document reconstruction reparses generated bytes | Release blocker for pipelines containing untrusted content |
| PdfTool/pdftoolactionlist.cpp | Action-list input parsing and candidate execution remain in-process | Release blocker; separate migration |
| PdfTool/pdftoolrepair.cpp | Repair input, candidate reopening and final validation remain in-process | Release blocker; separate migration |
| LoopEditor/editorhost.cpp and LoopLibInteraction/sources/documentloader.cpp | Editor holds parsed state and runs preflight in-process | Release blocker; separate migration |
| LoopLibCore/sources/pdfpagemasterexport.cpp and render/transform consumers | Core batch export and document transformations remain in-process | Release blocker; outside this isolated open/preflight slice |

Issue #21 owns module release admission. Merging #20 does not clear these blockers.

## Verification

UnitTestsPdfWorkerIsolation executes the actual worker open/preflight route on Windows and Linux and has no qualifying platform skips. Its separate, non-installed LoopPdfWorkerProbe injects crashes, hangs, resource faults, malformed/oversized frames, protocol and identity mismatches, and privacy markers. It also probes outside-file reads, network connection, child creation, runtime/snapshot/package writes, private runtime/profile cleanup and temporary writes. Core tests exercise lossless receipt round-trip, unsupported schemas, coverage omissions, inconsistent states and stale identities.

Exact source identity, commands, fixture digests and observed results are recorded in the implementation handoff and external build evidence. Source checks alone do not qualify sandbox enforcement, telemetry or installed runtime behavior.

Local Windows focused verification executed the isolation and Core verdict suites with zero failures and zero skips. The mapped change gate ran all 58 owning test executables and passed; exact snapshots and subsequent admission checks are recorded in the external evidence. Network denial uses a live loopback listener with a supervisor connection as its control; an AppContainer connection must return access denied or remain blocked until the bounded probe deadline. Linux executable qualification and installed-product qualification remain unavailable in this run.

The parser inventory used PDFDocumentReader, readFromFileOnHeap and readFromBuffer searches across PdfTool, LoopEditor, LoopLibInteraction, LoopLibCore/sources, LoopLibQuick and desktop/smoke tools. Reader implementation methods are the shared primitive rather than additional product entrypoints. Renderer and transformation consumers retain the broader blocker above; this inventory does not grant them untrusted-content admission.
