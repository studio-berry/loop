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
#include "pdfactionlist.h"
#include "pdfdocumentbuilder.h"
#include "pdfjobscheduler.h"
#include "pdfrepairdiff.h"

#include <QCryptographicHash>
#include <QJsonDocument>
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

pdf::PDFActionList bleedRecipe(const QString& id)
{
    pdf::PDFActionList actionList;
    const pdf::PDFOperationResult parsed = pdf::PDFActionList::fromJson(QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), id },
        { QStringLiteral("steps"), QJsonArray{QJsonObject{
            { QStringLiteral("id"), QStringLiteral("bleed") },
            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
            { QStringLiteral("params"), QJsonObject{{QStringLiteral("bleed_mm"), 3.0}, {QStringLiteral("force"), true}}}
        }} }
    }, &actionList);
    Q_ASSERT(parsed);
    return actionList;
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
                    { QStringLiteral("force"), true }
                } }
            }
        } }
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
                { QStringLiteral("params"), QJsonObject() }
            },
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("two") },
                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                { QStringLiteral("params"), QJsonObject{{QStringLiteral("force"), QStringLiteral("yes")}} }
            }
        } }
    }, &actionList));
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
        { QStringLiteral("steps"), QJsonArray{QJsonObject{
            { QStringLiteral("id"), QStringLiteral("bleed") },
            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
            { QStringLiteral("params"), QJsonObject{{QStringLiteral("bleed_mm"), 3.0}, {QStringLiteral("force"), true}}}
        }} }
    }, &actionList));
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
        { QStringLiteral("steps"), QJsonArray{QJsonObject{
            { QStringLiteral("id"), QStringLiteral("bleed") },
            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
            { QStringLiteral("params"), QJsonObject{{QStringLiteral("bleed_mm"), 3.0}, {QStringLiteral("force"), true}}}
        }} }
    }, &actionList));
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, {}, &candidate, &result));
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
                                                executeOutcome)(local.context);
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
    const pdf::PDFActionList actionList = bleedRecipe(QStringLiteral("parity"));

    pdf::PDFActionListExecutionResult cliResult;
    pdf::PDFDocument cliCandidate;
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, {}, &cliCandidate, &cliResult));

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
                                                QJsonObject(),
                                                adapterOutcome)(local.context);
    }
    QVERIFY(adapterOutcome->ok);
    QCOMPARE(adapterOutcome->executionResult.recipeHash, cliResult.recipeHash);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString outputPath = tempDir.filePath(QStringLiteral("parity.pdf"));
    QByteArray cliData;
    QByteArray adapterData;
    pdf::PDFDocument reopenedCli;
    pdf::PDFDocument reopenedAdapter;
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        cliCandidate, [](pdf::PDFDocument*) { return pdf::PDFOperationResult(true); }, outputPath, &reopenedCli, &cliData));
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        *adapterOutcome->candidate, [](pdf::PDFDocument*) { return pdf::PDFOperationResult(true); },
        tempDir.filePath(QStringLiteral("adapter.pdf")), &reopenedAdapter, &adapterData));
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(cliData, QCryptographicHash::Sha256).toHex()),
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
                { QStringLiteral("params"), QJsonObject() }
            },
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("two") },
                { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                { QStringLiteral("params"), QJsonObject{{QStringLiteral("force"), QStringLiteral("yes")}} }
            }
        } }
    }, &actionList));

    QStringList errors;
    QVERIFY(!pdf::PDFActionListExecutor().validate(actionList, {}, &errors));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("step 'one'")));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("step.two.params")));
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
        { QStringLiteral("steps"), QJsonArray{QJsonObject{
            { QStringLiteral("id"), QStringLiteral("bleed") },
            { QStringLiteral("operation"), QStringLiteral("add-bleed") },
            { QStringLiteral("params"), QJsonObject{{QStringLiteral("bleed_mm"), 3.0}, {QStringLiteral("force"), true}}}
        }} }
    }, &actionList));

    CancelActionListControl control;
    pdf::PDFActionListExecutionOptions options;
    options.operationControl = &control;
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    QVERIFY(!pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("cancelled"));
    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Cancelled);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
    QVERIFY(candidate == pdf::PDFDocument());
}

QTEST_GUILESS_MAIN(ActionListTest)

#include "tst_actionlisttest.moc"
