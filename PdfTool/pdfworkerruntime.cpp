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
#include "pdfoperationcontrol.h"
#include "pdfpreflightverdict.h"
#include "preflightclirun.h"
#include "preflightengine.h"

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
pdf::PDFArtifactIdentity artifactFromBytes(const QByteArray& bytes)
{
    pdf::PDFArtifactIdentity identity;
    identity.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size = bytes.size();
    identity.mediaType = QStringLiteral("application/pdf");
    identity.logicalName = QStringLiteral("input.pdf");
    identity.storageToken = QStringLiteral("worker-input");
    return identity;
}

bool fileDigestMatches(const QString& path, const QString& digest)
{
    if (!pdf::isPDFSha256(digest))
        return false;
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) &&
           QString::fromLatin1(hash.result().toHex()) == digest;
}
}

WorkerRuntime::WorkerRuntime(WorkerSandboxPaths sandboxPaths, QJsonObject sandboxStatus) :
    m_sandboxPaths(std::move(sandboxPaths)),
    m_sandboxStatus(std::move(sandboxStatus))
{
}

bool WorkerRuntime::pathIsInsideSandbox(const QString& candidate, const QString& root) const
{
    const QFileInfo file(candidate);
    const QString canonical = file.canonicalFilePath();
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return !canonical.isEmpty() && !canonicalRoot.isEmpty() && !file.isSymLink() &&
           (canonical.compare(canonicalRoot, sensitivity) == 0 ||
            canonical.startsWith(canonicalRoot + QLatin1Char('/'), sensitivity));
}

QJsonObject WorkerRuntime::handleRequest(const QJsonObject& request)
{
    const QString id = request.value(QStringLiteral("id")).toString();
    const QString op = request.value(QStringLiteral("op")).toString();
    if (m_sandboxStatus.value(QStringLiteral("applied")) != QJsonValue(true) ||
        request.value(QStringLiteral("v")) != QJsonValue(PROTOCOL_VERSION) ||
        !request.value(QStringLiteral("id")).isString() || id.isEmpty() || id.size() > 128 ||
        !request.value(QStringLiteral("op")).isString() || !isAllowedOperation(op))
    {
        return makeErrorResponse(id, op, QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.invalid-request"), QStringLiteral("Invalid worker request."));
    }
    if (op == QLatin1String("ping"))
        return handlePing(id);
    if (op == QLatin1String("cancel"))
        return handleCancel(id);
    if (!request.value(QStringLiteral("input_path")).isString() ||
        !request.value(QStringLiteral("input_sha256")).isString() ||
        !request.value(QStringLiteral("password")).isString() || !request.value(QStringLiteral("permissive")).isBool() ||
        !pathIsInsideSandbox(request.value(QStringLiteral("input_path")).toString(), m_sandboxPaths.inputPath))
    {
        return makeErrorResponse(id, op, QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.invalid-request"), QStringLiteral("Invalid worker request."));
    }
    if (op == QLatin1String("preflight") &&
        (!request.value(QStringLiteral("profile_path")).isString() ||
         !request.value(QStringLiteral("profile_sha256")).isString() ||
         !pathIsInsideSandbox(request.value(QStringLiteral("profile_path")).toString(), m_sandboxPaths.inputPath)))
    {
        return makeErrorResponse(id, op, QStringLiteral("invalid-invocation"),
                                 QStringLiteral("worker.invalid-request"), QStringLiteral("Invalid worker request."));
    }
    if (!fileDigestMatches(request.value(QStringLiteral("input_path")).toString(), request.value(QStringLiteral("input_sha256")).toString()) ||
        (op == QLatin1String("preflight") &&
         !fileDigestMatches(request.value(QStringLiteral("profile_path")).toString(), request.value(QStringLiteral("profile_sha256")).toString())))
    {
        return makeErrorResponse(id, op, QStringLiteral("input-error"),
                                 QStringLiteral("worker.snapshot-mismatch"), QStringLiteral("Snapshot identity does not match."));
    }
    return op == QLatin1String("open") ? handleOpen(id, request) : handlePreflight(id, request);
}

QJsonObject WorkerRuntime::handlePing(const QString& id)
{
    return makeOkResponse(id, QStringLiteral("ping"), QStringLiteral("success"),
                          QJsonObject{ { QStringLiteral("sandbox"), m_sandboxStatus } });
}

QJsonObject WorkerRuntime::handleCancel(const QString& id)
{
    return makeErrorResponse(id, QStringLiteral("cancel"), QStringLiteral("cancelled"),
                             QStringLiteral("worker.cancelled"), QStringLiteral("Cancellation requires supervisor termination."));
}

QJsonObject WorkerRuntime::handleOpen(const QString& id, const QJsonObject& request)
{
    const QString password = request.value(QStringLiteral("password")).toString();
    bool firstAttempt = true;
    pdf::PDFDocumentReader reader(nullptr, [&](bool* ok)
                                  {
        *ok = firstAttempt;
        firstAttempt = false;
        return password; }, request.value(QStringLiteral("permissive")).toBool(), false);
    const auto document = reader.readFromFile(request.value(QStringLiteral("input_path")).toString());
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        return makeErrorResponse(id, QStringLiteral("open"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.open-failed"), QStringLiteral("PDF could not be opened."));
    }
    const auto artifact = artifactFromBytes(reader.getSource());
    if (artifact.sha256 != request.value(QStringLiteral("input_sha256")).toString())
    {
        return makeErrorResponse(id, QStringLiteral("open"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.snapshot-mismatch"), QStringLiteral("Snapshot identity does not match."));
    }
    return makeOkResponse(id, QStringLiteral("open"), QStringLiteral("success"), QJsonObject{ { QStringLiteral("artifact"), artifact.toJson() }, { QStringLiteral("input_sha256"), artifact.sha256 }, { QStringLiteral("page_count"), static_cast<int>(document.getCatalog()->getPageCount()) } });
}

QJsonObject WorkerRuntime::handlePreflight(const QString& id, const QJsonObject& request)
{
    QJsonObject profile;
    QString error;
    QFile profileFile(request.value(QStringLiteral("profile_path")).toString());
    if (!profileFile.open(QIODevice::ReadOnly) || profileFile.size() > MAX_RESPONSE_BYTES ||
        !pdf::PreflightEngine::loadProfile(profileFile.fileName(), profile, error))
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("input-error"),
                                 QStringLiteral("worker.profile-invalid"), QStringLiteral("Profile could not be loaded."));
    }
    pdf::PreflightFileInspectionRequest inspectionRequest;
    inspectionRequest.documentPath = request.value(QStringLiteral("input_path")).toString();
    inspectionRequest.receiptDocumentId = id;
    inspectionRequest.createReceipt = true;
    inspectionRequest.password = request.value(QStringLiteral("password")).toString();
    inspectionRequest.permissiveReading = request.value(QStringLiteral("permissive")).toBool();
    inspectionRequest.profile = profile;
    inspectionRequest.plan.full = true;
    inspectionRequest.plan.reason = QStringLiteral("worker-preflight");
    const auto inspection = pdf::inspectPreflightFile(inspectionRequest);
    if (!inspection.documentReadOk || !inspection.receipt ||
        inspection.receipt->inputDigest != request.value(QStringLiteral("input_sha256")).toString())
    {
        return makeErrorResponse(id, QStringLiteral("preflight"), QStringLiteral("incomplete"),
                                 QStringLiteral("worker.inspection-incomplete"), QStringLiteral("Inspection did not complete."));
    }
    auto receipt = *inspection.receipt;
    const auto state = receipt.verdict.state;
    const QString status = state == pdf::PreflightVerdictState::Pass ? QStringLiteral("success") : state == pdf::PreflightVerdictState::Fail ? QStringLiteral("findings")
                                                                                               : state == pdf::PreflightVerdictState::Error  ? QStringLiteral("preflight-error")
                                                                                                                                             : QStringLiteral("incomplete");
    receipt.verdict.reason = QStringLiteral("Inspection result: %1.").arg(pdf::preflightVerdictStateToString(state));
    QJsonObject response = (state == pdf::PreflightVerdictState::Pass || state == pdf::PreflightVerdictState::Fail) ? makeOkResponse(id, QStringLiteral("preflight"), status) : makeErrorResponse(id, QStringLiteral("preflight"), status, QStringLiteral("worker.incomplete"), QStringLiteral("Inspection did not complete."));
    response.insert(QStringLiteral("input_sha256"), receipt.inputDigest);
    response.insert(QStringLiteral("profile_sha256"), request.value(QStringLiteral("profile_sha256")));
    response.insert(QStringLiteral("receipt"), receipt.toJson());
    return response;
}
}
