# Add the document lifecycle facade and the one command catalog

Category: internal
Audience: developers
Breaking-Change: no
Summary: Put the first behaviour into the P4-S1 seam. LoopLibInteraction gains DocumentFacade,
a host-neutral open/close/reopen/save lifecycle over the existing Core services, and
CommandCatalog, the single command registry. Both are drivable, and are tested, without a
QWidget or a QML engine (UnitTestsDocumentFacade, QTEST_GUILESS_MAIN, no Qt6::Widgets on the
link line), which is the P4-S2 exit condition.

No second anything: pdf::PDFDocumentContext stays the document identity and revision fence,
pdf::PDFJobScheduler stays the only scheduler (reached through P4-S1's IJobSubmitter, so the
async open and save are scheduler jobs rather than new QtConcurrent launches), and
pdf::PDFDocumentReader/PDFDocumentWriter stay the only reader and writer behind narrow
IDocumentLoader/IDocumentWriter seams. What the facade adds is the state machine a host can
bind to plus admission rules: a completion is admitted only if its document generation is
still current, so a superseded read is counted and dropped rather than patched into the
document that replaced it.

The command catalog extends docs/loop-shell-actions.json in place rather than forking a
second registry. All 107 Editor action IDs gain a `command` descriptor (label key, shortcut,
typed parameters, capability, cancellability, availability); the four document-lifecycle
commands are `implemented` and the remaining 103 are `declared`, which returns a typed
not-implemented terminal state and mutates nothing. scripts/verify-command-catalog.py checks
the block and checks shortcut parity against PDFActionManager::initActions, so the catalog
cannot drift into a second command truth wearing the Editor's ID set.

Fixes a latent LoopLibCore defect this slice is the first to hit. The owning overload
PDFDocumentContext::setDocument(PDFDocumentPointer, flags) passed document.data() and
std::move(document) as two arguments of one call. Argument evaluation order is unspecified,
and MSVC evaluates right to left, so the move nulled the pointer before data() was read: the
context ended up owning a document while reporting none, with an empty identity and an
invalid session. The overload had no callers anywhere in the repository, so this had never
been exercised. The fix reads the raw pointer before the move;
UnitTestsDocumentSession::setDocument_ownedPointerBindsTheDocument pins it, and was confirmed
to fail against the pre-fix code before the fix was applied.

Also: docs/schemas/loop-shell-actions.schema.json describes the new optional `command`
object (additive, schema_version stays 1); architecture invariant I22 binds the rule to
UnitTestsDocumentFacade; docs/LOOP_SHELL_CONTRACT.md documents the descriptor fields and
the DocumentFacade-to-status_contract.document projection. No behaviour is removed from
LoopLibWidgets or LoopLibGui -- the Widgets path keeps working as the Phase 4 migration
oracle -- and no installed artifact changes.

Phase 5 inherits one thing from this slice: loop-shell-actions.json derives its ID set from
LoopLibGui/pdfeditormainwindow.ui, so when that form is deleted the parity check in
verify-loop-shell-contract.ps1 loses its source and the file must become self-authoritative.
