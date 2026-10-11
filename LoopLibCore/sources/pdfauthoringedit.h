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

#ifndef PDFAUTHORINGEDIT_H
#define PDFAUTHORINGEDIT_H

#include "pdfaction.h"
#include "pdfdocument.h"
#include "pdfglobal.h"

#include <QList>
#include <QRectF>
#include <QString>

#include <limits>
#include <optional>

namespace pdf
{

/// One entry of the bookmark interchange file: a title and the 0-based page it
/// targets. `children` keeps the file's order, which is the outline order.
struct PDFBookmarkRecord
{
    QString title;
    int pageIndex = -1;
    QList<PDFBookmarkRecord> children;

    bool operator==(const PDFBookmarkRecord&) const = default;
};

/// The bookmark interchange file of the direct-edit family (ADR-014).
///
/// Canonical JSON: `{"schema_version":1,"bookmarks":[{"title":…,"page":…,
/// "children":[…]}]}`. The format carries titles, page targets and nesting
/// only; outline state it cannot represent (structure elements, non-GoTo
/// actions) is not silently dropped by export — export refuses instead.
/// Parsing is total: malformed input returns a typed reason and no partial
/// tree.
class LOOPLIBCORESHARED_EXPORT PDFBookmarkFile
{
public:
    static std::optional<PDFBookmarkFile> fromJson(const QByteArray& payload, QString& typedError);
    QByteArray toJson() const;

    /// Total records, including nested children.
    int count() const;

    QList<PDFBookmarkRecord> bookmarks;
};

/// What one authoring edit asks Core to write (ADR-014).
///
/// All rectangles are in PDF page space (y-up); the writer normalizes inverted
/// corners and refuses degenerate ones. `zoom` uses a quiet NaN to write the
/// PDF `null` zoom, which is the "inherit the viewer's zoom" destination.
struct PDFAuthoringEdit
{
    enum class Kind
    {
        ToggleBookmark,      ///< Add the page's bookmark, or remove it when one targets the page.
        GenerateBookmarks,   ///< Add a bookmark for every page that has none.
        ReplaceBookmarks,    ///< Replace the outline with `bookmarks`.
        AddUriLink,          ///< Link annotation with a URI action.
        AddDestinationLink   ///< Link annotation with a GoTo action of `destinationType`.
    };

    Kind kind = Kind::ToggleBookmark;
    QString text;                        ///< Bookmark title, or the URI of a URI link.
    int pageIndex = -1;                  ///< Bookmark target page, or the link rectangle's page.
    QRectF rectangle;                    ///< Link rectangle, page space.
    int targetPageIndex = -1;            ///< Link destination page.
    DestinationType destinationType = DestinationType::Fit;
    QRectF targetRectangle;              ///< FitR / XYZ destination rectangle, page space.
    PDFReal zoom = std::numeric_limits<PDFReal>::quiet_NaN();   ///< XYZ zoom; NaN inherits.
    QList<PDFBookmarkRecord> bookmarks;  ///< ReplaceBookmarks payload.
};

/// How one edit resolved. `Refused` means no document was written and
/// `typedError` says why; every other kind carries the new document.
struct PDFAuthoringEditResult
{
    enum class Kind
    {
        Refused,
        BookmarkAdded,
        BookmarkRemoved,
        BookmarksGenerated,
        BookmarksReplaced,
        LinkCreated
    };

    Kind kind = Kind::Refused;
    QString typedError;      ///< Set exactly when kind == Refused.
    int affectedCount = 0;   ///< Bookmarks written by Generated/Replaced.
    PDFDocumentPointer document;
    PDFModifiedDocument::ModificationFlags flags;

    bool applied() const { return kind != Kind::Refused; }
};

/// The Core writer behind the interaction layer's direct authoring edits
/// (ADR-014). Every edit builds a modified document through
/// PDFDocumentModifier/PDFDocumentBuilder; a refusal carries a typed reason and
/// leaves the input document untouched.
///
/// An outline rewrite cannot represent state the builder's outline writer does
/// not write back (structure elements, actions that are neither GoTo nor
/// GoToDp), so edits that keep existing items refuse with
/// `bookmark/unsupported-outline-item` rather than dropping that state.
class LOOPLIBCORESHARED_EXPORT PDFAuthoringEditWriter
{
public:
    static PDFAuthoringEditResult apply(const PDFDocument* document, const PDFAuthoringEdit& edit);

    /// The document's outline as interchange records, in outline order. An
    /// item without a resolvable page destination is refused with
    /// `bookmark/unsupported-outline-item` rather than skipped.
    static std::optional<QList<PDFBookmarkRecord>> readOutline(const PDFDocument* document,
                                                               QString& typedError);

    /// Flattened bookmark page targets in outline order, for bookmark
    /// navigation. Items without a page target are not visited.
    static QList<int> bookmarkPages(const PDFDocument* document);
};

}   // namespace pdf

#endif   // PDFAUTHORINGEDIT_H
