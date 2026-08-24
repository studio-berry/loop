#include "preflightcontroller.h"

namespace loupe::interaction
{

PreflightController::PreflightController(pdf::PDFJobScheduler* scheduler, QObject* parent) :
    QObject(parent),
    m_findings(this),
    m_scheduler(scheduler ? scheduler : &pdf::PDFJobScheduler::global())
{
    qRegisterMetaType<State>();
    qRegisterMetaType<EvidenceNavigationRequest>();
}

void PreflightController::setState(State state)
{
    if (m_state == state)
    {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

void PreflightController::setCurrentRevision(QString documentKey, QString documentRevision)
{
    const bool changed = documentKey != m_documentKey || documentRevision != m_documentRevision;
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    if (changed && m_state != State::NotChecked)
    {
        setState(State::Stale);
    }
}

bool PreflightController::updateProgress(const QString& jobId,
                                         const QString& documentRevision,
                                         int progress)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision || m_state != State::Running)
    {
        return false;
    }
    emit progressChanged(qBound(0, progress, 100));
    return true;
}

void PreflightController::beginRun(QString documentKey,
                                   QString documentRevision,
                                   QString profileDigest,
                                   QString jobId)
{
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_profileDigest = std::move(profileDigest);
    m_jobId = std::move(jobId);
    m_cancelRequested = false;
    m_findings.clear();
    setState(State::Running);
    emit progressChanged(0);
}

bool PreflightController::acceptResult(const QString& jobId,
                                       const QString& documentRevision,
                                       const pdf::PreflightResult& result)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision || m_cancelRequested)
    {
        return false;
    }

    m_findings.replace(m_documentKey, documentRevision, result.errors, result.warnings);
    emit progressChanged(100);
    if (!result.inspectionComplete)
    {
        setState(State::Incomplete);
    }
    else if (!result.errors.isEmpty() || !result.warnings.isEmpty())
    {
        setState(State::Findings);
    }
    else
    {
        setState(State::Pass);
    }
    return true;
}

bool PreflightController::cancelRun(const QString& jobId)
{
    if (jobId != m_jobId || m_state != State::Running)
    {
        return false;
    }
    m_cancelRequested = true;
    if (m_scheduler)
    {
        const pdf::PDFJobSnapshot snapshot = m_scheduler->snapshot(m_jobId);
        if (snapshot.jobId == m_jobId &&
            (snapshot.status == pdf::PDFJobStatus::Queued || snapshot.status == pdf::PDFJobStatus::Running))
        {
            m_scheduler->cancel(m_jobId);
        }
    }
    setState(State::Cancelled);
    return true;
}

bool PreflightController::navigationFor(const QString& findingId,
                                        EvidenceNavigationRequest* request) const
{
    if (!request || m_state == State::Stale || !m_findings.containsCurrent(findingId, m_documentRevision))
    {
        return false;
    }
    const PreflightFindingView* finding = m_findings.finding(findingId);
    if (!finding || finding->page <= 0)
    {
        return false;
    }

    request->findingId = finding->id;
    request->documentKey = finding->documentKey;
    request->documentRevision = finding->documentRevision;
    request->page = finding->page;
    request->bbox = finding->bbox;
    request->evidenceIds = finding->evidenceIds;
    return true;
}

QVector<FindingOverlay> PreflightController::overlaysForPage(int page) const
{
    if (m_state == State::Stale || m_state == State::Cancelled || m_state == State::NotChecked)
    {
        return {};
    }
    return m_findings.overlays(m_documentRevision, page);
}

} // namespace loupe::interaction
