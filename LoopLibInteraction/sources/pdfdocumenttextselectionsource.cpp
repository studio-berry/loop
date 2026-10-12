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

#include "pdfdocumenttextselectionsource.h"

#include "pdfcatalog.h"
#include "pdfdocumentsession.h"
#include "pdfexception.h"
#include "pdfpage.h"
#include "pdfprocessingbudget.h"
#include "pdfrenderer.h"
#include "pdftextlayoutgenerator.h"

namespace pdfinteraction
{

namespace
{

/// A selection's own colour is presentation state the host owns; Core is asked
/// for geometry and text, never for a palette.
const QColor SelectionScratchColor = QColor(0, 0, 0, 0);

/// Page-space rectangles of one selection item, one per line span. A selection
/// item stays inside one block -- PDFTextLayout::createTextSelection adds one
/// item per block -- so the line walk cannot cross a block boundary.
void appendItemGeometry(const pdf::PDFTextLayout& layout,
                        const pdf::PDFTextSelectionColoredItem& item,
                        QList<QRectF>* geometry)
{
    const pdf::PDFTextBlocks& blocks = layout.getTextBlocks();
    if (item.start.blockIndex >= blocks.size())
    {
        return;
    }

    const pdf::PDFTextLines& lines = blocks[item.start.blockIndex].getLines();
    const size_t firstLine = item.start.lineIndex;
    const size_t lastLine = item.end.blockIndex == item.start.blockIndex ? item.end.lineIndex : firstLine;

    for (size_t lineIndex = firstLine; lineIndex <= lastLine && lineIndex < lines.size(); ++lineIndex)
    {
        const pdf::TextCharacters& characters = lines[lineIndex].getCharacters();
        if (characters.empty())
        {
            continue;
        }

        const size_t firstCharacter = lineIndex == firstLine ? item.start.characterIndex : 0;
        const size_t lastCharacter =
            item.end.blockIndex == item.start.blockIndex && lineIndex == lastLine
                ? qMin(item.end.characterIndex, characters.size() - 1)
                : characters.size() - 1;

        QRectF lineBounds;
        bool any = false;
        for (size_t characterIndex = firstCharacter; characterIndex <= lastCharacter && characterIndex < characters.size(); ++characterIndex)
        {
            const QRectF bounds = characters[characterIndex].boundingBox.boundingRect();
            if (bounds.isEmpty())
            {
                continue;
            }

            lineBounds = any ? lineBounds.united(bounds) : bounds;
            any = true;
        }

        if (any)
        {
            geometry->push_back(lineBounds);
        }
    }
}

}   // namespace

PDFDocumentTextSelectionSource::PDFDocumentTextSelectionSource(pdf::PDFDocumentContext* context) :
    m_context(context)
{
}

void PDFDocumentTextSelectionSource::dropLayoutCacheIfStale() const
{
    const pdf::PDFRevisionIdentity revision = m_context ? m_context->getRevision() : pdf::PDFRevisionIdentity();
    if (revision != m_layoutRevision)
    {
        m_layouts.clear();
        m_layoutRevision = revision;
    }
}

pdf::PDFTextLayout* PDFDocumentTextSelectionSource::mutableTextLayout(int pageIndex, QString* unavailableReason) const
{
    const auto unavailable = [unavailableReason](const QString& reason) -> pdf::PDFTextLayout*
    {
        if (unavailableReason)
        {
            *unavailableReason = reason;
        }
        return nullptr;
    };

    pdf::PDFDocumentContext* context = m_context;
    if (!context)
    {
        return unavailable(tr("No document is open."));
    }

    pdf::PDFDocumentSession* session = context->getSession();
    const pdf::PDFDocument* document = context->getDocument();
    if (!session || !document)
    {
        return unavailable(tr("No document is open."));
    }

    dropLayoutCacheIfStale();

    const auto cached = m_layouts.find(pageIndex);
    if (cached != m_layouts.end())
    {
        // A page with no text layer caches an empty layout, and the refusal
        // must be the same on every call, not only the one that generated it.
        // Re-check rather than return the cached empty layout as available.
        if (cached.value().getTextBlocks().empty())
        {
            return unavailable(tr("This page has no text layer."));
        }
        return &cached.value();
    }

    const pdf::PDFCatalog* catalog = document->getCatalog();
    const pdf::PDFPage* page = catalog && pageIndex >= 0 ? catalog->getPage(static_cast<size_t>(pageIndex)) : nullptr;
    if (!page)
    {
        return unavailable(tr("The page does not exist."));
    }

    const pdf::PDFRenderer::Features features = pdf::PDFRenderer::IgnoreOptionalContent;
    const pdf::PDFMeshQualitySettings meshQuality;
    pdf::PDFProcessingBudget budget(session->getProcessingLimits());
    try
    {
        pdf::PDFTextLayoutGenerator generator(features, page, document,
                                              session->getFontCache(), session->getCMS(),
                                              session->getOptionalContentActivity(), QTransform(), meshQuality,
                                              &budget);
        generator.processContents();
        m_layouts.insert(pageIndex, generator.createTextLayout());
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        return unavailable(exception.getMessage());
    }

    pdf::PDFTextLayout& layout = m_layouts[pageIndex];
    if (layout.getTextBlocks().empty())
    {
        return unavailable(tr("This page has no text layer."));
    }

    return &layout;
}

int PDFDocumentTextSelectionSource::lineOrdinal(const pdf::PDFTextLayout& layout, size_t blockIndex, size_t lineIndex) const
{
    int ordinal = 0;
    const pdf::PDFTextBlocks& blocks = layout.getTextBlocks();
    for (size_t block = 0; block < blocks.size(); ++block)
    {
        const pdf::PDFTextLines& lines = blocks[block].getLines();
        for (size_t line = 0; line < lines.size(); ++line, ++ordinal)
        {
            if (block == blockIndex && line == lineIndex)
            {
                return ordinal;
            }
        }
    }

    return -1;
}

TextSelectionResult PDFDocumentTextSelectionSource::resultFrom(const pdf::PDFTextLayout& layout,
                                                               const pdf::PDFTextSelection& selection,
                                                               int pageIndex) const
{
    TextSelectionResult result;
    result.available = true;
    result.text = layout.getTextFromSelection(selection, pageIndex);

    for (auto item = selection.begin(pageIndex); item != selection.end(pageIndex); ++item)
    {
        if (item->start.pageIndex != pageIndex)
        {
            continue;
        }

        appendItemGeometry(layout, *item, &result.geometry);

        const int firstOrdinal = lineOrdinal(layout, item->start.blockIndex, item->start.lineIndex);
        const int lastOrdinal = lineOrdinal(layout, item->end.blockIndex, item->end.lineIndex);
        if (firstOrdinal >= 0)
        {
            result.firstLine = result.firstLine < 0 ? firstOrdinal : qMin(result.firstLine, firstOrdinal);
        }
        if (lastOrdinal >= 0)
        {
            result.lastLine = qMax(result.lastLine, lastOrdinal);
        }
    }

    return result;
}

bool PDFDocumentTextSelectionSource::hasTextLayer(int pageIndex) const
{
    return mutableTextLayout(pageIndex, nullptr) != nullptr;
}

TextSelectionResult PDFDocumentTextSelectionSource::selectRange(int pageIndex, QPointF startPagePoint, QPointF endPagePoint)
{
    QString unavailableReason;
    pdf::PDFTextLayout* layout = mutableTextLayout(pageIndex, &unavailableReason);
    if (!layout)
    {
        TextSelectionResult result;
        result.unavailableReason = unavailableReason;
        return result;
    }

    const pdf::PDFTextSelection selection =
        layout->createTextSelection(pageIndex, startPagePoint, endPagePoint, SelectionScratchColor, false);
    return resultFrom(*layout, selection, pageIndex);
}

TextSelectionResult PDFDocumentTextSelectionSource::selectPage(int pageIndex)
{
    QString unavailableReason;
    pdf::PDFTextLayout* layout = mutableTextLayout(pageIndex, &unavailableReason);
    if (!layout)
    {
        TextSelectionResult result;
        result.unavailableReason = unavailableReason;
        return result;
    }

    // Every block, in the layout's own reading order, as one selection. A
    // range selection over the media box would instead depend on where the box
    // corners fall relative to the text, which is not the same question.
    pdf::PDFTextSelection selection;
    const pdf::PDFTextBlocks& blocks = layout->getTextBlocks();
    for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
    {
        const pdf::PDFTextSelection blockSelection = layout->selectBlock(blockIndex, pageIndex, SelectionScratchColor);
        pdf::PDFTextSelectionItems items;
        for (auto item = blockSelection.begin(pageIndex); item != blockSelection.end(pageIndex); ++item)
        {
            items.push_back(pdf::PDFTextSelectionItem(item->start, item->end));
        }
        selection.addItems(items, SelectionScratchColor);
    }
    selection.build();

    return resultFrom(*layout, selection, pageIndex);
}

TextSelectionResult PDFDocumentTextSelectionSource::selectBlockAt(int pageIndex, QPointF pagePoint)
{
    QString unavailableReason;
    pdf::PDFTextLayout* layout = mutableTextLayout(pageIndex, &unavailableReason);
    if (!layout)
    {
        TextSelectionResult result;
        result.unavailableReason = unavailableReason;
        return result;
    }

    const pdf::PDFTextBlocks& blocks = layout->getTextBlocks();
    for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
    {
        if (!blocks[blockIndex].getBoundingBox().contains(pagePoint))
        {
            continue;
        }

        // The first block containing the point wins; a block may be a table or
        // a paragraph, and the text layer is what tells them apart.
        const pdf::PDFTextSelection selection = layout->selectBlock(blockIndex, pageIndex, SelectionScratchColor);
        return resultFrom(*layout, selection, pageIndex);
    }

    // No block under the point: an available page with nothing selected.
    TextSelectionResult result;
    result.available = true;
    return result;
}

int PDFDocumentTextSelectionSource::lineCount(int pageIndex) const
{
    QString unavailableReason;
    const pdf::PDFTextLayout* layout = mutableTextLayout(pageIndex, &unavailableReason);
    if (!layout)
    {
        return -1;
    }

    int count = 0;
    for (const pdf::PDFTextBlock& block : layout->getTextBlocks())
    {
        count += static_cast<int>(block.getLines().size());
    }
    return count;
}

TextSelectionResult PDFDocumentTextSelectionSource::selectLine(int pageIndex, int lineIndex)
{
    QString unavailableReason;
    pdf::PDFTextLayout* layout = mutableTextLayout(pageIndex, &unavailableReason);
    if (!layout)
    {
        TextSelectionResult result;
        result.unavailableReason = unavailableReason;
        return result;
    }

    int ordinal = 0;
    const pdf::PDFTextBlocks& blocks = layout->getTextBlocks();
    for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
    {
        const pdf::PDFTextLines& lines = blocks[blockIndex].getLines();
        for (size_t line = 0; line < lines.size(); ++line, ++ordinal)
        {
            if (ordinal != lineIndex)
            {
                continue;
            }

            const pdf::PDFTextSelection selection = layout->selectLineInBlock(blockIndex, line, pageIndex, SelectionScratchColor);
            TextSelectionResult result = resultFrom(*layout, selection, pageIndex);
            result.firstLine = lineIndex;
            result.lastLine = lineIndex;
            return result;
        }
    }

    TextSelectionResult result;
    result.available = true;
    return result;
}

}   // namespace pdfinteraction
