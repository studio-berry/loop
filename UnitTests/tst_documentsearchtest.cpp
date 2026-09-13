// MIT License
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
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
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 400, 400));
    pdf::PDFPageContentStreamBuilder streamBuilder(&builder);
    if (QPainter* painter = streamBuilder.begin(page))
    {
        painter->setPen(Qt::black);
        painter->drawText(QPointF(50, 100), QStringLiteral("needle text"));
        streamBuilder.end(painter);
    }
    return builder.build();
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

    const pdf::PDFDocumentSearchResult result = pdf::searchDocumentText(context.get(), QStringLiteral("needle"));
    QVERIFY(result.admitted);
    QVERIFY(result.complete);
    QVERIFY(!result.budgetExceeded);
    QCOMPARE(result.matches.size(), 1);
    QCOMPARE(result.matches.first().matched, QStringLiteral("needle"));
}

void DocumentSearchTest::searchReportsIncompleteWhenOperationBudgetExhausted()
{
    std::unique_ptr<pdf::PDFDocumentContext> context(contextForDocument(searchableDocument()));
    pdf::PDFDocumentSession* session = context->getSession();
    QVERIFY(session);

    pdf::PDFProcessingLimits limits;
    limits.maxRenderOperations = 0;
    session->setProcessingLimits(limits);

    const pdf::PDFDocumentSearchResult result = pdf::searchDocumentText(context.get(), QStringLiteral("needle"));
    QVERIFY(result.admitted);
    QVERIFY(!result.complete);
    QVERIFY(result.budgetExceeded);
    QVERIFY(result.matches.isEmpty());
}

QTEST_MAIN(DocumentSearchTest)
#include "tst_documentsearchtest.moc"
