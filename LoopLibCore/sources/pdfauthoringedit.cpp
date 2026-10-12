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

#include "pdfauthoringedit.h"

#include "pdfannotation.h"
#include "pdfartifactidentity.h"
#include "pdfcatalog.h"
#include "pdfdocumentbuilder.h"
#include "pdfoutline.h"
#include "pdfpage.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace pdf
{

namespace
{

constexpr int BookmarkFileSchemaVersion = 1;

void refusedEdit(const QString& typedError, PDFAuthoringEditResult* result)
{
    result->kind = PDFAuthoringEditResult::Kind::Refused;
    result->typedError = typedError;
    result->document.clear();
}

/// The document's outline as interchange records; `nullopt` when an item has no
/// page target to carry. Defined beside `readRecord` below.
std::optional<QList<PDFBookmarkRecord>> outlineRecords(const PDFDocument* document, QString& typedError);

QString pageTitle(int pageIndex)
{
    // 1-based, matching the shell's own page labels.
    return QStringLiteral("Page %1").arg(pageIndex + 1);
}

bool isPageIndexInRange(const PDFCatalog* catalog, int pageIndex)
{
    return pageIndex >= 0 && static_cast<size_t>(pageIndex) < catalog->getPageCount();
}

PDFObjectReference pageReference(const PDFCatalog* catalog, int pageIndex)
{
    return catalog->getPage(static_cast<size_t>(pageIndex))->getPageReference();
}

/// Resolves a destination's page. A destination may name the page by reference
/// or by index; a named or unparsed destination has no page target here
/// (callers refuse it with their own typed code), and a reference that
/// resolves to no page is not a page target.
bool resolveDestinationPage(const PDFCatalog* catalog, const PDFDestination& destination, int* pageIndex)
{
    if (!destination.isValid() || destination.isNamedDestination())
    {
        return false;
    }

    if (destination.getPageReference().isValid())
    {
        const size_t resolved = catalog->getPageIndexFromPageReference(destination.getPageReference());
        if (resolved == PDFCatalog::INVALID_PAGE_INDEX)
        {
            return false;
        }

        *pageIndex = static_cast<int>(resolved);
        return true;
    }

    const PDFInteger index = destination.getPageIndex();
    if (!isPageIndexInRange(catalog, static_cast<int>(index)))
    {
        return false;
    }

    *pageIndex = static_cast<int>(index);
    return true;
}

bool isUsableRectangle(const QRectF& rectangle)
{
    return rectangle.isValid() && !rectangle.isEmpty() && std::isfinite(rectangle.left()) &&
           std::isfinite(rectangle.top()) && std::isfinite(rectangle.right()) && std::isfinite(rectangle.bottom());
}

QSharedPointer<PDFOutlineItem> bookmarkItem(const PDFCatalog* catalog, const QString& title, int pageIndex)
{
    QSharedPointer<PDFOutlineItem> item(new PDFOutlineItem());
    item->setTitle(title);
    item->setAction(PDFActionPtr(new PDFActionGoTo(PDFDestination::createFit(pageReference(catalog, pageIndex)),
                                                   PDFDestination())));
    return item;
}

QSharedPointer<PDFOutlineItem> cloneOutlineRoot(const PDFDocument* document)
{
    const QSharedPointer<PDFOutlineItem> root = document->getCatalog()->getOutlineRootPtr();
    return root ? root->clone() : QSharedPointer<PDFOutlineItem>(new PDFOutlineItem());
}

void collectTargetedPages(const PDFCatalog* catalog, const PDFOutlineItem* item, QSet<int>& pages)
{
    int pageIndex = -1;
    const PDFActionGoTo* goTo = dynamic_cast<const PDFActionGoTo*>(item->getAction());
    if (goTo && resolveDestinationPage(catalog, goTo->getDestination(), &pageIndex))
    {
        pages.insert(pageIndex);
    }

    for (size_t i = 0; i < item->getChildCount(); ++i)
    {
        collectTargetedPages(catalog, item->getChild(i), pages);
    }
}

/// Removes every item whose destination resolves to `pageIndex`, at any depth.
bool removePageBookmarks(const PDFCatalog* catalog, PDFOutlineItem* item, int pageIndex)
{
    bool removed = false;
    for (size_t i = item->getChildCount(); i > 0; --i)
    {
        const QSharedPointer<PDFOutlineItem> child = item->getChildPtr(i - 1);
        int childPage = -1;
        const PDFActionGoTo* goTo = dynamic_cast<const PDFActionGoTo*>(child->getAction());
        if (goTo && resolveDestinationPage(catalog, goTo->getDestination(), &childPage) && childPage == pageIndex)
        {
            item->removeChild(i - 1);
            removed = true;
        }
        else
        {
            removed = removePageBookmarks(catalog, child.data(), pageIndex) || removed;
        }
    }
    return removed;
}

/// True when the builder's outline writer can write this item back unchanged.
/// It writes Title, a GoTo Dest, a GoToDp action, colour and flags; a structure
/// element, a GoTo whose destination did not parse (the writer would write PDF
/// null there) or any other action would be silently dropped.
bool isRewritableItem(const PDFOutlineItem* item)
{
    if (item->getStructureElement().isValid())
    {
        return false;
    }

    const PDFAction* action = item->getAction();
    if (!action)
    {
        // A title-only item has no state to lose.
        return true;
    }

    const auto* goTo = dynamic_cast<const PDFActionGoTo*>(action);
    if (goTo)
    {
        // A named destination is valid and round-trips (the object factory
        // writes the name); only a destination that failed to parse is lossy.
        return goTo->getDestination().isValid();
    }

    return dynamic_cast<const PDFActionGoToDp*>(action) != nullptr;
}

bool canRewriteOutline(const PDFOutlineItem* item)
{
    if (!isRewritableItem(item))
    {
        return false;
    }

    for (size_t i = 0; i < item->getChildCount(); ++i)
    {
        if (!canRewriteOutline(item->getChild(i)))
        {
            return false;
        }
    }
    return true;
}

void writeOutline(PDFDocumentModifier& modifier, const QSharedPointer<PDFOutlineItem>& root)
{
    modifier.markReset();
    modifier.getBuilder()->setOutline(root.data());
}

PDFAuthoringEditResult finishOutlineEdit(PDFDocumentModifier& modifier, PDFAuthoringEditResult::Kind kind)
{
    PDFAuthoringEditResult result;
    if (!modifier.finalize())
    {
        // An edit that writes exactly the outline the document already has is
        // reported, not counted as a new revision.
        refusedEdit(QStringLiteral("edit/no-change"), &result);
        return result;
    }

    result.kind = kind;
    result.document = modifier.getDocument();
    result.flags = modifier.getFlags();
    return result;
}

PDFAuthoringEditResult applyToggleBookmark(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    PDFAuthoringEditResult result;
    const PDFCatalog* catalog = document->getCatalog();
    if (!isPageIndexInRange(catalog, edit.pageIndex))
    {
        refusedEdit(QStringLiteral("bookmark/page-out-of-range"), &result);
        return result;
    }

    const QSharedPointer<PDFOutlineItem> root = cloneOutlineRoot(document);
    if (!canRewriteOutline(root.data()))
    {
        refusedEdit(QStringLiteral("bookmark/unsupported-outline-item"), &result);
        return result;
    }

    const bool removed = removePageBookmarks(catalog, root.data(), edit.pageIndex);
    if (!removed)
    {
        root->addChild(bookmarkItem(catalog,
                                    edit.text.isEmpty() ? pageTitle(edit.pageIndex) : edit.text,
                                    edit.pageIndex));
    }

    PDFDocumentModifier modifier(document);
    writeOutline(modifier, root);
    return finishOutlineEdit(modifier, removed ? PDFAuthoringEditResult::Kind::BookmarkRemoved
                                               : PDFAuthoringEditResult::Kind::BookmarkAdded);
}

PDFAuthoringEditResult applyGenerateBookmarks(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    Q_UNUSED(edit);
    PDFAuthoringEditResult result;
    const PDFCatalog* catalog = document->getCatalog();
    if (catalog->getPageCount() == 0)
    {
        refusedEdit(QStringLiteral("bookmark/no-pages"), &result);
        return result;
    }

    const QSharedPointer<PDFOutlineItem> root = cloneOutlineRoot(document);
    if (!canRewriteOutline(root.data()))
    {
        refusedEdit(QStringLiteral("bookmark/unsupported-outline-item"), &result);
        return result;
    }

    QSet<int> targeted;
    collectTargetedPages(catalog, root.data(), targeted);

    int generated = 0;
    for (size_t page = 0; page < catalog->getPageCount(); ++page)
    {
        if (targeted.contains(static_cast<int>(page)))
        {
            continue;
        }

        root->addChild(bookmarkItem(catalog, pageTitle(static_cast<int>(page)), static_cast<int>(page)));
        ++generated;
    }

    if (generated == 0)
    {
        refusedEdit(QStringLiteral("bookmark/nothing-to-generate"), &result);
        return result;
    }

    PDFDocumentModifier modifier(document);
    writeOutline(modifier, root);
    result = finishOutlineEdit(modifier, PDFAuthoringEditResult::Kind::BookmarksGenerated);
    result.affectedCount = generated;
    return result;
}

bool validateRecords(const PDFCatalog* catalog, const QList<PDFBookmarkRecord>& records, QString& typedError)
{
    for (const PDFBookmarkRecord& record : records)
    {
        if (record.title.isEmpty() || !isPageIndexInRange(catalog, record.pageIndex))
        {
            typedError = QStringLiteral("bookmark/invalid-record");
            return false;
        }

        if (!validateRecords(catalog, record.children, typedError))
        {
            return false;
        }
    }
    return true;
}

int countRecords(const QList<PDFBookmarkRecord>& records)
{
    int count = 0;
    for (const PDFBookmarkRecord& record : records)
    {
        count += 1 + countRecords(record.children);
    }
    return count;
}

void appendRecords(const PDFCatalog* catalog, PDFOutlineItem* parent, const QList<PDFBookmarkRecord>& records)
{
    for (const PDFBookmarkRecord& record : records)
    {
        const QSharedPointer<PDFOutlineItem> item = bookmarkItem(catalog, record.title, record.pageIndex);
        appendRecords(catalog, item.data(), record.children);
        parent->addChild(item);
    }
}

PDFAuthoringEditResult applyReplaceBookmarks(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    PDFAuthoringEditResult result;
    const PDFCatalog* catalog = document->getCatalog();
    QString typedError;
    if (!validateRecords(catalog, edit.bookmarks, typedError))
    {
        refusedEdit(typedError, &result);
        return result;
    }

    // Replacing the outline with exactly the records it already carries would
    // write a second identical revision; it is reported instead. An outline the
    // interchange format cannot read back (structure elements, foreign
    // actions) is not comparable, and replacing it is an explicit discard, so
    // the edit proceeds.
    QString outlineError;
    const std::optional<QList<PDFBookmarkRecord>> current = outlineRecords(document, outlineError);
    if (current.has_value() && *current == edit.bookmarks)
    {
        refusedEdit(QStringLiteral("edit/no-change"), &result);
        return result;
    }

    QSharedPointer<PDFOutlineItem> root(new PDFOutlineItem());
    appendRecords(catalog, root.data(), edit.bookmarks);

    PDFDocumentModifier modifier(document);
    writeOutline(modifier, root);
    result = finishOutlineEdit(modifier, PDFAuthoringEditResult::Kind::BookmarksReplaced);
    result.affectedCount = countRecords(edit.bookmarks);
    return result;
}

PDFAuthoringEditResult applyUriLink(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    PDFAuthoringEditResult result;
    const PDFCatalog* catalog = document->getCatalog();
    if (edit.text.isEmpty())
    {
        refusedEdit(QStringLiteral("hyperlink/uri-required"), &result);
        return result;
    }
    if (!isPageIndexInRange(catalog, edit.pageIndex))
    {
        refusedEdit(QStringLiteral("hyperlink/page-out-of-range"), &result);
        return result;
    }
    if (!isUsableRectangle(edit.rectangle))
    {
        refusedEdit(QStringLiteral("hyperlink/link-rectangle-required"), &result);
        return result;
    }

    PDFDocumentModifier modifier(document);
    modifier.getBuilder()->createAnnotationLink(pageReference(catalog, edit.pageIndex),
                                                edit.rectangle.normalized(),
                                                edit.text,
                                                LinkHighlightMode::Invert);
    modifier.markAnnotationsChanged();
    if (!modifier.finalize())
    {
        refusedEdit(QStringLiteral("edit/no-change"), &result);
        return result;
    }

    result.kind = PDFAuthoringEditResult::Kind::LinkCreated;
    result.document = modifier.getDocument();
    result.flags = modifier.getFlags();
    return result;
}

PDFAuthoringEditResult applyDestinationLink(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    PDFAuthoringEditResult result;
    const PDFCatalog* catalog = document->getCatalog();
    if (!isPageIndexInRange(catalog, edit.pageIndex))
    {
        refusedEdit(QStringLiteral("hyperlink/page-out-of-range"), &result);
        return result;
    }
    if (!isPageIndexInRange(catalog, edit.targetPageIndex))
    {
        refusedEdit(QStringLiteral("hyperlink/target-page-out-of-range"), &result);
        return result;
    }
    if (!isUsableRectangle(edit.rectangle))
    {
        refusedEdit(QStringLiteral("hyperlink/link-rectangle-required"), &result);
        return result;
    }
    if (edit.destinationType == DestinationType::Invalid || edit.destinationType == DestinationType::Named)
    {
        refusedEdit(QStringLiteral("hyperlink/unsupported-destination"), &result);
        return result;
    }
    if (edit.destinationType == DestinationType::FitR && !isUsableRectangle(edit.targetRectangle))
    {
        refusedEdit(QStringLiteral("hyperlink/target-rectangle-required"), &result);
        return result;
    }

    // The host builds the page-space rectangle exactly as the surface named its
    // corners: QRectF(QPointF(left, bottom), QPointF(right, top)). Page space is
    // y-up, so the PDF top is the numerically larger y; that construction stores
    // it in QRectF::bottom() (and the PDF bottom in QRectF::top()).
    const QRectF target = edit.targetRectangle.normalized();
    PDFDestination destination;
    destination.setDestinationType(edit.destinationType);
    destination.setPageIndex(edit.targetPageIndex);
    destination.setPageReference(pageReference(catalog, edit.targetPageIndex));
    switch (edit.destinationType)
    {
        case DestinationType::XYZ:
            destination.setLeft(target.isValid() && !target.isEmpty() ? target.left()
                                                                      : std::numeric_limits<PDFReal>::quiet_NaN());
            destination.setTop(target.isValid() && !target.isEmpty() ? target.bottom()
                                                                     : std::numeric_limits<PDFReal>::quiet_NaN());
            destination.setZoom(edit.zoom);
            break;

        case DestinationType::FitH:
        case DestinationType::FitBH:
            destination.setTop(target.isValid() && !target.isEmpty() ? target.bottom()
                                                                     : std::numeric_limits<PDFReal>::quiet_NaN());
            break;

        case DestinationType::FitV:
        case DestinationType::FitBV:
            destination.setLeft(target.isValid() && !target.isEmpty() ? target.left()
                                                                      : std::numeric_limits<PDFReal>::quiet_NaN());
            break;

        case DestinationType::FitR:
            destination.setLeft(target.left());
            destination.setBottom(target.top());
            destination.setRight(target.right());
            destination.setTop(target.bottom());
            break;

        case DestinationType::Fit:
        case DestinationType::FitB:
        case DestinationType::Invalid:
        case DestinationType::Named:
            break;
    }

    PDFDocumentModifier modifier(document);
    const PDFObjectReference action = modifier.getBuilder()->createActionGoTo(destination);
    modifier.getBuilder()->createAnnotationLink(pageReference(catalog, edit.pageIndex),
                                                edit.rectangle.normalized(),
                                                action,
                                                LinkHighlightMode::Invert);
    modifier.markAnnotationsChanged();
    if (!modifier.finalize())
    {
        refusedEdit(QStringLiteral("edit/no-change"), &result);
        return result;
    }

    result.kind = PDFAuthoringEditResult::Kind::LinkCreated;
    result.document = modifier.getDocument();
    result.flags = modifier.getFlags();
    return result;
}

bool readRecord(const PDFCatalog* catalog, const PDFOutlineItem* item, PDFBookmarkRecord& record, QString& typedError)
{
    record.title = item->getTitle();

    const PDFActionGoTo* goTo = dynamic_cast<const PDFActionGoTo*>(item->getAction());
    if (!goTo || !resolveDestinationPage(catalog, goTo->getDestination(), &record.pageIndex))
    {
        typedError = QStringLiteral("bookmark/unsupported-outline-item");
        return false;
    }

    for (size_t i = 0; i < item->getChildCount(); ++i)
    {
        PDFBookmarkRecord child;
        if (!readRecord(catalog, item->getChild(i), child, typedError))
        {
            return false;
        }
        record.children.append(child);
    }
    return true;
}

std::optional<QList<PDFBookmarkRecord>> outlineRecords(const PDFDocument* document, QString& typedError)
{
    const QSharedPointer<PDFOutlineItem> root = document->getCatalog()->getOutlineRootPtr();
    if (!root)
    {
        return QList<PDFBookmarkRecord>();
    }

    QList<PDFBookmarkRecord> records;
    for (size_t i = 0; i < root->getChildCount(); ++i)
    {
        PDFBookmarkRecord record;
        if (!readRecord(document->getCatalog(), root->getChild(i), record, typedError))
        {
            return std::nullopt;
        }
        records.append(record);
    }
    return records;
}

void collectBookmarkPages(const PDFCatalog* catalog, const PDFOutlineItem* item, QList<int>& pages)
{
    int pageIndex = -1;
    const PDFActionGoTo* goTo = dynamic_cast<const PDFActionGoTo*>(item->getAction());
    if (goTo && resolveDestinationPage(catalog, goTo->getDestination(), &pageIndex))
    {
        pages.append(pageIndex);
    }

    for (size_t i = 0; i < item->getChildCount(); ++i)
    {
        collectBookmarkPages(catalog, item->getChild(i), pages);
    }
}

QJsonObject recordToJson(const PDFBookmarkRecord& record)
{
    QJsonObject object;
    object.insert(QStringLiteral("title"), record.title);
    object.insert(QStringLiteral("page"), record.pageIndex);
    if (!record.children.isEmpty())
    {
        QJsonArray children;
        for (const PDFBookmarkRecord& child : record.children)
        {
            children.append(recordToJson(child));
        }
        object.insert(QStringLiteral("children"), children);
    }
    return object;
}

bool recordFromJson(const QJsonValue& value, PDFBookmarkRecord& record, QString& typedError)
{
    if (!value.isObject())
    {
        typedError = QStringLiteral("bookmark/invalid-file");
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue title = object.value(QStringLiteral("title"));
    const QJsonValue page = object.value(QStringLiteral("page"));
    if (!title.isString() || title.toString().isEmpty() || !page.isDouble() ||
        page.toDouble() != std::floor(page.toDouble()))
    {
        typedError = QStringLiteral("bookmark/invalid-file");
        return false;
    }

    record.title = title.toString();
    record.pageIndex = static_cast<int>(page.toDouble());
    record.children.clear();

    if (object.contains(QStringLiteral("children")))
    {
        const QJsonValue children = object.value(QStringLiteral("children"));
        if (!children.isArray())
        {
            typedError = QStringLiteral("bookmark/invalid-file");
            return false;
        }

        for (const QJsonValue& child : children.toArray())
        {
            PDFBookmarkRecord childRecord;
            if (!recordFromJson(child, childRecord, typedError))
            {
                return false;
            }
            record.children.append(childRecord);
        }
    }
    return true;
}

}   // namespace

std::optional<PDFBookmarkFile> PDFBookmarkFile::fromJson(const QByteArray& payload, QString& typedError)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        typedError = QStringLiteral("bookmark/invalid-file");
        return std::nullopt;
    }

    const QJsonObject object = document.object();
    const QJsonValue schemaVersion = object.value(QStringLiteral("schema_version"));
    const QJsonValue bookmarks = object.value(QStringLiteral("bookmarks"));
    if (!schemaVersion.isDouble() || static_cast<int>(schemaVersion.toDouble()) != BookmarkFileSchemaVersion ||
        !bookmarks.isArray())
    {
        typedError = QStringLiteral("bookmark/invalid-file");
        return std::nullopt;
    }

    PDFBookmarkFile file;
    for (const QJsonValue& value : bookmarks.toArray())
    {
        PDFBookmarkRecord record;
        if (!recordFromJson(value, record, typedError))
        {
            return std::nullopt;
        }
        file.bookmarks.append(record);
    }
    return file;
}

QByteArray PDFBookmarkFile::toJson() const
{
    QJsonArray array;
    for (const PDFBookmarkRecord& record : bookmarks)
    {
        array.append(recordToJson(record));
    }

    QJsonObject object;
    object.insert(QStringLiteral("schema_version"), BookmarkFileSchemaVersion);
    object.insert(QStringLiteral("bookmarks"), array);
    return canonicalJson(QJsonValue(object));
}

int PDFBookmarkFile::count() const
{
    return countRecords(bookmarks);
}

PDFAuthoringEditResult PDFAuthoringEditWriter::apply(const PDFDocument* document, const PDFAuthoringEdit& edit)
{
    if (!document)
    {
        PDFAuthoringEditResult result;
        refusedEdit(QStringLiteral("edit/document-missing"), &result);
        return result;
    }

    switch (edit.kind)
    {
        case PDFAuthoringEdit::Kind::ToggleBookmark:
            return applyToggleBookmark(document, edit);

        case PDFAuthoringEdit::Kind::GenerateBookmarks:
            return applyGenerateBookmarks(document, edit);

        case PDFAuthoringEdit::Kind::ReplaceBookmarks:
            return applyReplaceBookmarks(document, edit);

        case PDFAuthoringEdit::Kind::AddUriLink:
            return applyUriLink(document, edit);

        case PDFAuthoringEdit::Kind::AddDestinationLink:
            return applyDestinationLink(document, edit);
    }

    PDFAuthoringEditResult result;
    refusedEdit(QStringLiteral("edit/unknown-kind"), &result);
    return result;
}

std::optional<QList<PDFBookmarkRecord>> PDFAuthoringEditWriter::readOutline(const PDFDocument* document,
                                                                            QString& typedError)
{
    if (!document)
    {
        typedError = QStringLiteral("bookmark/document-missing");
        return std::nullopt;
    }

    return outlineRecords(document, typedError);
}

QList<int> PDFAuthoringEditWriter::bookmarkPages(const PDFDocument* document)
{
    QList<int> pages;
    if (!document)
    {
        return pages;
    }

    const QSharedPointer<PDFOutlineItem> root = document->getCatalog()->getOutlineRootPtr();
    if (root)
    {
        collectBookmarkPages(document->getCatalog(), root.data(), pages);
    }
    return pages;
}

}   // namespace pdf
