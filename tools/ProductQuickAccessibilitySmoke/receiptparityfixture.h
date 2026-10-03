// MIT License
#ifndef RECEIPTPARITYFIXTURE_H
#define RECEIPTPARITYFIXTURE_H

#include "pdfpreflightverdict.h"

namespace receiptparity
{

struct Fixture
{
    pdf::PreflightResult result;
    pdf::PreflightProfileData profile;
    pdf::PDFEvidenceGraph evidence;

    explicit Fixture(pdf::PreflightVerdictState verdict,
                     const QString& inputDigest = QString(64, QLatin1Char('a')))
    {
        result.documentRevisionDigest = inputDigest;
        result.effectiveProfileDigest = QString(64, QLatin1Char('b'));
        result.coverageScope = QJsonObject{
            { QStringLiteral("claim"), QStringLiteral("Enabled checks on page 1 only.") },
            { QStringLiteral("pages"), QJsonArray{ 1 } },
            { QStringLiteral("enabled_checks"), QJsonArray{ QStringLiteral("bleed") } }
        };
        profile.effectiveDigest = result.effectiveProfileDigest;
        profile.coverageScope = result.coverageScope;
        pdf::PreflightCheckConfig check;
        check.id = QStringLiteral("bleed");
        check.enabled = true;
        check.required = true;
        profile.checks.append(check);
        pdf::PreflightCheckStatus status;
        status.id = check.id;
        status.status = QStringLiteral("ok");
        result.checkStatuses.append(status);
        pdf::PDFEvidenceRecord record;
        record.id = QStringLiteral("adapter-parity-evidence");
        record.fidelity = QStringLiteral("exact");
        record.artifact.sha256 = result.documentRevisionDigest;
        if (verdict == pdf::PreflightVerdictState::Incomplete)
        {
            record.incompleteReason = QStringLiteral("Raster evidence is unavailable.");
        }
        evidence.records.append(record);

        pdf::PreflightFinding finding;
        finding.checkId = check.id;
        finding.type = QStringLiteral("bleed-missing");
        finding.scope = QStringLiteral("page");
        finding.page = 1;
        finding.severity = QStringLiteral("warning");
        finding.message = QStringLiteral("Bleed requires operator inspection.");
        finding.bbox = QRectF(1, 2, 3, 4);
        finding.evidenceIds.append(record.id);
        if (verdict == pdf::PreflightVerdictState::Fail)
        {
            finding.severity = QStringLiteral("error");
            result.errors.append(finding);
        }
        else
        {
            result.warnings.append(finding);
        }
        if (verdict == pdf::PreflightVerdictState::Error)
        {
            result.errorCode = QStringLiteral("fixture/engine-error");
            result.errorMessage = QStringLiteral("The fixture inspection could not complete.");
        }
    }
};

}   // namespace receiptparity

#endif   // RECEIPTPARITYFIXTURE_H
