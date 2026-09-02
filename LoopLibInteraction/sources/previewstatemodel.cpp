#include "previewstatemodel.h"

#include <utility>

namespace pdfinteraction
{

PreviewStateModel::PreviewStateModel(QObject* parent) :
    QObject(parent)
{
    qRegisterMetaType<Authority>();
    qRegisterMetaType<Status>();
}

void PreviewStateModel::setCurrentRevision(QString documentKey, QString documentRevision)
{
    if (documentKey == m_documentKey && documentRevision == m_documentRevision)
    {
        return;
    }

    const bool hadState = m_status != Status::Unavailable;
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_profileIdentity.clear();
    m_authority = Authority::None;
    m_summary = hadState ? QStringLiteral("Preview is stale") : QString();
    m_detail = hadState ? QStringLiteral("The document revision changed; preview must be regenerated") : QString();
    const bool statusChanged = m_status != (hadState ? Status::Stale : Status::Unavailable);
    setStatus(hadState ? Status::Stale : Status::Unavailable);
    if (!statusChanged)
    {
        Q_EMIT stateChanged();
    }
    Q_EMIT revisionChanged();
}

bool PreviewStateModel::setState(QString documentKey,
                                 QString documentRevision,
                                 Authority authority,
                                 QString summary,
                                 QString detail,
                                 QString profileIdentity)
{
    if (!isCurrent(documentKey, documentRevision) ||
        documentRevision.isEmpty() || authority == Authority::None || summary.isEmpty())
    {
        return false;
    }

    m_authority = authority;
    m_profileIdentity = std::move(profileIdentity);
    m_summary = std::move(summary);
    m_detail = std::move(detail);
    setStatus(Status::Ready);
    return true;
}

bool PreviewStateModel::setIncomplete(QString documentKey,
                                      QString documentRevision,
                                      QString summary,
                                      QString detail,
                                      QString profileIdentity)
{
    if (!isCurrent(documentKey, documentRevision) ||
        documentRevision.isEmpty() || summary.isEmpty())
    {
        return false;
    }

    const bool statusChanged = m_status != Status::Incomplete;
    m_authority = Authority::None;
    m_profileIdentity = std::move(profileIdentity);
    m_summary = std::move(summary);
    m_detail = std::move(detail);
    setStatus(Status::Incomplete);
    if (!statusChanged)
    {
        Q_EMIT stateChanged();
    }
    return true;
}

void PreviewStateModel::clear()
{
    const bool changed = m_status != Status::Unavailable || m_authority != Authority::None ||
                         !m_profileIdentity.isEmpty() || !m_summary.isEmpty() || !m_detail.isEmpty();
    if (!changed)
    {
        return;
    }

    const bool statusChanged = m_status != Status::Unavailable;
    m_authority = Authority::None;
    m_profileIdentity.clear();
    m_summary.clear();
    m_detail.clear();
    setStatus(Status::Unavailable);
    if (!statusChanged)
    {
        Q_EMIT stateChanged();
    }
}

bool PreviewStateModel::isCurrent(const QString& documentKey, const QString& documentRevision) const
{
    return documentKey == m_documentKey && documentRevision == m_documentRevision;
}

QString PreviewStateModel::authorityName(Authority authority)
{
    switch (authority)
    {
        case Authority::None:
            return QStringLiteral("none");
        case Authority::Approximate:
            return QStringLiteral("approximate");
        case Authority::Simulated:
            return QStringLiteral("simulated");
        case Authority::Authoritative:
            return QStringLiteral("authoritative");
    }
    return QStringLiteral("none");
}

QString PreviewStateModel::statusName(Status status)
{
    switch (status)
    {
        case Status::Unavailable:
            return QStringLiteral("unavailable");
        case Status::Ready:
            return QStringLiteral("ready");
        case Status::Stale:
            return QStringLiteral("stale");
        case Status::Incomplete:
            return QStringLiteral("incomplete");
    }
    return QStringLiteral("unavailable");
}

void PreviewStateModel::setStatus(Status status)
{
    const bool changed = m_status != status;
    m_status = status;
    if (changed || status == Status::Ready)
    {
        Q_EMIT stateChanged();
    }
}

}   // namespace pdfinteraction
