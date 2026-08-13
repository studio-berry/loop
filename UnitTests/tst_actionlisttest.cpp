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

#include "pdfactionlist.h"
#include "pdfdocumentbuilder.h"

#include <QJsonDocument>
#include <QtTest>

class CancelActionListControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return true;
    }
};

class ActionListTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesAndRoundTripsRecipe();
    void rejectsUnknownOperationAndWrongParameterType();
    void dryRunDoesNotMutateSource();
    void executesRegisteredOperationOnCandidate();
    void cancellationLeavesSourceUntouched();
};

void ActionListTest::parsesAndRoundTripsRecipe()
{
    const QJsonObject json{
        { QStringLiteral("schema"), QStringLiteral("loupe-action-list/1") },
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
        { QStringLiteral("schema"), QStringLiteral("loupe-action-list/1") },
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
        { QStringLiteral("schema"), QStringLiteral("loupe-action-list/1") },
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
        { QStringLiteral("schema"), QStringLiteral("loupe-action-list/1") },
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

void ActionListTest::cancellationLeavesSourceUntouched()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loupe-action-list/1") },
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
