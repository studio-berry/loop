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

#include "pdfworkerruntime.h"

#include "pdfworkerprotocol.h"

#include "pdfartifactidentity.h"
#include "pdfdocumentreader.h"
#include "pdfpreflightverdict.h"
#include "pdfsafefilewriter.h"
#include "preflightclirun.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace pdftool::worker
{

namespace
{

class CancelFence final : public pdf::PDFOperationControl
{
public:
    explicit CancelFence(const std::atomic_bool* flag) :
        m_flag(flag)
    {
    }

    bool isOperationCancelled() const override
    {
        return m_flag && m_flag->load();
    }

private:
    const std::atomic_bool* m_flag = nullptr;
};

pdf::PDFArtifactIdentity artifactFromBytes(const QByteArray& bytes, const QString& logicalName)
{
    pdf::PDFArtifactIdentity identity;
    identity.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size = bytes.size();
    identity.mediaType = QStringLiteral("application/pdf");
    identity.logicalName = pdf::sanitizeArtifactLogicalName(logicalName);
    identity.storageToken = QStringLiteral("worker-input");
    return identity;
}

QJsonObject revisionJson(const QByteArray& sourceData)
{
    const QByteArray hash = QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256);
    return QJsonObject{
        { QStringLiteral("source_sha256"), QString::fromLatin1(hash.toHex()) },
        { QStringLiteral("document_revision"), 0 },
        { QStringLiteral("cache_generation"), 0 },
        { QStringLiteral("effective_profile_identity"), QString() },
    };
}

}   // namespace

WorkerRuntime::WorkerRuntime(WorkerSandboxPaths sandboxPaths) :
    m_sandboxPaths(std::move(sandboxPaths))
{
    m_sandboxStatus = sandboxStatusJson(true, QStringLiteral("linux-landlock-seccomp-rlimit"));
}

void WorkerRuntime::requestCancel()
{
    m_cancelRequested.store(true);
}

bool WorkerRuntime::pathIsInsideSandbox(const QString& candidate, const QString& root) const
{
    const QString absoluteCandidate = QFileInfo(candidate).absoluteFilePath();
    const QString absoluteRoot = QFileInfo(root).absoluteFilePath();
    if (absoluteCandidate == absoluteRoot)
    {
        return true;
    }
    const QString prefix = absoluteRoot.endsWith(QLatin1Char('/')) ? absoluteRoot : absoluteRoot + QLatin1Char('/');
    return absoluteCandidate.startsWith(prefix);
}

QJsonObject WorkerRuntime::handleRequest(const QJsonObject& request)
{
    const int version = request.value(QStringLiteral("v")).toInt();
    const QString id = request.value(QStringLiteral("id")).toString();
    const QString op = request.value(QStringLiteral("op")).toString();

    if (version != PROTOCOL_VERSION)
    {
        return makeErrorResponse(id, op.isEmpty() ? QStringLiteral("unknown") : op,
                                 QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.protocol-version"),
                                 QStringLiteral("Unsupported protocol version."));
    }
    if (id.isEmpty() || !isAllowedOperation(op))
    {
        return makeErrorResponse(id, op.isEmpty() ? QStringLiteral("unknown") : op,
                                 QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.op-not-allowed"),
                                 QStringLiteral("Operation is outside the worker allowlist."));
    }

    if (op == QLatin1String("ping"))
    {
        return handlePing(id);
    }
    if (op == QLatin1String("cancel"))
    {
        return handleCancel(id);
    }
    if (op == QLatin1String("open"))
    {
        return handleOpen(id, request);
    }
    if (op == QLatin1String("preflight"))
    {
        return handlePreflight(id, request);
    }

    return makeErrorResponse(id, op, QStringLiteral("invalid-invocation"),
                             QStringLiteral("worker.op-not-allowed"),
                             QStringLiteral("Operation is outside the worker allowlist."));
}

QJsonObject WorkerRuntime::handlePing(const QString& id)
{
    return makeOkResponse(id, QStringLiteral("ping"), QStringLiteral("success"),
                          QJsonObject{ { QStringLiteral("sandbox"), m_sandboxStatus } });
}

QJsonObject WorkerRuntime::handleCancel(const QString& id)
{
    requestCancel();
    return makeOkResponse(id, QStringLiteral("cancel"), QStringLiteral("success"));
}

QJsonObject WorkerRuntime::handleOpen(const QString& id, const QJsonObject& request)
{
    m_cancelRequested.store(false);
    const QString inputPath = request.value(QStringLiteral("input_path")).toString();
    if (inputPath.isEmpty() ||
        !(pathIsInsideSandbox(inputPath, m_sandboxPaths.inputPath) ||
          QFileInfo(inputPath).absoluteFilePath() == QFileInfo(m_sandboxPaths.inputPath).absoluteFilePath()))
    {
        return makeErrorResponse(id, QStringLiteral("open"), QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.path-outside-sandbox"),
                                 QStringLiteral("input_path is outside the sandbox input root."));
    }

    CancelFence fence(&m_cancelRequested);
    const QString password = request.value(QStringLiteral("password")).toString();
    const bool permissive = request.value(QStringLiteral("permissive")).toBool(true);

    pdf::PDFDocumentReader reader(nullptr, [&password](bool* ok) -> QString
                                  {
                                      *ok = true;
                                      return password; }, permissive, false);
    reader.setOperationControl(&fence);

    pdf::PDFDocument document = reader.readFromFile(inputPath);
    if (fence.isOperationCancelled() || m_cancelRequested.load())
    {
        return makeErrorResponse(id, QStringLiteral("open"), QStringLiteral("cancelled"),
                                 QStringLiteral("worker.cancelled"),
                                 QStringLiteral("Open was cancelled."));
    }

    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        const QString status = reader.getReadingResult() == pdf::PDFDocumentReader::Result::Cancelled
                                   ? QStringLiteral("cancelled")
                                   : QStringLiteral("input-error");
        return makeErrorResponse(id, QStringLiteral("open"), status,
                                 QStringLiteral("worker.open-failed"),
                                 reader.getErrorMessage());
    }

    QFile file(inputPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return makeErrorResponse(id, QStringLiteral("open"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.open-failed"),
                                 QStringLiteral("Failed to read input bytes for artifact identity."));
    }
    const QByteArray sourceData = file.readAll();
    const pdf::PDFArtifactIdentity artifact = artifactFromBytes(sourceData, QFileInfo(inputPath).fileName());

    QJsonArray warnings;
    for (const QString& warning : reader.getWarnings())
    {
        warnings.append(warning);
    }

    return makeOkResponse(id, QStringLiteral("open"), QStringLiteral("success"), QJsonObject{
                                                                                     { QStringLiteral("artifact"), artifact.toJson() },
                                                                                     { QStringLiteral("revision"), revisionJson(sourceData) },
                                                                                     { QStringLiteral("page_count"), static_cast<int>(document.getCatalog()->getPageCount()) },
                                                                                     { QStringLiteral("warnings"), warnings },
                                                                                 });
}

QJsonObject WorkerRuntime::handlePreflight(const QString& id, const QJsonObject& request)
{
    m_cancelRequested.store(false);
    const QString inputPath = request.value(QStringLiteral("input_path")).toString();
    const QString profilePath = request.value(QStringLiteral("profile_path")).toString();
    const QString outputDir = request.value(QStringLiteral("output_dir")).toString();

    if (inputPath.isEmpty() ||
        !(pathIsInsideSandbox(inputPath, m_sandboxPaths.inputPath) ||
          QFileInfo(inputPath).absoluteFilePath() == QFileInfo(m_sandboxPaths.inputPath).absoluteFilePath()))
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.path-outside-sandbox"),
                                 QStringLiteral("input_path is outside the sandbox input root."));
    }
    if (!outputDir.isEmpty() && !pathIsInsideSandbox(outputDir, m_sandboxPaths.outputDir) &&
        QFileInfo(outputDir).absoluteFilePath() != QFileInfo(m_sandboxPaths.outputDir).absoluteFilePath())
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.path-outside-sandbox"),
                                 QStringLiteral("output_dir is outside the sandbox output root."));
    }
    if (!profilePath.isEmpty() &&
        !pathIsInsideSandbox(profilePath, m_sandboxPaths.tempDir) &&
        !pathIsInsideSandbox(profilePath, m_sandboxPaths.inputPath) &&
        !pathIsInsideSandbox(profilePath, m_sandboxPaths.outputDir))
    {
        // Profiles commonly live under the install share tree; allow absolute
        // paths that the supervisor staged into temp, otherwise reject.
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.path-outside-sandbox"),
                                 QStringLiteral("profile_path is outside sandbox roots."));
    }

    QJsonObject profile;
    QString profileError;
    const QString resolvedProfile = profilePath.isEmpty()
                                        ? QString()
                                        : profilePath;
    if (resolvedProfile.isEmpty())
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.profile-required"),
                                 QStringLiteral("profile_path is required for preflight."));
    }
    if (!pdf::PreflightEngine::loadProfile(resolvedProfile, profile, profileError))
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.profile-load-failed"),
                                 profileError);
    }

    CancelFence fence(&m_cancelRequested);
    pdf::PreflightFileInspectionRequest inspectionRequest;
    inspectionRequest.documentPath = inputPath;
    inspectionRequest.password = request.value(QStringLiteral("password")).toString();
    inspectionRequest.permissiveReading = request.value(QStringLiteral("permissive")).toBool(true);
    inspectionRequest.profile = profile;
    inspectionRequest.plan.full = true;
    inspectionRequest.plan.reason = QStringLiteral("worker-preflight");
    inspectionRequest.cancellation = &fence;

    const pdf::PreflightFileInspectionOutcome inspection = pdf::inspectPreflightFile(inspectionRequest);
    if (fence.isOperationCancelled() || m_cancelRequested.load())
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("cancelled"),
                                 QStringLiteral("worker.cancelled"),
                                 QStringLiteral("Preflight was cancelled."));
    }
    if (!inspection.documentReadOk)
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.open-failed"),
                                 inspection.readErrorMessage);
    }

    const pdf::PDFArtifactIdentity artifact = artifactFromBytes(inspection.sourceData, QFileInfo(inputPath).fileName());
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(inspection.report);
    QString status = QStringLiteral("success");
    switch (verdict.state)
    {
        case pdf::PreflightVerdictState::Pass:
            status = QStringLiteral("success");
            break;
        case pdf::PreflightVerdictState::Fail:
            status = QStringLiteral("findings");
            break;
        case pdf::PreflightVerdictState::Incomplete:
            status = QStringLiteral("incomplete");
            break;
        case pdf::PreflightVerdictState::Error:
            status = QStringLiteral("preflight-error");
            break;
    }

    QString publishedReport;
    if (!outputDir.isEmpty() && inspection.inspectionRan && status != QLatin1String("incomplete") &&
        status != QLatin1String("preflight-error"))
    {
        const QString reportPath = QDir(outputDir).filePath(QStringLiteral("preflight-report.json"));
        const QByteArray payload = QJsonDocument(inspection.report.toJson(inputPath)).toJson(QJsonDocument::Indented);
        const pdf::PDFOperationResult writeResult =
            pdf::PDFSafeFileWriter::writeData(reportPath, payload, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
        if (!writeResult)
        {
            return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("incomplete"),
                                     QStringLiteral("worker.report-write-failed"),
                                     writeResult.getErrorMessage());
        }
        publishedReport = reportPath;
    }
    else if (!outputDir.isEmpty() && (status == QLatin1String("incomplete") || status == QLatin1String("preflight-error")))
    {
        // Fail-closed: never publish a partial report as the artifact.
        publishedReport.clear();
    }

    QJsonObject data{
        { QStringLiteral("artifact"), artifact.toJson() },
        { QStringLiteral("revision"), revisionJson(inspection.sourceData) },
        { QStringLiteral("verdict"), pdf::preflightVerdictStateToString(verdict.state) },
        { QStringLiteral("report"), inspection.report.toJson(inputPath) },
    };
    if (!publishedReport.isEmpty())
    {
        data.insert(QStringLiteral("report_path"), publishedReport);
    }

    if (status == QLatin1String("incomplete"))
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("incomplete"),
                                 QStringLiteral("worker.incomplete"),
                                 QStringLiteral("Preflight inspection was incomplete."));
    }

    return makeOkResponse(id, QStringLiteral("preflight"), status, data);
}

}   // namespace pdftool::worker
