#ifndef PREFLIGHTPROFILECATALOG_H
#define PREFLIGHTPROFILECATALOG_H

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

class QFileSystemWatcher;

namespace pdfinteraction
{

struct PreflightProfileChoice
{
    QString id;
    QString name;
    QString version;
    QString source;
    QString diagnostic;
    QString digest;
    QJsonObject profile;
    QJsonObject variables;
    bool valid = false;
};

/// Bundled/local profile scan, digest/check-set staleness, and QFileSystemWatcher
/// for operator profile selection. EditorHost delegates catalog maintenance here.
class PreflightProfileCatalog final : public QObject
{
    Q_OBJECT

public:
    explicit PreflightProfileCatalog(QObject* parent = nullptr);
    ~PreflightProfileCatalog() override;

    PreflightProfileCatalog(const PreflightProfileCatalog&) = delete;
    PreflightProfileCatalog& operator=(const PreflightProfileCatalog&) = delete;

    void reload();
    const QList<PreflightProfileChoice>& profiles() const noexcept { return m_profiles; }
    QString selectedProfileId() const { return m_selectedProfileId; }
    const QJsonObject& bindings() const noexcept { return m_bindings; }

    const PreflightProfileChoice* selectedProfile() const;
    const PreflightProfileChoice* profileById(const QString& id) const;

    bool selectProfile(const QString& id);
    bool setVariable(const QString& name, const QVariant& value);

signals:
    void changed();
    void profileStaleRequested();
    void checkSetStaleRequested();

private:
    void updateWatch();

    QList<PreflightProfileChoice> m_profiles;
    QString m_selectedProfileId;
    QJsonObject m_bindings;
    QFileSystemWatcher* m_watcher = nullptr;
};

}   // namespace pdfinteraction

#endif   // PREFLIGHTPROFILECATALOG_H
