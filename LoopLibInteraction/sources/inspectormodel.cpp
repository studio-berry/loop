#include "inspectormodel.h"

#include <QStringList>

#include <utility>

namespace pdfinteraction
{

namespace
{

QString rectText(const QRectF& rect)
{
    return QStringLiteral("%1,%2 %3x%4")
        .arg(QString::number(rect.x()), QString::number(rect.y()), QString::number(rect.width()), QString::number(rect.height()));
}

}   // namespace

InspectorModel::InspectorModel(QObject* parent) :
    QAbstractListModel(parent)
{
    qRegisterMetaType<SelectionKind>();
}

int InspectorModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_properties.size();
}

QVariant InspectorModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_properties.size())
    {
        return {};
    }

    const InspectorProperty& property = m_properties.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case ValueRole:
            return property.value;
        case PropertyIdRole:
            return property.id;
        case LabelRole:
            return property.label;
        default:
            return {};
    }
}

QHash<int, QByteArray> InspectorModel::roleNames() const
{
    return {
        { PropertyIdRole, "propertyId" },
        { LabelRole, "label" },
        { ValueRole, "value" }
    };
}

void InspectorModel::setCurrentRevision(QString documentKey, QString documentRevision)
{
    if (documentKey == m_documentKey && documentRevision == m_documentRevision)
    {
        return;
    }

    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    clearSelection();
    Q_EMIT revisionChanged();
}

bool InspectorModel::setSelection(const Selection& selection)
{
    if (!isCurrent(selection.documentKey, selection.documentRevision))
    {
        return false;
    }
    if (selection.kind != SelectionKind::EmptyCanvas && selection.selectionId.isEmpty())
    {
        return false;
    }

    beginResetModel();
    m_selectionId = selection.selectionId;
    m_title = selection.title;
    m_selectionKind = selection.kind;
    m_properties = selection.properties;
    endResetModel();
    Q_EMIT selectionChanged();
    return true;
}

bool InspectorModel::setFindingSelection(const PreflightFindingsModel& findings,
                                         const QString& findingId,
                                         const QString& documentRevision)
{
    if (documentRevision != m_documentRevision ||
        findings.documentKey() != m_documentKey ||
        findings.documentRevision() != m_documentRevision)
    {
        return false;
    }

    const PreflightFindingView* finding = findings.finding(findingId);
    if (!finding)
    {
        return false;
    }

    Selection selection;
    selection.documentKey = m_documentKey;
    selection.documentRevision = m_documentRevision;
    selection.selectionId = finding->id;
    selection.title = finding->message;
    selection.kind = SelectionKind::Finding;
    selection.properties = {
        { QStringLiteral("severity"), QStringLiteral("Severity"), finding->severity },
        { QStringLiteral("check"), QStringLiteral("Check"), finding->checkId },
        { QStringLiteral("scope"), QStringLiteral("Scope"), finding->scope },
        { QStringLiteral("page"), QStringLiteral("Page"), QString::number(finding->page) },
        { QStringLiteral("object"), QStringLiteral("Object"), finding->objectId },
        { QStringLiteral("bounds"), QStringLiteral("Bounds"), rectText(finding->bbox) },
        { QStringLiteral("evidence"), QStringLiteral("Evidence"), finding->evidenceIds.join(QStringLiteral(", ")) },
        { QStringLiteral("message"), QStringLiteral("Finding"), finding->message }
    };
    return setSelection(selection);
}

void InspectorModel::clearSelection()
{
    const bool changed = !m_selectionId.isEmpty() || !m_properties.isEmpty() ||
                         m_selectionKind != SelectionKind::EmptyCanvas || !m_title.isEmpty();
    if (!changed)
    {
        return;
    }

    beginResetModel();
    m_properties.clear();
    m_selectionId.clear();
    m_title.clear();
    m_selectionKind = SelectionKind::EmptyCanvas;
    endResetModel();
    Q_EMIT selectionChanged();
}

bool InspectorModel::isCurrent(const QString& documentKey, const QString& documentRevision) const
{
    return documentKey == m_documentKey && documentRevision == m_documentRevision;
}

QString InspectorModel::selectionKindName(SelectionKind kind)
{
    switch (kind)
    {
        case SelectionKind::EmptyCanvas:
            return QStringLiteral("empty-canvas");
        case SelectionKind::Page:
            return QStringLiteral("page");
        case SelectionKind::Image:
            return QStringLiteral("image");
        case SelectionKind::Finding:
            return QStringLiteral("finding");
        case SelectionKind::Separation:
            return QStringLiteral("separation");
    }
    Q_UNREACHABLE_RETURN(QStringLiteral("empty-canvas"));
}

}   // namespace pdfinteraction
