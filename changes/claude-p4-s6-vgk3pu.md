# Scene-graph lifecycle and canvas parity for Quick product canvas

Category: internal
Audience: developers
Breaking-Change: no
Summary: Make the direct QQuickItem canvas releasable and recoverable (P4-S6).
Adds scene-graph invalidation and releaseResources handling, revision-fence tests,
UnitTestsCanvasParity against the non-installed Widgets layout oracle, and
first-view instrumentation via CanvasPresentMetrics.
