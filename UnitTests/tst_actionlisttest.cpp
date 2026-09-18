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

#include "actionlistrunsubmitter.h"
#include "actionlistcontroller.h"
#include "pdfactionlist.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfjobscheduler.h"
#include "pdfrepairdiff.h"

#include <QPainter>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class CancelActionListControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return true;
    }
};

namespace
{

const QString defaultPreflightProfilePath()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
}

pdf::PDFActionList bleedRecipe(const QString& id)
{
    pdf::PDFActionList actionList;
    const pdf::PDFOperationResult parsed = pdf::PDFActionList::fromJson(QJsonObject{
                                                                            { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                                                            { QStringLiteral("id"), id },
                                                                            { QStringLiteral("name"), id },
                                                                            { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                                                           { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                                                           { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                                                           { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } } },
                                                                        &actionList);
    Q_ASSERT(parsed);
    return actionList;
}

pdf::PDFDocument rgbDocumentMissingBleed()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF mediaBox(0, 0, 200, 200);
    const pdf::PDFObjectReference page = builder.appendPage(mediaBox);
    builder.setPageTrimBox(page, mediaBox.adjusted(20, 20, -20, -20));
    pdf::PDFPageContentStreamBuilder stream(&builder, pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = stream.begin(page))
    {
        painter->fillRect(mediaBox.adjusted(20, 20, -20, -20), Qt::red);
        stream.end(painter);
    }
    return builder.build();
}

}   // namespace

class ActionListTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesAndRoundTripsRecipe();
    void rejectsUnknownOperationAndWrongParameterType();
    void dryRunDoesNotMutateSource();
    void executesRegisteredOperationOnCandidate();
    void cancellationLeavesSourceUntouched();
    void adapterContractValidatePlanExecute();
    void cliParityRecipeHashAndOutputSha256();
    void surfacesPerStepValidationErrors();
    void controllerFencesValidationPlanAndStaleCompletion();
    void executeRequiresPostflightProfile();
    void addBleedRepairRejectsUnknownMode();
    void stepPreflightIsScopedToOperationImpact();
    void dryRunDoesNotRequirePreflightProfile();
};

void ActionListTest::parsesAndRoundTripsRecipe()
{
    const QJsonObject json{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
        { QStringLiteral("id"), QStringLiteral("press-ready") },
        { QStringLiteral("name"), QStringLiteral("Press ready") },
        { QStringLiteral("onFailure"), QStringLiteral("stop") },
        { QStringLiteral("steps"), QJsonArray{
                                       QJsonObject{
                                           { QStringLiteral("id"), QStringLiteral("bleed") },
                                           { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                           { QStringLiteral("params"), QJsonObject{
                                                                           { QStringLiteral("bleed_mm"), QStringLiteral("${job.bleed}") },
                                                                           { QStringLiteral("force"), true } } } } } }
    };
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(json, &actionList));
    QCOMPARE(actionList.id, QStringLiteral("press-ready"));
    QCOMPARE(actionList.steps.size(), 1);
    QCOMPARE(QJsonDocument(actionList.toJson()).toJson(QJsonDocument::Compact), QJsonDocument(json).toJson(QJsonDocument::Compact));
}

void ActionListTest::rejectsUnknownOperationAndWrongParameterType()
{
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                             { QStringLiteral("id"), QStringLiteral("bad") },
                                             { QStringLiteral("name"), QStringLiteral("Bad") },
                                             { QStringLiteral("steps"), QJsonArray{
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("one") },
                                                                                { QStringLiteral("operation"), QStringLiteral("missing") },
                                                                                { QStringLiteral("params"), QJsonObject() } },
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("two") },
                                                                                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("force"), QStringLiteral("yes") } } } } } } },
                                         &actionList));
    QStringList errors;
    QVERIFY(!pdf::PDFActionListExecutor().validate(actionList, {}, &errors));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("Unknown operation")));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("force must be a boolean")));
}

void ActionListTest::dryRunDoesNotMutateSource()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                             { QStringLiteral("id"), QStringLiteral("dry") },
                                             { QStringLiteral("name"), QStringLiteral("Dry") },
                                             { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                            { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                            { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } } },
                                         &actionList));
    pdf::PDFActionListExecutionOptions options;
    options.dryRun = true;
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("planned"));
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
    QVERIFY(candidate == pdf::PDFDocument());
}

void ActionListTest::executesRegisteredOperationOnCandidate()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                             { QStringLiteral("id"), QStringLiteral("execute") },
                                             { QStringLiteral("name"), QStringLiteral("Execute") },
                                             { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                            { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                            { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } } },
                                         &actionList));
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    pdf::PDFActionListExecutionOptions options;
    options.requirePostflight = false;
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("succeeded"));
    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Succeeded);
    QVERIFY(candidate != pdf::PDFDocument());
    QVERIFY(candidate.getCatalog()->getPage(0)->getMediaBox().width() > 100.0);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
}

void ActionListTest::adapterContractValidatePlanExecute()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const pdf::PDFDocumentPointer document(new pdf::PDFDocument(source));
    const pdf::PDFActionList actionList = bleedRecipe(QStringLiteral("adapter"));

    auto validateOutcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
    {
        struct LocalContext
        {
            pdf::PDFJobCancellationTokenPtr token = std::make_shared<pdf::PDFJobCancellationToken>();
            pdf::PDFJobContext context{ token, pdf::PDFProcessingLimits(), [](int) {} };
        } local;
        pdfinteraction::makeActionListRunWorker(pdfinteraction::ActionListRunPhase::Validate,
                                                actionList,
                                                document,
                                                QJsonObject(),
                                                validateOutcome)(local.context);
    }
    QVERIFY(validateOutcome->ok);
    QVERIFY(validateOutcome->validationErrors.isEmpty());

    auto planOutcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
    {
        struct LocalContext
        {
            pdf::PDFJobCancellationTokenPtr token = std::make_shared<pdf::PDFJobCancellationToken>();
            pdf::PDFJobContext context{ token, pdf::PDFProcessingLimits(), [](int) {} };
        } local;
        pdfinteraction::makeActionListRunWorker(pdfinteraction::ActionListRunPhase::Plan,
                                                actionList,
                                                document,
                                                QJsonObject(),
                                                planOutcome)(local.context);
    }
    QVERIFY(planOutcome->ok);
    QCOMPARE(planOutcome->executionResult.status, QStringLiteral("planned"));
    QVERIFY(!planOutcome->candidate);

    auto executeOutcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
    {
        struct LocalContext
        {
            pdf::PDFJobCancellationTokenPtr token = std::make_shared<pdf::PDFJobCancellationToken>();
            pdf::PDFJobContext context{ token, pdf::PDFProcessingLimits(), [](int) {} };
        } local;
        pdfinteraction::makeActionListRunWorker(pdfinteraction::ActionListRunPhase::Execute,
                                                actionList,
                                                document,
                                                QJsonObject(),
                                                executeOutcome,
                                                defaultPreflightProfilePath())(local.context);
    }
    QVERIFY(executeOutcome->ok);
    QCOMPARE(executeOutcome->executionResult.status, QStringLiteral("succeeded"));
    QVERIFY(executeOutcome->candidate);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
    QVERIFY(executeOutcome->candidate->getCatalog()->getPage(0)->getMediaBox().width() > 100.0);
}

void ActionListTest::cliParityRecipeHashAndOutputSha256()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const pdf::PDFDocumentPointer document(new pdf::PDFDocument(source));
    const QJsonObject recipeJson{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
        { QStringLiteral("id"), QStringLiteral("parity") },
        { QStringLiteral("name"), QStringLiteral("Parity") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("bleed") },
                                       { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), QStringLiteral("${job.bleed}") }, { QStringLiteral("force"), true } } } } } }
    };
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(recipeJson, &actionList));
    const QJsonObject bindings{ { QStringLiteral("bleed"), 3 } };

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString inputPath = tempDir.filePath(QStringLiteral("input.pdf"));
    const QString recipePath = tempDir.filePath(QStringLiteral("recipe.json"));
    const QString outputPath = tempDir.filePath(QStringLiteral("output.pdf"));
    QFile recipeFile(recipePath);
    QVERIFY(recipeFile.open(QIODevice::WriteOnly));
    QVERIFY(recipeFile.write(QJsonDocument(recipeJson).toJson(QJsonDocument::Indented)) > 0);
    recipeFile.close();
    pdf::PDFDocument reopenedInput;
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        source, [](pdf::PDFDocument*)
        { return pdf::PDFOperationResult(true); }, inputPath, &reopenedInput, nullptr));

    pdf::PDFActionListExecutionResult cliResult;
    pdf::PDFDocument cliCandidate;
    pdf::PDFActionListExecutionOptions adapterOptions;
    adapterOptions.bindings = bindings;
    adapterOptions.preflightProfilePath = defaultPreflightProfilePath();
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, adapterOptions, &cliCandidate, &cliResult));

    auto adapterOutcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
    {
        struct LocalContext
        {
            pdf::PDFJobCancellationTokenPtr token = std::make_shared<pdf::PDFJobCancellationToken>();
            pdf::PDFJobContext context{ token, pdf::PDFProcessingLimits(), [](int) {} };
        } local;
        pdfinteraction::makeActionListRunWorker(pdfinteraction::ActionListRunPhase::Execute,
                                                actionList,
                                                document,
                                                bindings,
                                                adapterOutcome,
                                                defaultPreflightProfilePath())(local.context);
    }
    QVERIFY(adapterOutcome->ok);
    QCOMPARE(adapterOutcome->executionResult.recipeHash, cliResult.recipeHash);

    QProcess process;
    const QString pdfTool = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("PdfTool.exe")
#else
        QStringLiteral("PdfTool")
#endif
    );
    QVERIFY2(QFileInfo::exists(pdfTool), qPrintable(QStringLiteral("PdfTool was not found at %1").arg(pdfTool)));
    process.start(pdfTool,
                  { QStringLiteral("action-list"), QStringLiteral("run"), recipePath, inputPath,
                    QStringLiteral("--param"), QStringLiteral("bleed=3"), QStringLiteral("--output"), outputPath,
                    QStringLiteral("--profile"), defaultPreflightProfilePath(),
                    QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    const QJsonDocument cliOutput = QJsonDocument::fromJson(process.readAllStandardOutput());
    QVERIFY2(cliOutput.isObject(), qPrintable(QString::fromLocal8Bit(process.readAllStandardError())));
    const QJsonObject cliData = cliOutput.object().value(QStringLiteral("data")).toObject();
    QCOMPARE(cliData.value(QStringLiteral("recipe_hash")).toString(), cliResult.recipeHash);
    const QString cliOutputHash = cliData.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString();
    QVERIFY(!cliOutputHash.isEmpty());

    QByteArray adapterData;
    pdf::PDFDocument reopenedAdapter;
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        *adapterOutcome->candidate, [](pdf::PDFDocument*)
        { return pdf::PDFOperationResult(true); },
        tempDir.filePath(QStringLiteral("adapter.pdf")), &reopenedAdapter, &adapterData));
    QCOMPARE(cliOutputHash,
             QString::fromLatin1(QCryptographicHash::hash(adapterData, QCryptographicHash::Sha256).toHex()));
}

void ActionListTest::surfacesPerStepValidationErrors()
{
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                             { QStringLiteral("id"), QStringLiteral("bad-steps") },
                                             { QStringLiteral("name"), QStringLiteral("Bad steps") },
                                             { QStringLiteral("steps"), QJsonArray{
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("one") },
                                                                                { QStringLiteral("operation"), QStringLiteral("missing") },
                                                                                { QStringLiteral("params"), QJsonObject() } },
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("two") },
                                                                                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("force"), QStringLiteral("yes") } } } } } } },
                                         &actionList));

    QStringList errors;
    QVERIFY(!pdf::PDFActionListExecutor().validate(actionList, {}, &errors));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("step 'one'")));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("step.two.params")));
}

void ActionListTest::controllerFencesValidationPlanAndStaleCompletion()
{
    pdfinteraction::ActionListController controller;
    controller.beginRun(pdfinteraction::ActionListController::State::Validating,
                        QStringLiteral("doc"),
                        QStringLiteral("rev-1"),
                        QStringLiteral("recipe"),
                        QStringLiteral("recipe-hash-a"),
                        QStringLiteral("bindings-hash-a"),
                        QStringLiteral("validate-job"));
    QVERIFY(controller.acceptValidation(QStringLiteral("validate-job"), QStringLiteral("rev-1"), {}, {}));
    QVERIFY(controller.validationReady());
    QVERIFY(controller.validationMatches(QStringLiteral("doc"), QStringLiteral("rev-1"),
                                         QStringLiteral("recipe-hash-a"), QStringLiteral("bindings-hash-a")));

    controller.beginRun(pdfinteraction::ActionListController::State::Planning,
                        QStringLiteral("doc"),
                        QStringLiteral("rev-1"),
                        QStringLiteral("recipe"),
                        QStringLiteral("recipe-hash-a"),
                        QStringLiteral("bindings-hash-a"),
                        QStringLiteral("plan-job"));
    pdf::PDFActionListExecutionResult plan;
    plan.recipeHash = QStringLiteral("recipe-hash-a");
    plan.status = QStringLiteral("planned");
    QVERIFY(controller.acceptPlan(QStringLiteral("plan-job"), QStringLiteral("rev-1"), plan));
    QVERIFY(controller.planMatches(QStringLiteral("doc"), QStringLiteral("rev-1"),
                                   QStringLiteral("recipe-hash-a"), QStringLiteral("bindings-hash-a")));
    QVERIFY(!controller.planMatches(QStringLiteral("doc"), QStringLiteral("rev-1"),
                                    QStringLiteral("recipe-hash-b"), QStringLiteral("bindings-hash-a")));

    controller.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QCOMPARE(controller.state(), pdfinteraction::ActionListController::State::Idle);
    QVERIFY(!controller.validationReady());

    controller.beginRun(pdfinteraction::ActionListController::State::Running,
                        QStringLiteral("doc"),
                        QStringLiteral("rev-2"),
                        QStringLiteral("recipe"),
                        QStringLiteral("recipe-hash-a"),
                        QStringLiteral("bindings-hash-a"),
                        QStringLiteral("run-job"));
    controller.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-3"));
    QCOMPARE(controller.state(), pdfinteraction::ActionListController::State::Idle);
    QVERIFY(!controller.validationReady());
}

void ActionListTest::cancellationLeavesSourceUntouched()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
                                             { QStringLiteral("id"), QStringLiteral("cancel") },
                                             { QStringLiteral("name"), QStringLiteral("Cancel") },
                                             { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                            { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                            { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } } },
                                         &actionList));

    CancelActionListControl control;
    pdf::PDFActionListExecutionOptions options;
    options.operationControl = &control;
    options.requirePostflight = false;
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    QVERIFY(!pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("cancelled"));
    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Cancelled);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
    QVERIFY(candidate == pdf::PDFDocument());
}

void ActionListTest::executeRequiresPostflightProfile()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const pdf::PDFActionList actionList = bleedRecipe(QStringLiteral("postflight-gate"));
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    const pdf::PDFOperationResult execution = pdf::PDFActionListExecutor().execute(actionList, source, {}, &candidate, &result);
    QVERIFY(!execution);
    QCOMPARE(result.status, QStringLiteral("failed"));
    QVERIFY(!result.postflight.isEmpty() || !result.diagnostics.isEmpty());
}

void ActionListTest::addBleedRepairRejectsUnknownMode()
{
    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed"));
    QVERIFY(operation);
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFRepairPlan plan;
    const pdf::PDFOperationResult analyze = operation->analyze(source,
                                                               QJsonObject{ { QStringLiteral("mode"), QStringLiteral("bogus") } },
                                                               &plan);
    QVERIFY(!analyze);
    QVERIFY(!plan.unsupportedReasons.isEmpty());
}

void ActionListTest::stepPreflightIsScopedToOperationImpact()
{
    const pdf::PDFDocument source = rgbDocumentMissingBleed();
    const pdf::PDFActionList actionList = bleedRecipe(QStringLiteral("scoped-step-postflight"));

    pdf::PDFActionListExecutionOptions options;
    options.preflightProfilePath = defaultPreflightProfilePath();
    options.requirePostflight = true;
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    const pdf::PDFOperationResult execution = pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result);

    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Succeeded);
    QVERIFY(!execution);
    QCOMPARE(result.status, QStringLiteral("failed"));
    QVERIFY(!result.postflight.isEmpty());
}

void ActionListTest::dryRunDoesNotRequirePreflightProfile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString inputPath = tempDir.filePath(QStringLiteral("input.pdf"));
    const QString recipePath = tempDir.filePath(QStringLiteral("recipe.json"));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFDocument reopenedInput;
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        source, [](pdf::PDFDocument*)
        { return pdf::PDFOperationResult(true); }, inputPath, &reopenedInput, nullptr));

    const QJsonObject recipeJson{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
        { QStringLiteral("id"), QStringLiteral("dry-run") },
        { QStringLiteral("name"), QStringLiteral("Dry run") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("bleed") },
                                       { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } }
    };
    QFile recipeFile(recipePath);
    QVERIFY(recipeFile.open(QIODevice::WriteOnly));
    QVERIFY(recipeFile.write(QJsonDocument(recipeJson).toJson(QJsonDocument::Indented)) > 0);
    recipeFile.close();

    QProcess process;
    const QString pdfTool = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("PdfTool.exe")
#else
        QStringLiteral("PdfTool")
#endif
    );
    QVERIFY2(QFileInfo::exists(pdfTool), qPrintable(QStringLiteral("PdfTool was not found at %1").arg(pdfTool)));
    process.start(pdfTool,
                  { QStringLiteral("action-list"), QStringLiteral("run"), recipePath, inputPath,
                    QStringLiteral("--dry-run"), QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
}

QTEST_GUILESS_MAIN(ActionListTest)

#include "tst_actionlisttest.moc"
