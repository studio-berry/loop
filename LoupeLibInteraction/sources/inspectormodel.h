#ifndef INSPECTORMODEL_H
#define INSPECTORMODEL_H

#include "preflightfindingsmodel.h"

#include <QAbstractListModel>
#include <QVector>

namespace loupe::interaction
{

struct InspectorProperty
{
    QString id;
    QString label;
    QString value;
};

class InspectorModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        PropertyIdRole = Qt::UserRole + 1,
        LabelRole,
        ValueRole
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
    void clearSelection();

    bool isCurrent(const QString& documentKey, const QString& documentRevision) const;
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString selectionId() const { return m_selectionId; }
    QString title() const { return m_title; }
    SelectionKind selectionKind() const { return m_selectionKind; }

    static QString selectionKindName(SelectionKind kind);

signals:
    void revisionChanged();
    void selectionChanged();

private:
    QVector<InspectorProperty> m_properties;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_selectionId;
    QString m_title;
    SelectionKind m_selectionKind = SelectionKind::EmptyCanvas;
};

} // namespace loupe::interaction

Q_DECLARE_METATYPE(loupe::interaction::InspectorModel::SelectionKind)

#endif // INSPECTORMODEL_H
