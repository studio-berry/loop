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

#ifndef PDFUNDOREDOMANAGER_H
#define PDFUNDOREDOMANAGER_H

#include "pdfdocument.h"
#include "pdfviewerglobal.h"

#include <QObject>

#include <QStringList>
#include <vector>

namespace pdfviewer
{

/// Undo/Redo document manager, it is managing undo and redo steps,
/// when document is modified.
class LOUPELIBGUILIBSHARED_EXPORT PDFUndoRedoManager : public QObject
{
    Q_OBJECT

private:
    using BaseClass = QObject;

public:
    explicit PDFUndoRedoManager(QObject* parent);
    virtual ~PDFUndoRedoManager() override;

    bool canUndo() const { return !m_undoSteps.empty(); }
    bool canRedo() const { return !m_redoSteps.empty(); }

    /// Performs single undo step. If Undo action can't be performed,
    /// then nothing happens.
    void doUndo();

    /// Performs single redo step. If Redo action can't be performed,
    /// then nothing happens.
    void doRedo();

    /// Clears all undo/redo steps
    void clear();

    /// Create undo action (after document modification)
    /// \param document Document created by modification
    /// \param oldDocument Old document
    void createUndo(pdf::PDFModifiedDocument document, pdf::PDFDocumentPointer oldDocument);

    /// Create a named undo action. The name is retained for a future history
    /// surface; the current 0.2.0-alpha implementation deliberately adds no GUI.
    void createUndo(pdf::PDFModifiedDocument document,
                     pdf::PDFDocumentPointer oldDocument,
                     const QString& name);

    /// Sets maximum steps for undo/redo
    /// \param undoLimit Maximum undo steps
    /// \param redoLimit Maximum redo steps
    void setMaximumSteps(size_t undoLimit, size_t redoLimit);

    /// Bounds retained history by estimated serialized document bytes in
    /// addition to the step limits. A zero value disables the byte bound.
    void setMemoryLimitBytes(size_t memoryLimitBytes);

    size_t getMemoryLimitBytes() const { return m_memoryLimitBytes; }
    size_t getHistoryMemoryBytes() const { return m_historyMemoryBytes; }
    bool isHistoryTruncated() const { return m_historyTruncated; }
    QStringList getUndoNames() const;

    /// Returns true, if document was saved
    bool isCurrentSaved() const;

    /// Sets flag, if document was saved
    void setIsCurrentSaved(bool newIsCurrentSaved = true);

signals:
    /// This signals are emitted, when undo/redo action availability has
    /// been changed (for example, user pressed undo/redo action)
    void undoRedoStateChanged();

    /// Emitted when the bounded history changes, including when old entries
    /// are evicted to stay within the configured memory budget.
    void undoHistoryChanged();

    /// Emitted when the memory or step bound evicts an undo/redo entry.
    void undoHistoryTruncated();

    /// This signal is being emitted, when user performs undo/redo action.
    /// Before signal is emitted, this object is in corrected state, as action
    /// is performed.
    /// \param document Document
    void documentChangeRequest(pdf::PDFModifiedDocument document);

private:
    /// Clamps undo/redo steps so they fit the limits
    void clampUndoRedoSteps();

    struct UndoRedoItem
    {
        explicit inline UndoRedoItem() = default;
        explicit inline UndoRedoItem(pdf::PDFDocumentPointer oldDocument,
                                     pdf::PDFDocumentPointer newDocument,
                                     pdf::PDFModifiedDocument::ModificationFlags flags,
                                     QString name,
                                     size_t estimatedBytes) :
            oldDocument(qMove(oldDocument)),
            newDocument(qMove(newDocument)),
            flags(flags),
            name(qMove(name)),
            estimatedBytes(estimatedBytes)
        {

        }

        pdf::PDFDocumentPointer oldDocument;
        pdf::PDFDocumentPointer newDocument;
        pdf::PDFModifiedDocument::ModificationFlags flags = pdf::PDFModifiedDocument::None;
        QString name;
        size_t estimatedBytes = 0;
    };

    static size_t estimateDocumentBytes(const pdf::PDFDocument* document);
    void markHistoryTruncated();

    size_t m_undoLimit = 0;
    size_t m_redoLimit = 0;
    size_t m_memoryLimitBytes = 256ULL * 1024ULL * 1024ULL;
    size_t m_historyMemoryBytes = 0;
    std::vector<UndoRedoItem> m_undoSteps;
    std::vector<UndoRedoItem> m_redoSteps;
    bool m_historyTruncated = false;
    bool m_isCurrentSaved = true;
};

}   // namespace pdfviewer

#endif // PDFUNDOREDOMANAGER_H
