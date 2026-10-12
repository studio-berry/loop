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

// The direct authoring-edit writer of ADR-014 (issue #233): bookmark create,
// generate, replace and interchange round trips, plus one link annotation per
// destination variant. Every destination is verified by serializing the
// modified document and reading it back through Core's own parser, not by
// trusting the in-memory object the writer just built.

#include <QtTest>

#include <QBuffer>

#include <optional>

#include "pdfannotation.h"
#include "pdfauthoringedit.h"
#include "pdfcatalog.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfoutline.h"
#include "pdfpage.h"

namespace
{

pdf::PDFDocument buildDocument(int pageCount)
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 200, 300));
    }
    return builder.build();
}

/// Serializes and re-parses the document, so a read-back proves what the
/// written file says, not what the writer's memory happens to hold.
std::optional<pdf::PDFDocument> writeAndRead(const pdf::PDFDocument& document)
{
    pdf::PDFDocumentWriter writer(nullptr);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const pdf::PDFOperationResult written = writer.write(&buffer, &document);
    if (!written)
    {
        return std::nullopt;
    }
    buffer.close();

    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    return reader.readFromBuffer(buffer.data());
}

std::vector<pdf::PDFAnnotationPtr> annotationsOnPage(const pdf::PDFDocument& document, int pageIndex)
{
    std::vector<pdf::PDFAnnotationPtr> annotations;
    const pdf::PDFPage* page = document.getCatalog()->getPage(pageIndex);
    if (!page)
    {
        return annotations;
    }

    for (const pdf::PDFObjectReference& reference : page->getAnnotations())
    {
        annotations.push_back(pdf::PDFAnnotation::parse(&document.getStorage(), reference));
    }
    return annotations;
}

pdf::PDFAnnotationPtr firstLinkOnPage(const pdf::PDFDocument& document, int pageIndex)
{
    for (const pdf::PDFAnnotationPtr& annotation : annotationsOnPage(document, pageIndex))
    {
        if (dynamic_cast<const pdf::PDFLinkAnnotation*>(annotation.data()))
        {
            // Return the owning pointer: the annotations list is a temporary, so
            // a raw pointer into it dangles as soon as the loop ends (glibc's
            // freed-chunk metadata then clobbers the vtable and the next
            // dynamic_cast crashes; MSVC happened to survive).
            return annotation;
        }
    }
    return nullptr;
}

int destinationPageIndex(const pdf::PDFDocument& document, const pdf::PDFDestination& destination)
{
    if (!destination.getPageReference().isValid())
    {
        return -1;
    }

    const size_t index = document.getCatalog()->getPageIndexFromPageReference(destination.getPageReference());
    return index == pdf::PDFCatalog::INVALID_PAGE_INDEX ? -1 : static_cast<int>(index);
}

std::optional<QList<pdf::PDFBookmarkRecord>> outlineRecords(const pdf::PDFDocument& document)
{
    QString typedError;
    return pdf::PDFAuthoringEditWriter::readOutline(&document, typedError);
}

pdf::PDFAuthoringEdit bookmarkEdit(pdf::PDFAuthoringEdit::Kind kind)
{
    pdf::PDFAuthoringEdit edit;
    edit.kind = kind;
    return edit;
}

/// A document whose outline carries a structure element, which the builder's
/// outline writer cannot write back.
pdf::PDFDocument buildDocumentWithStructureOutline()
{
    const pdf::PDFDocument document = buildDocument(1);
    pdf::PDFDocumentModifier modifier(&document);
    pdf::PDFDocumentBuilder* builder = modifier.getBuilder();

    pdf::PDFObjectFactory itemFactory;
    itemFactory.beginDictionary();
    itemFactory.beginDictionaryItem("Title");
    itemFactory << QStringLiteral("Tagged");
    itemFactory.endDictionaryItem();
    itemFactory.beginDictionaryItem("SE");
    itemFactory << document.getCatalog()->getPage(0)->getPageReference();
    itemFactory.endDictionaryItem();
    itemFactory.endDictionary();
    const pdf::PDFObjectReference itemReference = builder->addObject(itemFactory.takeObject());

    pdf::PDFObjectFactory rootFactory;
    rootFactory.beginDictionary();
    rootFactory.beginDictionaryItem("First");
    rootFactory << itemReference;
    rootFactory.endDictionaryItem();
    rootFactory.beginDictionaryItem("Last");
    rootFactory << itemReference;
    rootFactory.endDictionaryItem();
    rootFactory.beginDictionaryItem("Count");
    rootFactory << 1;
    rootFactory.endDictionaryItem();
    rootFactory.endDictionary();
    builder->setOutline(builder->addObject(rootFactory.takeObject()));

    if (!modifier.finalize())
    {
        return pdf::PDFDocument();
    }
    return *modifier.getDocument();
}

/// A document whose outline carries one item built by the caller, which the
/// outline writer may not be able to write back.
pdf::PDFDocument buildDocumentWithOutlineItem(pdf::PDFObject item)
{
    const pdf::PDFDocument document = buildDocument(1);
    pdf::PDFDocumentModifier modifier(&document);
    pdf::PDFDocumentBuilder* builder = modifier.getBuilder();

    const pdf::PDFObjectReference itemReference = builder->addObject(item);

    pdf::PDFObjectFactory rootFactory;
    rootFactory.beginDictionary();
    rootFactory.beginDictionaryItem("First");
    rootFactory << itemReference;
    rootFactory.endDictionaryItem();
    rootFactory.beginDictionaryItem("Last");
    rootFactory << itemReference;
    rootFactory.endDictionaryItem();
    rootFactory.beginDictionaryItem("Count");
    rootFactory << 1;
    rootFactory.endDictionaryItem();
    rootFactory.endDictionary();

    builder->setOutline(builder->addObject(rootFactory.takeObject()));

    if (!modifier.finalize())
    {
        return pdf::PDFDocument();
    }
    return *modifier.getDocument();
}

/// One outline item whose /Dest is a name: a valid destination that addresses
/// the catalog's name tree rather than a page.
pdf::PDFObject namedDestinationOutlineItem()
{
    pdf::PDFObjectFactory itemFactory;
    itemFactory.beginDictionary();
    itemFactory.beginDictionaryItem("Title");
    itemFactory << QStringLiteral("Named");
    itemFactory.endDictionaryItem();
    itemFactory.beginDictionaryItem("Dest");
    itemFactory << pdf::WrapName(QByteArrayLiteral("chapter-one"));
    itemFactory.endDictionaryItem();
    itemFactory.endDictionary();
    return itemFactory.takeObject();
}

/// One outline item whose /Dest is an array with an unknown view name, which
/// parses to an invalid destination.
pdf::PDFObject unparsedDestinationOutlineItem()
{
    pdf::PDFObjectFactory itemFactory;
    itemFactory.beginDictionary();
    itemFactory.beginDictionaryItem("Title");
    itemFactory << QStringLiteral("Broken target");
    itemFactory.endDictionaryItem();
    itemFactory.beginDictionaryItem("Dest");
    itemFactory.beginArray();
    itemFactory << 0;
    itemFactory << pdf::WrapName(QByteArrayLiteral("Bogus"));
    itemFactory.endArray();
    itemFactory.endDictionaryItem();
    itemFactory.endDictionary();
    return itemFactory.takeObject();
}

}   // namespace

class AuthoringEditTest : public QObject
{
    Q_OBJECT

private slots:
    void bookmarkFileRoundTripsCanonically();
    void bookmarkFileRefusesMalformedInput_data();
    void bookmarkFileRefusesMalformedInput();
    void toggleBookmarkAddsThenRemovesThePageBookmark();
    void generateBookmarksSkipsPagesThatAlreadyHaveOne();
    void replaceBookmarksRoundTripsTheInterchangeFile();
    void destinationVariantWritesItsPdfDestinationType_data();
    void destinationVariantWritesItsPdfDestinationType();
    void uriLinkWritesTheUriAction();
    void invalidEditsAreRefusedWithTypedReasons_data();
    void invalidEditsAreRefusedWithTypedReasons();
    void structureOutlineRefusesAnOutlineRewrite();
    void namedDestinationOutlineIsNeverAPageTarget();
    void unparsedDestinationOutlineRefusesAnOutlineRewrite();
};

void AuthoringEditTest::bookmarkFileRoundTripsCanonically()
{
    pdf::PDFBookmarkFile file;
    pdf::PDFBookmarkRecord chapter;
    chapter.title = QStringLiteral("Chapter 1");
    chapter.pageIndex = 0;
    pdf::PDFBookmarkRecord section;
    section.title = QStringLiteral("Section 1.1");
    section.pageIndex = 2;
    chapter.children.append(section);
    file.bookmarks.append(chapter);
    file.bookmarks.append(pdf::PDFBookmarkRecord{ QStringLiteral("Appendix"), 5, {} });

    const QByteArray bytes = file.toJson();

    QString typedError;
    const std::optional<pdf::PDFBookmarkFile> parsed = pdf::PDFBookmarkFile::fromJson(bytes, typedError);
    QVERIFY2(parsed.has_value(), qPrintable(typedError));
    QCOMPARE(parsed->bookmarks.size(), 2);
    QCOMPARE(parsed->bookmarks.at(0).title, QStringLiteral("Chapter 1"));
    QCOMPARE(parsed->bookmarks.at(0).children.size(), 1);
    QCOMPARE(parsed->bookmarks.at(0).children.at(0).title, QStringLiteral("Section 1.1"));
    QCOMPARE(parsed->bookmarks.at(0).children.at(0).pageIndex, 2);
    QCOMPARE(parsed->bookmarks.at(1).pageIndex, 5);
    QCOMPARE(parsed->count(), 3);

    // Canonical bytes: parse(toJson(x)) serializes to the same bytes.
    QCOMPARE(parsed->toJson(), bytes);
}

void AuthoringEditTest::bookmarkFileRefusesMalformedInput_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("not-json") << QByteArray("{bookmarks");
    QTest::newRow("wrong-root") << QByteArray("[1,2,3]");
    QTest::newRow("wrong-schema") << QByteArray("{\"schema_version\":2,\"bookmarks\":[]}");
    QTest::newRow("missing-bookmarks") << QByteArray("{\"schema_version\":1}");
    QTest::newRow("missing-title") << QByteArray("{\"schema_version\":1,\"bookmarks\":[{\"page\":0}]}");
    QTest::newRow("fractional-page") << QByteArray("{\"schema_version\":1,\"bookmarks\":[{\"title\":\"A\",\"page\":1.5}]}");
    QTest::newRow("string-page") << QByteArray("{\"schema_version\":1,\"bookmarks\":[{\"title\":\"A\",\"page\":\"1\"}]}");
    QTest::newRow("bad-children") << QByteArray("{\"schema_version\":1,\"bookmarks\":[{\"title\":\"A\",\"page\":1,\"children\":3}]}");
}

void AuthoringEditTest::bookmarkFileRefusesMalformedInput()
{
    QFETCH(QByteArray, payload);

    QString typedError;
    QVERIFY(!pdf::PDFBookmarkFile::fromJson(payload, typedError).has_value());
    QCOMPARE(typedError, QStringLiteral("bookmark/invalid-file"));
}

void AuthoringEditTest::toggleBookmarkAddsThenRemovesThePageBookmark()
{
    const pdf::PDFDocument document = buildDocument(3);

    pdf::PDFAuthoringEdit edit = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    edit.pageIndex = 1;
    edit.text = QStringLiteral("Chapter two");

    const pdf::PDFAuthoringEditResult added = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY2(added.applied(), qPrintable(added.typedError));
    QCOMPARE(added.kind, pdf::PDFAuthoringEditResult::Kind::BookmarkAdded);

    const std::optional<pdf::PDFDocument> parsed = writeAndRead(*added.document);
    QVERIFY(parsed.has_value());
    const std::optional<QList<pdf::PDFBookmarkRecord>> records = outlineRecords(*parsed);
    QVERIFY(records.has_value());
    QCOMPARE(records->size(), 1);
    QCOMPARE(records->at(0).title, QStringLiteral("Chapter two"));
    QCOMPARE(records->at(0).pageIndex, 1);

    // The same command on the same page removes the bookmark again.
    const pdf::PDFAuthoringEditResult removed = pdf::PDFAuthoringEditWriter::apply(added.document.data(), edit);
    QVERIFY2(removed.applied(), qPrintable(removed.typedError));
    QCOMPARE(removed.kind, pdf::PDFAuthoringEditResult::Kind::BookmarkRemoved);

    const std::optional<pdf::PDFDocument> parsedRemoved = writeAndRead(*removed.document);
    QVERIFY(parsedRemoved.has_value());
    QVERIFY(parsedRemoved->getCatalog()->getOutlineRootPtr().isNull() ||
            parsedRemoved->getCatalog()->getOutlineRootPtr()->getChildCount() == 0);

    // Without a title the generated title is the shell's page label.
    pdf::PDFAuthoringEdit untitled = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    untitled.pageIndex = 2;
    const pdf::PDFAuthoringEditResult defaulted = pdf::PDFAuthoringEditWriter::apply(&document, untitled);
    QVERIFY2(defaulted.applied(), qPrintable(defaulted.typedError));
    const std::optional<QList<pdf::PDFBookmarkRecord>> defaultedRecords = outlineRecords(*defaulted.document);
    QVERIFY(defaultedRecords.has_value());
    QCOMPARE(defaultedRecords->at(0).title, QStringLiteral("Page 3"));
}

void AuthoringEditTest::generateBookmarksSkipsPagesThatAlreadyHaveOne()
{
    const pdf::PDFDocument document = buildDocument(3);

    pdf::PDFAuthoringEdit toggle = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    toggle.pageIndex = 1;
    toggle.text = QStringLiteral("Existing");
    const pdf::PDFAuthoringEditResult existing = pdf::PDFAuthoringEditWriter::apply(&document, toggle);
    QVERIFY2(existing.applied(), qPrintable(existing.typedError));

    const pdf::PDFAuthoringEditResult generated =
        pdf::PDFAuthoringEditWriter::apply(existing.document.data(), bookmarkEdit(pdf::PDFAuthoringEdit::Kind::GenerateBookmarks));
    QVERIFY2(generated.applied(), qPrintable(generated.typedError));
    QCOMPARE(generated.kind, pdf::PDFAuthoringEditResult::Kind::BookmarksGenerated);
    QCOMPARE(generated.affectedCount, 2);

    const std::optional<QList<pdf::PDFBookmarkRecord>> records = outlineRecords(*generated.document);
    QVERIFY(records.has_value());
    QCOMPARE(records->size(), 3);
    QCOMPARE(records->at(0).title, QStringLiteral("Existing"));
    QCOMPARE(records->at(1).title, QStringLiteral("Page 1"));
    QCOMPARE(records->at(2).title, QStringLiteral("Page 3"));

    // A second run has nothing to generate and says so instead of writing a
    // revision that changes nothing.
    const pdf::PDFAuthoringEditResult again =
        pdf::PDFAuthoringEditWriter::apply(generated.document.data(), bookmarkEdit(pdf::PDFAuthoringEdit::Kind::GenerateBookmarks));
    QVERIFY(!again.applied());
    QCOMPARE(again.typedError, QStringLiteral("bookmark/nothing-to-generate"));
    QVERIFY(again.document.isNull());
}

void AuthoringEditTest::replaceBookmarksRoundTripsTheInterchangeFile()
{
    const pdf::PDFDocument source = buildDocument(4);

    pdf::PDFAuthoringEdit toggle = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    toggle.pageIndex = 3;
    toggle.text = QStringLiteral("Last page");
    const pdf::PDFAuthoringEditResult first = pdf::PDFAuthoringEditWriter::apply(&source, toggle);
    QVERIFY2(first.applied(), qPrintable(first.typedError));

    pdf::PDFAuthoringEdit nested = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    nested.pageIndex = 0;
    nested.text = QStringLiteral("First page");
    const pdf::PDFAuthoringEditResult second = pdf::PDFAuthoringEditWriter::apply(first.document.data(), nested);
    QVERIFY2(second.applied(), qPrintable(second.typedError));

    const std::optional<QList<pdf::PDFBookmarkRecord>> exported = outlineRecords(*second.document);
    QVERIFY(exported.has_value());
    QCOMPARE(exported->size(), 2);

    pdf::PDFBookmarkFile file;
    file.bookmarks = *exported;
    const QByteArray bytes = file.toJson();

    QString typedError;
    const std::optional<pdf::PDFBookmarkFile> parsed = pdf::PDFBookmarkFile::fromJson(bytes, typedError);
    QVERIFY2(parsed.has_value(), qPrintable(typedError));

    // Import into a different document: the outline becomes the file, and a
    // fresh export produces the same records.
    const pdf::PDFDocument target = buildDocument(4);
    pdf::PDFAuthoringEdit import = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ReplaceBookmarks);
    import.bookmarks = parsed->bookmarks;
    const pdf::PDFAuthoringEditResult imported = pdf::PDFAuthoringEditWriter::apply(&target, import);
    QVERIFY2(imported.applied(), qPrintable(imported.typedError));
    QCOMPARE(imported.kind, pdf::PDFAuthoringEditResult::Kind::BookmarksReplaced);
    QCOMPARE(imported.affectedCount, 2);

    const std::optional<QList<pdf::PDFBookmarkRecord>> reimported = outlineRecords(*imported.document);
    QVERIFY(reimported.has_value());
    QCOMPARE(*reimported, *exported);

    // Importing the same file into the document that already has it is a
    // no-change, not a second identical revision.
    const pdf::PDFAuthoringEditResult duplicate = pdf::PDFAuthoringEditWriter::apply(imported.document.data(), import);
    QVERIFY(!duplicate.applied());
    QCOMPARE(duplicate.typedError, QStringLiteral("edit/no-change"));

    // A record for a page the document does not have is refused before any
    // mutation, with a typed reason.
    pdf::PDFAuthoringEdit invalid = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ReplaceBookmarks);
    invalid.bookmarks = { pdf::PDFBookmarkRecord{ QStringLiteral("Ghost"), 9, {} } };
    const pdf::PDFAuthoringEditResult refused = pdf::PDFAuthoringEditWriter::apply(&target, invalid);
    QVERIFY(!refused.applied());
    QCOMPARE(refused.typedError, QStringLiteral("bookmark/invalid-record"));
    QVERIFY(refused.document.isNull());
}

void AuthoringEditTest::destinationVariantWritesItsPdfDestinationType_data()
{
    QTest::addColumn<int>("destinationType");

    QTest::newRow("Fit") << int(pdf::DestinationType::Fit);
    QTest::newRow("FitB") << int(pdf::DestinationType::FitB);
    QTest::newRow("FitBH") << int(pdf::DestinationType::FitBH);
    QTest::newRow("FitBV") << int(pdf::DestinationType::FitBV);
    QTest::newRow("FitH") << int(pdf::DestinationType::FitH);
    QTest::newRow("FitR") << int(pdf::DestinationType::FitR);
    QTest::newRow("FitV") << int(pdf::DestinationType::FitV);
    QTest::newRow("XYZ") << int(pdf::DestinationType::XYZ);
}

void AuthoringEditTest::destinationVariantWritesItsPdfDestinationType()
{
    QFETCH(int, destinationType);

    const pdf::PDFDocument document = buildDocument(2);
    const QRectF linkRectangle(QPointF(10, 20), QPointF(60, 40));
    // The page-space target, exactly as EditorHost builds it from the surface's
    // named corners: QRectF(QPointF(left, bottom), QPointF(right, top)). Page
    // space is y-up, so the PDF top is the numerically larger y.
    const qreal targetLeft = 5.0;
    const qreal targetBottom = 6.0;
    const qreal targetRight = 70.0;
    const qreal targetTop = 90.0;
    const QRectF targetRectangle(QPointF(targetLeft, targetBottom), QPointF(targetRight, targetTop));

    pdf::PDFAuthoringEdit edit;
    edit.kind = pdf::PDFAuthoringEdit::Kind::AddDestinationLink;
    edit.pageIndex = 0;
    edit.rectangle = linkRectangle;
    edit.targetPageIndex = 1;
    edit.destinationType = static_cast<pdf::DestinationType>(destinationType);
    edit.targetRectangle = targetRectangle;
    edit.zoom = 1.5;

    const pdf::PDFAuthoringEditResult applied = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY2(applied.applied(), qPrintable(applied.typedError));
    QCOMPARE(applied.kind, pdf::PDFAuthoringEditResult::Kind::LinkCreated);

    const std::optional<pdf::PDFDocument> parsed = writeAndRead(*applied.document);
    QVERIFY(parsed.has_value());

    const pdf::PDFAnnotationPtr linkAnnotation = firstLinkOnPage(*parsed, 0);
    const auto* link = dynamic_cast<const pdf::PDFLinkAnnotation*>(linkAnnotation.data());
    QVERIFY(link);
    QCOMPARE(link->getRectangle(), linkRectangle);

    const pdf::PDFActionGoTo* goTo = dynamic_cast<const pdf::PDFActionGoTo*>(link->getAction());
    QVERIFY(goTo);
    const pdf::PDFDestination& destination = goTo->getDestination();
    QCOMPARE(int(destination.getDestinationType()), destinationType);
    QCOMPARE(destinationPageIndex(*parsed, destination), 1);

    // The matching destination payload, per PDF 1.7 Table 151.
    switch (destination.getDestinationType())
    {
        case pdf::DestinationType::Fit:
        case pdf::DestinationType::FitB:
            QVERIFY(!destination.hasLeft());
            QVERIFY(!destination.hasTop());
            break;

        case pdf::DestinationType::FitH:
        case pdf::DestinationType::FitBH:
            QVERIFY(!destination.hasLeft());
            QVERIFY(destination.hasTop());
            QCOMPARE(destination.getTop(), targetTop);
            break;

        case pdf::DestinationType::FitV:
        case pdf::DestinationType::FitBV:
            QVERIFY(destination.hasLeft());
            QVERIFY(!destination.hasTop());
            QCOMPARE(destination.getLeft(), targetLeft);
            break;

        case pdf::DestinationType::FitR:
            QCOMPARE(destination.getLeft(), targetLeft);
            QCOMPARE(destination.getBottom(), targetBottom);
            QCOMPARE(destination.getRight(), targetRight);
            QCOMPARE(destination.getTop(), targetTop);
            break;

        case pdf::DestinationType::XYZ:
            QCOMPARE(destination.getLeft(), targetLeft);
            QCOMPARE(destination.getTop(), targetTop);
            QCOMPARE(destination.getZoom(), 1.5);
            break;

        case pdf::DestinationType::Invalid:
        case pdf::DestinationType::Named:
            QFAIL("the writer must never write an invalid destination");
    }
}

void AuthoringEditTest::uriLinkWritesTheUriAction()
{
    const pdf::PDFDocument document = buildDocument(1);

    pdf::PDFAuthoringEdit edit;
    edit.kind = pdf::PDFAuthoringEdit::Kind::AddUriLink;
    edit.pageIndex = 0;
    edit.rectangle = QRectF(QPointF(4, 8), QPointF(44, 24));
    edit.text = QStringLiteral("https://example.org/loop");

    const pdf::PDFAuthoringEditResult applied = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY2(applied.applied(), qPrintable(applied.typedError));

    const std::optional<pdf::PDFDocument> parsed = writeAndRead(*applied.document);
    QVERIFY(parsed.has_value());

    const pdf::PDFAnnotationPtr linkAnnotation = firstLinkOnPage(*parsed, 0);
    const auto* link = dynamic_cast<const pdf::PDFLinkAnnotation*>(linkAnnotation.data());
    QVERIFY(link);
    QCOMPARE(link->getRectangle(), edit.rectangle);

    const pdf::PDFActionURI* uri = dynamic_cast<const pdf::PDFActionURI*>(link->getAction());
    QVERIFY(uri);
    QCOMPARE(uri->getURI(), QByteArray("https://example.org/loop"));
}

void AuthoringEditTest::invalidEditsAreRefusedWithTypedReasons_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<int>("pageIndex");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("typedError");

    QTest::newRow("toggle-page-out-of-range")
        << int(pdf::PDFAuthoringEdit::Kind::ToggleBookmark) << 7 << QStringLiteral("A") << QStringLiteral("bookmark/page-out-of-range");
    QTest::newRow("uri-missing")
        << int(pdf::PDFAuthoringEdit::Kind::AddUriLink) << 0 << QString() << QStringLiteral("hyperlink/uri-required");
    QTest::newRow("link-page-out-of-range")
        << int(pdf::PDFAuthoringEdit::Kind::AddUriLink) << 7 << QStringLiteral("https://example.org") << QStringLiteral("hyperlink/page-out-of-range");
}

void AuthoringEditTest::invalidEditsAreRefusedWithTypedReasons()
{
    QFETCH(int, kind);
    QFETCH(int, pageIndex);
    QFETCH(QString, text);
    QFETCH(QString, typedError);

    const pdf::PDFDocument document = buildDocument(2);

    pdf::PDFAuthoringEdit edit;
    edit.kind = static_cast<pdf::PDFAuthoringEdit::Kind>(kind);
    edit.pageIndex = pageIndex;
    edit.text = text;
    edit.rectangle = QRectF(QPointF(1, 1), QPointF(10, 10));

    const pdf::PDFAuthoringEditResult refused = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY(!refused.applied());
    QCOMPARE(refused.typedError, typedError);
    QVERIFY(refused.document.isNull());

    // A degenerate link rectangle is refused rather than written as a
    // zero-area link.
    pdf::PDFAuthoringEdit degenerate;
    degenerate.kind = pdf::PDFAuthoringEdit::Kind::AddUriLink;
    degenerate.pageIndex = 0;
    degenerate.text = QStringLiteral("https://example.org");
    degenerate.rectangle = QRectF(QPointF(3, 3), QPointF(3, 3));
    const pdf::PDFAuthoringEditResult refusedDegenerate = pdf::PDFAuthoringEditWriter::apply(&document, degenerate);
    QVERIFY(!refusedDegenerate.applied());
    QCOMPARE(refusedDegenerate.typedError, QStringLiteral("hyperlink/link-rectangle-required"));

    // FitR without a target rectangle cannot describe its destination.
    pdf::PDFAuthoringEdit fitR;
    fitR.kind = pdf::PDFAuthoringEdit::Kind::AddDestinationLink;
    fitR.pageIndex = 0;
    fitR.rectangle = QRectF(QPointF(1, 1), QPointF(10, 10));
    fitR.targetPageIndex = 1;
    fitR.destinationType = pdf::DestinationType::FitR;
    const pdf::PDFAuthoringEditResult refusedFitR = pdf::PDFAuthoringEditWriter::apply(&document, fitR);
    QVERIFY(!refusedFitR.applied());
    QCOMPARE(refusedFitR.typedError, QStringLiteral("hyperlink/target-rectangle-required"));
}

void AuthoringEditTest::structureOutlineRefusesAnOutlineRewrite()
{
    const pdf::PDFDocument document = buildDocumentWithStructureOutline();
    QVERIFY(document.getCatalog()->getOutlineRootPtr());
    QCOMPARE(document.getCatalog()->getOutlineRootPtr()->getChildCount(), size_t(1));

    // The interchange format cannot carry the structure element, so reading the
    // outline refuses rather than exporting it without that state.
    QString typedError;
    QVERIFY(!pdf::PDFAuthoringEditWriter::readOutline(&document, typedError).has_value());
    QCOMPARE(typedError, QStringLiteral("bookmark/unsupported-outline-item"));

    // An edit that would keep the existing items refuses for the same reason.
    pdf::PDFAuthoringEdit edit = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    edit.pageIndex = 0;
    const pdf::PDFAuthoringEditResult refused = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY(!refused.applied());
    QCOMPARE(refused.typedError, QStringLiteral("bookmark/unsupported-outline-item"));

    // Replacing the outline is an explicit discard of the old outline, so it is
    // allowed to write the file's records.
    pdf::PDFAuthoringEdit replace = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ReplaceBookmarks);
    replace.bookmarks = { pdf::PDFBookmarkRecord{ QStringLiteral("Fresh"), 0, {} } };
    const pdf::PDFAuthoringEditResult replaced = pdf::PDFAuthoringEditWriter::apply(&document, replace);
    QVERIFY2(replaced.applied(), qPrintable(replaced.typedError));
    const std::optional<QList<pdf::PDFBookmarkRecord>> records = outlineRecords(*replaced.document);
    QVERIFY(records.has_value());
    QCOMPARE(records->size(), 1);
    QCOMPARE(records->at(0).title, QStringLiteral("Fresh"));
}

void AuthoringEditTest::namedDestinationOutlineIsNeverAPageTarget()
{
    const pdf::PDFDocument document = buildDocumentWithOutlineItem(namedDestinationOutlineItem());
    QVERIFY(document.getCatalog()->getOutlineRootPtr());
    QCOMPARE(document.getCatalog()->getOutlineRootPtr()->getChildCount(), size_t(1));

    // A named destination has no page here: the toggle must add a bookmark on
    // page 0 instead of removing the item it cannot address.
    pdf::PDFAuthoringEdit edit = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    edit.pageIndex = 0;
    edit.text = QStringLiteral("First page");
    const pdf::PDFAuthoringEditResult added = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY2(added.applied(), qPrintable(added.typedError));
    QCOMPARE(added.kind, pdf::PDFAuthoringEditResult::Kind::BookmarkAdded);

    // The file carries both items afterwards, and the named one kept its name.
    const std::optional<pdf::PDFDocument> parsed = writeAndRead(*added.document);
    QVERIFY(parsed.has_value());
    const QSharedPointer<pdf::PDFOutlineItem> root = parsed->getCatalog()->getOutlineRootPtr();
    QVERIFY(root);
    QCOMPARE(root->getChildCount(), size_t(2));
    QCOMPARE(root->getChild(0)->getTitle(), QStringLiteral("Named"));
    QCOMPARE(root->getChild(1)->getTitle(), QStringLiteral("First page"));
    const auto* namedGoTo = dynamic_cast<const pdf::PDFActionGoTo*>(root->getChild(0)->getAction());
    QVERIFY(namedGoTo);
    QVERIFY(namedGoTo->getDestination().isNamedDestination());
    QCOMPARE(namedGoTo->getDestination().getName(), QByteArrayLiteral("chapter-one"));
    QCOMPARE(root->getChild(1)->getAction()->getType(), pdf::ActionType::GoTo);

    // Exporting the outline refuses the named item with the typed code rather
    // than reporting page 0 for it.
    QString typedError;
    QVERIFY(!pdf::PDFAuthoringEditWriter::readOutline(&document, typedError).has_value());
    QCOMPARE(typedError, QStringLiteral("bookmark/unsupported-outline-item"));
}

void AuthoringEditTest::unparsedDestinationOutlineRefusesAnOutlineRewrite()
{
    const pdf::PDFDocument document = buildDocumentWithOutlineItem(unparsedDestinationOutlineItem());
    QVERIFY(document.getCatalog()->getOutlineRootPtr());
    QCOMPARE(document.getCatalog()->getOutlineRootPtr()->getChildCount(), size_t(1));

    // The outline writer would write the unparsed destination as PDF null, so
    // the item is not rewritable and the toggle refuses before writing.
    pdf::PDFAuthoringEdit edit = bookmarkEdit(pdf::PDFAuthoringEdit::Kind::ToggleBookmark);
    edit.pageIndex = 0;
    edit.text = QStringLiteral("First page");
    const pdf::PDFAuthoringEditResult refused = pdf::PDFAuthoringEditWriter::apply(&document, edit);
    QVERIFY(!refused.applied());
    QCOMPARE(refused.typedError, QStringLiteral("bookmark/unsupported-outline-item"));
    QVERIFY(refused.document.isNull());

    // The input document's outline is unchanged, destination included.
    const QSharedPointer<pdf::PDFOutlineItem> root = document.getCatalog()->getOutlineRootPtr();
    QCOMPARE(root->getChildCount(), size_t(1));
    QCOMPARE(root->getChild(0)->getTitle(), QStringLiteral("Broken target"));
    const auto* goTo = dynamic_cast<const pdf::PDFActionGoTo*>(root->getChild(0)->getAction());
    QVERIFY(goTo);
    QVERIFY(!goTo->getDestination().isValid());
}

QTEST_MAIN(AuthoringEditTest)

#include "tst_authoringedittest.moc"
