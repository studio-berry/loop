# Add the host-neutral LoupeLibInteraction seam

Category: internal
Audience: developers
Breaking-Change: no
Summary: Add LoupeLibInteraction, a non-installed STATIC library between LoupeLibCore and
the future Qt Quick layer, with the first two Core adapter seams (IDocumentRevisionSource
over PDFDocumentContext, IJobSubmitter over PDFJobScheduler). The target links neither
Qt6::Widgets nor Qt6::Qml/Quick, so Qt's per-module include paths are absent and a
presentation include there is a compile error rather than a review comment;
scripts/verify-interaction-boundary.py stops the link edge from being re-added and
architecture invariant I21 binds the rule to UnitTestsInteractionBoundary, which runs
guiless. No behavior moves out of LoupeLibWidgets and no installed artifact changes.
