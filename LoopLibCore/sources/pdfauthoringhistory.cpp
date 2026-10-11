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
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pdfauthoringhistory.h"

#include "pdfartifactstore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace pdf
{

namespace
{

QString sidecarDirectory(const QString& documentPath)
{
    return QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
}

}   // namespace

PDFOperationResult appendAuthoringEditEvent(const QString& documentPath,
                                            const QByteArray& inputBytes,
                                            const QByteArray& outputBytes,
                                            const QString& operationId,
                                            const QString& operatorIdentity,
                                            const QJsonObject& resultSummary,
                                            quint64* sequence)
{
    if (documentPath.trimmed().isEmpty())
    {
        return PDFOperationResult(QStringLiteral("Authoring history needs the document path the sidecar belongs to."));
    }
    if (inputBytes.isEmpty() || outputBytes.isEmpty())
    {
        return PDFOperationResult(QStringLiteral("Authoring history needs both the pre-edit and the new revision bytes."));
    }
    if (operationId.trimmed().isEmpty() || operatorIdentity.trimmed().isEmpty())
    {
        return PDFOperationResult(QStringLiteral("Authoring history needs an operation and an operator identity."));
    }

    const QString directory = sidecarDirectory(documentPath);
    if (!QDir().mkpath(directory))
    {
        return PDFOperationResult(QStringLiteral("Could not create the history sidecar directory."));
    }

    const PDFArtifactStore artifacts(directory);
    const PDFArtifactStoreResult inputImport = artifacts.importBytes(
        inputBytes, { QStringLiteral("application/pdf"), QStringLiteral("authoring-input.pdf") });
    if (!inputImport.success)
    {
        return PDFOperationResult(inputImport.errorMessage);
    }
    const PDFArtifactStoreResult outputImport = artifacts.importBytes(
        outputBytes, { QStringLiteral("application/pdf"), QStringLiteral("authoring-output.pdf") });
    if (!outputImport.success)
    {
        return PDFOperationResult(outputImport.errorMessage);
    }

    PDFOperationHistoryStore history(QDir(directory).filePath(QStringLiteral("history.sqlite3")));
    QString error;
    if (!history.open(&error))
    {
        return PDFOperationResult(error);
    }

    // The first edit's undo target is the as-opened revision; it is registered
    // once as the protected original input. Later edits chain onto the previous
    // edit's output point, so no new original is registered for them.
    QString pointsError;
    const QList<PDFRollbackPoint> points = history.rollbackPoints(&pointsError);
    if (!pointsError.isEmpty())
    {
        return PDFOperationResult(pointsError);
    }
    const bool inputHasRollbackPoint = std::any_of(
        points.cbegin(), points.cend(),
        [&inputImport](const PDFRollbackPoint& point)
        { return point.documentRevisionDigest == inputImport.artifact.sha256; });
    if (!inputHasRollbackPoint)
    {
        const PDFOperationResult registered = history.registerOriginalInput(inputImport.artifact);
        if (!registered)
        {
            return registered;
        }
    }

    PDFOperationHistoryExecution execution;
    execution.operationId = operationId;
    execution.input = inputImport.artifact;
    execution.startedUtc = QDateTime::currentDateTimeUtc();
    QUuid executionId;
    if (const PDFOperationResult begun = history.beginExecution(execution, &executionId); !begun)
    {
        return begun;
    }

    PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.kind = PDFOperationHistoryEventKind::FixApplied;
    accepted.status = PDFOperationHistoryStatus::Accepted;
    accepted.operatorIdentity = operatorIdentity;
    // The digest describes the revision the edit produced, which is what a later
    // undo resolves against (QUICK_REVISION_HISTORY.md).
    accepted.documentRevisionDigest = outputImport.artifact.sha256;
    accepted.output = outputImport.artifact;
    accepted.resultSummary = resultSummary;
    accepted.resultSummary.insert(QStringLiteral("model"), QStringLiteral("direct-revision"));
    accepted.approval.kind = PDFApprovalKind::System;
    accepted.approval.actorId = operatorIdentity;
    accepted.approval.decision = QStringLiteral("accept");
    accepted.approval.policyId = QStringLiteral("desktop-authoring");
    accepted.approval.rationale = QStringLiteral("Interactive authoring edit applied as a direct revision (ADR-014).");
    accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    qint64 appendedSequence = 0;
    if (const PDFOperationResult appended = history.appendEvent(accepted, &appendedSequence); !appended)
    {
        return appended;
    }
    if (sequence)
    {
        *sequence = quint64(appendedSequence);
    }
    return PDFOperationResult(true);
}

}   // namespace pdf
