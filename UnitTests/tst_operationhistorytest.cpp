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

#include "pdfartifactstore.h"
#include "pdfoperationhistorystore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#if defined(Q_OS_WIN)
// TEMP-DIAG: UnitTestsOperationHistory dies on Windows CI with *zero* captured
// output (no QtTest banner, no crash dialog text) even when the child process
// is launched directly and stdout/stderr are redirected to separate files
// outside of ctest. That signature - truly nothing written before the process
// disappears - means whatever kills it bypasses ordinary C++/SEH unwinding
// and buffered stdio flushing. These handlers are installed by a static
// initializer (below), which runs during CRT startup before main() is even
// entered, so they're live for the earliest possible failure window,
// including failures during Qt/plugin static initialization.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <malloc.h>

namespace
{

void diagFlushAndExit(int code)
{
    std::fflush(stderr);
    std::fflush(stdout);
    // _exit (not exit/abort) to skip further CRT/Qt teardown that could
    // itself re-trigger whatever corrupted state caused the failure.
    _exit(code);
}

// A genuine stack overflow leaves only the reserved guard page of stack space
// by the time this filter runs. CRT stdio (fprintf, locale-aware formatting,
// stream locking, possible heap allocation) is not guaranteed to fit in that
// budget and can silently re-fault before writing anything - which would
// look identical to the "nothing ever gets to run" failures already ruled
// out. Write via the raw Win32 API with a fixed-size stack buffer and no CRT
// stdio involvement so the filter itself has the best chance of surviving.
void diagRawWrite(const char* text)
{
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(handle, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }
}

void diagHexAppend(char* buffer, size_t& offset, size_t capacity, unsigned long value)
{
    static const char digits[] = "0123456789ABCDEF";
    char temp[8];
    for (int i = 7; i >= 0; --i)
    {
        temp[i] = digits[value & 0xF];
        value >>= 4;
    }
    for (int i = 0; i < 8 && offset + 1 < capacity; ++i)
        buffer[offset++] = temp[i];
}

LONG WINAPI diagUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    const DWORD exceptionCode = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    if (exceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        // Reclaims the guard page so the handler below has real stack to run
        // on; without this, code past this point can re-fault immediately.
        _resetstkoflw();
    }
    char buffer[96];
    size_t offset = 0;
    const char prefix[] = "[diag] SetUnhandledExceptionFilter: code=0x";
    for (const char* p = prefix; *p && offset + 1 < sizeof(buffer); ++p)
        buffer[offset++] = *p;
    diagHexAppend(buffer, offset, sizeof(buffer), exceptionCode);
    if (offset + 1 < sizeof(buffer))
        buffer[offset++] = '\n';
    buffer[offset] = '\0';
    diagRawWrite(buffer);
    diagFlushAndExit(3);
    return EXCEPTION_EXECUTE_HANDLER;
}

void diagInvalidParameterHandler(const wchar_t* expression, const wchar_t* function,
                                 const wchar_t* file, unsigned int line, uintptr_t)
{
    std::fwprintf(stderr, L"[diag] _set_invalid_parameter_handler: expr=%ls func=%ls file=%ls line=%u\n",
                  expression ? expression : L"?", function ? function : L"?",
                  file ? file : L"?", line);
    diagFlushAndExit(4);
}

void diagPurecallHandler()
{
    std::fprintf(stderr, "[diag] _set_purecall_handler: pure virtual function called\n");
    diagFlushAndExit(5);
}

extern "C" void diagSignalHandler(int signalNumber)
{
    std::fprintf(stderr, "[diag] signal handler: signal=%d\n", signalNumber);
    diagFlushAndExit(6);
}

struct DiagInstaller
{
    DiagInstaller()
    {
        // stderr can be fully buffered when redirected to a file/pipe (as
        // ctest and our own diagnostic runs both do); force it unbuffered so
        // a partial write survives even a hard crash a few lines later.
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::fprintf(stderr, "[diag] handlers installing (static init)\n");
        std::fflush(stderr);
        SetUnhandledExceptionFilter(diagUnhandledExceptionFilter);
        _set_invalid_parameter_handler(diagInvalidParameterHandler);
        _set_purecall_handler(diagPurecallHandler);
        std::signal(SIGABRT, diagSignalHandler);
        std::signal(SIGSEGV, diagSignalHandler);
        std::fprintf(stderr, "[diag] handlers installed\n");
        std::fflush(stderr);
    }
};

const DiagInstaller diagInstaller;

}   // namespace

// TEMP-DIAG: per-slot bisection (see UnitTests/CMakeLists.txt) showed every
// slot that touches QTemporaryDir/PDFArtifactStore crashes on Windows while
// the one slot that doesn't (canonicalJsonIsStableAndRedacted) passes. This
// traces individual statements within the simplest failing slot so the next
// Windows run pinpoints which specific line is the last one that ran.
static void diagTrace(const char* text)
{
    std::fprintf(stderr, "[trace] %s\n", text);
    std::fflush(stderr);
}
#else
static void diagTrace(const char*) {}
#endif

class OperationHistoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void canonicalJsonIsStableAndRedacted();
    // TEMP-DIAG: finer bisection than artifactStoreStreamsAndDetectsTampering.
    void diagTemporaryDirOnly();
    void diagArtifactStoreConstructionOnly();
    void diagArtifactStoreImportOnly();
    void artifactStoreStreamsAndDetectsTampering();
    void lifecycleApprovalAndRollbackResolution();
    void rollbackPointsRetentionAndAtomicity();
    void externalPayloadTamperingCompromisesChain();
    void provenanceKindsRoundTripAndMiddleDeletionCompromisesChain();
};

void OperationHistoryTest::canonicalJsonIsStableAndRedacted()
{
    const QJsonObject first{ { QStringLiteral("z"), 1 }, { QStringLiteral("a"), QJsonObject{ { QStringLiteral("token"), QStringLiteral("secret") } } } };
    const QJsonObject second{ { QStringLiteral("a"), QJsonObject{ { QStringLiteral("token"), QStringLiteral("secret") } } }, { QStringLiteral("z"), 1 } };
    QCOMPARE(pdf::canonicalJson(first), pdf::canonicalJson(second));
    const QJsonObject redacted = pdf::redactSensitiveJson(first).toObject();
    QCOMPARE(redacted.value(QStringLiteral("a")).toObject().value(QStringLiteral("token")).toString(), QStringLiteral("[REDACTED]"));
}

void OperationHistoryTest::diagTemporaryDirOnly()
{
    diagTrace("diagTemporaryDirOnly: before QTemporaryDir()");
    QTemporaryDir temporary;
    diagTrace("diagTemporaryDirOnly: after QTemporaryDir()");
    QVERIFY(temporary.isValid());
    diagTrace("diagTemporaryDirOnly: after isValid()");
}

void OperationHistoryTest::diagArtifactStoreConstructionOnly()
{
    diagTrace("diagArtifactStoreConstructionOnly: before QTemporaryDir()");
    QTemporaryDir temporary;
    diagTrace("diagArtifactStoreConstructionOnly: after QTemporaryDir()");
    QVERIFY(temporary.isValid());
    diagTrace("diagArtifactStoreConstructionOnly: before PDFArtifactStore()");
    pdf::PDFArtifactStore store(temporary.path());
    diagTrace("diagArtifactStoreConstructionOnly: after PDFArtifactStore()");
}

void OperationHistoryTest::diagArtifactStoreImportOnly()
{
    diagTrace("diagArtifactStoreImportOnly: before QTemporaryDir()");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore store(temporary.path());
    diagTrace("diagArtifactStoreImportOnly: before importBytes()");
    const QByteArray payload("immutable artifact payload");
    const pdf::PDFArtifactStoreResult first = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    diagTrace("diagArtifactStoreImportOnly: after importBytes()");
    diagTrace(first.success ? "diagArtifactStoreImportOnly: first.success == true" : "diagArtifactStoreImportOnly: first.success == false");
    diagTrace(first.errorMessage.isNull() ? "diagArtifactStoreImportOnly: errorMessage isNull" : "diagArtifactStoreImportOnly: errorMessage not null");
    {
        const QByteArray errBytes = first.errorMessage.toLocal8Bit();
        diagTrace("diagArtifactStoreImportOnly: toLocal8Bit() completed");
        const char* errPtr = errBytes.constData();
        diagTrace((errPtr && *errPtr) ? "diagArtifactStoreImportOnly: constData() non-empty" : "diagArtifactStoreImportOnly: constData() empty/null");
        // TEMP-DIAG: print the actual failure text via our own already-proven-safe
        // toLocal8Bit()/constData() path (not qPrintable/QVERIFY2) so we learn *why*
        // importBytes() is failing without going anywhere near the call that crashes.
        const QByteArray labeled = (QStringLiteral("diagArtifactStoreImportOnly: errorMessage text = [") +
                                    first.errorMessage + QStringLiteral("]"))
                                       .toLocal8Bit();
        diagTrace(labeled.constData());
    }
    // TEMP-DIAG: the previous run showed the crash happens somewhere inside
    // QVERIFY2(first.success, qPrintable(first.errorMessage)) itself, even though
    // every manual read of first.success/errorMessage above succeeded. Swap to a
    // plain QVERIFY (no description argument) to learn whether ANY QTest failure
    // report crashes on this Windows build, or specifically QVERIFY2's two-argument
    // failure path with a qPrintable() description.
    diagTrace("diagArtifactStoreImportOnly: before QVERIFY (no description)");
    QVERIFY(first.success);
    diagTrace("diagArtifactStoreImportOnly: done");
}

void OperationHistoryTest::artifactStoreStreamsAndDetectsTampering()
{
    diagTrace("artifactStoreStreamsAndDetectsTampering: before QTemporaryDir()");
    QTemporaryDir temporary;
    diagTrace("artifactStoreStreamsAndDetectsTampering: after QTemporaryDir()");
    QVERIFY(temporary.isValid());
    diagTrace("artifactStoreStreamsAndDetectsTampering: before PDFArtifactStore()");
    pdf::PDFArtifactStore store(temporary.path());
    diagTrace("artifactStoreStreamsAndDetectsTampering: after PDFArtifactStore()");
    const QByteArray payload("immutable artifact payload");
    diagTrace("artifactStoreStreamsAndDetectsTampering: before first importBytes()");
    const pdf::PDFArtifactStoreResult first = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    diagTrace("artifactStoreStreamsAndDetectsTampering: after first importBytes()");
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    QCOMPARE(first.artifact.sha256, QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    QVERIFY(first.artifact.isValid());
    QVERIFY(store.verify(first.artifact));
    diagTrace("artifactStoreStreamsAndDetectsTampering: before permissions check");
    QVERIFY(!(QFileInfo(store.pathFor(first.artifact)).permissions() & QFileDevice::WriteOwner));
    diagTrace("artifactStoreStreamsAndDetectsTampering: after permissions check");

    const pdf::PDFArtifactStoreResult second = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("copy.pdf") });
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QVERIFY(second.reused);
    QVERIFY(store.verify(second.artifact));

    QFile file(store.pathFor(first.artifact));
    QVERIFY(QFile::setPermissions(file.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("tamper") > 0);
    file.close();
    QVERIFY(QFile::setPermissions(file.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(!store.verify(first.artifact));
}

void OperationHistoryTest::lifecycleApprovalAndRollbackResolution()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto output = artifacts.importBytes("output", { QStringLiteral("application/pdf"), QStringLiteral("output.pdf") });
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    QVERIFY2(output.success, qPrintable(output.errorMessage));

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerArtifact(input.artifact));
    QVERIFY(history.registerArtifact(output.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("test.operation");
    execution.operationVersion = 2;
    execution.input = input.artifact;
    execution.sourceDocumentRevision = 7;
    execution.parameters = QJsonObject{ { QStringLiteral("password"), QStringLiteral("do-not-store") }, { QStringLiteral("mode"), QStringLiteral("safe") } };
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    QVERIFY(history.appendEvent(running));

    pdf::PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    accepted.output = output.artifact;
    accepted.resultSummary = QJsonObject{ { QStringLiteral("password"), QStringLiteral("do-not-store") } };
    accepted.approval.kind = pdf::PDFApprovalKind::Human;
    accepted.approval.actorId = QStringLiteral("local-user:test");
    accepted.approval.decision = QStringLiteral("approve");
    accepted.approval.rationale = QStringLiteral("postflight passed");
    accepted.approval.evidenceSha256 = output.artifact.sha256;
    accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    QVERIFY(history.appendEvent(accepted));

    const auto verification = history.verify();
    QVERIFY(verification.verified);
    QCOMPARE(verification.eventsChecked, qint64(2));
    const auto rows = history.events();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(1).resultSummary.value(QStringLiteral("password")).toString(), QStringLiteral("[REDACTED]"));
    QVERIFY(rows.at(1).approval.isValid());

    pdf::PDFRollbackRequest rollback;
    rollback.currentArtifactSha256 = input.artifact.sha256;
    rollback.targetArtifactSha256 = output.artifact.sha256;
    rollback.targetExecutionId = executionId;
    pdf::PDFArtifactIdentity resolved;
    QVERIFY(history.resolveRollbackTarget(rollback, &resolved));
    QCOMPARE(resolved.sha256, output.artifact.sha256);
    const QString rollbackPath = QDir(temporary.path()).filePath(QStringLiteral("rollback.pdf"));
    QVERIFY(artifacts.restoreToFile(resolved, rollbackPath).success);
    QFile rollbackFile(rollbackPath);
    QVERIFY(rollbackFile.open(QIODevice::ReadOnly));
    QCOMPARE(rollbackFile.readAll(), QByteArray("output"));
}

void OperationHistoryTest::rollbackPointsRetentionAndAtomicity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("input", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto middle = artifacts.importBytes("middle", { QStringLiteral("application/pdf"), QStringLiteral("middle.pdf") });
    const auto final = artifacts.importBytes("final", { QStringLiteral("application/pdf"), QStringLiteral("final.pdf") });
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    QVERIFY2(middle.success, qPrintable(middle.errorMessage));
    QVERIFY2(final.success, qPrintable(final.errorMessage));

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerOriginalInput(input.artifact));
    QVERIFY(history.registerArtifact(middle.artifact));
    QVERIFY(history.registerArtifact(final.artifact));

    auto appendAccepted = [&](const pdf::PDFArtifactIdentity& source,
                              const pdf::PDFArtifactIdentity& output,
                              const QString& operation,
                              bool approved,
                              QUuid* executionId)
    {
        pdf::PDFOperationHistoryExecution execution;
        execution.operationId = operation;
        execution.input = source;
        if (!history.beginExecution(execution, executionId))
            return false;
        pdf::PDFOperationHistoryEvent event;
        event.executionId = *executionId;
        event.status = pdf::PDFOperationHistoryStatus::Accepted;
        event.output = output;
        if (approved)
        {
            event.approval.kind = pdf::PDFApprovalKind::Human;
            event.approval.actorId = QStringLiteral("test-user");
            event.approval.decision = QStringLiteral("approve");
            event.approval.decidedUtc = QDateTime::currentDateTimeUtc();
        }
        return static_cast<bool>(history.appendEvent(event));
    };

    QUuid middleExecution;
    QUuid finalExecution;
    QVERIFY(appendAccepted(input.artifact, middle.artifact, QStringLiteral("test.middle"), false, &middleExecution));
    QVERIFY(appendAccepted(middle.artifact, final.artifact, QStringLiteral("test.final"), true, &finalExecution));
    QCOMPARE(history.rollbackPoints().size(), 3);

    pdf::PDFHistoryRetentionPolicy policy;
    policy.maxPointsPerJob = 2;
    policy.maxBytesPerJob = 1024 * 1024;
    policy.maxAgeDays = 365;
    const auto retention = history.enforceRetention(policy, artifacts);
    QVERIFY(retention.success);
    QCOMPARE(retention.pointsEvicted, 1);
    QVERIFY(!artifacts.verify(middle.artifact));
    QVERIFY(artifacts.verify(input.artifact));
    QVERIFY(artifacts.verify(final.artifact));

    QFile current(QDir(temporary.path()).filePath(QStringLiteral("current.pdf")));
    QVERIFY(current.open(QIODevice::WriteOnly));
    QVERIFY(current.write("input") == 5);
    current.close();

    pdf::PDFRollbackRequest rollback;
    rollback.currentArtifactSha256 = input.artifact.sha256;
    rollback.targetArtifactSha256 = final.artifact.sha256;
    rollback.targetExecutionId = finalExecution;
    rollback.reason = QStringLiteral("test rollback");
    rollback.approval.kind = pdf::PDFApprovalKind::System;
    rollback.approval.actorId = QStringLiteral("test-system");
    rollback.approval.decision = QStringLiteral("approve");
    rollback.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    const auto eventsBeforeRollback = history.events();
    QCOMPARE(eventsBeforeRollback.size(), 2);
    QVERIFY(history.rollbackTo(rollback, artifacts, current.fileName()));
    const auto eventsAfterRollback = history.events();
    QCOMPARE(eventsAfterRollback.size(), 4);
    QCOMPARE(eventsAfterRollback.at(0).entryId, eventsBeforeRollback.at(0).entryId);
    QCOMPARE(eventsAfterRollback.at(1).entryId, eventsBeforeRollback.at(1).entryId);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("final"));
    current.close();
    QVERIFY(history.verify().verified);

    QFile corrupt(artifacts.pathFor(final.artifact));
    QVERIFY(QFile::setPermissions(corrupt.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(corrupt.open(QIODevice::Append));
    QVERIFY(corrupt.write("corrupt") > 0);
    corrupt.close();
    QVERIFY(QFile::setPermissions(corrupt.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(!history.rollbackTo(rollback, artifacts, current.fileName()));
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("final"));
}

void OperationHistoryTest::externalPayloadTamperingCompromisesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerArtifact(input.artifact));
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("tamper.test");
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));
    pdf::PDFOperationHistoryEvent event;
    event.executionId = executionId;
    event.status = pdf::PDFOperationHistoryStatus::Rejected;
    QVERIFY(history.appendEvent(event));

    const QString connectionName = QStringLiteral("history-tamper-test");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("UPDATE history_events SET result_json = '{\"changed\":true}' WHERE sequence = 1")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    const auto verification = history.verify();
    QVERIFY(!verification.verified);
    QCOMPARE(verification.integrity, QStringLiteral("compromised"));
}

void OperationHistoryTest::provenanceKindsRoundTripAndMiddleDeletionCompromisesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY2(input.success, qPrintable(input.errorMessage));

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerArtifact(input.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("provenance.test");
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    const QString digest(64, QLatin1Char('a'));
    const QList<pdf::PDFOperationHistoryEventKind> kinds{
        pdf::PDFOperationHistoryEventKind::DocumentOpened,
        pdf::PDFOperationHistoryEventKind::PreflightRun,
        pdf::PDFOperationHistoryEventKind::FixApplied,
        pdf::PDFOperationHistoryEventKind::DecisionRecorded,
        pdf::PDFOperationHistoryEventKind::DecisionInvalidated,
        pdf::PDFOperationHistoryEventKind::CertificateIssued,
        pdf::PDFOperationHistoryEventKind::CertificateInvalidated
    };

    for (const auto kind : kinds)
    {
        pdf::PDFOperationHistoryEvent event;
        event.executionId = executionId;
        event.kind = kind;
        event.status = pdf::PDFOperationHistoryStatus::Rejected;
        event.operatorIdentity = QStringLiteral("local-user:test");
        event.documentRevisionDigest = digest;
        event.effectiveProfileDigest = digest;
        event.approval.decisionReference = QStringLiteral("decision-test");
        QVERIFY(history.appendEvent(event));
    }

    const auto rows = history.events();
    QCOMPARE(rows.size(), kinds.size());
    for (int index = 0; index < kinds.size(); ++index)
    {
        QCOMPARE(static_cast<int>(rows.at(index).kind), static_cast<int>(kinds.at(index)));
        QCOMPARE(rows.at(index).operatorIdentity, QStringLiteral("local-user:test"));
        QCOMPARE(rows.at(index).documentRevisionDigest, digest);
        QCOMPARE(rows.at(index).effectiveProfileDigest, digest);
        QCOMPARE(rows.at(index).approval.decisionReference, QStringLiteral("decision-test"));
    }
    QVERIFY(history.verify().verified);

    const QString connectionName = QStringLiteral("provenance-middle-delete-test");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(history.databasePath());
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("DELETE FROM history_events WHERE sequence = 4")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    const auto verification = history.verify();
    QVERIFY(!verification.verified);
    QCOMPARE(verification.integrity, QStringLiteral("compromised"));
}

QTEST_MAIN(OperationHistoryTest)
#include "tst_operationhistorytest.moc"
