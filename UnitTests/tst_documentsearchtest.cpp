// MIT License
#include "pdfdocumentcontext.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsearch.h"
#include "pdfdocumentsession.h"
#include "pdfprocessingbudget.h"

#include <QThread>
#include <QtTest>

class DocumentSearchTest final : public QObject
{
    Q_OBJECT

private slots:
    void searchUsesFreshBudgetAfterSessionElapsed();
    void searchReportsIncompleteWhenOperationBudgetExhausted();
};

namespace
{

pdf::PDFDocument searchableDocument()
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    const QString fixturePath = QString(LOOP_PREFLIGHT_SOURCE_DIR) + QStringLiteral("/testdata/fixtures/font-embedded.pdf");
    return reader.readFromFile(fixturePath);
}

pdf::PDFDocumentContext* contextForDocument(pdf::PDFDocument document)
{
    return new pdf::PDFDocumentContext(pdf::PDFDocumentPointer(new pdf::PDFDocument(std::move(document))));
}

}   // namespace

void DocumentSearchTest::searchUsesFreshBudgetAfterSessionElapsed()
{
    std::unique_ptr<pdf::PDFDocumentContext> context(contextForDocument(searchableDocument()));
    pdf::PDFDocumentSession* session = context->getSession();
    QVERIFY(session);

    pdf::PDFProcessingLimits limits;
    limits.maxElapsed = std::chrono::milliseconds(1);
    session->setProcessingLimits(limits);
    QThread::msleep(5);

    bool sessionElapsedFailed = false;
    try
    {
        session->getProcessingBudget()->checkElapsed(QStringLiteral("session idle"));
    }
    catch (const pdf::PDFBudgetExceededException&)
    {
        sessionElapsedFailed = true;
    }
    QVERIFY(sessionElapsedFailed);

    const pdf::PDFDocumentSearchResult result = pdf::searchDocumentText(context.get(), QStringLiteral("embedded"));
    QVERIFY(result.admitted);
    QVERIFY(result.completed);
    QVERIFY(!result.budgetExceeded);
    QCOMPARE(result.matches.size(), 1);
    QCOMPARE(result.matches.first().matched, QStringLiteral("embedded"));
}

void DocumentSearchTest::searchReportsIncompleteWhenOperationBudgetExhausted()
{
    std::unique_ptr<pdf::PDFDocumentContext> context(contextForDocument(searchableDocument()));
    pdf::PDFDocumentSession* session = context->getSession();
    QVERIFY(session);

    pdf::PDFProcessingLimits limits;
    limits.maxRenderOperations = 0;
    session->setProcessingLimits(limits);

    const pdf::PDFDocumentSearchResult result = pdf::searchDocumentText(context.get(), QStringLiteral("embedded"));
    // Core returns before the admission check when the budget stops the search, so
    // the result is neither admitted nor completed; QuickDocumentModelTest covers
    // the same contract from the Quick side.
    QVERIFY(!result.admitted);
    QVERIFY(!result.completed);
    QVERIFY(result.budgetExceeded);
    QVERIFY(result.matches.isEmpty());
}

QTEST_MAIN(DocumentSearchTest)
#include "tst_documentsearchtest.moc"
