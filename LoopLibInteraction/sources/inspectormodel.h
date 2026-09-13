#ifndef INSPECTORMODEL_H
#define INSPECTORMODEL_H

#include "preflightfindingsmodel.h"

#include <QAbstractListModel>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <functional>

namespace pdfinteraction
{

struct InspectorProperty
{
    QString id;
    QString label;
    QString value;
    QString section;
};

/// A presentation-layer request. The Inspector never applies an operation;
/// its consumer owns the existing PDFRepairTransaction flow.
struct InspectorCorrectiveOperationIntent
{
    QString documentKey;
    QString documentRevision;
    QString findingId;
    QString operationId;
    QJsonObject parameters;
};

using EvidenceRenderer = std::function<QVector<InspectorProperty>(const QJsonObject&)>;

class InspectorModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        PropertyIdRole = Qt::UserRole + 1,
        LabelRole,
        ValueRole,
        SectionRole
    };
    Q_ENUM(Role)

    enum class SelectionKind
    {
        EmptyCanvas,
        Page,
        Image,
        Finding,
        Separation
    };
    Q_ENUM(SelectionKind)

    Q_PROPERTY(QString documentKey READ documentKey NOTIFY revisionChanged)
    Q_PROPERTY(QString documentRevision READ documentRevision NOTIFY revisionChanged)
    Q_PROPERTY(QString selectionId READ selectionId NOTIFY selectionChanged)
    Q_PROPERTY(QString title READ title NOTIFY selectionChanged)
    Q_PROPERTY(SelectionKind selectionKind READ selectionKind NOTIFY selectionChanged)
    Q_PROPERTY(QStringList correctiveOperationIds READ correctiveOperationIds NOTIFY selectionChanged)
    Q_PROPERTY(bool hasCorrectiveOperation READ hasCorrectiveOperation NOTIFY selectionChanged)

    struct Selection
    {
        QString documentKey;
        QString documentRevision;
        QString selectionId;
        QString title;
        SelectionKind kind = SelectionKind::EmptyCanvas;
        QVector<InspectorProperty> properties;
    };

    explicit InspectorModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setCurrentRevision(QString documentKey, QString documentRevision);
    bool setSelection(const Selection& selection);
    bool setFindingSelection(const PreflightFindingsModel& findings,
                             const QString& findingId,
                             const QString& documentRevision);
    /// Registers an optional per-check evidence projection. Unregistered
    /// checks are rendered by the neutral flat key/value fallback.
    static bool registerEvidenceRenderer(QString checkId, EvidenceRenderer renderer);

    /// Emits an intent for a report-advertised, implemented operation. It is
    /// deliberately not a mutation API.
    Q_INVOKABLE bool requestCorrectiveOperation(const QString& operationId);
    void clearSelection();

    bool isCurrent(const QString& documentKey, const QString& documentRevision) const;
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString selectionId() const { return m_selectionId; }
    QString title() const { return m_title; }
    SelectionKind selectionKind() const { return m_selectionKind; }
    QStringList correctiveOperationIds() const { return m_correctiveOperationIds; }
    bool hasCorrectiveOperation() const noexcept { return !m_correctiveOperationIds.isEmpty(); }

    static QString selectionKindName(SelectionKind kind);

signals:
    void revisionChanged();
    void selectionChanged();
    void correctiveOperationRequested(pdfinteraction::InspectorCorrectiveOperationIntent intent);

private:
    QVector<InspectorProperty> m_properties;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_selectionId;
    QString m_title;
    SelectionKind m_selectionKind = SelectionKind::EmptyCanvas;
    QHash<QString, QJsonObject> m_correctiveParameters;
    QStringList m_correctiveOperationIds;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::InspectorModel::SelectionKind)
Q_DECLARE_METATYPE(pdfinteraction::InspectorCorrectiveOperationIntent)

#endif   // INSPECTORMODEL_H
