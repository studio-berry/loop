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

#ifndef PDFPREFLIGHTAUDIT_H
#define PDFPREFLIGHTAUDIT_H

#include "pdfoperationhistory.h"
#include "pdfutils.h"
#include "preflightengine.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace pdf
{

/// Appends one completed preflight act to the canonical operation-history
/// chain. The document bytes are retained as an immutable artifact outside the
/// PDF; no provenance is written into the document itself.
LOOPLIBCORESHARED_EXPORT PDFOperationResult appendPreflightAuditRun(
    const QString& documentPath,
    const QByteArray& documentBytes,
    const PreflightResult& result,
    PDFOperationHistoryStatus status,
    const QString& operatorIdentity,
    const QJsonObject& summary = QJsonObject());

}   // namespace pdf

#endif   // PDFPREFLIGHTAUDIT_H
