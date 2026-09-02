#include "productionmodel.h"

#include <QSet>
#include <QVariant>

#include <utility>

namespace pdfinteraction
{

ProductionModel::ProductionModel(QObject* parent) :
    QAbstractListModel(parent)
{
    qRegisterMetaType<State>();
}

int ProductionModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_steps.size();
}

QVariant ProductionModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_steps.size())
    {
        return {};
    }

    const ProductionStepView& step = m_steps.at(index.row());
    switch (role)
    {
        case Qt::DisplayRole:
        case DisplayNameRole:
            return step.displayName;
        case StepIdRole:
            return step.id;
        case KindRole:
            return step.kind;
        case TypeRole:
            return step.type;
        case SpotColorNameRole:
            return step.spotColorName;
        case ShouldPrintRole:
            return step.shouldPrint;
        case OverprintRole:
            return step.overprint;
        case PageIndicesRole:
        {
            QVariantList pages;
            for (const int page : step.pageIndices)
            {
                pages.push_back(page);
            }
            return pages;
        }
        default:
            return {};
    }
}

QHash<int, QByteArray> ProductionModel::roleNames() const
{
    return {
        { StepIdRole, "stepId" },
        { KindRole, "kind" },
        { TypeRole, "type" },
        { DisplayNameRole, "displayName" },
        { SpotColorNameRole, "spotColorName" },
        { ShouldPrintRole, "shouldPrint" },
        { OverprintRole, "overprint" },
        { PageIndicesRole, "pageIndices" }
    };
}

bool ProductionModel::replace(QString documentKey,
                              QString documentRevision,
                              const QVector<pdf::PDFProcessingStep>& processingSteps)
{
    if ((documentKey.isEmpty() || documentRevision.isEmpty()) && !processingSteps.isEmpty())
    {
        return false;
    }

    QVector<ProductionStepView> next;
    next.reserve(processingSteps.size());
    QSet<QString> ids;
    for (const pdf::PDFProcessingStep& step : processingSteps)
    {
        if (step.id.isEmpty() || ids.contains(step.id))
        {
            return false;
        }
        ids.insert(step.id);
        next.push_back({ step.id,
                         pdf::pdfProcessingStepKindToString(step.kind),
                         pdf::pdfProcessingStepTypeToString(step.type),
                         step.displayName,
                         step.spotColorName,
                         step.shouldPrint,
                         step.overprint,
                         step.pageIndices });
    }

    if (documentKey != m_documentKey || documentRevision != m_documentRevision)
    {
        setCurrentRevision(std::move(documentKey), std::move(documentRevision));
    }

    beginResetModel();
    m_steps = std::move(next);
    endResetModel();
    return true;
}

bool ProductionModel::setState(QString documentKey,
                               QString documentRevision,
                               State state,
                               QString detail)
{
    if (!isCurrent(documentKey, documentRevision) || documentRevision.isEmpty())
    {
        return false;
    }

    const bool changed = m_state != state || m_stateDetail != detail;
    m_state = state;
    m_stateDetail = std::move(detail);
    if (changed)
    {
        Q_EMIT stateChanged(m_state);
    }
    return true;
}

void ProductionModel::setCurrentRevision(QString documentKey, QString documentRevision)
{
    if (documentKey == m_documentKey && documentRevision == m_documentRevision)
    {
        return;
    }

    beginResetModel();
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_steps.clear();
    endResetModel();

    const bool revisionStateChanged = m_state != State::NotReady || !m_stateDetail.isEmpty();
    m_stateDetail.clear();
    if (revisionStateChanged)
    {
        m_state = State::NotReady;
        Q_EMIT stateChanged(m_state);
    }
    Q_EMIT revisionChanged();
}

void ProductionModel::clear()
{
    if (m_steps.isEmpty() && m_documentKey.isEmpty() && m_documentRevision.isEmpty() &&
        m_state == State::NotReady && m_stateDetail.isEmpty())
    {
        return;
    }

    beginResetModel();
    m_steps.clear();
    m_documentKey.clear();
    m_documentRevision.clear();
    endResetModel();
    m_stateDetail.clear();
    m_state = State::NotReady;
    Q_EMIT stateChanged(m_state);
    Q_EMIT revisionChanged();
}

bool ProductionModel::isCurrent(const QString& documentKey, const QString& documentRevision) const
{
    return documentKey == m_documentKey && documentRevision == m_documentRevision;
}

const ProductionStepView* ProductionModel::step(const QString& stepId) const
{
    for (const ProductionStepView& item : m_steps)
    {
        if (item.id == stepId)
        {
            return &item;
        }
    }
    return nullptr;
}

QString ProductionModel::stateName(State state)
{
    switch (state)
    {
        case State::NotReady:
            return QStringLiteral("NOT_READY");
        case State::Ready:
            return QStringLiteral("READY");
        case State::OperationPending:
            return QStringLiteral("OPERATION_PENDING");
        case State::ApprovalRequired:
            return QStringLiteral("APPROVAL_REQUIRED");
        case State::OutputWritten:
            return QStringLiteral("OUTPUT_WRITTEN");
    }
    return QStringLiteral("NOT_READY");
}

}   // namespace pdfinteraction
