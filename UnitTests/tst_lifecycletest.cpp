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
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistorystore.h"
#include "pdfsavepolicy.h"
#include "pdfworkloadenvelope.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>
#include <QtTest>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

class LifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    void boundedTraceGenerationIsDeterministic();
    void seededSequencePreservesInvariants();
    void injectedStaleResultIsCaught();
    void injectedOverwriteIsCaught();
    void injectedRollbackHistoryDefectIsCaught();
};

namespace
{

enum class TraceCommandKind
{
    Open,
    RenderPreflight,
    Cancel,
    ReplaceRevision,
    SaveReopen,
    Rollback,
    Close,
};

QString traceCommandName(TraceCommandKind kind)
{
    switch (kind)
    {
        case TraceCommandKind::Open:
            return QStringLiteral("open");
        case TraceCommandKind::RenderPreflight:
            return QStringLiteral("render-preflight");
        case TraceCommandKind::Cancel:
            return QStringLiteral("cancel");
        case TraceCommandKind::ReplaceRevision:
            return QStringLiteral("replace-revision");
        case TraceCommandKind::SaveReopen:
            return QStringLiteral("save-reopen");
        case TraceCommandKind::Rollback:
            return QStringLiteral("rollback");
        case TraceCommandKind::Close:
            return QStringLiteral("close");
    }
    return QStringLiteral("unknown");
}

struct TraceCommand
{
    TraceCommandKind kind;
    quint64 argument = 0;
};

quint64 nextTraceRandom(quint64& state)
{
    state += UINT64_C(0x9e3779b97f4a7c15);
    quint64 value = state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

QVector<TraceCommand> generateTrace(quint64 seed)
{
    const QVector<TraceCommandKind> activeCoverage = {
        TraceCommandKind::Open,
        TraceCommandKind::RenderPreflight,
        TraceCommandKind::Cancel,
        TraceCommandKind::ReplaceRevision,
        TraceCommandKind::SaveReopen,
        TraceCommandKind::Rollback,
    };
    QVector<TraceCommand> trace;
    trace.reserve(32);
    quint64 state = seed;
    for (const TraceCommandKind kind : activeCoverage)
    {
        trace.append({ kind, nextTraceRandom(state) });
    }
    while (trace.size() < 31)
    {
        const auto kind = activeCoverage.at(static_cast<qsizetype>(nextTraceRandom(state) % activeCoverage.size()));
        trace.append({ kind, nextTraceRandom(state) });
    }
    trace.append({ TraceCommandKind::Close, nextTraceRandom(state) });
    return trace;
}

QJsonObject traceToJson(quint64 seed, const QVector<TraceCommand>& trace)
{
    QJsonArray commands;
    for (qsizetype index = 0; index < trace.size(); ++index)
    {
        commands.append(QJsonObject{
            { QStringLiteral("index"), static_cast<int>(index) },
            { QStringLiteral("kind"), traceCommandName(trace.at(index).kind) },
            { QStringLiteral("argument"), QString::number(trace.at(index).argument) } });
    }
    return QJsonObject{
        { QStringLiteral("schema_kind"), QStringLiteral("loop-lifecycle-trace") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("seed"), static_cast<qint64>(seed) },
        { QStringLiteral("initial_artifact_digest"), pdf::PDFRunIdentity::digestBytes(QByteArrayLiteral("lifecycle-source-v1")) },
        { QStringLiteral("commands"), commands },
        { QStringLiteral("expected_invariants"), QJsonArray{
                                                     QStringLiteral("source-immutable"),
                                                     QStringLiteral("cancel-is-terminal"),
                                                     QStringLiteral("stale-results-rejected"),
                                                     QStringLiteral("history-append-only") } }
    };
}

struct LifecycleState
{
    QString sourceDigest;
    quint64 lastRevision = 0;
    bool open = false;
    bool recovered = false;
    bool certified = false;
    bool lastCancelled = false;
    bool lastSucceeded = false;
    bool acceptedStale = false;
    bool sourceOverwritten = false;
    bool historyMutated = false;
    pdf::PDFSaveMode lastSaveMode = pdf::PDFSaveMode::IncrementalAppend;
    QList<QUuid> eventIds;
    pdf::PDFArtifactIdentity original;
    pdf::PDFArtifactIdentity current;
    QUuid lastAcceptedExecution;
};

QString invariantFailure(const LifecycleState& state, const pdf::PDFArtifactStore& artifacts, const pdf::PDFOperationHistoryStore& history)
{
    if (!state.sourceDigest.isEmpty() && state.original.sha256 != state.sourceDigest)
    {
        return QStringLiteral("source-digest-changed");
    }
    if (state.sourceOverwritten)
    {
        return QStringLiteral("source-overwritten");
    }
    if (state.acceptedStale)
    {
        return QStringLiteral("stale-result-accepted");
    }
    if (state.lastCancelled && state.lastSucceeded)
    {
        return QStringLiteral("cancel-marked-success");
    }
    if (state.recovered && state.certified)
    {
        return QStringLiteral("recovered-output-certified");
    }
    if (state.historyMutated)
    {
        return QStringLiteral("rollback-history-mutated");
    }

    const QList<pdf::PDFOperationHistoryEvent> events = history.events();
    QList<QUuid> ids;
    qint64 previous = 0;
    for (const pdf::PDFOperationHistoryEvent& event : events)
    {
        if (event.sequence <= previous && previous != 0)
        {
            return QStringLiteral("event-sequence-not-monotonic");
        }
        previous = event.sequence;
        ids.append(event.entryId);
    }
    if (ids.size() < state.eventIds.size())
    {
        return QStringLiteral("provenance-not-append-only");
    }
    if (state.open && !artifacts.verify(state.original))
    {
        return QStringLiteral("original-artifact-unverified");
    }
    return QString();
}

bool appendEvent(pdf::PDFOperationHistoryStore& history,
                 const pdf::PDFArtifactIdentity& input,
                 pdf::PDFOperationHistoryEventKind kind,
                 pdf::PDFOperationHistoryStatus status,
                 QUuid* executionId,
                 LifecycleState* state,
                 const std::optional<pdf::PDFArtifactIdentity>& output = std::nullopt)
{
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("lifecycle.test");
    execution.input = input;
    if (!history.beginExecution(execution, executionId))
    {
        return false;
    }
    pdf::PDFOperationHistoryEvent event;
    event.executionId = *executionId;
    event.kind = kind;
    event.status = status;
    event.output = output;
    if (!history.appendEvent(event))
    {
        return false;
    }
    const QList<pdf::PDFOperationHistoryEvent> events = history.events();
    if (events.isEmpty())
    {
        return false;
    }
    state->eventIds.append(events.last().entryId);
    return true;
}

}   // namespace

void LifecycleTest::boundedTraceGenerationIsDeterministic()
{
    constexpr quint64 seed = UINT64_C(0x20260821);
    const QVector<TraceCommand> first = generateTrace(seed);
    const QVector<TraceCommand> second = generateTrace(seed);
    QCOMPARE(first.size(), 32);
    QCOMPARE(QJsonDocument(traceToJson(seed, first)).toJson(QJsonDocument::Compact),
             QJsonDocument(traceToJson(seed, second)).toJson(QJsonDocument::Compact));

    QFile goldenFile(QStringLiteral(LOOP_UNITTEST_SOURCE_DIR "/testdata/lifecycle/seed-20260821.json"));
    QVERIFY(goldenFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument goldenDocument = QJsonDocument::fromJson(goldenFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(QJsonDocument(traceToJson(seed, first)).toJson(QJsonDocument::Compact),
             QJsonDocument(goldenDocument.object()).toJson(QJsonDocument::Compact));

    bool opened = false;
    bool cancelled = false;
    quint64 revision = 0;
    for (const TraceCommand& command : first)
    {
        switch (command.kind)
        {
            case TraceCommandKind::Open:
                opened = true;
                break;
            case TraceCommandKind::RenderPreflight:
                QVERIFY(opened);
                break;
            case TraceCommandKind::Cancel:
                cancelled = true;
                break;
            case TraceCommandKind::ReplaceRevision:
                QVERIFY(opened);
                ++revision;
                break;
            case TraceCommandKind::SaveReopen:
                QVERIFY(opened);
                break;
            case TraceCommandKind::Rollback:
                QVERIFY(opened);
                break;
            case TraceCommandKind::Close:
                opened = false;
                break;
        }
    }
    QVERIFY(cancelled);
    QVERIFY(revision > 0);
}

void LifecycleTest::seededSequencePreservesInvariants()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);
    const QByteArray originalBytes("lifecycle-source-v1");
    const auto imported = artifacts.importBytes(originalBytes, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    QVERIFY(imported.success);
    QVERIFY(history.registerOriginalInput(imported.artifact));

    LifecycleState state;
    state.original = imported.artifact;
    state.current = imported.artifact;
    state.sourceDigest = imported.artifact.sha256;
    state.open = true;
    state.lastRevision = context.getRevision().documentRevision;
    QUuid openedId;
    QVERIFY(appendEvent(history, imported.artifact, pdf::PDFOperationHistoryEventKind::DocumentOpened,
                        pdf::PDFOperationHistoryStatus::Accepted, &openedId, &state, imported.artifact));

    const pdf::PDFRevisionIdentity beforeEdit = context.getRevision();
    context.markModified(pdf::PDFModifiedDocument::PageContents);
    QVERIFY(context.getRevision().documentRevision > beforeEdit.documentRevision);
    state.lastRevision = context.getRevision().documentRevision;

    pdf::PDFJobScheduler scheduler(1);
    std::atomic_bool started = false;
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.documentRevision = context.getRevision().toString();
    const QString jobId = scheduler.submit(spec, [&started](pdf::PDFJobContext& jobContext)
                                           {
                                               started = true;
                                               while (!jobContext.isCancellationRequested())
                                               {
                                                   std::this_thread::yield();
                                               } });
    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    QVERIFY(scheduler.cancel(jobId));
    QVERIFY(scheduler.waitForFinished(jobId, 1000));
    QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Cancelled);
    state.lastCancelled = true;
    state.lastSucceeded = false;
    QUuid cancelledId;
    QVERIFY(appendEvent(history, state.current, pdf::PDFOperationHistoryEventKind::PreflightRun,
                        pdf::PDFOperationHistoryStatus::Cancelled, &cancelledId, &state));

    const pdf::PDFOperationSavePolicy incremental = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("edit"));
    QCOMPARE(incremental.mode, pdf::PDFSaveMode::IncrementalAppend);
    state.lastSaveMode = incremental.mode;
    const auto saved = artifacts.importBytes(QByteArray("lifecycle-source-v1-incremental"),
                                             { QStringLiteral("application/pdf"), QStringLiteral("edited.pdf") });
    QVERIFY(saved.success);
    QVERIFY(history.registerArtifact(saved.artifact));
    QVERIFY(artifacts.verify(state.original));
    state.current = saved.artifact;
    QUuid savedId;
    QVERIFY(appendEvent(history, state.original, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &savedId, &state, saved.artifact));
    state.lastAcceptedExecution = savedId;

    const pdf::PDFOperationSavePolicy saveAs = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("export"));
    QCOMPARE(saveAs.mode, pdf::PDFSaveMode::SaveAsNewArtifact);
    state.lastSaveMode = saveAs.mode;

    const quint64 revisionBeforeRollback = context.getRevision().documentRevision;
    QUuid rollbackId;
    QVERIFY(appendEvent(history, state.current, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::RolledBack, &rollbackId, &state, state.current));
    context.markModified(pdf::PDFModifiedDocument::PageContents);
    QVERIFY(context.getRevision().documentRevision > revisionBeforeRollback);

    state.recovered = true;
    state.certified = false;
    QVERIFY(artifacts.verify(state.original));
    QCOMPARE(invariantFailure(state, artifacts, history), QString());
}

void LifecycleTest::injectedStaleResultIsCaught()
{
    pdf::PDFJobScheduler scheduler(1);
    scheduler.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("revision-2"));
    std::atomic_bool ran = false;
    pdf::PDFJobSpec spec;
    spec.documentKey = QStringLiteral("doc");
    spec.documentRevision = QStringLiteral("revision-1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    const QString jobId = scheduler.submit(spec, [&ran](pdf::PDFJobContext&)
                                           { ran = true; });
    QVERIFY(scheduler.waitForFinished(jobId, 1000));
    QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Stale);
    QVERIFY(!ran.load(std::memory_order_acquire));

    LifecycleState state;
    state.acceptedStale = true;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("stale-result-accepted"));
}

void LifecycleTest::injectedOverwriteIsCaught()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto imported = artifacts.importBytes("protected-source", { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    QVERIFY(imported.success);
    QVERIFY(artifacts.verify(imported.artifact));

    QFile file(artifacts.pathFor(imported.artifact));
    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("overwrite") > 0);
    file.close();

    LifecycleState state;
    state.original = imported.artifact;
    state.sourceDigest = imported.artifact.sha256;
    state.open = true;
    state.sourceOverwritten = !artifacts.verify(imported.artifact);
    QVERIFY(state.sourceOverwritten);

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("source-overwritten"));
}

void LifecycleTest::injectedRollbackHistoryDefectIsCaught()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto first = artifacts.importBytes("one", { QStringLiteral("application/pdf"), QStringLiteral("one.pdf") });
    const auto second = artifacts.importBytes("two", { QStringLiteral("application/pdf"), QStringLiteral("two.pdf") });
    QVERIFY(first.success);
    QVERIFY(second.success);

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QVERIFY(history.open());
    QVERIFY(history.registerArtifact(first.artifact));
    QVERIFY(history.registerArtifact(second.artifact));

    LifecycleState state;
    QUuid firstId;
    QUuid secondId;
    QVERIFY(appendEvent(history, first.artifact, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &firstId, &state, first.artifact));
    QVERIFY(appendEvent(history, second.artifact, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &secondId, &state, second.artifact));
    const int before = history.events().size();

    const QString connectionName = QStringLiteral("lifecycle-history-mutate");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("DELETE FROM history_events WHERE sequence = 1")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY(history.events().size() < before);
    state.historyMutated = true;
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("rollback-history-mutated"));
}

QTEST_GUILESS_MAIN(LifecycleTest)
#include "tst_lifecycletest.moc"
