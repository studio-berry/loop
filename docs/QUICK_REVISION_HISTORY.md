# Quick governed revision history

Issue [#124](https://github.com/studio-berry/loop/issues/124) implements Edit → Undo
and Redo as forward rollback through Core. Each confirmation is bound to the
shown document revision and target identity. Viewport, workspace, and selection
changes never enter the document history.

The Fix workspace shows canonical event identities, artifact digests, approval
actors and decisions, and governed publication receipt/sign-off details. It
checks Core's history chain before enabling navigation. Undo of an accepted
correction targets that execution's input. Redo follows recorded rollback
transitions; a new correction clears the redo branch. These choices are rebuilt
from the retained chain after reopen, rather than from a transient byte stack.

Editor corrections now retain their exact published artifact as a unique sibling
file and append Core's execution and receipt to its existing `.loop-history`
sidecar format. A subsequent publication or rollback carries a verified snapshot
of the preceding chain and immutable artifact store. The source file and its
approved events stay intact. Open published artifact opens the retained output;
ordinary Save remains the document facade's existing save behavior.

The human-approved Core contract extension permits a retained original input as
a rollback target only when its digest is the input of the accepted execution
being undone and the request's current digest identifies that accepted output.
It does not synthesize an acceptance event for the original input. Existing
request types and persisted schemas are unchanged. Core still verifies target
bytes, requires approval, revalidates, and appends a new rolled-back receipt.

History snapshots require the previous SQLite writer to have closed/checkpointed
and refuse a live WAL. They copy existing metadata and retained artifacts,
verify the resulting chain, and publish the sidecar before the new artifact.
A failed or cancelled request can retain a failed-history sidecar; it never
removes an approved revision or receipt. A rollback already committed by Core
remains retained when the owner completion is cancelled or stale; it is not opened.

`UnitTestsOperationHistory` covers first-correction rollback, incorrect execution
and current identities, tampered original bytes, unchanged approved events, and
reopen. `UnitTestsEditorHost` covers publish → Undo → reopen → Redo → reopen,
artifact/receipt identity, stale confirmations, and presentation-only state.
The mapped Core, Interaction, Quick, and product Quick smoke lanes provide the
remaining regression proof. No package or native accessibility qualification
is asserted by the software Quick smoke.
