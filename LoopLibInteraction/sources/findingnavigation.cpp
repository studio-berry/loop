// MIT License

#include "findingnavigation.h"

#include <QtGlobal>

#include <utility>

namespace pdfinteraction
{

namespace
{

FindingTargetingCapability pageOnly(const QString& checkId)
{
    Q_UNUSED(checkId);
    FindingTargetingCapability capability;
    capability.supportsPageNavigation = true;
    capability.supportsOverlayEvidence = true;
    capability.inspectionMode = QStringLiteral("page");
    return capability;
}

FindingTargetingCapability objectAndEvidence(const QString& checkId, FindingInspectionMode mode)
{
    Q_UNUSED(checkId);
    FindingTargetingCapability capability;
    capability.supportsPageNavigation = true;
    capability.supportsObjectTargeting = true;
    capability.supportsOverlayEvidence = true;
    capability.inspectionMode = QString::fromLatin1(getFindingInspectionModeName(mode));
    return capability;
}

bool hasCapability(const FindingTargetingCapability& capability, FindingNavigationCapability flag)
{
    switch (flag)
    {
        case FindingNavigationCapability::None:
            return false;
        case FindingNavigationCapability::PageNavigation:
            return capability.supportsPageNavigation;
        case FindingNavigationCapability::ObjectTargeting:
            return capability.supportsObjectTargeting;
        case FindingNavigationCapability::OverlayEvidence:
            return capability.supportsOverlayEvidence;
        case FindingNavigationCapability::InspectionMode:
            return capability.inspectionMode != QStringLiteral("page") &&
                   capability.inspectionMode != QStringLiteral("none") && !capability.inspectionMode.isEmpty();
    }
    return false;
}

FindingInspectionMode modeForCapability(const FindingTargetingCapability& capability)
{
    if (capability.inspectionMode == QStringLiteral("probe"))
    {
        return FindingInspectionMode::Probe;
    }
    if (capability.inspectionMode == QStringLiteral("render"))
    {
        return FindingInspectionMode::Render;
    }
    return FindingInspectionMode::None;
}

bool usableBounds(const QRectF& bounds)
{
    return !bounds.isNull() && !bounds.isEmpty() && bounds.width() > 0.0 && bounds.height() > 0.0;
}

}   // namespace

const char* getFindingInspectionModeName(FindingInspectionMode mode)
{
    switch (mode)
    {
        case FindingInspectionMode::None:
            return "none";
        case FindingInspectionMode::Render:
            return "render";
        case FindingInspectionMode::Probe:
            return "probe";
    }
    return "none";
}

const char* getFindingNavigationOutcomeName(FindingNavigationOutcome outcome)
{
    switch (outcome)
    {
        case FindingNavigationOutcome::Rejected:
            return "rejected";
        case FindingNavigationOutcome::Stale:
            return "stale";
        case FindingNavigationOutcome::UnsupportedCheck:
            return "unsupported-check";
        case FindingNavigationOutcome::DocumentFallback:
            return "document-fallback";
        case FindingNavigationOutcome::PageFallback:
            return "page-fallback";
        case FindingNavigationOutcome::ObjectTargeted:
            return "object-targeted";
    }
    return "rejected";
}

FindingNavigationRequest FindingNavigationRequest::fromFinding(const PreflightFindingView& finding)
{
    FindingNavigationRequest request;
    request.findingId = finding.id;
    request.checkId = finding.checkId;
    request.documentKey = finding.documentKey;
    request.documentRevision = finding.documentRevision;
    request.page = finding.page;
    request.objectId = finding.objectId;
    request.pageBounds = finding.bbox;
    request.evidenceIds = finding.evidenceIds;
    return request;
}

bool FindingTargetingCapabilityRegistry::registerCapability(const QString& checkId, FindingTargetingCapability capability)
{
    const QString normalized = checkId.trimmed();
    if (normalized.isEmpty())
    {
        return false;
    }

    m_capabilities.insert(normalized, std::move(capability));
    return true;
}

bool FindingTargetingCapabilityRegistry::contains(const QString& checkId) const
{
    return m_capabilities.contains(checkId);
}

std::optional<FindingTargetingCapability> FindingTargetingCapabilityRegistry::capabilityFor(const QString& checkId) const
{
    const auto it = m_capabilities.constFind(checkId);
    if (it != m_capabilities.constEnd())
    {
        return it.value();
    }
    return m_allowConservativeFallback ? std::optional<FindingTargetingCapability>(fallbackCapability()) : std::nullopt;
}

FindingTargetingCapabilityRegistry FindingTargetingCapabilityRegistry::defaultRegistry()
{
    FindingTargetingCapabilityRegistry registry;
    registry.m_allowConservativeFallback = true;
    const auto add = [&registry](const QString& checkId, FindingTargetingCapability capability)
    {
        registry.m_capabilities.insert(checkId, std::move(capability));
    };
    for (const QString& checkId : { QStringLiteral("bleed"),
                                    QStringLiteral("content-bleed"),
                                    QStringLiteral("trim"),
                                    QStringLiteral("page-size"),
                                    QStringLiteral("output-intent"),
                                    QStringLiteral("thin-parts"),
                                    QStringLiteral("ink-coverage") })
    {
        add(checkId, pageOnly(checkId));
    }

    add(QStringLiteral("image-resolution"), objectAndEvidence(QStringLiteral("image-resolution"), FindingInspectionMode::Probe));
    add(QStringLiteral("color-mode"), objectAndEvidence(QStringLiteral("color-mode"), FindingInspectionMode::Probe));
    add(QStringLiteral("color-inventory"), objectAndEvidence(QStringLiteral("color-inventory"), FindingInspectionMode::Probe));
    add(QStringLiteral("transparency-risk"), objectAndEvidence(QStringLiteral("transparency-risk"), FindingInspectionMode::Render));
    add(QStringLiteral("white-overprint"), objectAndEvidence(QStringLiteral("white-overprint"), FindingInspectionMode::Render));
    add(QStringLiteral("embedded-fonts"), objectAndEvidence(QStringLiteral("embedded-fonts"), FindingInspectionMode::Probe));
    add(QStringLiteral("thin-strokes"), objectAndEvidence(QStringLiteral("thin-strokes"), FindingInspectionMode::Probe));
    return registry;
}

FindingTargetingCapability FindingTargetingCapabilityRegistry::fallbackCapability()
{
    // An unregistered check may still be useful at document/page scope. It is
    // intentionally unable to select an object, paint evidence, or request a
    // specialized inspection mode.
    return {};
}

FindingCanvasNavigator::FindingCanvasNavigator(IDocumentRevisionSource& revisions,
                                               ViewportController& viewport,
                                               InteractionController& interaction,
                                               OverlayBuilder& overlays,
                                               FindingTargetingCapabilityRegistry registry,
                                               QObject* parent) :
    QObject(parent),
    m_revisions(&revisions),
    m_viewport(&viewport),
    m_interaction(&interaction),
    m_overlays(&overlays),
    m_registry(std::move(registry))
{
    qRegisterMetaType<FindingNavigationResult>();
    qRegisterMetaType<FindingInspectionModeRequest>();
}

bool FindingCanvasNavigator::requestMatchesCurrentRevision(const FindingNavigationRequest& request) const
{
    if (!m_revisions || !request.isValid())
    {
        return false;
    }

    const pdf::PDFRevisionIdentity current = m_revisions->currentRevision();
    return current.isValid() && request.documentKey == current.document.documentId &&
           request.documentRevision == current.toString();
}

bool FindingCanvasNavigator::revealPage(int pageIndex)
{
    if (!m_viewport || pageIndex < 0 || pageIndex >= m_viewport->pageCount())
    {
        return false;
    }

    if (m_viewport->isBlockMode())
    {
        m_viewport->setBlockIndex(m_viewport->blockIndexForPage(pageIndex));
    }

    const QRect pageRect = m_viewport->placedPageRect(pageIndex);
    const QRect viewportRect = m_viewport->viewportRect();
    if (!pageRect.isValid() || viewportRect.isEmpty())
    {
        return false;
    }

    const QPointF delta = QPointF(viewportRect.center()) - QPointF(pageRect.center());
    m_viewport->setOffset(m_viewport->offset() + QPoint(qRound(delta.x()), qRound(delta.y())));
    return m_viewport->placedPageRect(pageIndex).isValid();
}

bool FindingCanvasNavigator::focusRegion(int pageIndex, QRectF pageBounds, qreal contextMargin)
{
    if (!m_viewport || !usableBounds(pageBounds) || !revealPage(pageIndex))
    {
        return false;
    }

    const qreal margin = qBound(0.0, contextMargin, 1.0);
    pageBounds = pageBounds.normalized();
    pageBounds = pageBounds.adjusted(-pageBounds.width() * margin,
                                     -pageBounds.height() * margin,
                                     pageBounds.width() * margin,
                                     pageBounds.height() * margin);

    const QRect viewportRect = m_viewport->viewportRect();
    const QPointF viewportCenter(viewportRect.center());
    QTransform pageToViewport = m_viewport->pagePointToViewportMatrix(pageIndex);
    QRectF deviceBounds = pageToViewport.mapRect(pageBounds).normalized();
    if (!usableBounds(deviceBounds) || viewportRect.isEmpty())
    {
        return false;
    }

    const qreal availableWidth = qMax<qreal>(1.0, viewportRect.width() * 0.75);
    const qreal availableHeight = qMax<qreal>(1.0, viewportRect.height() * 0.75);
    const qreal scaleFactor = qMin(availableWidth / deviceBounds.width(), availableHeight / deviceBounds.height());
    m_viewport->setZoom(m_viewport->zoom() * scaleFactor, viewportCenter);

    pageToViewport = m_viewport->pagePointToViewportMatrix(pageIndex);
    deviceBounds = pageToViewport.mapRect(pageBounds).normalized();
    const QPointF delta = viewportCenter - deviceBounds.center();
    m_viewport->setOffset(m_viewport->offset() + QPoint(qRound(delta.x()), qRound(delta.y())));
    return true;
}

FindingNavigationResult FindingCanvasNavigator::navigate(const FindingNavigationRequest& request)
{
    FindingNavigationResult result;
    result.findingId = request.findingId;
    result.checkId = request.checkId;

    const std::optional<FindingTargetingCapability> capability = m_registry.capabilityFor(request.checkId);
    if (!request.isValid())
    {
        result.reason = QStringLiteral("finding-navigation/invalid-request");
    }
    else if (!capability.has_value())
    {
        result.outcome = FindingNavigationOutcome::UnsupportedCheck;
        result.reason = QStringLiteral("finding-navigation/check-not-registered");
    }
    else if (!requestMatchesCurrentRevision(request))
    {
        clearPresentationState(true);
        result.outcome = FindingNavigationOutcome::Stale;
        result.reason = QStringLiteral("finding-navigation/stale-revision");
    }
    else
    {
        const FindingTargetingCapability& declared = *capability;
        ++m_navigationGeneration;
        result.navigationGeneration = m_navigationGeneration;
        m_currentFindingId = request.findingId;
        m_currentCheckId = request.checkId;
        m_currentDocumentRevision = request.documentRevision;
        const bool hadMode = m_currentMode != FindingInspectionMode::None;
        m_currentMode = FindingInspectionMode::None;
        m_modeRequest = FindingInspectionModeRequest();
        if (hadMode)
        {
            Q_EMIT inspectionModeReset();
        }
        m_overlays->setEvidence({});
        m_overlays->setFocusedId({});

        const int pageIndex = request.page - 1;
        const bool pageAvailable = hasCapability(declared, FindingNavigationCapability::PageNavigation) &&
                                   revealPage(pageIndex);
        if (!pageAvailable)
        {
            m_interaction->selectTarget(InteractionTarget());
            result.outcome = FindingNavigationOutcome::DocumentFallback;
            result.reason = QStringLiteral("finding-navigation/page-unavailable");
        }
        else
        {
            result.pageIndex = pageIndex;
            m_overlays->setFocusedId(request.findingId);
            InteractionTarget selection;
            selection.kind = InteractionTargetKind::Finding;
            selection.pageIndex = pageIndex;
            selection.id = request.findingId;
            selection.pageBounds = request.pageBounds.normalized();

            const bool objectTargeted = hasCapability(declared, FindingNavigationCapability::ObjectTargeting) &&
                                        !request.objectId.isEmpty() &&
                                        focusRegion(pageIndex, request.pageBounds, declared.contextMargin);
            if (usableBounds(request.pageBounds) && hasCapability(declared, FindingNavigationCapability::OverlayEvidence))
            {
                m_interaction->selectTarget(selection);
            }
            else
            {
                m_interaction->selectTarget(InteractionTarget());
            }

            if (hasCapability(declared, FindingNavigationCapability::OverlayEvidence))
            {
                QList<InteractionTarget> evidence;
                for (const FindingEvidenceTarget& candidate : request.evidenceTargets)
                {
                    if (candidate.evidenceId.isEmpty() || candidate.page <= 0 || !usableBounds(candidate.pageBounds))
                    {
                        continue;
                    }

                    InteractionTarget target;
                    target.kind = InteractionTargetKind::Finding;
                    target.pageIndex = candidate.page - 1;
                    target.id = candidate.evidenceId;
                    target.pageBounds = candidate.pageBounds.normalized();
                    evidence.push_back(std::move(target));
                }
                m_overlays->setEvidence(std::move(evidence));
            }

            result.outcome = objectTargeted ? FindingNavigationOutcome::ObjectTargeted : FindingNavigationOutcome::PageFallback;
            result.reason = objectTargeted ? QStringLiteral("finding-navigation/object-targeted")
                                           : QStringLiteral("finding-navigation/page-fallback");

            if (hasCapability(declared, FindingNavigationCapability::InspectionMode))
            {
                m_currentMode = modeForCapability(declared);
                m_modeRequest = { request.findingId,
                                  request.checkId,
                                  request.documentKey,
                                  request.documentRevision,
                                  request.page,
                                  request.objectId,
                                  request.evidenceIds,
                                  m_currentMode,
                                  m_navigationGeneration };
                Q_EMIT inspectionModeRequested(m_modeRequest);
                result.inspectionMode = m_currentMode;
            }

            // Builder inputs are presentation state. Refresh explicitly so an
            // evidence-only request (no selectable finding bounds) still
            // reaches the existing immutable overlay frame.
            m_interaction->refreshOverlay();
        }
    }

    Q_EMIT navigationApplied(result);
    return result;
}

void FindingCanvasNavigator::clearPresentationState(bool advanceGeneration)
{
    if (advanceGeneration)
    {
        ++m_navigationGeneration;
    }

    const bool hadMode = m_currentMode != FindingInspectionMode::None;
    m_currentFindingId.clear();
    m_currentCheckId.clear();
    m_currentDocumentRevision.clear();
    m_currentMode = FindingInspectionMode::None;
    m_modeRequest = FindingInspectionModeRequest();
    if (m_overlays)
    {
        m_overlays->setEvidence({});
        m_overlays->setFocusedId({});
    }
    if (m_interaction)
    {
        m_interaction->selectTarget(InteractionTarget());
        m_interaction->refreshOverlay();
    }
    if (hadMode)
    {
        Q_EMIT inspectionModeReset();
    }
}

void FindingCanvasNavigator::deselect()
{
    clearPresentationState(true);
}

void FindingCanvasNavigator::invalidate()
{
    clearPresentationState(true);
}

bool FindingCanvasNavigator::isCurrent(const FindingNavigationResult& result) const
{
    if (!result.accepted() || result.navigationGeneration == 0 || result.navigationGeneration != m_navigationGeneration ||
        !m_revisions)
    {
        return false;
    }

    const pdf::PDFRevisionIdentity current = m_revisions->currentRevision();
    return current.isValid() && result.findingId == m_currentFindingId && result.checkId == m_currentCheckId &&
           result.navigationGeneration == m_navigationGeneration && m_currentDocumentRevision == current.toString();
}

}   // namespace pdfinteraction
