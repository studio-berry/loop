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

#ifndef DOCUMENTCONTEXTSOURCE_H
#define DOCUMENTCONTEXTSOURCE_H

#include "interactionglobal.h"

#include "pdfdocumentcontext.h"

#include <QObject>
#include <QPointer>

namespace pdfinteraction
{

/// Read-only view of the one revision fence, for code that must decide whether a
/// result is still current without holding a pdf::PDFDocumentContext.
///
/// The fence is the complete pdf::PDFRevisionIdentity value. Comparing a subset
/// of its fields, or reconciling a mismatch heuristically, is what this seam
/// exists to prevent; see docs/REVISION_CONTEXT.md.
class IDocumentRevisionSource
{
public:
    virtual ~IDocumentRevisionSource() = default;

    /// Returns the active revision, or a default-constructed (invalid) identity
    /// when no document is bound.
    virtual pdf::PDFRevisionIdentity currentRevision() const = 0;

    /// Returns whether a captured revision still matches the active one.
    virtual bool isCurrent(const pdf::PDFRevisionIdentity& revision) const = 0;
};

/// Binds IDocumentRevisionSource to a live pdf::PDFDocumentContext.
///
/// The context is observed, never owned, and may outlive or predecease this
/// object in either order. A destroyed context degrades to an invalid revision
/// rather than a dangling read, so a late async completion tested against this
/// source is rejected instead of admitted.
class PDFDocumentContextSource final : public QObject, public IDocumentRevisionSource
{
    Q_OBJECT

public:
    explicit PDFDocumentContextSource(pdf::PDFDocumentContext* context, QObject* parent = nullptr);
    ~PDFDocumentContextSource() override;

    PDFDocumentContextSource(const PDFDocumentContextSource&) = delete;
    PDFDocumentContextSource& operator=(const PDFDocumentContextSource&) = delete;

    pdf::PDFRevisionIdentity currentRevision() const override;
    bool isCurrent(const pdf::PDFRevisionIdentity& revision) const override;

    /// Returns the observed context, or nullptr once it has been destroyed.
    pdf::PDFDocumentContext* context() const;

    /// Returns whether a context is still bound.
    bool isBound() const;

    /// Returns the key under which pdf::PDFJobScheduler tracks this document's
    /// current revision. Empty when no context is bound, which the scheduler
    /// treats as "never stale" — callers must not submit work under an empty key
    /// and expect staleness fencing.
    QString documentKey() const;

signals:
    /// Forwards pdf::PDFDocumentContext::revisionChanged so presentation code can
    /// observe the fence without depending on the context type.
    void revisionChanged(const pdf::PDFRevisionIdentity& previous, const pdf::PDFRevisionIdentity& current);

private:
    QPointer<pdf::PDFDocumentContext> m_context;
};

}   // namespace pdfinteraction

#endif   // DOCUMENTCONTEXTSOURCE_H
