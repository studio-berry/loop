// MIT License
#include "quickdocumentmodel.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsearch.h"
#include "pdfdocumentsession.h"
#include "pdfprocessingbudget.h"

#include <QSignalSpy>
#include <QtTest>

class QuickDocumentModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyModelIsSafe();
    void searchResultsExposeOnlyValueRoles();
    void searchesUseAnIndependentProcessingBudget();
    void exhaustedSearchReturnsAnIncompleteResult();
    void documentCapabilitiesAreValueState();
    void lifecycleStateTracksOutputAndErrors();
    void annotationRowsCarryReviewFacts();
};

void QuickDocumentModelTest::emptyModelIsSafe()
{
    QuickDocumentModel model;

    QCOMPARE(model.pages()->rowCount(), 0);
    QCOMPARE(model.outline()->rowCount(), 0);
    QCOMPARE(model.searchResults()->rowCount(), 0);
    QVERIFY(!model.hasOutline());
    QVERIFY(!model.hasAttachments());
    QVERIFY(!model.hasOptionalContent());
    QVERIFY(!model.search(QStringLiteral("text")));
}

void QuickDocumentModelTest::searchResultsExposeOnlyValueRoles()
{
    QuickSearchResultModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    model.replace({ { 3, QStringLiteral("match"), QStringLiteral("before match after") } },
                  QStringLiteral("match"), QStringLiteral("revision"));

    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(model.rowCount(), 1);
    const QModelIndex index = model.index(0, 0);
    QCOMPARE(model.data(index, QuickSearchResultModel::PageRole).toInt(), 3);
    QCOMPARE(model.data(index, QuickSearchResultModel::MatchedRole).toString(), QStringLiteral("match"));
    QCOMPARE(model.data(index, QuickSearchResultModel::ContextRole).toString(), QStringLiteral("before match after"));
    QCOMPARE(model.query(), QStringLiteral("match"));
    QCOMPARE(model.revision(), QStringLiteral("revision"));
}

void QuickDocumentModelTest::searchesUseAnIndependentProcessingBudget()
{
    // #652 decision: searchDocumentText builds an operation-scoped budget from
    // the session limits and must not share the session counters. Content
    // "q\nQ\n" lexes as three tokens and therefore charges three render
    // operations (empty pages charge zero and cannot falsify session reuse).
    // Exhaust the session budget first; leave the shared limit high enough
    // that a fresh search budget still completes.
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 100, 100));
    QByteArray content("q\nQ\n");
    pdf::PDFDictionary streamDictionary;
    streamDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"),
                              pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference streamReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            std::move(streamDictionary), std::move(content))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(streamReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(
                        std::move(pageUpdate))));

    pdf::PDFDocumentContext context(pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build())));
    pdf::PDFProcessingLimits limits = context.getSession()->getProcessingLimits();
    limits.maxRenderOperations = 8;
    context.getSession()->setProcessingLimits(limits);
    context.getSession()->getProcessingBudget()->chargeRenderOperation(8, QStringLiteral("previous search"));

    const pdf::PDFDocumentSearchResult first = pdf::searchDocumentText(&context, QStringLiteral("absent"));
    const pdf::PDFDocumentSearchResult second = pdf::searchDocumentText(&context, QStringLiteral("absent"));
    QVERIFY(first.completed);
    QVERIFY(first.admitted);
    QVERIFY(!first.budgetExceeded);
    QVERIFY(second.completed);
    QVERIFY(second.admitted);
    QVERIFY(!second.budgetExceeded);
}

void QuickDocumentModelTest::exhaustedSearchReturnsAnIncompleteResult()
{
    // #652 decision: when the operation-scoped search budget is exhausted,
    // Core returns an incomplete, non-admitted result and surfaces the budget
    // kind name (render-operations) via PDFException::getMessage().
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 100, 100));
    QByteArray content("q\n");
    pdf::PDFDictionary streamDictionary;
    streamDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"),
                              pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference streamReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            std::move(streamDictionary), std::move(content))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(streamReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(
                        std::move(pageUpdate))));

    pdf::PDFDocumentContext context(pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build())));
    pdf::PDFProcessingLimits limits = context.getSession()->getProcessingLimits();
    limits.maxRenderOperations = 0;
    context.getSession()->setProcessingLimits(limits);

    const pdf::PDFDocumentSearchResult result = pdf::searchDocumentText(&context, QStringLiteral("absent"));
    QVERIFY(!result.completed);
    QVERIFY(!result.admitted);
    QVERIFY(result.budgetExceeded);
    QVERIFY(result.matches.isEmpty());
    QVERIFY(result.errorMessage.contains(QStringLiteral("render-operations")));
}

void QuickDocumentModelTest::documentCapabilitiesAreValueState()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocumentContext context(pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build())));
    QuickDocumentModel model;

    QSignalSpy changedSpy(&model, &QuickDocumentModel::changed);
    model.setDocument(&context);

    QVERIFY(changedSpy.count() > 0);
    QCOMPARE(model.pages()->rowCount(), 1);
    QVERIFY(!model.encrypted());
    QVERIFY(model.canPrint());
    QVERIFY(model.canHighResolutionPrint());
    QVERIFY(model.canCopy());
    QVERIFY(model.canModify());
    QVERIFY(model.canComment());
    QVERIFY(model.canFillForms());
    QVERIFY(model.canAssemble());
    QVERIFY(model.canAccessibility());
    QVERIFY(!model.hasForm());
    QVERIFY(!model.modified());
    QCOMPARE(model.revision(), context.getRevision().toString());
}

void QuickDocumentModelTest::lifecycleStateTracksOutputAndErrors()
{
    QuickDocumentModel model;
    QSignalSpy changedSpy(&model, &QuickDocumentModel::changed);

    model.setLifecycleState(QStringLiteral("ready"), true, false, QStringLiteral("pending"), {});
    QVERIFY(model.modified());
    QVERIFY(!model.stale());
    QVERIFY(model.outputPending());
    QVERIFY(!model.outputSaved());
    QCOMPARE(model.lifecycleState(), QStringLiteral("ready"));
    QCOMPARE(model.outputState(), QStringLiteral("pending"));

    model.setLifecycleState(QStringLiteral("ready"), false, false, QStringLiteral("saved"), {});
    QVERIFY(!model.modified());
    QVERIFY(!model.outputPending());
    QVERIFY(model.outputSaved());

    model.setLifecycleState(QStringLiteral("error"), false, false, QStringLiteral("none"),
                            QStringLiteral("document/load-failed"));
    QCOMPARE(model.lifecycleState(), QStringLiteral("error"));
    QCOMPARE(model.typedError(), QStringLiteral("document/load-failed"));
    QVERIFY(changedSpy.count() >= 3);
}

void QuickDocumentModelTest::annotationRowsCarryReviewFacts()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFDictionary annotation;
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Annot"));
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Text"));
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("Rect"),
                        pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                            pdf::PDFObject::createReal(24.0),
                            pdf::PDFObject::createReal(30.0),
                            pdf::PDFObject::createReal(48.0),
                            pdf::PDFObject::createReal(54.0) })));
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("T"), pdf::PDFObject::createString("Client"));
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createString("Please fix the bleed"));
    annotation.addEntry(pdf::PDFInplaceOrMemoryString("Subj"), pdf::PDFObject::createString("Comment"));
    const pdf::PDFObjectReference annotationReference = builder.addObject(
        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(annotation))));

    // A popup is the open state of a parent annotation, not a separate review row.
    pdf::PDFDictionary popup;
    popup.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Annot"));
    popup.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Popup"));
    popup.addEntry(pdf::PDFInplaceOrMemoryString("Rect"),
                   pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                       pdf::PDFObject::createReal(60.0),
                       pdf::PDFObject::createReal(60.0),
                       pdf::PDFObject::createReal(120.0),
                       pdf::PDFObject::createReal(120.0) })));
    const pdf::PDFObjectReference popupReference = builder.addObject(
        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(popup))));

    pdf::PDFDictionary pageAnnots;
    pageAnnots.addEntry(pdf::PDFInplaceOrMemoryString("Annots"),
                        pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::vector<pdf::PDFObject>{
                            pdf::PDFObject::createReference(annotationReference),
                            pdf::PDFObject::createReference(popupReference) })));
    builder.appendTo(pageReference,
                     pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageAnnots))));

    pdf::PDFDocumentContext context(pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build())));
    QuickDocumentModel model;
    model.setDocument(&context);

    QAbstractItemModel* annotations = model.annotations();
    QCOMPARE(model.annotationCount(), 1);

    const QModelIndex index = annotations->index(0, 0);
    QCOMPARE(annotations->data(index, QuickAnnotationModel::PageIndexRole).toInt(), 0);
    QCOMPARE(annotations->data(index, QuickAnnotationModel::PageNumberRole).toInt(), 1);
    QCOMPARE(annotations->data(index, QuickAnnotationModel::KindRole).toString(), QStringLiteral("text"));
    QCOMPARE(annotations->data(index, QuickAnnotationModel::TitleRole).toString(), QStringLiteral("Client"));
    QCOMPARE(annotations->data(index, QuickAnnotationModel::ContentsRole).toString(), QStringLiteral("Please fix the bleed"));
    QCOMPARE(annotations->data(index, QuickAnnotationModel::SubjectRole).toString(), QStringLiteral("Comment"));
    QCOMPARE(annotations->data(index, QuickAnnotationModel::BoundsRole).toRectF(), QRectF(24.0, 30.0, 24.0, 24.0));
    QVERIFY(!annotations->data(index, QuickAnnotationModel::HiddenRole).toBool());
    QVERIFY(!annotations->data(index, QuickAnnotationModel::SelectedRole).toBool());
    QVERIFY(!annotations->data(index, QuickAnnotationModel::ReferenceRole).toString().isEmpty());

    model.annotationModel().setSelectedRow(0);
    QVERIFY(annotations->data(index, QuickAnnotationModel::SelectedRole).toBool());

    model.clear();
    QCOMPARE(model.annotationCount(), 0);
}

QTEST_GUILESS_MAIN(QuickDocumentModelTest)
#include "tst_quickdocumentmodeltest.moc"
