#ifndef PRODUCTIONMODEL_H
#define PRODUCTIONMODEL_H

#include "pdfproductiongeometry.h"

#include <QAbstractListModel>
#include <QVector>

namespace pdfinteraction
{

struct ProductionStepView
{
    QString id;
    QString kind;
    QString type;
    QString displayName;
    QString spotColorName;
    bool shouldPrint = true;
    bool overprint = false;
    QVector<int> pageIndices;
};

class ProductionModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        StepIdRole = Qt::UserRole + 1,
        KindRole,
        TypeRole,
        DisplayNameRole,
        SpotColorNameRole,
        ShouldPrintRole,
        OverprintRole,
        PageIndicesRole
    };
    Q_ENUM(Role)

    enum class State
    {
        NotReady,
        Ready,
        OperationPending,
        ApprovalRequired,
        OutputWritten
    };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateDetail READ stateDetail NOTIFY stateChanged)
    Q_PROPERTY(QString documentKey READ documentKey NOTIFY revisionChanged)
    Q_PROPERTY(QString documentRevision READ documentRevision NOTIFY revisionChanged)

    explicit ProductionModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool replace(QString documentKey,
                 QString documentRevision,
                 const QVector<pdf::PDFProcessingStep>& processingSteps);
    bool setState(QString documentKey, QString documentRevision, State state, QString detail = {});
    void setCurrentRevision(QString documentKey, QString documentRevision);
    void clear();

    bool isCurrent(const QString& documentKey, const QString& documentRevision) const;
    const ProductionStepView* step(const QString& stepId) const;
    State state() const { return m_state; }
    QString stateDetail() const { return m_stateDetail; }
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }

    static QString stateName(State state);

signals:
    void revisionChanged();
    void stateChanged(pdfinteraction::ProductionModel::State state);

private:
    QVector<ProductionStepView> m_steps;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_stateDetail;
    State m_state = State::NotReady;
};

} // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::ProductionModel::State)

#endif // PRODUCTIONMODEL_H
