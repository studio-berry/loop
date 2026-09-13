#include "inspectormodel.h"

#include "pdffixupregistry.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>

#include <QHash>
#include <algorithm>

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

QHash<QString, EvidenceRenderer>& evidenceRenderers()
{
    static QHash<QString, EvidenceRenderer> renderers;
    return renderers;
}

QString jsonScalarText(const QJsonValue& value)
{
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isBool())
    {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble())
    {
        return QString::number(value.toDouble(), 'g', 15);
    }
    if (value.isNull())
    {
        return QStringLiteral("null");
    }
    return {};
}

void appendEvidenceProperties(QVector<InspectorProperty>& properties,
                              const QJsonValue& value,
                              const QString& path,
                              const QString& section)
{
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& key : keys)
        {
            const QString childPath = path.isEmpty() ? key : path + QLatin1Char('.') + key;
            appendEvidenceProperties(properties, object.value(key), childPath, section);
        }
        return;
    }

    if (value.isArray())
    {
        const QJsonArray array = value.toArray();
        if (array.isEmpty())
        {
            properties.push_back({ path, path, QStringLiteral("[]"), section });
            return;
        }
        for (qsizetype index = 0; index < array.size(); ++index)
        {
            appendEvidenceProperties(properties, array.at(index),
                                     QStringLiteral("%1[%2]").arg(path).arg(index), section);
        }
        return;
    }

    const QString scalar = jsonScalarText(value);
    if (!scalar.isEmpty())
    {
        properties.push_back({ path, path, scalar, section });
    }
}

void addProperty(QVector<InspectorProperty>& properties,
                 const QString& id,
                 const QString& label,
                 const QString& value,
                 const QString& section)
{
    if (!value.isEmpty())
    {
        properties.push_back({ id, label, value, section });
    }
}

bool isFixupApplicable(const PreflightFindingView& finding, const pdf::PreflightFixupConfig& fixup)
{
    // The report owns availability; this identity bridge only relates a known
    // corrective operation to the check it can address. Unknown pairs stay
    // unavailable instead of offering a repair for an unrelated finding.
    return fixup.id == finding.checkId ||
           (finding.checkId == QStringLiteral("bleed") && fixup.id == QStringLiteral("add-bleed")) ||
           (finding.checkId == QStringLiteral("image-resolution") && fixup.id == QStringLiteral("downsample-images"));
}

QJsonObject fixupParameters(const pdf::PreflightFixupConfig& fixup)
{
    QJsonObject parameters = fixup.params;
    if (fixup.amountPt > 0.0 && !parameters.contains(QStringLiteral("amount_pt")))
    {
        parameters.insert(QStringLiteral("amount_pt"), fixup.amountPt);
    }
    return parameters;
}

}   // namespace

InspectorModel::InspectorModel(QObject* parent) :
    QAbstractListModel(parent)
{
    qRegisterMetaType<SelectionKind>();
    qRegisterMetaType<InspectorCorrectiveOperationIntent>();
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
        case SectionRole:
            return property.section;
        default:
            return {};
    }
}

QHash<int, QByteArray> InspectorModel::roleNames() const
{
    return {
        { PropertyIdRole, "propertyId" },
        { LabelRole, "label" },
        { ValueRole, "value" },
        { SectionRole, "section" }
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
    if (selection.kind != SelectionKind::Finding)
    {
        m_correctiveParameters.clear();
        m_correctiveOperationIds.clear();
    }
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

    addProperty(selection.properties, QStringLiteral("severity"), QStringLiteral("Severity"),
                finding->severity, QStringLiteral("identity"));
    addProperty(selection.properties, QStringLiteral("check"), QStringLiteral("Check"),
                finding->checkId, QStringLiteral("identity"));
    addProperty(selection.properties, QStringLiteral("type"), QStringLiteral("Type"),
                finding->type, QStringLiteral("identity"));
    addProperty(selection.properties, QStringLiteral("scope"), QStringLiteral("Scope"),
                finding->scope, QStringLiteral("identity"));
    if (finding->page > 0)
    {
        addProperty(selection.properties, QStringLiteral("page"), QStringLiteral("Page"),
                    QString::number(finding->page), QStringLiteral("identity"));
    }
    if (!finding->objectId.isNull() && !finding->objectId.isEmpty())
    {
        addProperty(selection.properties, QStringLiteral("object"), QStringLiteral("Object"),
                    finding->objectId, QStringLiteral("identity"));
    }
    if (finding->bbox.isValid() && !finding->bbox.isEmpty())
    {
        addProperty(selection.properties, QStringLiteral("bounds"), QStringLiteral("Bounds"),
                    rectText(finding->bbox), QStringLiteral("geometry"));
    }
    if (!finding->evidenceIds.isEmpty())
    {
        addProperty(selection.properties, QStringLiteral("evidence-ids"), QStringLiteral("Evidence IDs"),
                    finding->evidenceIds.join(QStringLiteral(", ")), QStringLiteral("evidence"));
    }

    QJsonObject displayEvidence = finding->evidence;
    const QJsonValue objectContext = displayEvidence.take(QStringLiteral("object_context"));
    const auto renderer = evidenceRenderers().value(finding->checkId);
    if (renderer && !displayEvidence.isEmpty())
    {
        const QVector<InspectorProperty> rendered = renderer(displayEvidence);
        for (InspectorProperty property : rendered)
        {
            if (property.section.isEmpty())
            {
                property.section = QStringLiteral("evidence");
            }
            selection.properties.push_back(std::move(property));
        }
    }
    else if (!displayEvidence.isEmpty())
    {
        appendEvidenceProperties(selection.properties, displayEvidence,
                                 QStringLiteral("evidence"), QStringLiteral("evidence"));
    }

    if (!objectContext.isUndefined())
    {
        appendEvidenceProperties(selection.properties,
                                 objectContext,
                                 QStringLiteral("object-context"), QStringLiteral("object-context"));
    }

    if (const pdf::PreflightCheckStatus* status = findings.checkStatus(finding->checkId))
    {
        addProperty(selection.properties, QStringLiteral("check-status"), QStringLiteral("Status"),
                    status->status, QStringLiteral("check-status"));
        addProperty(selection.properties, QStringLiteral("check-reason"), QStringLiteral("Reason"),
                    status->reason, QStringLiteral("check-status"));
        addProperty(selection.properties, QStringLiteral("budget-kind"), QStringLiteral("Budget kind"),
                    status->budgetKind, QStringLiteral("check-status"));
        addProperty(selection.properties, QStringLiteral("budget-pool"), QStringLiteral("Budget pool"),
                    status->budgetPool, QStringLiteral("check-status"));
        if (!status->budgetKind.isEmpty() || !status->budgetPool.isEmpty())
        {
            addProperty(selection.properties, QStringLiteral("budget-limit"), QStringLiteral("Budget limit"),
                        QString::number(status->budgetLimit), QStringLiteral("check-status"));
            addProperty(selection.properties, QStringLiteral("budget-attempted"), QStringLiteral("Budget attempted"),
                        QString::number(status->budgetAttempted), QStringLiteral("check-status"));
        }
        addProperty(selection.properties, QStringLiteral("budget-context"), QStringLiteral("Budget context"),
                    status->budgetContext, QStringLiteral("check-status"));
        if (status->budgetKind == QStringLiteral("decompression-ratio"))
        {
            addProperty(selection.properties, QStringLiteral("budget-observed-bytes"), QStringLiteral("Observed bytes"),
                        QString::number(status->budgetObservedBytes), QStringLiteral("check-status"));
            addProperty(selection.properties, QStringLiteral("budget-compressed-bytes"), QStringLiteral("Compressed bytes"),
                        QString::number(status->budgetCompressedBytes), QStringLiteral("check-status"));
        }
    }

    bool hasCorrectiveOperation = false;
    bool hasApplicableFixup = false;
    QHash<QString, QJsonObject> correctiveParameters;
    for (const pdf::PreflightFixupConfig& fixup : findings.fixupsAvailable())
    {
        if (!isFixupApplicable(*finding, fixup))
        {
            continue;
        }
        hasApplicableFixup = true;
        const bool implemented = pdf::isImplementedFixupId(fixup.id);
        if (implemented)
        {
            hasCorrectiveOperation = true;
            addProperty(selection.properties, QStringLiteral("corrective.%1").arg(fixup.id),
                        QStringLiteral("Available correction"),
                        fixup.description.isEmpty() ? fixup.id : fixup.description,
                        QStringLiteral("corrective-availability"));
            correctiveParameters.insert(fixup.id, fixupParameters(fixup));
        }
        else
        {
            addProperty(selection.properties, QStringLiteral("corrective-unavailable.%1").arg(fixup.id),
                        QStringLiteral("Automated fix"), QStringLiteral("No automated fix available"),
                        QStringLiteral("corrective-availability"));
            addProperty(selection.properties, QStringLiteral("corrective-unavailable-reason.%1").arg(fixup.id),
                        QStringLiteral("Reason"),
                        QStringLiteral("The report-advertised operation is not implemented in this build."),
                        QStringLiteral("corrective-availability"));
        }
    }
    if (!hasCorrectiveOperation && !hasApplicableFixup)
    {
        addProperty(selection.properties, QStringLiteral("corrective-none"), QStringLiteral("Automated fix"),
                    QStringLiteral("No automated fix available"), QStringLiteral("corrective-availability"));
        addProperty(selection.properties, QStringLiteral("corrective-none-reason"), QStringLiteral("Reason"),
                    QStringLiteral("The preflight report advertises no corrective operation for this finding."),
                    QStringLiteral("corrective-availability"));
    }
    addProperty(selection.properties, QStringLiteral("message"), QStringLiteral("Finding"),
                finding->message, QStringLiteral("identity"));
    m_correctiveParameters = std::move(correctiveParameters);
    m_correctiveOperationIds = m_correctiveParameters.keys();
    std::sort(m_correctiveOperationIds.begin(), m_correctiveOperationIds.end());
    const bool accepted = setSelection(selection);
    if (!accepted)
    {
        m_correctiveParameters.clear();
        m_correctiveOperationIds.clear();
    }
    return accepted;
}

bool InspectorModel::registerEvidenceRenderer(QString checkId, EvidenceRenderer renderer)
{
    if (checkId.trimmed().isEmpty() || !renderer)
    {
        return false;
    }
    evidenceRenderers().insert(std::move(checkId), std::move(renderer));
    return true;
}

bool InspectorModel::requestCorrectiveOperation(const QString& operationId)
{
    if (m_selectionKind != SelectionKind::Finding || m_selectionId.isEmpty() ||
        !m_correctiveParameters.contains(operationId) || !pdf::isImplementedFixupId(operationId))
    {
        return false;
    }

    InspectorCorrectiveOperationIntent intent;
    intent.documentKey = m_documentKey;
    intent.documentRevision = m_documentRevision;
    intent.findingId = m_selectionId;
    intent.operationId = operationId;
    intent.parameters = m_correctiveParameters.value(operationId);
    Q_EMIT correctiveOperationRequested(intent);
    return true;
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
    m_correctiveParameters.clear();
    m_correctiveOperationIds.clear();
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
