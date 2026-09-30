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

#include "pdfworkerclient.h"
#include "pdfworkerprotocol.h"
#include "pdfartifactidentity.h"
#include "preflightprofileresolver.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace pdftool
{
namespace
{
WorkerClientOutcome outcomeFromStatus(const QString& status)
{
    if (status == QLatin1String("success") || status == QLatin1String("findings"))
        return WorkerClientOutcome::Success;
    if (status == QLatin1String("cancelled"))
        return WorkerClientOutcome::Cancelled;
    if (status == QLatin1String("invalid-invocation"))
        return WorkerClientOutcome::InvalidInvocation;
    if (status == QLatin1String("input-error"))
        return WorkerClientOutcome::InputError;
    if (status == QLatin1String("unavailable"))
        return WorkerClientOutcome::Unavailable;
    return WorkerClientOutcome::Incomplete;
}

pdf::PDFRevisionIdentity requestRevision(const QJsonObject& request)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.sourceDataHash = QByteArray::fromHex(request.value(QStringLiteral("input_sha256")).toString().toLatin1());
    revision.document.documentId = request.value(QStringLiteral("id")).toString();
    return revision;
}

bool responseEnvelopeValid(const QJsonObject& response, const QJsonObject& request)
{
    if (response.value(QStringLiteral("v")) != QJsonValue(worker::PROTOCOL_VERSION) ||
        response.value(QStringLiteral("id")) != request.value(QStringLiteral("id")) ||
        response.value(QStringLiteral("op")) != request.value(QStringLiteral("op")) ||
        !response.value(QStringLiteral("ok")).isBool() || !response.value(QStringLiteral("status")).isString())
    {
        return false;
    }
    const QString status = response.value(QStringLiteral("status")).toString();
    const bool success = status == QLatin1String("success") || status == QLatin1String("findings");
    const QStringList failures{ QStringLiteral("incomplete"), QStringLiteral("cancelled"), QStringLiteral("invalid-invocation"),
                                QStringLiteral("input-error"), QStringLiteral("unavailable"), QStringLiteral("preflight-error") };
    if (response.value(QStringLiteral("ok")).toBool() != success || (!success && !failures.contains(status)) ||
        (status == QLatin1String("findings") && request.value(QStringLiteral("op")) != QJsonValue(QStringLiteral("preflight"))))
    {
        return false;
    }
    return success || (response.value(QStringLiteral("code")).isString() && response.value(QStringLiteral("reason")).isString());
}
}

PdfWorkerClient::PdfWorkerClient() = default;
PdfWorkerClient::~PdfWorkerClient() { killWorker(); }

QString PdfWorkerClient::defaultWorkerExecutable()
{
    const QDir dir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    return dir.filePath(QStringLiteral("loop-pdf-worker.exe"));
#else
    return dir.filePath(QStringLiteral("loop-pdf-worker"));
#endif
}

bool PdfWorkerClient::start(const QString& workerExecutable, const QString& sandboxInput, const QString& sandboxTemp,
                            const QString& sandboxOutput, QString* errorMessage)
{
    Q_UNUSED(sandboxInput);
    killWorker();
    m_workerExecutable = workerExecutable;
    m_sandboxInput = m_snapshots.path();
    m_sandboxTemp = sandboxTemp;
    m_sandboxOutput = sandboxOutput;
    QString error;
    const QStringList arguments{
        QStringLiteral("--sandbox-input"), m_sandboxInput,
        QStringLiteral("--sandbox-temp"), sandboxTemp, QStringLiteral("--sandbox-output"), sandboxOutput
    };
    if (!m_snapshots.isValid() ||
        !m_process.start(workerExecutable, arguments, m_sandboxInput, sandboxTemp, sandboxOutput, error))
    {
        if (errorMessage)
            *errorMessage = error.isEmpty() ? QStringLiteral("worker.launch.snapshots") : error;
        return false;
    }
    const WorkerClientResult handshake = ping();
    if (handshake.outcome != WorkerClientOutcome::Success)
    {
        killWorker();
        if (errorMessage)
            *errorMessage = QStringLiteral("Worker containment handshake failed (exit %1).").arg(m_process.exitCode());
        return false;
    }
    return true;
}

bool PdfWorkerClient::isRunning() const { return m_process.running(); }
qint64 PdfWorkerClient::workerPid() const { return m_process.pid(); }
void PdfWorkerClient::killWorker() { m_process.stop(); }
bool PdfWorkerClient::replaceWorker(QString* errorMessage)
{
    return start(m_workerExecutable, m_sandboxInput, m_sandboxTemp, m_sandboxOutput, errorMessage);
}

bool PdfWorkerClient::stageSnapshot(const QString& source, const QString& name, QString& target, QString& digest)
{
    QFile input(source);
    target = QDir(m_snapshots.path()).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + name);
    QFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd())
    {
        const QByteArray chunk = input.read(65536);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size())
        {
            output.close();
            QFile::remove(target);
            return false;
        }
        hash.addData(chunk);
    }
    if (input.error() != QFileDevice::NoError || !output.flush())
    {
        output.close();
        QFile::remove(target);
        return false;
    }
    output.close();
    if (!QFile::setPermissions(target, QFileDevice::ReadOwner))
    {
        QFile::remove(target);
        return false;
    }
    digest = QString::fromLatin1(hash.result().toHex());
    return true;
}

WorkerClientResult PdfWorkerClient::fromWorkerFailure(const QJsonObject& request, const QString& code)
{
    WorkerClientResult result;
    result.outcome = code == QLatin1String("worker.cancelled") ? WorkerClientOutcome::Cancelled : WorkerClientOutcome::Incomplete;
    result.code = code;
    result.reason = QStringLiteral("Isolated operation did not complete.");
    result.response = worker::makeErrorResponse(request.value(QStringLiteral("id")).toString(),
                                                request.value(QStringLiteral("op")).toString(),
                                                result.outcome == WorkerClientOutcome::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("incomplete"), code, result.reason);
    if (request.value(QStringLiteral("op")) == QJsonValue(QStringLiteral("preflight")))
    {
        const QString profileDigest = m_expectedProfile ? m_expectedProfile->effectiveDigest : request.value(QStringLiteral("profile_sha256")).toString();
        const auto receipt = pdf::buildTerminalPreflightReceipt(request.value(QStringLiteral("input_sha256")).toString(),
                                                                requestRevision(request), profileDigest, code);
        result.response.insert(QStringLiteral("receipt"), receipt.toJson());
    }
    return result;
}

WorkerClientResult PdfWorkerClient::call(const QJsonObject& request, int timeoutMs)
{
    m_cancelled.store(false);
    if (!isRunning())
        return fromWorkerFailure(request, QStringLiteral("worker.unavailable"));
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > worker::MAX_REQUEST_BYTES || timeoutMs <= 0)
    {
        return fromWorkerFailure(request, QStringLiteral("worker.request-limit"));
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray frame;
    if (!m_process.write(line, timer, timeoutMs, m_cancelled))
    {
        killWorker();
        return fromWorkerFailure(request, m_cancelled.load() ? QStringLiteral("worker.cancelled") : QStringLiteral("worker.write-failed"));
    }
    while (timer.elapsed() < timeoutMs && !m_cancelled.load())
    {
        const QByteArray chunk = m_process.read(worker::MAX_RESPONSE_BYTES + 1 - frame.size());
        frame.append(chunk);
        if (frame.size() > worker::MAX_RESPONSE_BYTES)
        {
            killWorker();
            return fromWorkerFailure(request, QStringLiteral("worker.response-limit"));
        }
        const qsizetype end = frame.indexOf('\n');
        if (end >= 0)
        {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(frame.first(end), &error);
            if (end != frame.size() - 1 || error.error != QJsonParseError::NoError || !document.isObject() ||
                !responseEnvelopeValid(document.object(), request))
            {
                killWorker();
                return fromWorkerFailure(request, QStringLiteral("worker.invalid-response"));
            }
            const QJsonObject response = document.object();
            const QString op = request.value(QStringLiteral("op")).toString();
            const QString status = response.value(QStringLiteral("status")).toString();
            bool valid = true;
            QJsonObject admitted;
            if (op == QLatin1String("ping"))
            {
                const QJsonObject sandbox = response.value(QStringLiteral("sandbox")).toObject();
                valid = status == QLatin1String("success") &&
                        sandbox.value(QStringLiteral("applied")) == QJsonValue(true) &&
                        sandbox.value(QStringLiteral("rss_limit_bytes")) == QJsonValue(worker::DEFAULT_RSS_LIMIT_BYTES) &&
                        sandbox.value(QStringLiteral("cpu_limit_seconds")) == QJsonValue(worker::DEFAULT_CPU_SECONDS);
                admitted.insert(QStringLiteral("sandbox"), QJsonObject{
                                                               { QStringLiteral("applied"), true },
#if defined(Q_OS_WIN)
                                                               { QStringLiteral("platform"), QStringLiteral("windows") },
#else
                                                               { QStringLiteral("platform"), QStringLiteral("linux") },
#endif
                                                               { QStringLiteral("detail"), QStringLiteral("enforced") },
                                                               { QStringLiteral("rss_limit_bytes"), worker::DEFAULT_RSS_LIMIT_BYTES },
                                                               { QStringLiteral("cpu_limit_seconds"), worker::DEFAULT_CPU_SECONDS } });
            }
            else if (response.value(QStringLiteral("ok")).toBool() || response.contains(QStringLiteral("receipt")))
            {
                valid = response.value(QStringLiteral("input_sha256")) == request.value(QStringLiteral("input_sha256"));
                if (op == QLatin1String("open"))
                {
                    const QJsonObject artifact = response.value(QStringLiteral("artifact")).toObject();
                    const QJsonValue pages = response.value(QStringLiteral("page_count"));
                    pdf::PDFArtifactIdentity expectedArtifact;
                    expectedArtifact.sha256 = request.value(QStringLiteral("input_sha256")).toString();
                    expectedArtifact.size = QFileInfo(request.value(QStringLiteral("input_path")).toString()).size();
                    expectedArtifact.mediaType = QStringLiteral("application/pdf");
                    expectedArtifact.logicalName = QStringLiteral("input.pdf");
                    expectedArtifact.storageToken = QStringLiteral("worker-input");
                    const QJsonObject expected = expectedArtifact.toJson();
                    for (auto it = expected.begin(); it != expected.end(); ++it)
                        valid &= artifact.value(it.key()) == it.value();
                    valid &=
                        pages.isDouble() && pages.toDouble() > 0 && pages.toDouble() == pages.toInt(-1);
                    admitted.insert(QStringLiteral("artifact"), expected);
                    admitted.insert(QStringLiteral("page_count"), pages.toInt());
                }
                if (op == QLatin1String("preflight"))
                {
                    pdf::PreflightInspectionReceipt receipt;
                    QString receiptError;
                    valid &= m_expectedProfile.has_value() &&
                             response.value(QStringLiteral("profile_sha256")) == request.value(QStringLiteral("profile_sha256")) &&
                             response.value(QStringLiteral("receipt")).isObject() &&
                             pdf::preflightInspectionReceiptFromJson(response.value(QStringLiteral("receipt")).toObject(), receipt, receiptError) &&
                             pdf::validatePreflightInspectionReceipt(receipt, request.value(QStringLiteral("input_sha256")).toString(),
                                                                     requestRevision(request), *m_expectedProfile, receiptError);
                    const QString expectedStatus = receipt.verdict.isPass() ? QStringLiteral("success") : receipt.verdict.state == pdf::PreflightVerdictState::Fail ? QStringLiteral("findings")
                                                                                                      : receipt.verdict.state == pdf::PreflightVerdictState::Error  ? QStringLiteral("preflight-error")
                                                                                                                                                                    : QStringLiteral("incomplete");
                    valid &= status == expectedStatus;
                    admitted.insert(QStringLiteral("receipt"), receipt.toJson());
                    admitted.insert(QStringLiteral("profile_sha256"), request.value(QStringLiteral("profile_sha256")));
                }
            }
            if (op != QLatin1String("ping"))
                admitted.insert(QStringLiteral("input_sha256"), request.value(QStringLiteral("input_sha256")));
            if (!valid)
            {
                killWorker();
                return fromWorkerFailure(request, QStringLiteral("worker.identity-mismatch"));
            }
            if (!response.value(QStringLiteral("ok")).toBool() && !response.contains(QStringLiteral("receipt")))
            {
                return fromWorkerFailure(request, QStringLiteral("worker.operation-failed"));
            }
            WorkerClientResult result;
            result.outcome = outcomeFromStatus(status);
            result.response = worker::makeOkResponse(request.value(QStringLiteral("id")).toString(), op, status, admitted);
            result.response.insert(QStringLiteral("ok"), response.value(QStringLiteral("ok")));
            // Raw worker diagnostics never cross the host's diagnostic gateway.
            if (result.outcome != WorkerClientOutcome::Success)
            {
                result.code = QStringLiteral("worker.incomplete");
                result.reason = QStringLiteral("Isolated inspection did not complete.");
                result.response.insert(QStringLiteral("code"), result.code);
                result.response.insert(QStringLiteral("reason"), result.reason);
            }
            return result;
        }
        if (!isRunning())
        {
            killWorker();
            return fromWorkerFailure(request, QStringLiteral("worker.exited"));
        }
    }
    killWorker();
    return fromWorkerFailure(request, m_cancelled.load() ? QStringLiteral("worker.cancelled") : QStringLiteral("worker.timeout"));
}

WorkerClientResult PdfWorkerClient::ping(int timeoutMs)
{
    return call(QJsonObject{ { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                             { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                             { QStringLiteral("op"), QStringLiteral("ping") } },
                timeoutMs);
}

WorkerClientResult PdfWorkerClient::openDocument(const QString& inputPath, const QString& password, bool permissive, int timeoutMs)
{
    QJsonObject request{ { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                         { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                         { QStringLiteral("op"), QStringLiteral("open") },
                         { QStringLiteral("password"), password },
                         { QStringLiteral("permissive"), permissive } };
    QString snapshot, digest;
    if (!stageSnapshot(inputPath, QStringLiteral("-input.pdf"), snapshot, digest))
        return fromWorkerFailure(request, QStringLiteral("worker.snapshot-failed"));
    request.insert(QStringLiteral("input_path"), snapshot);
    request.insert(QStringLiteral("input_sha256"), digest);
    auto result = call(request, timeoutMs);
    if (!QFile::setPermissions(snapshot, QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !QFile::remove(snapshot))
    {
        killWorker();
        return fromWorkerFailure(request, QStringLiteral("worker.snapshot-cleanup-failed"));
    }
    return result;
}

WorkerClientResult PdfWorkerClient::preflight(const QString& inputPath, const QString& profilePath, const QString& outputDir,
                                              const QString& password, bool permissive, int timeoutMs)
{
    Q_UNUSED(outputDir);
    m_expectedProfile.reset();
    QJsonObject request{ { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                         { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                         { QStringLiteral("op"), QStringLiteral("preflight") },
                         { QStringLiteral("password"), password },
                         { QStringLiteral("permissive"), permissive } };
    QString input, digest, profile, profileDigest;
    if (!stageSnapshot(inputPath, QStringLiteral("-input.pdf"), input, digest))
    {
        return fromWorkerFailure(request, QStringLiteral("worker.snapshot-failed"));
    }
    request.insert(QStringLiteral("input_path"), input);
    request.insert(QStringLiteral("input_sha256"), digest);
    if (!stageSnapshot(profilePath, QStringLiteral("-profile.json"), profile, profileDigest))
    {
        const bool removed = QFile::setPermissions(input, QFileDevice::ReadOwner | QFileDevice::WriteOwner) && QFile::remove(input);
        if (!removed)
            killWorker();
        return fromWorkerFailure(request, removed ? QStringLiteral("worker.snapshot-failed") : QStringLiteral("worker.snapshot-cleanup-failed"));
    }
    request.insert(QStringLiteral("profile_path"), profile);
    request.insert(QStringLiteral("profile_sha256"), profileDigest);
    QFile profileFile(profile);
    QString error;
    pdf::PreflightProfileData data;
    bool valid = profileFile.open(QIODevice::ReadOnly) && profileFile.size() <= worker::MAX_RESPONSE_BYTES;
    QJsonParseError parseError;
    const QJsonDocument document = valid ? QJsonDocument::fromJson(profileFile.readAll(), &parseError) : QJsonDocument();
    const auto imported = pdf::importPreflightProfile(document.object());
    const auto bound = pdf::bindPreflightProfileVariables(imported.profile, QJsonObject{});
    valid &= parseError.error == QJsonParseError::NoError && document.isObject() && imported.ok && bound.ok &&
             pdf::PreflightEngine::parseProfile(bound.profile, data, error);
    if (valid)
    {
        data.variableBindings = bound.bindings;
        data.fileDigest = imported.identity.digest;
        data.effectiveDigest = pdf::computeProfileDigest(bound.profile);
        data.provisional = imported.identity.provisional;
        data.profileIdentity = imported.identity.toJson();
        data.profileIdentity.insert(QStringLiteral("digest"), data.fileDigest);
        data.profileIdentity.insert(QStringLiteral("effective_digest"), data.effectiveDigest);
        m_expectedProfile = data;
    }
    auto result = valid ? call(request, timeoutMs) : fromWorkerFailure(request, QStringLiteral("worker.profile-invalid"));
    profileFile.close();
    const bool inputRemoved = QFile::setPermissions(input, QFileDevice::ReadOwner | QFileDevice::WriteOwner) && QFile::remove(input);
    const bool profileRemoved = QFile::setPermissions(profile, QFileDevice::ReadOwner | QFileDevice::WriteOwner) && QFile::remove(profile);
    if (!inputRemoved || !profileRemoved)
    {
        killWorker();
        return fromWorkerFailure(request, QStringLiteral("worker.snapshot-cleanup-failed"));
    }
    return result;
}

WorkerClientResult PdfWorkerClient::cancel(int timeoutMs)
{
    Q_UNUSED(timeoutMs);
    m_cancelled.store(true);
    WorkerClientResult result;
    result.outcome = WorkerClientOutcome::Cancelled;
    result.code = QStringLiteral("worker.cancelled");
    result.reason = QStringLiteral("Isolated operation cancelled.");
    return result;
}
}
