#ifndef PREFLIGHTCONTROLLER_H
#define PREFLIGHTCONTROLLER_H

#include "pdfjobscheduler.h"
#include "preflightengine.h"
#include "preflightfindingsmodel.h"

#include <QObject>

namespace pdfinteraction
{

class PreflightController final : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        NotChecked,
        Running,
        Cancelled,
        Pass,
        Findings,
        Stale,
        Incomplete
    };
    Q_ENUM(State)

    Q_PROPERTY(PreflightFindingsModel* findingsModel READ findingsModel CONSTANT)

    struct EvidenceNavigationRequest
    {
        QString findingId;
        QString documentKey;
        QString documentRevision;
        int page = 0;
        QRectF bbox;
        QStringList evidenceIds;
    };

    explicit PreflightController(pdf::PDFJobScheduler* scheduler = nullptr, QObject* parent = nullptr);

    PreflightFindingsModel* findingsModel() { return &m_findings; }
    const PreflightFindingsModel* findingsModel() const { return &m_findings; }
    State state() const { return m_state; }
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString profileDigest() const { return m_profileDigest; }
    QString jobId() const { return m_jobId; }

    void setCurrentRevision(QString documentKey, QString documentRevision);
    void beginRun(QString documentKey, QString documentRevision, QString profileDigest, QString jobId);
    bool updateProgress(const QString& jobId, const QString& documentRevision, int progress);
    bool acceptResult(const QString& jobId, const QString& documentRevision, const pdf::PreflightResult& result);
    bool cancelRun(const QString& jobId);
    bool navigationFor(const QString& findingId, EvidenceNavigationRequest* request) const;
    QVector<FindingOverlay> overlaysForPage(int page) const;

signals:
    void stateChanged(pdfinteraction::PreflightController::State state);
    void progressChanged(int progress);
    void navigationRequested(pdfinteraction::PreflightController::EvidenceNavigationRequest request);

private:
    void setState(State state);

    PreflightFindingsModel m_findings;
    State m_state = State::NotChecked;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_profileDigest;
    QString m_jobId;
    bool m_cancelRequested = false;
    pdf::PDFJobScheduler* m_scheduler = nullptr;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::PreflightController::State)
Q_DECLARE_METATYPE(pdfinteraction::PreflightController::EvidenceNavigationRequest)

#endif   // PREFLIGHTCONTROLLER_H
