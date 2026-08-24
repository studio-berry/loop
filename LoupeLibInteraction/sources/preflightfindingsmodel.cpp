#include "preflightfindingsmodel.h"

#include <QVariant>

namespace pdfinteraction
{

PreflightFindingsModel::PreflightFindingsModel(QObject* parent) : QAbstractListModel(parent)
{
}

int PreflightFindingsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_findings.size();
}

QVariant PreflightFindingsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_findings.size())
    {
        return {};
    }

    const PreflightFindingView& finding = m_findings.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case MessageRole: return finding.message;
        case FindingIdRole: return finding.id;
        case DocumentKeyRole: return finding.documentKey;
        case DocumentRevisionRole: return finding.documentRevision;
        case ScopeRole: return finding.scope;
        case PageRole: return finding.page;
        case ObjectIdRole: return finding.objectId;
        case SeverityRole: return finding.severity;
        case TypeRole: return finding.type;
        case CheckIdRole: return finding.checkId;
        case BoundingBoxRole: return finding.bbox;
        case EvidenceIdsRole: return finding.evidenceIds;
        case SelectedRole: return finding.selected;
    }
    return {};
}

QHash<int, QByteArray> PreflightFindingsModel::roleNames() const
{
    return {
        { FindingIdRole, "findingId" },
        { DocumentKeyRole, "documentKey" },
        { DocumentRevisionRole, "documentRevision" },
        { ScopeRole, "scope" },
        { PageRole, "page" },
        { ObjectIdRole, "objectId" },
        { SeverityRole, "severity" },
        { TypeRole, "type" },
        { MessageRole, "message" },
        { CheckIdRole, "checkId" },
        { BoundingBoxRole, "boundingBox" },
        { EvidenceIdsRole, "evidenceIds" },
        { SelectedRole, "selected" }
    };
}

PreflightFindingView PreflightFindingsModel::makeView(const QString& documentKey,
                                                      const QString& documentRevision,
                                                      const pdf::PreflightFinding& finding)
{
    return {
        finding.stableId(), documentKey, documentRevision, finding.scope, finding.page,
        finding.objectId, finding.severity, finding.type, finding.message, finding.checkId,
        finding.bbox, finding.evidenceIds, false
    };
}

void PreflightFindingsModel::replace(QString documentKey,
                                     QString documentRevision,
                                     const QList<pdf::PreflightFinding>& errors,
                                     const QList<pdf::PreflightFinding>& warnings)
{
    QVector<PreflightFindingView> next;
    next.reserve(errors.size() + warnings.size());
    for (const pdf::PreflightFinding& finding : errors)
    {
        next.push_back(makeView(documentKey, documentRevision, finding));
    }
    for (const pdf::PreflightFinding& finding : warnings)
    {
        next.push_back(makeView(documentKey, documentRevision, finding));
    }

    beginResetModel();
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_findings = std::move(next);
    m_selectedFindingId.clear();
    endResetModel();
}

void PreflightFindingsModel::clear()
{
    beginResetModel();
    m_findings.clear();
    m_documentKey.clear();
    m_documentRevision.clear();
    m_selectedFindingId.clear();
    endResetModel();
}

void PreflightFindingsModel::setSelectedFinding(const QString& findingId)
{
    if (!findingId.isEmpty() && !finding(findingId))
    {
        return;
    }

    const QString previous = m_selectedFindingId;
    m_selectedFindingId = findingId;
    for (PreflightFindingView& item : m_findings)
    {
        item.selected = item.id == m_selectedFindingId;
    }

    for (int row = 0; row < m_findings.size(); ++row)
    {
        if (m_findings.at(row).id == previous || m_findings.at(row).id == m_selectedFindingId)
        {
            Q_EMIT dataChanged(index(row), index(row), { SelectedRole });
        }
    }
}

bool PreflightFindingsModel::containsCurrent(const QString& findingId, const QString& documentRevision) const
{
    return documentRevision == m_documentRevision && finding(findingId) != nullptr;
}

const PreflightFindingView* PreflightFindingsModel::finding(const QString& findingId) const
{
    for (const PreflightFindingView& item : m_findings)
    {
        if (item.id == findingId)
        {
            return &item;
        }
    }
    return nullptr;
}

QVector<FindingOverlay> PreflightFindingsModel::overlays(const QString& documentRevision, int page) const
{
    QVector<FindingOverlay> result;
    if (documentRevision != m_documentRevision)
    {
        return result;
    }
    for (const PreflightFindingView& finding : m_findings)
    {
        if (finding.page == page)
        {
            result.push_back({ finding.id, finding.documentRevision, finding.page, finding.bbox,
                               finding.severity, finding.selected });
        }
    }
    return result;
}

QVector<PreflightFindingView> PreflightFindingsModel::filtered(QString severity, QString checkId, int page) const
{
    QVector<PreflightFindingView> result;
    for (const PreflightFindingView& finding : m_findings)
    {
        if (!severity.isEmpty() && finding.severity != severity)
        {
            continue;
        }
        if (!checkId.isEmpty() && finding.checkId != checkId)
        {
            continue;
        }
        if (page > 0 && finding.page != page)
        {
            continue;
        }
        result.push_back(finding);
    }
    return result;
}

QHash<QString, int> PreflightFindingsModel::groupCounts(QString severity) const
{
    QHash<QString, int> result;
    for (const PreflightFindingView& finding : m_findings)
    {
        if (!severity.isEmpty() && finding.severity != severity)
        {
            continue;
        }
        ++result[finding.checkId];
    }
    return result;
}

} // namespace pdfinteraction
