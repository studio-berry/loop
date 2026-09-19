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
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfimage.h"
#include "pdfimageoptimizer.h"
#include "pdfjobscheduler.h"
#include "pdfobjectselector.h"
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

QString revisionDigestForDocument(const pdf::PDFDocument& document)
{
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    return QString::fromLatin1(QCryptographicHash::hash(revision.toString().toUtf8(), QCryptographicHash::Sha256).toHex());
}

pdf::PDFObjectReference addImageObject(pdf::PDFDocumentBuilder& builder, int pixels)
{
    QImage image(pixels, pixels, QImage::Format_RGB32);
    for (int y = 0; y < pixels; ++y)
    {
        for (int x = 0; x < pixels; ++x)
        {
            image.setPixel(x, y, qRgb((x * 17 + y * 13) % 256, (x * 7 + y * 29) % 256, (x * 31 + y * 3) % 256));
        }
    }
    pdf::PDFImage::ImageEncodeOptions options;
    options.compression = pdf::PDFImage::ImageCompression::Flate;
    options.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    options.alphaHandling = pdf::PDFImage::AlphaHandling::FlattenToWhite;
    pdf::PDFStream imageStream = pdf::PDFImage::createStreamFromImage(image, options);
    return builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(imageStream))));
}

pdf::PDFDocument createTwoPageDistinctImageDocument(int pixels)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    for (int page = 0; page < 2; ++page)
    {
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
        const pdf::PDFObjectReference imageReference = addImageObject(builder, pixels);
        const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
        pdf::PDFDictionary contentDictionary;
        contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                                   pdf::PDFObject::createInteger(pageContent.size()));
        const pdf::PDFObjectReference contentReference = builder.addObject(
            pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
                pdf::PDFStream(std::move(contentDictionary), QByteArray(pageContent)))));

        pdf::PDFDictionary xObject;
        xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
        pdf::PDFDictionary resources;
        resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
        pdf::PDFDictionary pageUpdate;
        pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
        pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
        builder.mergeTo(pageReference,
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    }
    return builder.build();
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
    void confirmedPlanDigestIsRequiredForSuccessfulExecution();
    void executeRequiresPostflightProfile();
    void addBleedRepairRejectsUnknownMode();
    void stepPreflightIsScopedToOperationImpact();
    void dryRunDoesNotRequirePreflightProfile();
    void selectStepValidatesAndPlansWithScopedSelection();
    void selectScopedExecuteFailsClosedOnScopeViolation();
    void selectExecuteFailsClosedWhenRevisionDigestStaleAtExecute();
    void selectExecuteFailsClosedWhenRevisionDigestStaleWithFrozenRevision();
    void rejectsNonObjectSelectValue();
    void rejectsMalformedStepInput();
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
    QVERIFY2(adapterOutcome->ok,
             qPrintable(QString::fromUtf8(QJsonDocument(adapterOutcome->executionResult.toJson()).toJson(QJsonDocument::Compact))));
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
    const QString cliStdoutPath = tempDir.filePath(QStringLiteral("cli.stdout"));
    const QString cliStderrPath = tempDir.filePath(QStringLiteral("cli.stderr"));
    process.setStandardOutputFile(cliStdoutPath);
    process.setStandardErrorFile(cliStderrPath);
    process.start(pdfTool,
                  { QStringLiteral("action-list"), QStringLiteral("run"), recipePath, inputPath,
                    QStringLiteral("--param"), QStringLiteral("bleed=3"), QStringLiteral("--output"), outputPath,
                    QStringLiteral("--profile"), defaultPreflightProfilePath(),
                    QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,
             qPrintable(QStringLiteral("PdfTool action-list failed with exit %1.").arg(process.exitCode())));
    QFile cliStdoutFile(cliStdoutPath);
    QVERIFY2(cliStdoutFile.open(QIODevice::ReadOnly), qPrintable(cliStdoutFile.errorString()));
    const QByteArray cliOutputBytes = cliStdoutFile.readAll();
    QFile cliStderrFile(cliStderrPath);
    QVERIFY2(cliStderrFile.open(QIODevice::ReadOnly), qPrintable(cliStderrFile.errorString()));
    const QByteArray cliStderr = cliStderrFile.readAll();
    const QJsonDocument cliOutput = QJsonDocument::fromJson(cliOutputBytes);
    QVERIFY2(cliOutput.isObject(), qPrintable(QStringLiteral("PdfTool returned non-JSON output. stdout=%1 stderr=%2 error=%3 input=%4 recipe=%5 output=%6")
                                                  .arg(QString::fromLocal8Bit(cliOutputBytes),
                                                       QString::fromLocal8Bit(cliStderr),
                                                       process.errorString(),
                                                       inputPath,
                                                       recipePath,
                                                       outputPath)));
    const QJsonObject cliData = cliOutput.object().value(QStringLiteral("data")).toObject();
    QCOMPARE(cliData.value(QStringLiteral("recipe_hash")).toString(), cliResult.recipeHash);
    const QString cliOutputHash = cliData.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString();
    QVERIFY(!cliOutputHash.isEmpty());

    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly));
    const QByteArray publishedBytes = outputFile.readAll();
    QCOMPARE(cliOutputHash,
             QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(cliData.value(QStringLiteral("recipe_hash")).toString(), cliResult.recipeHash);
    QCOMPARE(cliData.value(QStringLiteral("plan_digest")).toString(), adapterOutcome->executionResult.planDigest);
    QCOMPARE(cliData.value(QStringLiteral("source_sha256")).toString(), adapterOutcome->executionResult.sourceSha256);

    const QJsonObject governed = cliData.value(QStringLiteral("governed")).toObject();
    const QJsonObject revalidation = governed.value(QStringLiteral("revalidation")).toObject();
    const QJsonObject signOff = governed.value(QStringLiteral("sign_off")).toObject();
    QVERIFY(revalidation.value(QStringLiteral("bytes_verified")).toBool());
    QVERIFY(revalidation.value(QStringLiteral("sign_off_eligible")).toBool());
    QCOMPARE(signOff.value(QStringLiteral("plan_digest")).toString(), cliData.value(QStringLiteral("plan_digest")).toString());
    QCOMPARE(signOff.value(QStringLiteral("source_sha256")).toString(), cliData.value(QStringLiteral("source_sha256")).toString());
    QCOMPARE(signOff.value(QStringLiteral("candidate_sha256")).toString(), cliOutputHash);
    QCOMPARE(signOff.value(QStringLiteral("published_sha256")).toString(), cliOutputHash);
    QCOMPARE(adapterOutcome->executionResult.governed.value(QStringLiteral("sign_off")).toObject().value(QStringLiteral("plan_digest")).toString(),
             adapterOutcome->executionResult.planDigest);
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
    plan.planDigest = QStringLiteral("confirmed-plan-digest");
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

void ActionListTest::confirmedPlanDigestIsRequiredForSuccessfulExecution()
{
    pdfinteraction::ActionListController controller;
    const auto begin = [&](pdfinteraction::ActionListController::State phase, const QString& jobId)
    {
        controller.beginRun(phase, QStringLiteral("document"), QStringLiteral("revision"),
                            QStringLiteral("recipe"), QStringLiteral("recipe-hash"),
                            QStringLiteral("profile-and-bindings-hash"), jobId);
    };
    begin(pdfinteraction::ActionListController::State::Validating, QStringLiteral("validation"));
    QVERIFY(controller.acceptValidation(QStringLiteral("validation"), QStringLiteral("revision"), {}, {}));

    begin(pdfinteraction::ActionListController::State::Planning, QStringLiteral("planning"));
    pdf::PDFActionListExecutionResult plan;
    plan.status = QStringLiteral("planned");
    plan.planDigest = QStringLiteral("confirmed-plan");
    QVERIFY(controller.acceptPlan(QStringLiteral("planning"), QStringLiteral("revision"), plan));
    QVERIFY(controller.planMatches(QStringLiteral("document"), QStringLiteral("revision"),
                                   QStringLiteral("recipe-hash"), QStringLiteral("profile-and-bindings-hash")));
    QVERIFY(!controller.planMatches(QStringLiteral("document"), QStringLiteral("revision"),
                                    QStringLiteral("recipe-hash"), QStringLiteral("changed-profile-and-bindings")));

    begin(pdfinteraction::ActionListController::State::Running, QStringLiteral("execution"));
    pdf::PDFActionListExecutionResult executed;
    executed.status = QStringLiteral("succeeded");
    executed.planDigest = QStringLiteral("changed-plan");
    QVERIFY(!controller.acceptExecution(QStringLiteral("execution"), QStringLiteral("revision"), executed));
    QCOMPARE(controller.state(), pdfinteraction::ActionListController::State::Failed);
    QCOMPARE(controller.result().status, QStringLiteral("failed"));
    QVERIFY(controller.operatorSummary().contains(QStringLiteral("confirmed plan")));

    controller.clear();
    begin(pdfinteraction::ActionListController::State::Validating, QStringLiteral("validation-2"));
    QVERIFY(controller.acceptValidation(QStringLiteral("validation-2"), QStringLiteral("revision"), {}, {}));
    begin(pdfinteraction::ActionListController::State::Planning, QStringLiteral("planning-2"));
    QVERIFY(controller.acceptPlan(QStringLiteral("planning-2"), QStringLiteral("revision"), plan));
    begin(pdfinteraction::ActionListController::State::Running, QStringLiteral("execution-2"));
    executed.planDigest = plan.planDigest;
    QVERIFY(controller.acceptExecution(QStringLiteral("execution-2"), QStringLiteral("revision"), executed));
    QCOMPARE(controller.state(), pdfinteraction::ActionListController::State::Succeeded);
}

void ActionListTest::selectStepValidatesAndPlansWithScopedSelection()
{
    const pdf::PDFDocument source = createTwoPageDistinctImageDocument(600);
    const QJsonObject recipeJson{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
        { QStringLiteral("id"), QStringLiteral("select-plan") },
        { QStringLiteral("name"), QStringLiteral("Select plan") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("downsample") },
                                       { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 150 } } },
                                       { QStringLiteral("select"), QJsonObject{
                                                                       { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                       { QStringLiteral("predicate"), QJsonObject{
                                                                                                          { QStringLiteral("and"), QJsonArray{
                                                                                                                                       QJsonObject{ { QStringLiteral("pages"), QStringLiteral("1") } },
                                                                                                                                       QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } } } } } } } } }
    };
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(recipeJson, &actionList));
    pdf::PDFActionListExecutionOptions options;
    options.revision = pdf::revisionIdentityForDocument(source);
    QVERIFY(pdf::PDFActionListExecutor().validate(actionList, options));

    pdf::PDFActionListExecutionResult planResult;
    QVERIFY(pdf::PDFActionListExecutor().plan(actionList, source, options, &planResult));
    QCOMPARE(planResult.status, QStringLiteral("planned"));
    QCOMPARE(planResult.steps.size(), 1);
    QVERIFY(!planResult.steps.front().selectionScope.value(QStringLiteral("empty")).toBool());
    QCOMPARE(planResult.steps.front().selectionScope.value(QStringLiteral("count")).toInt(), 1);
}

void ActionListTest::selectScopedExecuteFailsClosedOnScopeViolation()
{
    const pdf::PDFDocument source = createTwoPageDistinctImageDocument(600);
    const std::vector<pdf::PDFImageOptimizer::ImageInfo> sourceInfos = pdf::PDFImageOptimizer::collectImageInfos(&source);
    QCOMPARE(sourceInfos.size(), size_t(2));

    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                             { QStringLiteral("id"), QStringLiteral("select-fence") },
                                             { QStringLiteral("name"), QStringLiteral("Select fence") },
                                             { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                            { QStringLiteral("id"), QStringLiteral("downsample") },
                                                                            { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                                                            { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 72 } } },
                                                                            { QStringLiteral("select"), QJsonObject{
                                                                                                            { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                                                            { QStringLiteral("predicate"), QJsonObject{
                                                                                                                                               { QStringLiteral("and"), QJsonArray{
                                                                                                                                                                            QJsonObject{ { QStringLiteral("pages"), QStringLiteral("1") } },
                                                                                                                                                                            QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } } } } } } } } } },
                                         &actionList));

    pdf::PDFActionListExecutionOptions options;
    options.revision = pdf::revisionIdentityForDocument(source);
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    QVERIFY(!pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("failed"));
    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Failed);
    bool sawScopeViolation = false;
    for (const QJsonValue& value : result.steps.front().diagnostics)
    {
        if (value.toObject().value(QStringLiteral("code")).toString() == QStringLiteral("action-list.select-scope-violation"))
        {
            sawScopeViolation = true;
            break;
        }
    }
    QVERIFY(sawScopeViolation);
    QCOMPARE(pdf::PDFImageOptimizer::collectImageInfos(&source).front().pixelSize.width(), sourceInfos.front().pixelSize.width());
    QCOMPARE(pdf::PDFImageOptimizer::collectImageInfos(&source).back().pixelSize.width(), sourceInfos.back().pixelSize.width());
}

void ActionListTest::selectExecuteFailsClosedWhenRevisionDigestStaleAtExecute()
{
    const pdf::PDFDocument source = createTwoPageDistinctImageDocument(600);
    const QString originalDigest = revisionDigestForDocument(source);

    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                             { QStringLiteral("id"), QStringLiteral("select-stale") },
                                             { QStringLiteral("name"), QStringLiteral("Select stale") },
                                             { QStringLiteral("steps"), QJsonArray{
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } },
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("downsample") },
                                                                                { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 72 } } },
                                                                                { QStringLiteral("select"), QJsonObject{
                                                                                                                { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                                                                { QStringLiteral("revisionDigest"), originalDigest },
                                                                                                                { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } } } } } } },
                                         &actionList));

    pdf::PDFActionListExecutionResult planResult;
    QVERIFY(pdf::PDFActionListExecutor().plan(actionList, source, {}, &planResult));
    QCOMPARE(planResult.status, QStringLiteral("planned"));
    QCOMPARE(planResult.steps.size(), 2);

    pdf::PDFActionListExecutionResult executeResult;
    pdf::PDFDocument candidate;
    QVERIFY(!pdf::PDFActionListExecutor().execute(actionList, source, {}, &candidate, &executeResult));
    QCOMPARE(executeResult.status, QStringLiteral("failed"));
    QCOMPARE(executeResult.steps.size(), 2);
    QCOMPARE(executeResult.steps.front().status, pdf::PDFActionListStepStatus::Succeeded);
    QCOMPARE(executeResult.steps.back().status, pdf::PDFActionListStepStatus::Failed);
    bool sawExecuteStale = false;
    for (const QJsonValue& value : executeResult.steps.back().diagnostics)
    {
        const QJsonObject diagnostic = value.toObject();
        if (diagnostic.value(QStringLiteral("code")).toString() == QStringLiteral("action-list.select-stale-at-execute"))
        {
            sawExecuteStale = true;
            break;
        }
    }
    QVERIFY(sawExecuteStale);
    QCOMPARE(source.getCatalog()->getPageCount(), pdf::PDFInteger(2));
}

void ActionListTest::rejectsNonObjectSelectValue()
{
    const QJsonObject recipeJson{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
        { QStringLiteral("id"), QStringLiteral("bad-select") },
        { QStringLiteral("name"), QStringLiteral("Bad select") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("downsample") },
                                       { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 150 } } },
                                       { QStringLiteral("select"), QJsonArray{} } } } }
    };
    pdf::PDFActionList actionList;
    QVERIFY(!pdf::PDFActionList::fromJson(recipeJson, &actionList));
}

void ActionListTest::rejectsMalformedStepInput()
{
    const QJsonObject validStep{
        { QStringLiteral("id"), QStringLiteral("repair") },
        { QStringLiteral("operation"), QStringLiteral("add-bleed") },
        { QStringLiteral("params"), QJsonObject() }
    };
    const auto rejectStep = [&](const QJsonObject& step, const QString& field)
    {
        const QJsonObject recipe{
            { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
            { QStringLiteral("id"), QStringLiteral("invalid-step") },
            { QStringLiteral("name"), QStringLiteral("Invalid step") },
            { QStringLiteral("steps"), QJsonArray{ step } }
        };
        pdf::PDFActionList parsed;
        parsed.id = QStringLiteral("unchanged");
        const pdf::PDFOperationResult outcome = pdf::PDFActionList::fromJson(recipe, &parsed);
        QVERIFY(!outcome);
        QVERIFY2(outcome.getErrorMessage().contains(field), qPrintable(outcome.getErrorMessage()));
        QCOMPARE(parsed.id, QStringLiteral("unchanged"));
    };

    QJsonObject step = validStep;
    step.insert(QStringLiteral("params"), QJsonArray());
    rejectStep(step, QStringLiteral("params"));
    step.insert(QStringLiteral("params"), QJsonValue(QJsonValue::Null));
    rejectStep(step, QStringLiteral("params"));
    step.remove(QStringLiteral("params"));
    rejectStep(step, QStringLiteral("params"));

    step = validStep;
    step.insert(QStringLiteral("when"), QStringLiteral("not a condition"));
    rejectStep(step, QStringLiteral("when"));
    step.insert(QStringLiteral("when"), QJsonValue(QJsonValue::Null));
    rejectStep(step, QStringLiteral("when"));

    step.insert(QStringLiteral("when"), QJsonObject{
                                            { QStringLiteral("previousStepStatus"), QStringLiteral("succeeded") } });
    rejectStep(step, QStringLiteral("when.previousStepStatus"));
    step.insert(QStringLiteral("when"), QJsonObject{
                                            { QStringLiteral("previousStepStatus"), QJsonValue(QJsonValue::Null) } });
    rejectStep(step, QStringLiteral("when.previousStepStatus"));

    pdf::PDFActionList parsed;
    QVERIFY(!pdf::PDFActionList::fromJson(QJsonObject{
                                              { QStringLiteral("steps"), QStringLiteral("not an array") } },
                                          &parsed));
    QVERIFY(!pdf::PDFActionList::fromJson(QJsonObject{
                                              { QStringLiteral("steps"), QJsonArray{ QStringLiteral("not a step object") } } },
                                          &parsed));
}

void ActionListTest::selectExecuteFailsClosedWhenRevisionDigestStaleWithFrozenRevision()
{
    const pdf::PDFDocument source = createTwoPageDistinctImageDocument(600);
    const QString originalDigest = revisionDigestForDocument(source);

    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                             { QStringLiteral("id"), QStringLiteral("select-stale-frozen") },
                                             { QStringLiteral("name"), QStringLiteral("Select stale frozen") },
                                             { QStringLiteral("steps"), QJsonArray{
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("bleed") },
                                                                                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } },
                                                                            QJsonObject{
                                                                                { QStringLiteral("id"), QStringLiteral("downsample") },
                                                                                { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                                                                { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 72 } } },
                                                                                { QStringLiteral("select"), QJsonObject{
                                                                                                                { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                                                                { QStringLiteral("revisionDigest"), originalDigest },
                                                                                                                { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } } } } } } },
                                         &actionList));

    const pdf::PDFActionListExecutionOptions options = pdf::makeActionListExecutionOptions(source);
    pdf::PDFActionListExecutionResult executeResult;
    pdf::PDFDocument candidate;
    QVERIFY(!pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &executeResult));
    QCOMPARE(executeResult.status, QStringLiteral("failed"));
    QCOMPARE(executeResult.steps.back().status, pdf::PDFActionListStepStatus::Failed);
    bool sawExecuteStale = false;
    for (const QJsonValue& value : executeResult.steps.back().diagnostics)
    {
        if (value.toObject().value(QStringLiteral("code")).toString() == QStringLiteral("action-list.select-stale-at-execute"))
        {
            sawExecuteStale = true;
            break;
        }
    }
    QVERIFY(sawExecuteStale);
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
