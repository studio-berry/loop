# Add the host-neutral LoopLibInteraction seam

Category: internal
Audience: developers
Breaking-Change: no
Summary: Add LoopLibInteraction, a non-installed STATIC library between LoopLibCore and
the future Qt Quick layer, with the first two Core adapter seams (IDocumentRevisionSource
over PDFDocumentContext, IJobSubmitter over PDFJobScheduler). The target links neither
Qt6::Widgets nor Qt6::Qml/Quick, so Qt's per-module include paths are absent and a
presentation include there is a compile error rather than a review comment;
scripts/verify-interaction-boundary.py stops the link edge from being re-added and
architecture invariant I21 binds the rule to UnitTestsInteractionBoundary, which runs
guiless. No behavior moves out of LoopLibWidgets and no installed artifact changes.

Also closes the pre-Phase-4 admission checklist (S21/S22) that this seam landed
without: harvests and reconciles the QuickShellSmoke and CanvasBenchmark
qualification harnesses, quick-shell-policy/design-token/threat-model contracts,
and their verifiers from three previously-unmerged topic branches; amends ADR-009
to correct its S21 admission outcome (was "GO: retain Widgets", now "GO: admit a
direct QQuickItem adapter", matching the binding Quick-only decision); reconciles
two divergent ADR-010 drafts into one canonical S22 admission contract and amends
ADR-007 to mark its staged mixed-mode migration text as historical; wires both
qualification targets into CMake (non-installed, opt-in, default OFF) and CI
(Windows + Linux native/software backend smoke, canvas-benchmark, and
focus-bridge runs -- previously only claimed on one author's local machine).
Final-artifact SBOM/notices and product accessibility runtime remain explicitly
open: no shipped product Quick module exists yet at this stage to produce that
evidence from.
