#ifndef PREFLIGHTFINDINGSMODEL_H
#define PREFLIGHTFINDINGSMODEL_H

#include "preflightengine.h"

#include <QAbstractListModel>
#include <QHash>
#include <QRectF>
#include <QStringList>

namespace pdfinteraction
{

struct PreflightFindingView
{
    QString id;
    QString documentKey;
    QString documentRevision;
    QString scope;
    int page = 0;
    QString objectId;
    QString severity;
    QString type;
    QString message;
    QString checkId;
    QRectF bbox;
    QStringList evidenceIds;
    bool selected = false;
};

struct FindingOverlay
{
    QString findingId;
    QString documentRevision;
    int page = 0;
    QRectF bbox;
    QString severity;
    bool selected = false;
};

class PreflightFindingsModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        FindingIdRole = Qt::UserRole + 1,
        DocumentKeyRole,
        DocumentRevisionRole,
        ScopeRole,
        PageRole,
        ObjectIdRole,
        SeverityRole,
        TypeRole,
        MessageRole,
        CheckIdRole,
        BoundingBoxRole,
        EvidenceIdsRole,
        SelectedRole
    };
    Q_ENUM(Role)

    explicit PreflightFindingsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(QString documentKey,
                 QString documentRevision,
                 const QList<pdf::PreflightFinding>& errors,
                 const QList<pdf::PreflightFinding>& warnings);
    void clear();
    void setSelectedFinding(const QString& findingId);

    bool containsCurrent(const QString& findingId, const QString& documentRevision) const;
    const PreflightFindingView* finding(const QString& findingId) const;
    QVector<FindingOverlay> overlays(const QString& documentRevision, int page) const;
    QVector<PreflightFindingView> filtered(QString severity = {}, QString checkId = {}, int page = 0) const;
    QHash<QString, int> groupCounts(QString severity = {}) const;
    QString selectedFindingId() const { return m_selectedFindingId; }
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }

private:
    static PreflightFindingView makeView(const QString& documentKey,
                                         const QString& documentRevision,
                                         const pdf::PreflightFinding& finding);

    QVector<PreflightFindingView> m_findings;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_selectedFindingId;
};

} // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::FindingOverlay)

#endif // PREFLIGHTFINDINGSMODEL_H
