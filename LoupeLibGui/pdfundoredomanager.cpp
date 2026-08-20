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

#include "pdfundoredomanager.h"
#include "pdfdbgheap.h"
#include "pdfdocumentwriter.h"

#include <algorithm>

namespace pdfviewer
{

PDFUndoRedoManager::PDFUndoRedoManager(QObject* parent) :
    BaseClass(parent)
{

}

PDFUndoRedoManager::~PDFUndoRedoManager()
{

}

void PDFUndoRedoManager::doUndo()
{
    if (!canUndo())
    {
        // Undo operation can't be performed
        return;
    }

    UndoRedoItem item = m_undoSteps.back();
    m_undoSteps.pop_back();
    m_isCurrentSaved = false;
    m_redoSteps.insert(m_redoSteps.begin(), item);
    clampUndoRedoSteps();

    Q_EMIT undoRedoStateChanged();
    Q_EMIT undoHistoryChanged();
    Q_EMIT documentChangeRequest(pdf::PDFModifiedDocument(item.oldDocument, nullptr, item.flags));
}

void PDFUndoRedoManager::doRedo()
{
    if (!canRedo())
    {
        // Redo operation can't be performed
        return;
    }

    UndoRedoItem item = m_redoSteps.front();
    m_redoSteps.erase(m_redoSteps.begin());
    m_isCurrentSaved = false;
    m_undoSteps.push_back(item);
    clampUndoRedoSteps();

    Q_EMIT undoRedoStateChanged();
    Q_EMIT undoHistoryChanged();
    Q_EMIT documentChangeRequest(pdf::PDFModifiedDocument(item.newDocument, nullptr, item.flags));
}

void PDFUndoRedoManager::clear()
{
    if (canUndo() || canRedo())
    {
        m_undoSteps.clear();
        m_redoSteps.clear();
        m_historyMemoryBytes = 0;
        m_historyTruncated = false;
        Q_EMIT undoRedoStateChanged();
        Q_EMIT undoHistoryChanged();
    }
}

void PDFUndoRedoManager::createUndo(pdf::PDFModifiedDocument document, pdf::PDFDocumentPointer oldDocument)
{
    createUndo(qMove(document), qMove(oldDocument), QString());
}

void PDFUndoRedoManager::createUndo(pdf::PDFModifiedDocument document,
                                    pdf::PDFDocumentPointer oldDocument,
                                    const QString& name)
{
    QString resolvedName = name;
    if (resolvedName.isEmpty())
    {
        if (document.hasPageContentsChanged())
        {
            resolvedName = QStringLiteral("Edit page content");
        }
        else if (document.hasFlag(pdf::PDFModifiedDocument::Annotation))
        {
            resolvedName = QStringLiteral("Edit annotation");
        }
        else if (document.hasFlag(pdf::PDFModifiedDocument::FormField))
        {
            resolvedName = QStringLiteral("Edit form field");
        }
        else
        {
            resolvedName = QStringLiteral("Document change");
        }
    }
    const size_t estimatedBytes = estimateDocumentBytes(oldDocument.data())
        + estimateDocumentBytes(document.getDocument());
    m_undoSteps.emplace_back(oldDocument, document, document.getFlags(),
                             qMove(resolvedName),
                             estimatedBytes);
    m_historyMemoryBytes += estimatedBytes;
    for (const UndoRedoItem& item : m_redoSteps)
    {
        m_historyMemoryBytes -= std::min(m_historyMemoryBytes, item.estimatedBytes);
    }
    m_redoSteps.clear();
    m_isCurrentSaved = false;
    clampUndoRedoSteps();
    Q_EMIT undoRedoStateChanged();
    Q_EMIT undoHistoryChanged();
}

void PDFUndoRedoManager::setMaximumSteps(size_t undoLimit, size_t redoLimit)
{
    if (m_undoLimit != undoLimit || m_redoLimit != redoLimit)
    {
        m_undoLimit = undoLimit;
        m_redoLimit = redoLimit;
        clampUndoRedoSteps();
        Q_EMIT undoRedoStateChanged();
        Q_EMIT undoHistoryChanged();
    }
}

void PDFUndoRedoManager::setMemoryLimitBytes(size_t memoryLimitBytes)
{
    if (m_memoryLimitBytes != memoryLimitBytes)
    {
        m_memoryLimitBytes = memoryLimitBytes;
        clampUndoRedoSteps();
        Q_EMIT undoRedoStateChanged();
        Q_EMIT undoHistoryChanged();
    }
}

void PDFUndoRedoManager::clampUndoRedoSteps()
{
    const size_t beforeUndoSize = m_undoSteps.size();
    const size_t beforeRedoSize = m_redoSteps.size();
    if (m_undoSteps.size() > m_undoLimit)
    {
        // We erase from oldest steps to newest
        for (auto it = m_undoSteps.begin(); it != std::next(m_undoSteps.begin(), m_undoSteps.size() - m_undoLimit); ++it)
        {
            m_historyMemoryBytes -= std::min(m_historyMemoryBytes, it->estimatedBytes);
        }
        m_undoSteps.erase(m_undoSteps.begin(), std::next(m_undoSteps.begin(), m_undoSteps.size() - m_undoLimit));
    }
    if (m_redoSteps.size() > m_redoLimit)
    {
        // Newest steps are erased
        for (auto it = std::next(m_redoSteps.begin(), m_redoLimit); it != m_redoSteps.end(); ++it)
        {
            m_historyMemoryBytes -= std::min(m_historyMemoryBytes, it->estimatedBytes);
        }
        m_redoSteps.resize(m_redoLimit);
    }

    while (m_memoryLimitBytes > 0 && m_historyMemoryBytes > m_memoryLimitBytes && !m_undoSteps.empty())
    {
        m_historyMemoryBytes -= std::min(m_historyMemoryBytes, m_undoSteps.front().estimatedBytes);
        m_undoSteps.erase(m_undoSteps.begin());
    }
    while (m_memoryLimitBytes > 0 && m_historyMemoryBytes > m_memoryLimitBytes && !m_redoSteps.empty())
    {
        m_historyMemoryBytes -= std::min(m_historyMemoryBytes, m_redoSteps.back().estimatedBytes);
        m_redoSteps.pop_back();
    }

    if (beforeUndoSize != m_undoSteps.size() || beforeRedoSize != m_redoSteps.size())
    {
        markHistoryTruncated();
    }
}

size_t PDFUndoRedoManager::estimateDocumentBytes(const pdf::PDFDocument* document)
{
    if (!document)
    {
        return 0;
    }

    const qint64 fileSize = pdf::PDFDocumentWriter::getDocumentFileSize(document);
    if (fileSize <= 0)
    {
        return 0;
    }
    return static_cast<size_t>(fileSize);
}

void PDFUndoRedoManager::markHistoryTruncated()
{
    if (!m_historyTruncated)
    {
        m_historyTruncated = true;
    }
    Q_EMIT undoHistoryTruncated();
}

QStringList PDFUndoRedoManager::getUndoNames() const
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(m_undoSteps.size()));
    for (const UndoRedoItem& item : m_undoSteps)
    {
        names.append(item.name);
    }
    return names;
}

bool PDFUndoRedoManager::isCurrentSaved() const
{
    return m_isCurrentSaved;
}

void PDFUndoRedoManager::setIsCurrentSaved(bool newIsCurrentSaved)
{
    m_isCurrentSaved = newIsCurrentSaved;
}

}   // namespace pdfviewer
