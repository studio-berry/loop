#ifndef PREVIEWSTATEMODEL_H
#define PREVIEWSTATEMODEL_H

#include <QObject>

namespace loupe::interaction
{

class PreviewStateModel final : public QObject
{
    Q_OBJECT

public:
    enum class Authority
    {
        None,
        Approximate,
        Simulated,
        Authoritative
    };
    Q_ENUM(Authority)

    enum class Status
    {
        Unavailable,
        Ready,
        Stale,
        Incomplete
    };
    Q_ENUM(Status)

    Q_PROPERTY(Status status READ status NOTIFY stateChanged)
    Q_PROPERTY(Authority authority READ authority NOTIFY stateChanged)
    Q_PROPERTY(QString documentKey READ documentKey NOTIFY revisionChanged)
    Q_PROPERTY(QString documentRevision READ documentRevision NOTIFY revisionChanged)
    Q_PROPERTY(QString profileIdentity READ profileIdentity NOTIFY stateChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)

    explicit PreviewStateModel(QObject* parent = nullptr);

    void setCurrentRevision(QString documentKey, QString documentRevision);
    bool setState(QString documentKey,
                  QString documentRevision,
                  Authority authority,
                  QString summary,
                  QString detail = {},
                  QString profileIdentity = {});
    bool setIncomplete(QString documentKey,
                       QString documentRevision,
                       QString summary,
                       QString detail = {},
                       QString profileIdentity = {});
    void clear();

    bool isCurrent(const QString& documentKey, const QString& documentRevision) const;
    Status status() const { return m_status; }
    Authority authority() const { return m_authority; }
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString profileIdentity() const { return m_profileIdentity; }
    QString summary() const { return m_summary; }
    QString detail() const { return m_detail; }

    static QString authorityName(Authority authority);
    static QString statusName(Status status);

signals:
    void revisionChanged();
    void stateChanged();

private:
    void setStatus(Status status);

    QString m_documentKey;
    QString m_documentRevision;
    QString m_profileIdentity;
    QString m_summary;
    QString m_detail;
    Authority m_authority = Authority::None;
    Status m_status = Status::Unavailable;
};

} // namespace loupe::interaction

Q_DECLARE_METATYPE(loupe::interaction::PreviewStateModel::Authority)
Q_DECLARE_METATYPE(loupe::interaction::PreviewStateModel::Status)

#endif // PREVIEWSTATEMODEL_H
