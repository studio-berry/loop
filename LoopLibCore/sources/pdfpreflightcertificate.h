// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

#ifndef PDFPREFLIGHTCERTIFICATE_H
#define PDFPREFLIGHTCERTIFICATE_H

#include "pdfoperationhistory.h"
#include "preflightengine.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace pdf
{

enum class PreflightCertificateState
{
    Valid,
    InvalidDocumentChanged,
    InvalidAuditChainBroken,
    InvalidDecisionStale,
    InvalidProfileProvisional,
    InvalidInspection,
    InvalidCertificate
};

struct LOOPLIBCORESHARED_EXPORT PreflightCertificate
{
    QString certificateId;
    QDateTime issuedAtUtc;
    QString issuedBy;
    QString documentRevisionDigest;
    QString effectiveProfileDigest;
    QString reportDigest;
    QString auditChainHeadEventId;
    int errorCount = 0;
    int waivedErrorCount = 0;
    QStringList coveringDecisionIds;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& object,
                         PreflightCertificate& certificate,
                         QString& errorMessage);
};

struct LOOPLIBCORESHARED_EXPORT PreflightCertificateVerification
{
    PreflightCertificateState state = PreflightCertificateState::InvalidCertificate;
    QString reason;

    bool isValid() const { return state == PreflightCertificateState::Valid; }
    QJsonObject toJson() const;
};

LOOPLIBCORESHARED_EXPORT QString preflightCertificateStateToString(PreflightCertificateState state);
LOOPLIBCORESHARED_EXPORT QString preflightDecisionIdentity(const PreflightDecision& decision);

LOOPLIBCORESHARED_EXPORT bool issuePreflightCertificate(
    const PreflightResult& result,
    const QJsonObject& report,
    const QByteArray& documentBytes,
    const QList<PDFOperationHistoryEvent>& history,
    const QString& issuedBy,
    PreflightCertificate& certificate,
    QString& errorMessage);

LOOPLIBCORESHARED_EXPORT PreflightCertificateVerification verifyPreflightCertificate(
    const PreflightCertificate& certificate,
    const QByteArray& documentBytes,
    const QList<PDFOperationHistoryEvent>& history);

}   // namespace pdf

#endif   // PDFPREFLIGHTCERTIFICATE_H
