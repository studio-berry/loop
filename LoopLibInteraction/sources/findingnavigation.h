// MIT License

#ifndef FINDINGNAVIGATION_H
#define FINDINGNAVIGATION_H

#include "documentcontextsource.h"
#include "interactioncontroller.h"
#include "overlaybuilder.h"
#include "preflightfindingsmodel.h"
#include "viewportcontroller.h"

#include <QFlags>
#include <QHash>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <optional>

namespace pdfinteraction
{

/// Conservative, check-owned targeting facts. Unknown checks are absent from
/// the registry and therefore cannot acquire an object target or overlay by
/// inference.
struct FindingTargetingCapability
{
    bool supportsPageNavigation = true;
    bool supportsObjectTargeting = false;
    bool supportsOverlayEvidence = false;
    QString inspectionMode = QStringLiteral("page");
    qreal contextMargin = 0.2;

    bool hasTarget() const noexcept
    {
        return supportsObjectTargeting || supportsOverlayEvidence;
    }
};

/// Capabilities are declared by check id rather than inferred from a finding.
/// This keeps an unusual or incomplete finding from accidentally becoming an
/// object-selection request. The registry is the one place where product
/// knowledge about check targeting lives.
enum class FindingNavigationCapability : quint8
{
    None = 0,
    PageNavigation = 1 << 0,
    ObjectTargeting = 1 << 1,
    OverlayEvidence = 1 << 2,
    InspectionMode = 1 << 3
};
Q_DECLARE_FLAGS(FindingNavigationCapabilities, FindingNavigationCapability)

enum class FindingInspectionMode
{
    None,
    Render,
    Probe
};

const char* getFindingInspectionModeName(FindingInspectionMode mode);

/// A check-owned declaration of what the canvas may do for its findings.
/// Stable evidence geometry supplied by Core's report/evidence graph. The
/// navigation layer never manufactures geometry from an id.
struct FindingEvidenceTarget
{
    QString evidenceId;
    int page = 0;
    QRectF pageBounds;
};

/// Typed input to the canvas navigation seam. `findingId` must be the value
/// returned by pdf::PreflightFinding::stableId(); rows and container positions
/// are intentionally absent.
struct FindingNavigationRequest
{
    QString findingId;
    QString checkId;
    QString documentKey;
    QString documentRevision;
    int page = 0;
    QString objectId;
    QRectF pageBounds;
    QStringList evidenceIds;
    QList<FindingEvidenceTarget> evidenceTargets;

    bool isValid() const
    {
        return !findingId.isEmpty() && !checkId.isEmpty() && !documentRevision.isEmpty();
    }

    static FindingNavigationRequest fromFinding(const PreflightFindingView& finding);
};

struct FindingInspectionModeRequest
{
    QString findingId;
    QString checkId;
    QString documentKey;
    QString documentRevision;
    int page = 0;
    QString objectId;
    QStringList evidenceIds;
    FindingInspectionMode mode = FindingInspectionMode::None;
    quint64 navigationGeneration = 0;

    bool isValid() const
    {
        return !findingId.isEmpty() && !checkId.isEmpty() && !documentRevision.isEmpty() &&
               mode != FindingInspectionMode::None;
    }
};

enum class FindingNavigationOutcome
{
    Rejected,
    Stale,
    UnsupportedCheck,
    DocumentFallback,
    PageFallback,
    ObjectTargeted
};

const char* getFindingNavigationOutcomeName(FindingNavigationOutcome outcome);

struct FindingNavigationResult
{
    FindingNavigationOutcome outcome = FindingNavigationOutcome::Rejected;
    QString findingId;
    QString checkId;
    QString reason;
    int pageIndex = -1;
    FindingInspectionMode inspectionMode = FindingInspectionMode::None;
    quint64 navigationGeneration = 0;

    bool accepted() const
    {
        return outcome == FindingNavigationOutcome::DocumentFallback ||
               outcome == FindingNavigationOutcome::PageFallback ||
               outcome == FindingNavigationOutcome::ObjectTargeted;
    }
};

/// Registry of explicit check-id targeting capabilities. An empty registry is
/// useful for tests and for a host that has not opted into a check. Unknown
/// checks are never treated as page-capable by fallback inference.
class FindingTargetingCapabilityRegistry final
{
public:
    bool registerCapability(const QString& checkId, FindingTargetingCapability capability);
    bool contains(const QString& checkId) const;
    std::optional<FindingTargetingCapability> capabilityFor(const QString& checkId) const;

    /// Explicit declarations for the checks shipped in the current preflight
    /// catalog. Callers may copy this registry and narrow it for a host/profile.
    static FindingTargetingCapabilityRegistry defaultRegistry();
    static FindingTargetingCapability fallbackCapability();

private:
    QHash<QString, FindingTargetingCapability> m_capabilities;
    bool m_allowConservativeFallback = false;
};

/// Connects a stable finding request to the existing viewport, interaction and
/// overlay machinery. It owns no document, renderer, or evidence graph.
///
/// Inspection modes are requests only: the host can route Render and Probe to
/// its existing page-surface/evidence machinery. This class never renders or
/// probes, and it never creates a second overlay stack.
class FindingCanvasNavigator final : public QObject
{
    Q_OBJECT

public:
    FindingCanvasNavigator(IDocumentRevisionSource& revisions,
                           ViewportController& viewport,
                           InteractionController& interaction,
                           OverlayBuilder& overlays,
                           FindingTargetingCapabilityRegistry registry,
                           QObject* parent = nullptr);

    FindingNavigationResult navigate(const FindingNavigationRequest& request);
    void deselect();
    void invalidate();

    FindingInspectionMode currentInspectionMode() const noexcept { return m_currentMode; }
    const FindingInspectionModeRequest& currentInspectionRequest() const noexcept { return m_modeRequest; }
    quint64 navigationGeneration() const noexcept { return m_navigationGeneration; }
    bool isCurrent(const FindingNavigationResult& result) const;

signals:
    void navigationApplied(pdfinteraction::FindingNavigationResult result);
    void inspectionModeRequested(pdfinteraction::FindingInspectionModeRequest request);
    void inspectionModeReset();

private:
    bool requestMatchesCurrentRevision(const FindingNavigationRequest& request) const;
    bool revealPage(int pageIndex);
    bool focusRegion(int pageIndex, QRectF pageBounds, qreal contextMargin);
    void clearPresentationState(bool advanceGeneration);

    IDocumentRevisionSource* m_revisions = nullptr;
    ViewportController* m_viewport = nullptr;
    InteractionController* m_interaction = nullptr;
    OverlayBuilder* m_overlays = nullptr;
    FindingTargetingCapabilityRegistry m_registry;
    QString m_currentFindingId;
    QString m_currentCheckId;
    QString m_currentDocumentRevision;
    FindingInspectionMode m_currentMode = FindingInspectionMode::None;
    FindingInspectionModeRequest m_modeRequest;
    quint64 m_navigationGeneration = 0;
};

}   // namespace pdfinteraction

Q_DECLARE_OPERATORS_FOR_FLAGS(pdfinteraction::FindingNavigationCapabilities)
Q_DECLARE_METATYPE(pdfinteraction::FindingInspectionMode)
Q_DECLARE_METATYPE(pdfinteraction::FindingNavigationOutcome)
Q_DECLARE_METATYPE(pdfinteraction::FindingNavigationResult)
Q_DECLARE_METATYPE(pdfinteraction::FindingInspectionModeRequest)

#endif   // FINDINGNAVIGATION_H
