// MIT License
// Copyright (c) 2026 Loop contributors

#ifndef OPERATORHELP_H
#define OPERATORHELP_H

#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QVariantMap>

class OperatorHelp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap catalog READ catalog CONSTANT)
    Q_PROPERTY(QVariantMap guide READ guide CONSTANT)
    Q_PROPERTY(QVariantList sampleJobs READ sampleJobs CONSTANT)
    Q_PROPERTY(bool welcomeNeeded READ welcomeNeeded NOTIFY welcomeChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic CONSTANT)

public:
    OperatorHelp(QSettings& settings, QString sampleDirectory, QObject* parent = nullptr);

    QVariantMap catalog() const { return m_catalog.toVariantMap(); }
    QVariantMap guide() const { return m_guide.toVariantMap(); }
    QVariantList sampleJobs() const;
    bool welcomeNeeded() const;
    QString diagnostic() const { return m_diagnostic; }
    Q_INVOKABLE QString workspaceHelp(int workspace) const;
    Q_INVOKABLE QVariantMap findingHelp(const QString& checkId, const QString& findingType) const;
    Q_INVOKABLE QVariantMap prepareSample(const QString& id) const;
    Q_INVOKABLE bool acknowledgeWelcome();

signals:
    void welcomeChanged();

private:
    QJsonObject readResource(const QString& path);
    QSettings& m_settings;
    QString m_sampleDirectory;
    QJsonObject m_catalog;
    QJsonObject m_guide;
    QJsonObject m_samples;
    QString m_diagnostic;
};

#endif
