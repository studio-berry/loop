// MIT License

#include "pdfpreflightcertificate.h"

#include "pdfpreflightverdict.h"
#include "pdfutils.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QUuid>

namespace pdf
{

namespace
{

QString dateTimeString(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromString(const QString& value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool validDigest(const QString& value)
{
    return isPDFSha256(value);
}

QString reportDigest(const QJsonObject& report)
{
    return digest(canonicalJson(report));
}

}   // namespace

QString preflightCertificateStateToString(PreflightCertificateState state)
{
    switch (state)
    {
        case PreflightCertificateState::Valid:
            return QStringLiteral("valid");
        case PreflightCertificateState::InvalidDocumentChanged:
            return QStringLiteral("invalid-document-changed");
        case PreflightCertificateState::InvalidAuditChainBroken:
            return QStringLiteral("invalid-audit-chain-broken");
        case PreflightCertificateState::InvalidDecisionStale:
            return QStringLiteral("invalid-decision-stale");
        case PreflightCertificateState::InvalidProfileProvisional:
            return QStringLiteral("invalid-profile-provisional");
        case PreflightCertificateState::InvalidInspection:
            return QStringLiteral("invalid-inspection");
        case PreflightCertificateState::InvalidCertificate:
            return QStringLiteral("invalid-certificate");
    }
    return QStringLiteral("invalid-certificate");
}

QJsonObject PreflightCertificate::toJson() const
{
    return QJsonObject{
        { QStringLiteral("certificate_id"), certificateId },
        { QStringLiteral("issued_at_utc"), dateTimeString(issuedAtUtc) },
        { QStringLiteral("issued_by"), issuedBy },
        { QStringLiteral("document_revision_digest"), documentRevisionDigest },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("report_digest"), reportDigest },
        { QStringLiteral("audit_chain_head_event_id"), auditChainHeadEventId },
        { QStringLiteral("error_count"), errorCount },
        { QStringLiteral("waived_error_count"), waivedErrorCount },
        { QStringLiteral("covering_decision_ids"), QJsonArray::fromStringList(coveringDecisionIds) },
        { QStringLiteral("type"), QStringLiteral("loop-certified-preflight") },
        { QStringLiteral("trust_model"), QStringLiteral("tamper-evident attribution, not a digital signature") }
    };
}

bool PreflightCertificate::fromJson(const QJsonObject& object,
                                    PreflightCertificate& certificate,
                                    QString& errorMessage)
{
    certificate = {};
    certificate.certificateId = object.value(QStringLiteral("certificate_id")).toString().trimmed();
    certificate.issuedAtUtc = dateTimeFromString(object.value(QStringLiteral("issued_at_utc")).toString());
    certificate.issuedBy = object.value(QStringLiteral("issued_by")).toString().trimmed();
    certificate.documentRevisionDigest = object.value(QStringLiteral("document_revision_digest")).toString().trimmed().toLower();
    certificate.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString().trimmed().toLower();
    certificate.reportDigest = object.value(QStringLiteral("report_digest")).toString().trimmed().toLower();
    certificate.auditChainHeadEventId = object.value(QStringLiteral("audit_chain_head_event_id")).toString().trimmed();
    certificate.errorCount = object.value(QStringLiteral("error_count")).toInt(-1);
    certificate.waivedErrorCount = object.value(QStringLiteral("waived_error_count")).toInt(-1);
    for (const QJsonValue& value : object.value(QStringLiteral("covering_decision_ids")).toArray())
        certificate.coveringDecisionIds.append(value.toString());

    if (certificate.certificateId.isEmpty() || !certificate.issuedAtUtc.isValid() || certificate.issuedBy.isEmpty() ||
        !validDigest(certificate.documentRevisionDigest) || !validDigest(certificate.effectiveProfileDigest) ||
        !validDigest(certificate.reportDigest) || certificate.auditChainHeadEventId.isEmpty() ||
        certificate.errorCount < 0 || certificate.waivedErrorCount < 0)
    {
        errorMessage = QStringLiteral("Certificate fields are missing or invalid.");
        return false;
    }
    errorMessage.clear();
    return true;
}

QJsonObject PreflightCertificateVerification::toJson() const
{
    return QJsonObject{
        { QStringLiteral("state"), preflightCertificateStateToString(state) },
        { QStringLiteral("valid"), isValid() },
        { QStringLiteral("reason"), reason }
    };
}

QString preflightDecisionIdentity(const PreflightDecision& decision)
{
    return digest(canonicalJson(decision.toJson(decision.documentRevisionDigest,
                                                decision.effectiveProfileDigest)));
}

bool issuePreflightCertificate(const PreflightResult& result,
                               const QJsonObject& report,
                               const QByteArray& documentBytes,
                               const QList<PDFOperationHistoryEvent>& history,
                               const QString& issuedBy,
                               PreflightCertificate& certificate,
                               QString& errorMessage)
{
    certificate = {};
    const QString documentDigest = digest(documentBytes);
    if (result.documentRevisionDigest.compare(documentDigest, Qt::CaseInsensitive) != 0)
    {
        errorMessage = QStringLiteral("The document changed before certification.");
        return false;
    }
    if (!preflightAllowsCertification(result))
    {
        errorMessage = QStringLiteral("The preflight result does not meet certification requirements.");
        return false;
    }
    if (issuedBy.trimmed().isEmpty() || !validDigest(result.effectiveProfileDigest))
    {
        errorMessage = QStringLiteral("Certification requires an operator and an effective profile digest.");
        return false;
    }

    const QString expectedReportDigest = reportDigest(report);
    const PDFOperationHistoryEvent* head = nullptr;
    for (const PDFOperationHistoryEvent& event : history)
    {
        if (event.kind == PDFOperationHistoryEventKind::PreflightRun &&
            event.status == PDFOperationHistoryStatus::Accepted &&
            event.documentRevisionDigest.compare(documentDigest, Qt::CaseInsensitive) == 0 &&
            event.effectiveProfileDigest.compare(result.effectiveProfileDigest, Qt::CaseInsensitive) == 0)
        {
            head = &event;
        }
    }
    if (!head)
    {
        errorMessage = QStringLiteral("Certification requires an accepted preflight event in the audit chain.");
        return false;
    }

    const PreflightVerdict verdict = reducePreflightVerdict(result);
    certificate.certificateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    certificate.issuedAtUtc = QDateTime::currentDateTimeUtc();
    certificate.issuedBy = issuedBy.trimmed();
    certificate.documentRevisionDigest = documentDigest;
    certificate.effectiveProfileDigest = result.effectiveProfileDigest.toLower();
    certificate.reportDigest = expectedReportDigest;
    certificate.auditChainHeadEventId = head->entryId.toString(QUuid::WithoutBraces);
    certificate.errorCount = result.errors.size();
    certificate.waivedErrorCount = verdict.waivedFindingIds.size();
    for (const PreflightDecision& decision : result.decisions)
    {
        if (decision.countsForSignoff(documentDigest, result.effectiveProfileDigest))
            certificate.coveringDecisionIds.append(preflightDecisionIdentity(decision));
    }
    certificate.coveringDecisionIds.removeDuplicates();
    errorMessage.clear();
    return true;
}

PreflightCertificateVerification verifyPreflightCertificate(const PreflightCertificate& certificate,
                                                            const QByteArray& documentBytes,
                                                            const QList<PDFOperationHistoryEvent>& history)
{
    PreflightCertificateVerification result;
    if (certificate.certificateId.isEmpty() || !certificate.issuedAtUtc.isValid() ||
        !validDigest(certificate.documentRevisionDigest) || !validDigest(certificate.effectiveProfileDigest) ||
        !validDigest(certificate.reportDigest) || certificate.auditChainHeadEventId.isEmpty())
    {
        result.state = PreflightCertificateState::InvalidCertificate;
        result.reason = QStringLiteral("Certificate fields are missing or invalid.");
        return result;
    }
    if (digest(documentBytes).compare(certificate.documentRevisionDigest, Qt::CaseInsensitive) != 0)
    {
        result.state = PreflightCertificateState::InvalidDocumentChanged;
        result.reason = QStringLiteral("The document bytes do not match the certified revision.");
        return result;
    }

    QByteArray previous;
    qint64 expectedSequence = history.isEmpty() ? 0 : 1;
    bool headFound = false;
    bool issuanceFound = false;
    for (const PDFOperationHistoryEvent& event : history)
    {
        if (event.sequence != expectedSequence || event.previousEventHash != previous ||
            event.eventHash != computeOperationHistoryEventHash(event, previous))
        {
            result.state = PreflightCertificateState::InvalidAuditChainBroken;
            result.reason = QStringLiteral("The operation history hash chain is broken.");
            return result;
        }
        headFound = headFound || event.entryId.toString(QUuid::WithoutBraces) == certificate.auditChainHeadEventId;
        if (event.kind == PDFOperationHistoryEventKind::CertificateIssued &&
            event.approval.decisionReference == certificate.certificateId &&
            event.resultSummary.value(QStringLiteral("report_digest")).toString().compare(certificate.reportDigest, Qt::CaseInsensitive) == 0)
        {
            issuanceFound = true;
        }
        previous = event.eventHash;
        ++expectedSequence;
    }
    if (!headFound)
    {
        result.state = PreflightCertificateState::InvalidAuditChainBroken;
        result.reason = QStringLiteral("The certificate audit-chain head is not present.");
        return result;
    }
    if (!issuanceFound)
    {
        result.state = PreflightCertificateState::InvalidCertificate;
        result.reason = QStringLiteral("The certificate issuance event is missing or does not match the certificate.");
        return result;
    }

    for (const QString& decisionId : certificate.coveringDecisionIds)
    {
        bool recorded = false;
        bool invalidated = false;
        for (const PDFOperationHistoryEvent& event : history)
        {
            if (event.kind == PDFOperationHistoryEventKind::DecisionRecorded &&
                event.resultSummary.value(QStringLiteral("decision_id")).toString() == decisionId)
                recorded = true;
            if (event.kind == PDFOperationHistoryEventKind::DecisionInvalidated &&
                event.approval.decisionReference == decisionId)
                invalidated = true;
        }
        if (!recorded || invalidated)
        {
            result.state = PreflightCertificateState::InvalidDecisionStale;
            result.reason = QStringLiteral("A decision covering this certificate is stale or missing.");
            return result;
        }
    }
    result.state = PreflightCertificateState::Valid;
    result.reason = QStringLiteral("The certificate matches the document and verified audit chain.");
    return result;
}

}   // namespace pdf
